// 2-D textures.
//
// Holds the D3D11 texture, its shader resource views, and the surface proxies
// for individual mip levels. Two views are kept -- one linear, one sRGB --
// because D3D9 chooses sRGB decoding through a sampler state the game may
// change at any moment, while D3D11 bakes it into the view's format.
// Recreating a view mid-frame would be far more expensive than keeping both.
//
// Surface proxies for mip levels are created on demand and cached, since D3D9
// requires GetSurfaceLevel to return the same pointer for the same level.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Texture.h>
#include <d3d9proxy/D9Device.h>
#include <core/FormatConverter.h>
#include <cstring>
#include <algorithm>

namespace dx9to11 {

namespace {
UINT MipExtent(UINT base, UINT level) noexcept
{
    return std::max<UINT>(1, base >> level);
}
}

D9Texture::D9Texture(
    D9Device*                        pDevice,
    UINT                              width,
    UINT                              height,
    UINT                              levels,
    DWORD                             usage,
    D3DFORMAT                         format,
    D3DPOOL                           pool,
    ComPtr<ID3D11Texture2D>           texture,
    ComPtr<ID3D11ShaderResourceView>  srv,
    ComPtr<ID3D11Texture2D>           staging,
    ComPtr<ID3D11ShaderResourceView>  srvSrgb) noexcept
    : m_device(pDevice)
    , m_width(width)
    , m_height(height)
    , m_levels(levels)
    , m_usage(usage)
    , m_format(format)
    , m_pool(pool)
    , m_texture(std::move(texture))
    , m_srv(std::move(srv))
    , m_srvSrgb(std::move(srvSrgb))
    , m_staging(std::move(staging))
    , m_levelSurfaces(levels, nullptr)
{
    if (m_pool == D3DPOOL_DEFAULT)
        m_device->TrackDefaultPoolObject(this, "Texture");
}

D9Texture::~D9Texture()
{
    m_device->UntrackDefaultPoolObject(this);

    for (D9Surface* s : m_levelSurfaces) {
        delete s;
    }
}

HRESULT STDMETHODCALLTYPE D9Texture::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;

    if (riid == __uuidof(IUnknown)              ||
        riid == __uuidof(IDirect3DResource9)    ||
        riid == __uuidof(IDirect3DBaseTexture9) ||
        riid == __uuidof(IDirect3DTexture9))
    {
        *ppvObj = static_cast<IDirect3DTexture9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Texture::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9Texture::Release()
{
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        delete this;
    }
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9Texture::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void**>(ppDevice));
}

HRESULT STDMETHODCALLTYPE D9Texture::SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9Texture::GetPrivateData(REFGUID, void*, DWORD*)              { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9Texture::FreePrivateData(REFGUID)                            { return D3DERR_NOTAVAILABLE; }
DWORD   STDMETHODCALLTYPE D9Texture::SetPriority(DWORD)                                  { return 0; }
DWORD   STDMETHODCALLTYPE D9Texture::GetPriority()                                       { return 0; }
void    STDMETHODCALLTYPE D9Texture::PreLoad()                                           { }
D3DRESOURCETYPE STDMETHODCALLTYPE D9Texture::GetType()                                   { return D3DRTYPE_TEXTURE; }

DWORD STDMETHODCALLTYPE D9Texture::SetLOD(DWORD LODNew)
{

    if (m_pool != D3DPOOL_MANAGED) return 0;
    const DWORD old = m_lod;
    m_lod = std::min<DWORD>(LODNew, m_levels - 1);
    return old;
}
DWORD STDMETHODCALLTYPE D9Texture::GetLOD() { return m_lod; }
DWORD STDMETHODCALLTYPE D9Texture::GetLevelCount() { return m_levels; }

HRESULT STDMETHODCALLTYPE D9Texture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE FilterType)
{
    if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP)) return D3DERR_INVALIDCALL;
    m_autoGenFilter = FilterType;
    return D3D_OK;
}
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE D9Texture::GetAutoGenFilterType() { return m_autoGenFilter; }

void STDMETHODCALLTYPE D9Texture::GenerateMipSubLevels()
{
    if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && m_srv) {
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->GenerateMips(m_srv.Get());
        m_device->Ctx()->UnlockContext();
    }
}

