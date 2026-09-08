// Cube textures.
//
// A single D3D11 texture with six array slices. D3D9 face order matches D3D11
// array slice order, so no remapping is needed, but subresource indices must
// be computed as slice * mipLevels + level. Getting that wrong presents as a
// scene lit from impossible directions rather than as an indexing fault.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9CubeTexture.h>
#include <d3d9proxy/D9Device.h>
#include <core/FormatConverter.h>
#include <algorithm>
#include <cstring>

namespace dx9to11 {

namespace {
UINT MipExtent(UINT base, UINT level) noexcept
{
    return std::max<UINT>(1u, base >> level);
}
}

D9CubeTexture::D9CubeTexture(
    D9Device*          pDevice,
    UINT               edgeLength,
    UINT               levels,
    DWORD              usage,
    D3DFORMAT          format,
    D3DPOOL            pool,
    std::array<Face,6> faces) noexcept
    : m_device(pDevice)
    , m_edgeLength(edgeLength)
    , m_levels(levels)
    , m_usage(usage)
    , m_format(format)
    , m_pool(pool)
    , m_faces(std::move(faces))
{
    for (auto& faceVec : m_faceSurfaces) {
        faceVec.assign(levels, nullptr);
    }
    if (m_pool == D3DPOOL_DEFAULT)
        m_device->TrackDefaultPoolObject(this, "CubeTexture");
}

D9CubeTexture::~D9CubeTexture()
{
    m_device->UntrackDefaultPoolObject(this);

    for (auto& faceVec : m_faceSurfaces) {
        for (D9Surface* s : faceVec) {
            delete s;
        }
    }
}

HRESULT STDMETHODCALLTYPE D9CubeTexture::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == __uuidof(IUnknown)              ||
        riid == __uuidof(IDirect3DResource9)    ||
        riid == __uuidof(IDirect3DBaseTexture9) ||
        riid == __uuidof(IDirect3DCubeTexture9))
    {
        *ppvObj = static_cast<IDirect3DCubeTexture9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9CubeTexture::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9CubeTexture::Release()
{
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9),
                                    reinterpret_cast<void**>(ppDevice));
}

HRESULT STDMETHODCALLTYPE D9CubeTexture::SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9CubeTexture::GetPrivateData(REFGUID, void*, DWORD*)              { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9CubeTexture::FreePrivateData(REFGUID)                            { return D3DERR_NOTAVAILABLE; }
DWORD   STDMETHODCALLTYPE D9CubeTexture::SetPriority(DWORD)                                  { return 0; }
DWORD   STDMETHODCALLTYPE D9CubeTexture::GetPriority()                                       { return 0; }
void    STDMETHODCALLTYPE D9CubeTexture::PreLoad()                                           {}
D3DRESOURCETYPE STDMETHODCALLTYPE D9CubeTexture::GetType()                                   { return D3DRTYPE_CUBETEXTURE; }

DWORD STDMETHODCALLTYPE D9CubeTexture::SetLOD(DWORD LODNew)
{
    if (m_pool != D3DPOOL_MANAGED) return 0;
    DWORD old = m_lod;
    m_lod = std::min<DWORD>(LODNew, m_levels - 1);
    return old;
}
DWORD STDMETHODCALLTYPE D9CubeTexture::GetLOD()       { return m_lod; }
DWORD STDMETHODCALLTYPE D9CubeTexture::GetLevelCount() { return m_levels; }

HRESULT STDMETHODCALLTYPE D9CubeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE f)
{
    if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP)) return D3DERR_INVALIDCALL;
    m_autoGenFilter = f;
    return D3D_OK;
}
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE D9CubeTexture::GetAutoGenFilterType() { return m_autoGenFilter; }

