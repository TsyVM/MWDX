// Resource creation, locking, and copying between resources.
//
// Locking is where D3D9 and D3D11 diverge most. D3D9 lets a game map nearly
// anything; D3D11 permits mapping only on resources created for it. Resources
// the game may lock therefore carry a staging companion, and a lock copies
// down into it, hands back its memory, and copies back on unlock.
//
// The lock flags are worth honouring rather than ignoring. D3DLOCK_DISCARD and
// D3DLOCK_NOOVERWRITE are promises about what the caller will touch, and they
// map onto the D3D11 map types that let the driver avoid waiting for the GPU.
// Treating every lock as a full read-modify-write is correct but slow enough
// to be visible.
//
// D3DLOCK_READONLY skips the copy back on unlock, which matters because that
// copy is the expensive half.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Device.h>
#include <d3d9proxy/D9Texture.h>
#include <d3d9proxy/D9VolumeTexture.h>
#include <d3d9proxy/D9CubeTexture.h>
#include <d3d9proxy/D9Surface.h>
#include <d3d9proxy/D9VertexBuffer.h>
#include <d3d9proxy/D9IndexBuffer.h>
#include <core/ResourceManager.h>
#include <core/FormatConverter.h>
#include <core/Log.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

namespace dx9to11 {

ResourceManager* D9Device::ResMgr() noexcept
{
    if (!m_resMgr) {
        m_resMgr = std::make_unique<ResourceManager>(m_ctx.get());
    }
    return m_resMgr.get();
}

HRESULT STDMETHODCALLTYPE D9Device::CreateTexture(
    UINT Width, UINT Height, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DTexture9** ppTexture, HANDLE*  )
{
    if (!ppTexture) return D3DERR_INVALIDCALL;
    *ppTexture = nullptr;

    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11ShaderResourceView> srv, srvSrgb;
    ComPtr<ID3D11Texture2D>          staging;

    HRESULT hr = ResMgr()->CreateTexture2D(
        Width, Height, Levels, Usage, Format, Pool,
        tex.GetAddressOf(), srv.GetAddressOf(), staging.GetAddressOf(),
        srvSrgb.GetAddressOf());
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC desc{};
    tex->GetDesc(&desc);

    const UINT reportedLevels =
        (Usage & D3DUSAGE_AUTOGENMIPMAP) ? 1u : desc.MipLevels;

    auto* obj = new (std::nothrow) D9Texture(
        this, Width, Height, reportedLevels, Usage, Format, Pool,
        std::move(tex), std::move(srv), std::move(staging), std::move(srvSrgb));
    if (!obj) return E_OUTOFMEMORY;

    *ppTexture = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateVolumeTexture(
    UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE*  )
{
    if (!ppVolumeTexture) return D3DERR_INVALIDCALL;
    *ppVolumeTexture = nullptr;

    ComPtr<ID3D11Texture3D>          tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Texture3D>          staging;

    HRESULT hr = ResMgr()->CreateTexture3D(
        Width, Height, Depth, Levels, Usage, Format, Pool,
        tex.GetAddressOf(), srv.GetAddressOf(), staging.GetAddressOf());
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE3D_DESC desc{};
    tex->GetDesc(&desc);

    auto* obj = new (std::nothrow) D9VolumeTexture(
        this, Width, Height, Depth, desc.MipLevels, Usage, Format, Pool,
        std::move(tex), std::move(srv), std::move(staging));
    if (!obj) return E_OUTOFMEMORY;

    *ppVolumeTexture = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateCubeTexture(
    UINT EdgeLength, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture9** ppCubeTexture, HANDLE*  )
{
    if (!ppCubeTexture) return D3DERR_INVALIDCALL;
    *ppCubeTexture = nullptr;

    std::array<D9CubeTexture::Face, 6> faces{};

    for (UINT fi = 0; fi < 6; ++fi) {
        ComPtr<ID3D11Texture2D>          tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        ComPtr<ID3D11Texture2D>          staging;

        HRESULT hr = ResMgr()->CreateTexture2D(
            EdgeLength, EdgeLength, Levels, Usage, Format, Pool,
            tex.GetAddressOf(), srv.GetAddressOf(), staging.GetAddressOf());
        if (FAILED(hr)) return hr;

        faces[fi].texture = std::move(tex);
        faces[fi].srv     = std::move(srv);
        faces[fi].staging = std::move(staging);
    }

    D3D11_TEXTURE2D_DESC desc{};
    faces[0].texture->GetDesc(&desc);

    auto* obj = new (std::nothrow) D9CubeTexture(
        this, EdgeLength, desc.MipLevels, Usage, Format, Pool, std::move(faces));
    if (!obj) return E_OUTOFMEMORY;

    *ppCubeTexture = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateVertexBuffer(
    UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE*  )
{
    if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
    *ppVertexBuffer = nullptr;

    ComPtr<ID3D11Buffer>  buffer;
    ComPtr<ID3D11Buffer>  staging;

    if (Usage & D3DUSAGE_DYNAMIC) {

        HRESULT hr = ResMgr()->CreateDynamicBuffer(
            Length, D3D11_BIND_VERTEX_BUFFER, buffer.GetAddressOf());
        if (FAILED(hr)) return hr;
    } else {
        HRESULT hr = ResMgr()->CreateStaticBuffer(
            Length, D3D11_BIND_VERTEX_BUFFER, Pool,
            buffer.GetAddressOf(), staging.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    auto* obj = new (std::nothrow) D9VertexBuffer(
        this, Length, Usage, FVF, Pool,
        std::move(buffer), std::move(staging));
    if (!obj) return E_OUTOFMEMORY;

    *ppVertexBuffer = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateIndexBuffer(
    UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE*  )
{
    if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
    if (Format != D3DFMT_INDEX16 && Format != D3DFMT_INDEX32)
        return D3DERR_INVALIDCALL;
    *ppIndexBuffer = nullptr;

    ComPtr<ID3D11Buffer>  buffer;
    ComPtr<ID3D11Buffer>  staging;

    if (Usage & D3DUSAGE_DYNAMIC) {

        HRESULT hr = ResMgr()->CreateDynamicBuffer(
            Length, D3D11_BIND_INDEX_BUFFER, buffer.GetAddressOf());
        if (FAILED(hr)) return hr;
    } else {
        HRESULT hr = ResMgr()->CreateStaticBuffer(
            Length, D3D11_BIND_INDEX_BUFFER, Pool,
            buffer.GetAddressOf(), staging.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    auto* obj = new (std::nothrow) D9IndexBuffer(
        this, Length, Usage, Format, Pool,
        std::move(buffer), std::move(staging));
    if (!obj) return E_OUTOFMEMORY;

    *ppIndexBuffer = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateRenderTarget(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE*  )
{
    if (!ppSurface) return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;

    if (FormatConverter::IsNullSurfaceFormat(Format)) {
        D9Surface::Desc ndesc{};
        ndesc.width       = Width;
        ndesc.height      = Height;
        ndesc.format      = Format;
        ndesc.pool        = D3DPOOL_DEFAULT;
        ndesc.usage       = D3DUSAGE_RENDERTARGET;
        ndesc.multiSample = MultiSample;
        ndesc.subresource = 0;
        ndesc.lockable    = Lockable;

        auto* nobj = new (std::nothrow) D9Surface(
            this, ndesc,   nullptr,   nullptr,
              nullptr,   nullptr,   nullptr,   nullptr);
        if (!nobj) return E_OUTOFMEMORY;
        *ppSurface = nobj;
        return D3D_OK;
    }

    ComPtr<ID3D11Texture2D>         tex;
    ComPtr<ID3D11RenderTargetView>  rtv, rtvSrgb;
    ComPtr<ID3D11Texture2D>         staging;

    HRESULT hr = ResMgr()->CreateRenderTargetTexture(
        Width, Height, Format, MultiSample, MultisampleQuality, Lockable,
        tex.GetAddressOf(), rtv.GetAddressOf(), staging.GetAddressOf(),
        rtvSrgb.GetAddressOf());
    if (FAILED(hr)) return hr;

    D9Surface::Desc desc{};
    desc.width       = Width;
    desc.height      = Height;
    desc.format      = Format;
    desc.pool        = D3DPOOL_DEFAULT;
    desc.usage       = D3DUSAGE_RENDERTARGET;
    desc.multiSample = MultiSample;
    desc.subresource = 0;
    desc.lockable    = Lockable;

    auto* obj = new (std::nothrow) D9Surface(
        this, desc, std::move(tex), std::move(staging),
        std::move(rtv),   nullptr,   nullptr,   nullptr,
        std::move(rtvSrgb));
    if (!obj) return E_OUTOFMEMORY;

    *ppSurface = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateDepthStencilSurface(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE*  )
{
    if (!ppSurface) return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;

    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11DepthStencilView>   dsv;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11Texture2D>          staging;

    HRESULT hr = ResMgr()->CreateDepthStencilTexture(
        Width, Height, Format, MultiSample, MultisampleQuality, Discard,
        tex.GetAddressOf(), dsv.GetAddressOf(), srv.GetAddressOf(), staging.GetAddressOf());
    if (FAILED(hr)) return hr;

    D9Surface::Desc desc{};
    desc.width       = Width;
    desc.height      = Height;
    desc.format      = Format;
    desc.pool        = D3DPOOL_DEFAULT;
    desc.usage       = D3DUSAGE_DEPTHSTENCIL;
    desc.multiSample = MultiSample;
    desc.subresource = 0;
    desc.lockable    = FALSE;

    auto* obj = new (std::nothrow) D9Surface(
        this, desc, std::move(tex), std::move(staging),
          nullptr, std::move(dsv), std::move(srv),   nullptr);
    if (!obj) return E_OUTOFMEMORY;

    *ppSurface = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateOffscreenPlainSurface(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DSurface9** ppSurface, HANDLE*  )
{
    if (!ppSurface) return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;

    ComPtr<ID3D11Texture2D> staging;
    HRESULT hr = ResMgr()->CreateOffscreenPlainSurface(
        Width, Height, Format, Pool, staging.GetAddressOf());
    if (FAILED(hr)) return hr;

    D9Surface::Desc desc{};
    desc.width       = Width;
    desc.height      = Height;
    desc.format      = Format;
    desc.pool        = Pool;
    desc.usage       = 0;
    desc.multiSample = D3DMULTISAMPLE_NONE;
    desc.subresource = 0;
    desc.lockable    = TRUE;

    auto* obj = new (std::nothrow) D9Surface(
        this, desc,   nullptr, std::move(staging),
        nullptr, nullptr, nullptr, nullptr);
    if (!obj) return E_OUTOFMEMORY;

    *ppSurface = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
{
    if (RenderTargetIndex >= 4) return D3DERR_INVALIDCALL;

    if (RenderTargetIndex == 0 && !pRenderTarget) return D3DERR_INVALIDCALL;

    if (m_renderTargets[RenderTargetIndex]) {
        m_renderTargets[RenderTargetIndex]->Release();
    }
    m_renderTargets[RenderTargetIndex] = static_cast<D9Surface*>(pRenderTarget);
    if (m_renderTargets[RenderTargetIndex]) {
        m_renderTargets[RenderTargetIndex]->AddRef();
    }
    m_bind.rtDirty = true;

    if (auto* newRT = m_renderTargets[RenderTargetIndex]) {
        auto* rtRes = static_cast<ID3D11Resource*>(newRT->Texture());
        if (rtRes) {
            for (DWORD s = 0; s < 8; ++s) {
                if (m_bind.srvResource[s] == rtRes) {
                    ID3D11ShaderResourceView* nullSrv = nullptr;
                    m_ctx->Context()->PSSetShaderResources(s, 1, &nullSrv);
                    m_bind.srv[s]         = nullptr;
                    m_bind.srvResource[s] = nullptr;
                }
            }
        }
    }

    if (log::Verbose()) {
        if (auto* rt = m_renderTargets[RenderTargetIndex]) {
            const auto& d = rt->Info();
            DXLOG_TRACE("SetRenderTarget[%u] = %ux%u d3d9fmt=%d usage=0x%X",
                        RenderTargetIndex, d.width, d.height,
                        (int)d.format, (unsigned)d.usage);
        } else {
            DXLOG_TRACE("SetRenderTarget[%u] = (unbound)", RenderTargetIndex);
        }
    }

    if (RenderTargetIndex == 0 && m_renderTargets[0]) {
        const auto& d = m_renderTargets[0]->Info();
        if (m_viewport.Width != d.width || m_viewport.Height != d.height)
            m_emuDirty = true;
        m_viewport.X      = 0;
        m_viewport.Y      = 0;
        m_viewport.Width  = d.width;
        m_viewport.Height = d.height;
        m_viewport.MinZ   = 0.0f;
        m_viewport.MaxZ   = 1.0f;
        m_scissorRect     = { 0, 0,
                              static_cast<LONG>(d.width),
                              static_cast<LONG>(d.height) };
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget)
{
    if (!ppRenderTarget) return D3DERR_INVALIDCALL;
    if (RenderTargetIndex >= 4) return D3DERR_INVALIDCALL;

    if (!m_renderTargets[RenderTargetIndex]) {
        *ppRenderTarget = nullptr;
        return D3DERR_NOTFOUND;
    }
    *ppRenderTarget = m_renderTargets[RenderTargetIndex];
    (*ppRenderTarget)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
{
    if (m_depthStencil) m_depthStencil->Release();
    m_depthStencil = static_cast<D9Surface*>(pNewZStencil);
    if (m_depthStencil) m_depthStencil->AddRef();
    m_bind.rtDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface)
{
    if (!ppZStencilSurface) return D3DERR_INVALIDCALL;

    if (!m_depthStencil) {
        *ppZStencilSurface = nullptr;
        return D3DERR_NOTFOUND;
    }
    *ppZStencilSurface = m_depthStencil;
    (*ppZStencilSurface)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::UpdateSurface(
    IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
    IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
{
    if (!pSourceSurface || !pDestinationSurface) return D3DERR_INVALIDCALL;

    auto* src  = static_cast<D9Surface*>(pSourceSurface);
    auto* dst  = static_cast<D9Surface*>(pDestinationSurface);

    if (static_cast<UINT>(src->Info().multiSample) > 1 ||
        static_cast<UINT>(dst->Info().multiSample) > 1)
        return D3DERR_INVALIDCALL;

    ID3D11Texture2D* srcRes = src->Texture() ? src->Texture() : src->Staging();
    ID3D11Texture2D* dstRes = dst->Texture() ? dst->Texture() : dst->Staging();
    if (!srcRes || !dstRes) return D3DERR_INVALIDCALL;

    D3D11_BOX srcBox{};
    const D3D11_BOX* pBox = nullptr;
    if (pSourceRect) {
        srcBox.left   = static_cast<UINT>(pSourceRect->left);
        srcBox.top    = static_cast<UINT>(pSourceRect->top);
        srcBox.right  = static_cast<UINT>(pSourceRect->right);
        srcBox.bottom = static_cast<UINT>(pSourceRect->bottom);
        srcBox.front  = 0;
        srcBox.back   = 1;
        pBox = &srcBox;
    }

    UINT dstX = pDestPoint ? static_cast<UINT>(pDestPoint->x) : 0;
    UINT dstY = pDestPoint ? static_cast<UINT>(pDestPoint->y) : 0;

    m_ctx->LockContext();
    m_ctx->Context()->CopySubresourceRegion(
        dstRes, dst->Subresource(), dstX, dstY, 0,
        srcRes, src->Subresource(), pBox);
    m_ctx->UnlockContext();

    dst->NotifyContentsChanged();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::UpdateTexture(
    IDirect3DBaseTexture9* pSourceTexture,
    IDirect3DBaseTexture9* pDestinationTexture)
{
    if (!pSourceTexture || !pDestinationTexture) return D3DERR_INVALIDCALL;

    if (pSourceTexture->GetType()      != D3DRTYPE_TEXTURE ||
        pDestinationTexture->GetType() != D3DRTYPE_TEXTURE)
        return E_NOTIMPL;

    auto* srcTex = static_cast<D9Texture*>(pSourceTexture);
    auto* dstTex = static_cast<D9Texture*>(pDestinationTexture);

    DWORD levels = std::min(srcTex->GetLevelCount(), dstTex->GetLevelCount());
    m_ctx->LockContext();
    for (DWORD i = 0; i < levels; ++i) {
        IDirect3DSurface9* pSrcSurf = nullptr;
        IDirect3DSurface9* pDstSurf = nullptr;
        if (SUCCEEDED(srcTex->GetSurfaceLevel(i, &pSrcSurf)) &&
            SUCCEEDED(dstTex->GetSurfaceLevel(i, &pDstSurf)))
        {
            auto* s = static_cast<D9Surface*>(pSrcSurf);
            auto* d = static_cast<D9Surface*>(pDstSurf);
            if (s->Texture() && d->Texture()) {
                m_ctx->Context()->CopySubresourceRegion(
                    d->Texture(), d->Subresource(), 0, 0, 0,
                    s->Texture(), s->Subresource(), nullptr);
            }
        }
        if (pSrcSurf) pSrcSurf->Release();
        if (pDstSurf) pDstSurf->Release();
    }
    m_ctx->UnlockContext();

    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetRenderTargetData(
    IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
{
    if (!pRenderTarget || !pDestSurface) return D3DERR_INVALIDCALL;

    auto* src = static_cast<D9Surface*>(pRenderTarget);
    auto* dst = static_cast<D9Surface*>(pDestSurface);

    if (!src->Texture() || !dst->Staging()) return D3DERR_INVALIDCALL;

    if (static_cast<UINT>(src->Info().multiSample) > 1)
        return D3DERR_INVALIDCALL;

    m_ctx->LockContext();
    m_ctx->Context()->CopySubresourceRegion(
        dst->Staging(), 0, 0, 0, 0,
        src->Texture(), src->Subresource(), nullptr);
    m_ctx->UnlockContext();

    return D3D_OK;
}

HRESULT D9Device::EnsureBlitPass() noexcept
{
    if (m_blitVS && m_blitPS && m_blitCB && m_blitSampPoint && m_blitSampLinear)
        return S_OK;

    static const char kBlitHlsl[] =
        "cbuffer B : register(b0) { float4 uvST; }\n"
        "struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "O vsmain(uint id : SV_VertexID) {\n"
        "    O o;\n"
        "    float2 t = float2((id << 1) & 2, id & 2);\n"
        "    o.p  = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
        "    o.uv = t * uvST.xy + uvST.zw;\n"
        "    return o;\n"
        "}\n"
        "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
        "float4 psmain(O i) : SV_Target { return t0.Sample(s0, i.uv); }\n";

    ComPtr<ID3DBlob> vsb, psb, err;
    HRESULT hr = D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "dx9to11_blit",
                            nullptr, nullptr, "vsmain", "vs_4_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                            vsb.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = D3DCompile(kBlitHlsl, sizeof(kBlitHlsl) - 1, "dx9to11_blit",
                    nullptr, nullptr, "psmain", "ps_4_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                    psb.GetAddressOf(), err.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    auto* dev = m_ctx->Device();
    hr = dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                 nullptr, m_blitVS.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(),
                                nullptr, m_blitPS.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = 16;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = dev->CreateBuffer(&bd, nullptr, m_blitCB.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    D3D11_SAMPLER_DESC sdp{};
    sdp.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sdp.AddressU = sdp.AddressV = sdp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdp.MaxLOD   = D3D11_FLOAT32_MAX;
    hr = dev->CreateSamplerState(&sdp, m_blitSampPoint.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    sdp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    return dev->CreateSamplerState(&sdp, m_blitSampLinear.ReleaseAndGetAddressOf());
}

HRESULT D9Device::ShaderBlit(D9Surface* src, const RECT& srcRect,
                             D9Surface* dst, const RECT& dstRect,
                             D3DTEXTUREFILTERTYPE filter) noexcept
{
    HRESULT hr = EnsureBlitPass();
    if (FAILED(hr)) return hr;

    ID3D11ShaderResourceView* srv = src->GetOrCreateBlitSRV();
    ID3D11RenderTargetView*   rtv = dst->RTV();
    if (!srv || !rtv) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            OutputDebugStringA("[dx9to11] StretchRect: scaling blit needs a "
                               "shader-readable source and a render-target "
                               "destination; this pair lacks one - call "
                               "refused\n");
        }
        return D3DERR_INVALIDCALL;
    }

    auto* ctx = m_ctx->Context();
    m_ctx->LockContext();

    const auto& si = src->Info();
    const float invW = si.width  ? 1.0f / static_cast<float>(si.width)  : 0.0f;
    const float invH = si.height ? 1.0f / static_cast<float>(si.height) : 0.0f;
    const float st[4] = {
        static_cast<float>(srcRect.right  - srcRect.left) * invW,
        static_cast<float>(srcRect.bottom - srcRect.top)  * invH,
        static_cast<float>(srcRect.left) * invW,
        static_cast<float>(srcRect.top)  * invH,
    };
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(m_blitCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, st, sizeof(st));
        ctx->Unmap(m_blitCB.Get(), 0);
    }

    ID3D11RenderTargetView* rtvs[1] = { rtv };
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(nullptr, 0);
    ctx->RSSetState(nullptr);
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(dstRect.left);
    vp.TopLeftY = static_cast<float>(dstRect.top);
    vp.Width    = static_cast<float>(dstRect.right - dstRect.left);
    vp.Height   = static_cast<float>(dstRect.bottom - dstRect.top);
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(m_blitVS.Get(), nullptr, 0);
    ctx->PSSetShader(m_blitPS.Get(), nullptr, 0);
    ID3D11Buffer* cb = m_blitCB.Get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* samp = (filter == D3DTEXF_POINT || filter == D3DTEXF_NONE)
                             ? m_blitSampPoint.Get() : m_blitSampLinear.Get();
    ctx->PSSetSamplers(0, 1, &samp);
    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[1] = {};
    ctx->PSSetShaderResources(0, 1, nullSrv);
    m_ctx->UnlockContext();

    m_bind.Invalidate();
    m_rst.MarkAllDirty();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::StretchRect(
    IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect,
    IDirect3DSurface9* pDestSurface,   CONST RECT* pDestRect,
    D3DTEXTUREFILTERTYPE Filter)
{

    if (!pSourceSurface || !pDestSurface) return D3DERR_INVALIDCALL;

    auto* src = static_cast<D9Surface*>(pSourceSurface);
    auto* dst = static_cast<D9Surface*>(pDestSurface);

    ID3D11Texture2D* srcRes = src->Texture() ? src->Texture() : src->Staging();
    ID3D11Texture2D* dstRes = dst->Texture() ? dst->Texture() : dst->Staging();
    if (!srcRes || !dstRes) return D3DERR_INVALIDCALL;

    const auto& sd = src->Info();
    const auto& dd = dst->Info();

    const LONG srcW = pSourceRect ? (pSourceRect->right  - pSourceRect->left)
                                  : static_cast<LONG>(sd.width);
    const LONG srcH = pSourceRect ? (pSourceRect->bottom - pSourceRect->top)
                                  : static_cast<LONG>(sd.height);
    const LONG dstW = pDestRect   ? (pDestRect->right  - pDestRect->left)
                                  : static_cast<LONG>(dd.width);
    const LONG dstH = pDestRect   ? (pDestRect->bottom - pDestRect->top)
                                  : static_cast<LONG>(dd.height);
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return D3DERR_INVALIDCALL;

    if (static_cast<UINT>(sd.multiSample) > 1 &&
        static_cast<UINT>(dd.multiSample) <= 1) {
        const FormatMapping fm = FormatConverter::ToDxgi(dd.format);
        if (!fm.IsValid()) return D3DERR_INVALIDCALL;
        m_ctx->LockContext();
        m_ctx->Context()->ResolveSubresource(
            dstRes, dst->Subresource(),
            srcRes, src->Subresource(), fm.dxgiFormat);
        m_ctx->UnlockContext();
        dst->NotifyContentsChanged();
        return D3D_OK;
    }

    if (srcW == dstW && srcH == dstH) {
        D3D11_BOX box{};
        const D3D11_BOX* pBox = nullptr;
        if (pSourceRect) {
            box = { static_cast<UINT>(pSourceRect->left),
                    static_cast<UINT>(pSourceRect->top), 0,
                    static_cast<UINT>(pSourceRect->right),
                    static_cast<UINT>(pSourceRect->bottom), 1 };
            pBox = &box;
        }
        const UINT dx = pDestRect ? static_cast<UINT>(pDestRect->left) : 0u;
        const UINT dy = pDestRect ? static_cast<UINT>(pDestRect->top)  : 0u;
        m_ctx->LockContext();
        m_ctx->Context()->CopySubresourceRegion(
            dstRes, dst->Subresource(), dx, dy, 0,
            srcRes, src->Subresource(), pBox);
        m_ctx->UnlockContext();
        dst->NotifyContentsChanged();
        return D3D_OK;
    }

    const RECT sr = pSourceRect ? *pSourceRect
                                : RECT{ 0, 0, static_cast<LONG>(sd.width),
                                              static_cast<LONG>(sd.height) };
    const RECT dr = pDestRect ? *pDestRect
                              : RECT{ 0, 0, static_cast<LONG>(dd.width),
                                            static_cast<LONG>(dd.height) };
    HRESULT hr = ShaderBlit(src, sr, dst, dr, Filter);
    if (SUCCEEDED(hr))
        dst->NotifyContentsChanged();
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Device::ColorFill(
    IDirect3DSurface9* pSurface, CONST RECT*  , D3DCOLOR color)
{
    if (!pSurface) return D3DERR_INVALIDCALL;

    auto* s = static_cast<D9Surface*>(pSurface);
    if (!s->RTV()) return D3DERR_INVALIDCALL;

    const float r = ((color >> 16) & 0xFF) / 255.0f;
    const float g = ((color >>  8) & 0xFF) / 255.0f;
    const float b = ((color      ) & 0xFF) / 255.0f;
    const float a = ((color >> 24) & 0xFF) / 255.0f;
    const float rgba[4] = { r, g, b, a };

    m_ctx->LockContext();
    m_ctx->Context()->ClearRenderTargetView(s->RTV(), rgba);
    m_ctx->UnlockContext();

    s->NotifyContentsChanged();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateRenderTargetEx(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD  )
{
    return CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality,
                               Lockable, ppSurface, pSharedHandle);
}

HRESULT STDMETHODCALLTYPE D9Device::CreateOffscreenPlainSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD  )
{
    return CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}

HRESULT STDMETHODCALLTYPE D9Device::CreateDepthStencilSurfaceEx(
    UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
    BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD  )
{
    return CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality,
                                      Discard, ppSurface, pSharedHandle);
}

}