HRESULT STDMETHODCALLTYPE D9Texture::GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc)
{
    if (!pDesc || Level >= m_levels) return D3DERR_INVALIDCALL;
    pDesc->Format             = m_format;
    pDesc->Type               = D3DRTYPE_TEXTURE;
    pDesc->Usage              = m_usage;
    pDesc->Pool               = m_pool;
    pDesc->MultiSampleType    = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width              = MipExtent(m_width, Level);
    pDesc->Height             = MipExtent(m_height, Level);
    return D3D_OK;
}

HRESULT D9Texture::GetOrCreateLevelSurface(UINT level, D9Surface** ppOut) noexcept
{
    if (level >= m_levels) return D3DERR_INVALIDCALL;

    if (!m_levelSurfaces[level]) {
        D9Surface::Desc desc{};
        desc.width       = MipExtent(m_width, level);
        desc.height      = MipExtent(m_height, level);
        desc.format      = m_format;
        desc.pool        = m_pool;
        desc.usage       = m_usage;
        desc.multiSample = D3DMULTISAMPLE_NONE;
        desc.subresource = D3D11CalcSubresource(level, 0, m_levels);
        desc.lockable    = TRUE;

        ComPtr<ID3D11RenderTargetView> rtv, rtvSrgb;
        ComPtr<ID3D11DepthStencilView> dsv;
        ID3D11Device1* dev11 = m_device->Ctx()->Device();

        if (m_usage & D3DUSAGE_RENDERTARGET) {
            const FormatMapping mapping = FormatConverter::ToDxgi(m_format);
            if (mapping.IsValid()) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format             = mapping.dxgiFormat;
                rd.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
                rd.Texture2D.MipSlice = level;
                dev11->CreateRenderTargetView(m_texture.Get(), &rd, rtv.GetAddressOf());

                if (m_srvSrgb) {

                    D3D11_RENDER_TARGET_VIEW_DESC rdSrgb = rd;
                    rdSrgb.Format = FormatConverter::ToSRGBView(mapping.dxgiFormat);
                    if (rdSrgb.Format != DXGI_FORMAT_UNKNOWN)
                        dev11->CreateRenderTargetView(m_texture.Get(), &rdSrgb, rtvSrgb.GetAddressOf());
                }
            }
        }
        if (m_usage & D3DUSAGE_DEPTHSTENCIL) {

            FormatConverter::DepthFormatViews dfv{};
            if (FormatConverter::GetDepthFormatViews(m_format, dfv)) {
                D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
                dd.Format             = dfv.dsv;
                dd.ViewDimension      = D3D11_DSV_DIMENSION_TEXTURE2D;
                dd.Texture2D.MipSlice = level;
                dev11->CreateDepthStencilView(m_texture.Get(), &dd, dsv.GetAddressOf());
            }
        }

        auto* surface = new D9Surface(
            m_device, desc, m_texture, m_staging,
            std::move(rtv), std::move(dsv), m_srv,
              static_cast<IDirect3DTexture9*>(this),
            std::move(rtvSrgb));

        if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && level == 0)
            surface->SetAutogenOwner(this);
        m_levelSurfaces[level] = surface;
    }

    m_levelSurfaces[level]->AddRef();
    *ppOut = m_levelSurfaces[level];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Texture::GetSurfaceLevel(UINT Level, IDirect3DSurface9** ppSurfaceLevel)
{
    if (!ppSurfaceLevel) return D3DERR_INVALIDCALL;
    D9Surface* surface = nullptr;
    HRESULT hr = GetOrCreateLevelSurface(Level, &surface);
    if (FAILED(hr)) { *ppSurfaceLevel = nullptr; return hr; }
    *ppSurfaceLevel = surface;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Texture::LockRect(UINT Level, D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags)
{
    D9Surface* surface = nullptr;
    HRESULT hr = GetOrCreateLevelSurface(Level, &surface);
    if (FAILED(hr)) return hr;
    hr = surface->LockRect(pLockedRect, pRect, Flags);
    surface->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Texture::UnlockRect(UINT Level)
{
    if (Level >= m_levels || !m_levelSurfaces[Level]) return D3DERR_INVALIDCALL;
    return m_levelSurfaces[Level]->UnlockRect();
}

HRESULT STDMETHODCALLTYPE D9Texture::AddDirtyRect(CONST RECT*)
{

    return D3D_OK;
}

}