void STDMETHODCALLTYPE D9CubeTexture::GenerateMipSubLevels()
{
    if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP)) return;
    m_device->Ctx()->LockContext();
    for (auto& face : m_faces) {
        if (face.srv) {
            m_device->Ctx()->Context()->GenerateMips(face.srv.Get());
        }
    }
    m_device->Ctx()->UnlockContext();
    m_cubeSrvDirty = true;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture::GetLevelDesc(UINT Level, D3DSURFACE_DESC* pDesc)
{
    if (!pDesc || Level >= m_levels) return D3DERR_INVALIDCALL;
    pDesc->Format             = m_format;
    pDesc->Type               = D3DRTYPE_SURFACE;
    pDesc->Usage              = m_usage;
    pDesc->Pool               = m_pool;
    pDesc->MultiSampleType    = D3DMULTISAMPLE_NONE;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width              = MipExtent(m_edgeLength, Level);
    pDesc->Height             = MipExtent(m_edgeLength, Level);
    return D3D_OK;
}

HRESULT D9CubeTexture::GetOrCreateFaceLevelSurface(UINT faceIdx, UINT level, D9Surface** ppOut) noexcept
{
    if (faceIdx >= 6 || level >= m_levels) return D3DERR_INVALIDCALL;

    if (!m_faceSurfaces[faceIdx][level]) {
        D9Surface::Desc desc{};
        desc.width       = MipExtent(m_edgeLength, level);
        desc.height      = MipExtent(m_edgeLength, level);
        desc.format      = m_format;
        desc.pool        = m_pool;
        desc.usage       = m_usage;
        desc.multiSample = D3DMULTISAMPLE_NONE;
        desc.subresource = D3D11CalcSubresource(level, 0, m_levels);
        desc.lockable    = TRUE;

        const Face& f = m_faces[faceIdx];

        ComPtr<ID3D11RenderTargetView> rtv;
        if ((m_usage & D3DUSAGE_RENDERTARGET) && f.texture) {
            const FormatMapping mapping = FormatConverter::ToDxgi(m_format);
            if (mapping.IsValid()) {
                D3D11_RENDER_TARGET_VIEW_DESC rd{};
                rd.Format             = mapping.dxgiFormat;
                rd.ViewDimension      = D3D11_RTV_DIMENSION_TEXTURE2D;
                rd.Texture2D.MipSlice = level;
                m_device->Ctx()->Device()->CreateRenderTargetView(
                    f.texture.Get(), &rd, rtv.GetAddressOf());
            }
        }

        auto* surface = new D9Surface(
            m_device, desc, f.texture, f.staging,
            std::move(rtv),   nullptr, f.srv,
            static_cast<IDirect3DCubeTexture9*>(this));

        surface->SetCubeOwner(this);

        m_faceSurfaces[faceIdx][level] = surface;
    }

    m_faceSurfaces[faceIdx][level]->AddRef();
    *ppOut = m_faceSurfaces[faceIdx][level];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture::GetCubeMapSurface(
    D3DCUBEMAP_FACES FaceType, UINT Level, IDirect3DSurface9** ppSurface)
{
    if (!ppSurface) return D3DERR_INVALIDCALL;
    D9Surface* s = nullptr;
    HRESULT hr = GetOrCreateFaceLevelSurface(static_cast<UINT>(FaceType), Level, &s);
    if (FAILED(hr)) { *ppSurface = nullptr; return hr; }
    *ppSurface = s;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture::LockRect(
    D3DCUBEMAP_FACES FaceType, UINT Level,
    D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags)
{
    D9Surface* s = nullptr;
    HRESULT hr = GetOrCreateFaceLevelSurface(static_cast<UINT>(FaceType), Level, &s);
    if (FAILED(hr)) return hr;
    hr = s->LockRect(pLockedRect, pRect, Flags);
    s->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture::UnlockRect(D3DCUBEMAP_FACES FaceType, UINT Level)
{
    UINT fi = static_cast<UINT>(FaceType);
    if (fi >= 6 || Level >= m_levels || !m_faceSurfaces[fi][Level])
        return D3DERR_INVALIDCALL;
    HRESULT hr = m_faceSurfaces[fi][Level]->UnlockRect();
    if (SUCCEEDED(hr)) m_cubeSrvDirty = true;
    return hr;
}

HRESULT STDMETHODCALLTYPE D9CubeTexture::AddDirtyRect(D3DCUBEMAP_FACES, CONST RECT*)
{
    return D3D_OK;
}

ID3D11ShaderResourceView* D9CubeTexture::SRV() noexcept
{
    return SRV(false);
}

ID3D11ShaderResourceView* D9CubeTexture::SRV(bool srgb) noexcept
{
    if (m_cubeSrvDirty || !m_cubeSrv) {
        if (FAILED(RebuildCombinedSRV()))
            return nullptr;
        m_cubeSrvDirty = false;
    }
    return (srgb && m_cubeSrvSrgb) ? m_cubeSrvSrgb.Get() : m_cubeSrv.Get();
}

HRESULT D9CubeTexture::RebuildCombinedSRV() noexcept
{
    if (!m_faces[0].texture)
        return E_FAIL;

    ID3D11Device1*        dev = m_device->Ctx()->Device();
    ID3D11DeviceContext1* ctx = m_device->Ctx()->Context();

    D3D11_TEXTURE2D_DESC faceDesc{};
    m_faces[0].texture->GetDesc(&faceDesc);

    const bool wantsSrgbView = FormatConverter::HasSRGBVariant(faceDesc.Format);
    const DXGI_FORMAT arrayFormat = wantsSrgbView
        ? FormatConverter::ToTypeless(faceDesc.Format)
        : faceDesc.Format;

    if (!m_cubeTex) {
        D3D11_TEXTURE2D_DESC cd = faceDesc;
        cd.Format         = arrayFormat;
        cd.ArraySize      = 6;
        cd.Usage          = D3D11_USAGE_DEFAULT;
        cd.BindFlags      = D3D11_BIND_SHADER_RESOURCE;
        cd.CPUAccessFlags = 0;
        cd.MiscFlags      = D3D11_RESOURCE_MISC_TEXTURECUBE;
        HRESULT hr = dev->CreateTexture2D(&cd, nullptr, m_cubeTex.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return hr;

        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format                      = wantsSrgbView ? FormatConverter::ToLinearView(arrayFormat)
                                                         : faceDesc.Format;
        sd.ViewDimension               = D3D11_SRV_DIMENSION_TEXTURECUBE;
        sd.TextureCube.MostDetailedMip = 0;
        sd.TextureCube.MipLevels       = faceDesc.MipLevels;
        hr = dev->CreateShaderResourceView(m_cubeTex.Get(), &sd, m_cubeSrv.ReleaseAndGetAddressOf());
        if (FAILED(hr)) { m_cubeTex.Reset(); return hr; }

        if (wantsSrgbView) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sdSrgb = sd;
            sdSrgb.Format = FormatConverter::ToSRGBView(arrayFormat);
            hr = dev->CreateShaderResourceView(m_cubeTex.Get(), &sdSrgb, m_cubeSrvSrgb.ReleaseAndGetAddressOf());
            if (FAILED(hr)) {

                m_cubeSrvSrgb.Reset();
                OutputDebugStringA("[dx9to11] sRGB SRV creation failed for a "
                                    "cube texture; falling back to "
                                    "linear-only sampling\n");
            }
        }
    }

    m_device->Ctx()->LockContext();
    for (UINT f = 0; f < 6; ++f) {
        if (!m_faces[f].texture) continue;
        for (UINT mip = 0; mip < faceDesc.MipLevels; ++mip) {
            ctx->CopySubresourceRegion(
                m_cubeTex.Get(), D3D11CalcSubresource(mip, f, faceDesc.MipLevels),
                0, 0, 0,
                m_faces[f].texture.Get(), mip, nullptr);
        }
    }
    m_device->Ctx()->UnlockContext();
    return S_OK;
}

}
