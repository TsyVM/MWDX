// Creates D3D11 resources from D3D9 descriptions.
//
// Turns a D3D9 pool and usage combination into D3D11 usage, bind and CPU
// access flags, and creates the staging companion for resources the game may
// lock. Views are created up front, because D3D11 will not add a bind flag to
// an existing resource -- a texture that later turns out to be needed as a
// render target cannot be promoted, so anything that might be one has to be
// created that way from the start.
//
// Resource creation failures are logged in full, with the dimensions, format
// and flags that were requested. A game that cannot create a texture generally
// continues without it, so the visible symptom is an untextured object rather
// than an error, and the log is the only place the cause appears.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <core/ResourceManager.h>
#include <core/FormatConverter.h>
#include <core/Log.h>

namespace dx9to11 {

namespace {
constexpr UINT kDynamicRingSize = 64u * 1024u * 1024u;

UINT BindFlagsForUsage(DWORD d3d9Usage) noexcept
{
    UINT flags = D3D11_BIND_SHADER_RESOURCE;
    if (d3d9Usage & D3DUSAGE_RENDERTARGET)   flags |= D3D11_BIND_RENDER_TARGET;
    if (d3d9Usage & D3DUSAGE_DEPTHSTENCIL)   flags |= D3D11_BIND_DEPTH_STENCIL;
    return flags;
}
}

ResourceManager::ResourceManager(DeviceContext11* ctx) noexcept
    : m_ctx(ctx)
{
}

PoolPolicy ResourceManager::MapPool(D3DPOOL pool, DWORD usage) noexcept
{
    PoolPolicy policy;
    const bool dynamic = (usage & D3DUSAGE_DYNAMIC) != 0;

    switch (pool) {
        case D3DPOOL_DEFAULT:
            policy.usage          = dynamic ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_DEFAULT;
            policy.cpuAccessFlags = dynamic ? D3D11_CPU_ACCESS_WRITE : 0;

            policy.needsStaging   = false;
            policy.needsShadowCopy = false;
            policy.gpuVisible     = true;
            break;

        case D3DPOOL_MANAGED:

            policy.usage           = D3D11_USAGE_DEFAULT;
            policy.cpuAccessFlags  = 0;
            policy.needsShadowCopy = true;
            policy.needsStaging    = false;
            policy.gpuVisible      = true;
            break;

        case D3DPOOL_SYSTEMMEM:
            policy.usage           = D3D11_USAGE_STAGING;
            policy.cpuAccessFlags  = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            policy.needsShadowCopy = false;
            policy.needsStaging    = true;
            policy.gpuVisible      = false;
            break;

        case D3DPOOL_SCRATCH:
        default:

            policy.usage      = D3D11_USAGE_STAGING;
            policy.gpuVisible = false;
            break;
    }
    return policy;
}

HRESULT ResourceManager::CreateTexture2D(
    UINT                       width,
    UINT                       height,
    UINT                       levels,
    DWORD                      usage,
    D3DFORMAT                  format,
    D3DPOOL                    pool,
    ID3D11Texture2D**          ppTexture,
    ID3D11ShaderResourceView** ppSRV,
    ID3D11Texture2D**          ppStaging,
    ID3D11ShaderResourceView** ppSRVSrgb) noexcept
{
    if (!ppTexture || width == 0 || height == 0)
        return D3DERR_INVALIDCALL;

    const FormatMapping mapping = FormatConverter::ToDxgi(format);
    if (!mapping.IsValid()) {
        DXLOG_WARN("CreateTexture2D rejected: D3DFORMAT=%d (0x%08X) has no DXGI "
                   "mapping - the game's CreateTexture will fail for this format "
                   "(%ux%u usage=0x%X)",
                   (int)format, (unsigned)format, width, height, (unsigned)usage);
        return D3DERR_NOTAVAILABLE;
    }

    const PoolPolicy policy = MapPool(pool, usage);
    const UINT mipLevels = (levels == 0) ? 0u   : levels;

    const bool isDepth = FormatConverter::IsDepthFormat(format) ||
                         (usage & D3DUSAGE_DEPTHSTENCIL) != 0;
    FormatConverter::DepthFormatViews dv{};
    const bool depthReadable = isDepth &&
                               FormatConverter::GetDepthFormatViews(format, dv) &&
                               dv.srvReadable;

    const bool wantsSrgbView = !isDepth && (ppSRVSrgb != nullptr) &&
                               FormatConverter::HasSRGBVariant(mapping.dxgiFormat);
    const DXGI_FORMAT resourceFormat =
        isDepth        ? dv.resource
      : wantsSrgbView  ? FormatConverter::ToTypeless(mapping.dxgiFormat)
                       : mapping.dxgiFormat;

    const bool wantsAutogen  = (mipLevels == 0) && (usage & D3DUSAGE_AUTOGENMIPMAP);
    bool       autogenViable = wantsAutogen && !isDepth &&
                               !FormatConverter::IsBlockCompressed(format);
    if (autogenViable) {
        switch (mapping.dxgiFormat) {
        case DXGI_FORMAT_B5G6R5_UNORM:
        case DXGI_FORMAT_B5G5R5A1_UNORM:
        case DXGI_FORMAT_B4G4R4A4_UNORM:
        case DXGI_FORMAT_A8_UNORM:
            autogenViable = false;
            break;
        default: break;
        }
    }
    if (wantsAutogen && !autogenViable) {
        static bool s_autogenFallback = false;
        if (!s_autogenFallback) {
            s_autogenFallback = true;
            OutputDebugStringA("[dx9to11] AUTOGENMIPMAP on a format D3D11 "
                               "cannot GenerateMips (DXT/16bpp/A8) - created "
                               "single-level, matching the D3D9 hint-fallback "
                               "\n");
        }
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = width;
    desc.Height         = height;
    desc.MipLevels      = (wantsAutogen && !autogenViable) ? 1u : mipLevels;
    desc.ArraySize      = 1;
    desc.Format         = resourceFormat;
    desc.SampleDesc     = { 1, 0 };
    desc.Usage          = policy.usage;
    desc.BindFlags      = (policy.usage == D3D11_USAGE_STAGING) ? 0 : BindFlagsForUsage(usage);
    desc.CPUAccessFlags = policy.cpuAccessFlags;
    desc.MiscFlags      = autogenViable ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

    if (desc.MiscFlags & D3D11_RESOURCE_MISC_GENERATE_MIPS)
        desc.BindFlags |= D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (isDepth && !depthReadable)
        desc.BindFlags &= ~UINT(D3D11_BIND_SHADER_RESOURCE);

    HRESULT hr = m_ctx->Device()->CreateTexture2D(&desc, nullptr, ppTexture);
    if (FAILED(hr)) {
        DXLOG_HR(hr, "CreateTexture2D failed: %ux%u mips=%u d3d9fmt=%d "
                     "dxgiRes=%d bind=0x%X usage=0x%X pool=%d",
                 width, height, mipLevels, (int)format, (int)resourceFormat,
                 (unsigned)desc.BindFlags, (unsigned)usage, (int)pool);
        return hr;
    }

    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) {
        DXLOG_INFO("%s created: %ux%u mips=%u d3d9fmt=%d dxgiRes=%d dxgiMap=%d "
                   "bind=0x%X pool=%d",
                   (usage & D3DUSAGE_RENDERTARGET) ? "RENDERTARGET" : "DEPTHSTENCIL",
                   width, height, mipLevels, (int)format, (int)resourceFormat,
                   (int)mapping.dxgiFormat, (unsigned)desc.BindFlags, (int)pool);
    } else {
        DXLOG_TRACE("texture created: %ux%u mips=%u d3d9fmt=%d dxgiRes=%d bind=0x%X",
                    width, height, mipLevels, (int)format, (int)resourceFormat,
                    (unsigned)desc.BindFlags);
    }

    if (ppSRVSrgb) *ppSRVSrgb = nullptr;

    if (isDepth) {
        if (ppSRV) {
            *ppSRV = nullptr;
            if (depthReadable && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
                D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
                svd.Format                    = dv.srv;
                svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
                svd.Texture2D.MostDetailedMip = 0;
                svd.Texture2D.MipLevels       = (mipLevels == 0) ? static_cast<UINT>(-1) : mipLevels;
                hr = m_ctx->Device()->CreateShaderResourceView(*ppTexture, &svd, ppSRV);
                if (FAILED(hr))
                    *ppSRV = nullptr;
            }
        }
    } else if (ppSRV && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {

        if (wantsSrgbView) {
            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            svd.Format                    = FormatConverter::ToLinearView(resourceFormat);
            svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MostDetailedMip = 0;
            svd.Texture2D.MipLevels       = mipLevels == 0 ? static_cast<UINT>(-1) : mipLevels;
            hr = m_ctx->Device()->CreateShaderResourceView(*ppTexture, &svd, ppSRV);
        } else {
            hr = m_ctx->Device()->CreateShaderResourceView(*ppTexture, nullptr, ppSRV);
        }
        if (FAILED(hr)) {
            (*ppTexture)->Release();
            *ppTexture = nullptr;
            return hr;
        }

        if (wantsSrgbView) {
            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            svd.Format                    = FormatConverter::ToSRGBView(resourceFormat);
            svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MostDetailedMip = 0;
            svd.Texture2D.MipLevels       = mipLevels == 0 ? static_cast<UINT>(-1) : mipLevels;
            hr = m_ctx->Device()->CreateShaderResourceView(*ppTexture, &svd, ppSRVSrgb);
            if (FAILED(hr)) {

                *ppSRVSrgb = nullptr;
                OutputDebugStringA("[dx9to11] sRGB SRV creation failed for an "
                                    "sRGB-capable format; falling back to "
                                    "linear-only sampling for this texture\n");
            }
        }
    } else if (ppSRV) {
        *ppSRV = nullptr;
    }

    if (ppStaging) {
        *ppStaging = nullptr;

        if (pool == D3DPOOL_DEFAULT && !(usage & D3DUSAGE_DYNAMIC) && !isDepth) {

            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.BindFlags      = 0;
            stagingDesc.Usage          = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            stagingDesc.MiscFlags      = 0;
            hr = m_ctx->Device()->CreateTexture2D(&stagingDesc, nullptr, ppStaging);
            if (FAILED(hr)) {
                if (ppSRVSrgb && *ppSRVSrgb) { (*ppSRVSrgb)->Release(); *ppSRVSrgb = nullptr; }
                if (ppSRV && *ppSRV) { (*ppSRV)->Release(); *ppSRV = nullptr; }
                (*ppTexture)->Release();
                *ppTexture = nullptr;
                return hr;
            }
        }
    }

    return S_OK;
}

HRESULT ResourceManager::CreateTexture3D(
    UINT                       width,
    UINT                       height,
    UINT                       depth,
    UINT                       levels,
    DWORD                      usage,
    D3DFORMAT                  format,
    D3DPOOL                    pool,
    ID3D11Texture3D**          ppTexture,
    ID3D11ShaderResourceView** ppSRV,
    ID3D11Texture3D**          ppStaging) noexcept
{
    if (!ppTexture || width == 0 || height == 0 || depth == 0)
        return D3DERR_INVALIDCALL;

    const FormatMapping mapping = FormatConverter::ToDxgiVolume(format);
    if (!mapping.IsValid())
        return D3DERR_NOTAVAILABLE;
    if (FormatConverter::IsCpuExpandedLuminance(format)) {
        static bool s_lumVolWarned = false;
        if (!s_lumVolWarned) {
            s_lumVolWarned = true;
            OutputDebugStringA("[dx9to11] luminance-format VOLUME texture "
                               "created; samples as (L,0,0,·) not (L,L,L,·) "
                               "(known gap - 2D/cube are hue-correct)\n");
        }
    }

    const PoolPolicy policy = MapPool(pool, usage);

    D3D11_TEXTURE3D_DESC desc{};
    desc.Width          = width;
    desc.Height         = height;
    desc.Depth          = depth;
    desc.MipLevels      = levels;
    desc.Format         = mapping.dxgiFormat;
    desc.Usage          = policy.usage;
    desc.BindFlags      = (policy.usage == D3D11_USAGE_STAGING) ? 0 : D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = policy.cpuAccessFlags;

    HRESULT hr = m_ctx->Device()->CreateTexture3D(&desc, nullptr, ppTexture);
    if (FAILED(hr))
        return hr;

    if (ppSRV && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE)) {
        hr = m_ctx->Device()->CreateShaderResourceView(*ppTexture, nullptr, ppSRV);
        if (FAILED(hr)) {
            (*ppTexture)->Release();
            *ppTexture = nullptr;
            return hr;
        }
    } else if (ppSRV) {
        *ppSRV = nullptr;
    }

    if (ppStaging) {
        *ppStaging = nullptr;
        if (pool == D3DPOOL_DEFAULT && !(usage & D3DUSAGE_DYNAMIC)) {
            D3D11_TEXTURE3D_DESC stagingDesc = desc;
            stagingDesc.BindFlags      = 0;
            stagingDesc.Usage          = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            hr = m_ctx->Device()->CreateTexture3D(&stagingDesc, nullptr, ppStaging);
            if (FAILED(hr)) {
                if (ppSRV && *ppSRV) { (*ppSRV)->Release(); *ppSRV = nullptr; }
                (*ppTexture)->Release();
                *ppTexture = nullptr;
                return hr;
            }
        }
    }

    return S_OK;
}

HRESULT ResourceManager::CreateRenderTargetTexture(
    UINT                     width,
    UINT                     height,
    D3DFORMAT                format,
    D3DMULTISAMPLE_TYPE      multiSample,
    DWORD                    multiSampleQuality,
    BOOL                     lockable,
    ID3D11Texture2D**        ppTexture,
    ID3D11RenderTargetView** ppRTV,
    ID3D11Texture2D**        ppStaging,
    ID3D11RenderTargetView** ppRTVSrgb) noexcept
{
    if (!ppTexture || !ppRTV || width == 0 || height == 0)
        return D3DERR_INVALIDCALL;

    const FormatMapping mapping = FormatConverter::ToDxgi(format);
    if (!mapping.IsValid())
        return D3DERR_NOTAVAILABLE;

    const bool wantsSrgbView = (ppRTVSrgb != nullptr) &&
                               FormatConverter::HasSRGBVariant(mapping.dxgiFormat);
    const DXGI_FORMAT resourceFormat = wantsSrgbView
        ? FormatConverter::ToTypeless(mapping.dxgiFormat)
        : mapping.dxgiFormat;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = width;
    desc.Height         = height;
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = resourceFormat;
    desc.SampleDesc     = { static_cast<UINT>(multiSample) > 1 ? static_cast<UINT>(multiSample) : 1u,
                             multiSampleQuality };
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = m_ctx->Device()->CreateTexture2D(&desc, nullptr, ppTexture);
    if (FAILED(hr))
        return hr;

    if (ppRTVSrgb) *ppRTVSrgb = nullptr;

    if (wantsSrgbView) {
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format        = FormatConverter::ToLinearView(resourceFormat);
        rd.ViewDimension = (desc.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                                        : D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = m_ctx->Device()->CreateRenderTargetView(*ppTexture, &rd, ppRTV);
    } else {
        hr = m_ctx->Device()->CreateRenderTargetView(*ppTexture, nullptr, ppRTV);
    }
    if (FAILED(hr)) {
        (*ppTexture)->Release();
        *ppTexture = nullptr;
        return hr;
    }

    if (wantsSrgbView) {
        D3D11_RENDER_TARGET_VIEW_DESC rdSrgb{};
        rdSrgb.Format        = FormatConverter::ToSRGBView(resourceFormat);
        rdSrgb.ViewDimension = (desc.SampleDesc.Count > 1) ? D3D11_RTV_DIMENSION_TEXTURE2DMS
                                                            : D3D11_RTV_DIMENSION_TEXTURE2D;
        hr = m_ctx->Device()->CreateRenderTargetView(*ppTexture, &rdSrgb, ppRTVSrgb);
        if (FAILED(hr)) {

            *ppRTVSrgb = nullptr;
            OutputDebugStringA("[dx9to11] sRGB RTV creation failed for an "
                                "sRGB-capable format; falling back to "
                                "linear-only writes for this render target\n");
        }
    }

    if (ppStaging) {
        *ppStaging = nullptr;
        if (lockable) {
            D3D11_TEXTURE2D_DESC stagingDesc = desc;
            stagingDesc.BindFlags      = 0;
            stagingDesc.Usage          = D3D11_USAGE_STAGING;
            stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
            stagingDesc.SampleDesc     = { 1, 0 };
            hr = m_ctx->Device()->CreateTexture2D(&stagingDesc, nullptr, ppStaging);
            if (FAILED(hr)) {
                if (ppRTVSrgb && *ppRTVSrgb) { (*ppRTVSrgb)->Release(); *ppRTVSrgb = nullptr; }
                (*ppRTV)->Release();     *ppRTV = nullptr;
                (*ppTexture)->Release(); *ppTexture = nullptr;
                return hr;
            }
        }
    }

    return S_OK;
}

HRESULT ResourceManager::CreateDepthStencilTexture(
    UINT                       width,
    UINT                       height,
    D3DFORMAT                  format,
    D3DMULTISAMPLE_TYPE        multiSample,
    DWORD                      multiSampleQuality,
    BOOL                       discard,
    ID3D11Texture2D**          ppTexture,
    ID3D11DepthStencilView**   ppDSV,
    ID3D11ShaderResourceView** ppSRV,
    ID3D11Texture2D**          ppStaging) noexcept
{
    (void)discard;

    if (!ppTexture || !ppDSV || width == 0 || height == 0)
        return D3DERR_INVALIDCALL;

    FormatConverter::DepthFormatViews dv{};
    if (!FormatConverter::GetDepthFormatViews(format, dv))
        return D3DERR_INVALIDCALL;

    const bool multisampled = static_cast<UINT>(multiSample) > 1;

    const bool srvReadable = dv.srvReadable && !multisampled;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = width;
    desc.Height         = height;
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = dv.resource;
    desc.SampleDesc     = { multisampled ? static_cast<UINT>(multiSample) : 1u,
                             multiSampleQuality };
    desc.Usage          = D3D11_USAGE_DEFAULT;
    desc.BindFlags      = D3D11_BIND_DEPTH_STENCIL |
                          (srvReadable ? D3D11_BIND_SHADER_RESOURCE : 0u);

    HRESULT hr = m_ctx->Device()->CreateTexture2D(&desc, nullptr, ppTexture);
    if (FAILED(hr))
        return hr;

    D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
    dvd.Format        = dv.dsv;
    dvd.ViewDimension = multisampled ? D3D11_DSV_DIMENSION_TEXTURE2DMS
                                     : D3D11_DSV_DIMENSION_TEXTURE2D;

    hr = m_ctx->Device()->CreateDepthStencilView(*ppTexture, &dvd, ppDSV);
    if (FAILED(hr)) {
        (*ppTexture)->Release();
        *ppTexture = nullptr;
        return hr;
    }

    if (ppSRV) {
        *ppSRV = nullptr;
        if (srvReadable) {

            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            svd.Format                    = dv.srv;
            svd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            svd.Texture2D.MostDetailedMip = 0;
            svd.Texture2D.MipLevels       = 1;
            hr = m_ctx->Device()->CreateShaderResourceView(*ppTexture, &svd, ppSRV);
            if (FAILED(hr))
                *ppSRV = nullptr;
        }
    }

    if (ppStaging) {
        *ppStaging = nullptr;

        D3D11_TEXTURE2D_DESC stagingDesc = desc;
        stagingDesc.BindFlags      = 0;
        stagingDesc.Usage          = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        stagingDesc.SampleDesc     = { 1, 0 };
        hr = m_ctx->Device()->CreateTexture2D(&stagingDesc, nullptr, ppStaging);
        if (FAILED(hr)) {

            *ppStaging = nullptr;
        }
    }

    return S_OK;
}

HRESULT ResourceManager::CreateOffscreenPlainSurface(
    UINT              width,
    UINT              height,
    D3DFORMAT         format,
    D3DPOOL           pool,
    ID3D11Texture2D** ppStaging) noexcept
{
    if (!ppStaging || width == 0 || height == 0)
        return D3DERR_INVALIDCALL;

    if (pool != D3DPOOL_SYSTEMMEM && pool != D3DPOOL_DEFAULT)
        return D3DERR_INVALIDCALL;

    const FormatMapping mapping = FormatConverter::ToDxgi(format);
    if (!mapping.IsValid())
        return D3DERR_NOTAVAILABLE;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width          = width;
    desc.Height         = height;
    desc.MipLevels      = 1;
    desc.ArraySize      = 1;
    desc.Format         = mapping.dxgiFormat;
    desc.SampleDesc     = { 1, 0 };
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;

    return m_ctx->Device()->CreateTexture2D(&desc, nullptr, ppStaging);
}

HRESULT ResourceManager::CreateStaticBuffer(
    UINT           byteWidth,
    UINT           bindFlags,
    D3DPOOL        pool,
    ID3D11Buffer** ppBuffer,
    ID3D11Buffer** ppStaging) noexcept
{
    if (!ppBuffer || byteWidth == 0)
        return D3DERR_INVALIDCALL;

    const PoolPolicy policy = MapPool(pool, 0  );

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth      = byteWidth;
    desc.Usage          = policy.usage;
    desc.BindFlags      = (policy.usage == D3D11_USAGE_STAGING) ? 0 : bindFlags;
    desc.CPUAccessFlags = policy.cpuAccessFlags;

    HRESULT hr = m_ctx->Device()->CreateBuffer(&desc, nullptr, ppBuffer);
    if (FAILED(hr))
        return hr;

    if (ppStaging) {
        *ppStaging = nullptr;

        D3D11_BUFFER_DESC stagingDesc{};
        stagingDesc.ByteWidth      = byteWidth;
        stagingDesc.Usage          = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
        hr = m_ctx->Device()->CreateBuffer(&stagingDesc, nullptr, ppStaging);
        if (FAILED(hr)) {
            (*ppBuffer)->Release();
            *ppBuffer = nullptr;
            return hr;
        }
    }

    return S_OK;
}

HRESULT ResourceManager::CreateDynamicBuffer(
    UINT           byteWidth,
    UINT           bindFlags,
    ID3D11Buffer** ppBuffer) noexcept
{
    if (!ppBuffer || byteWidth == 0)
        return D3DERR_INVALIDCALL;

    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth      = byteWidth;
    desc.Usage          = D3D11_USAGE_DYNAMIC;
    desc.BindFlags      = bindFlags;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_ctx->Device()->CreateBuffer(&desc, nullptr, ppBuffer);
    if (FAILED(hr))
        DXLOG_HR(hr, "CreateDynamicBuffer failed: %u bytes bind=0x%X",
                 byteWidth, (unsigned)bindFlags);
    return hr;
}

DynamicRingBuffer* ResourceManager::GetOrCreateDynamicVertexRing() noexcept
{
    if (!m_dynamicVertexRing) {
        DynamicRingBuffer* raw = nullptr;
        if (SUCCEEDED(DynamicRingBuffer::Create(
                m_ctx->Device(), kDynamicRingSize, D3D11_BIND_VERTEX_BUFFER, &raw))) {
            m_dynamicVertexRing.reset(raw);
        }
    }
    return m_dynamicVertexRing.get();
}

DynamicRingBuffer* ResourceManager::GetOrCreateDynamicIndexRing() noexcept
{
    if (!m_dynamicIndexRing) {
        DynamicRingBuffer* raw = nullptr;
        if (SUCCEEDED(DynamicRingBuffer::Create(
                m_ctx->Device(), kDynamicRingSize, D3D11_BIND_INDEX_BUFFER, &raw))) {
            m_dynamicIndexRing.reset(raw);
        }
    }
    return m_dynamicIndexRing.get();
}

DynamicRingBuffer* ResourceManager::GetOrCreateFanIndexRing() noexcept
{
    static constexpr UINT kFanRingSize = 4u * 1024u * 1024u;
    if (!m_fanIndexRing) {
        DynamicRingBuffer* raw = nullptr;
        if (SUCCEEDED(DynamicRingBuffer::Create(
                m_ctx->Device(), kFanRingSize, D3D11_BIND_INDEX_BUFFER, &raw))) {
            m_fanIndexRing.reset(raw);
        }
    }
    return m_fanIndexRing.get();
}

}
