// Volume (3-D) textures.
//
// Rarely used but not optional -- shaders sample them for effects such as the
// metallic flake in car paint. The row and slice pitches reported from a lock
// must be the ones the driver actually returns, not values computed from the
// format, because drivers pad both for alignment.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9VolumeTexture.h>
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

D9VolumeTexture::D9VolumeTexture(
    D9Device*                        pDevice,
    UINT                              width,
    UINT                              height,
    UINT                              depth,
    UINT                              levels,
    DWORD                             usage,
    D3DFORMAT                         format,
    D3DPOOL                           pool,
    ComPtr<ID3D11Texture3D>           texture,
    ComPtr<ID3D11ShaderResourceView>  srv,
    ComPtr<ID3D11Texture3D>           staging) noexcept
    : m_device(pDevice)
    , m_width(width), m_height(height), m_depth(depth), m_levels(levels)
    , m_usage(usage), m_format(format), m_pool(pool)
    , m_texture(std::move(texture))
    , m_srv(std::move(srv))
    , m_staging(std::move(staging))
{
    if (m_pool == D3DPOOL_DEFAULT)
        m_device->TrackDefaultPoolObject(this, "VolumeTexture");
}

D9VolumeTexture::~D9VolumeTexture()
{
    m_device->UntrackDefaultPoolObject(this);
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == __uuidof(IUnknown)               ||
        riid == __uuidof(IDirect3DResource9)      ||
        riid == __uuidof(IDirect3DBaseTexture9)   ||
        riid == __uuidof(IDirect3DVolumeTexture9))
    {
        *ppvObj = static_cast<IDirect3DVolumeTexture9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9VolumeTexture::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9VolumeTexture::Release()
{
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9),
                                    reinterpret_cast<void**>(ppDevice));
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture::SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9VolumeTexture::GetPrivateData(REFGUID, void*, DWORD*)              { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9VolumeTexture::FreePrivateData(REFGUID)                            { return D3DERR_NOTAVAILABLE; }
DWORD   STDMETHODCALLTYPE D9VolumeTexture::SetPriority(DWORD)                                  { return 0; }
DWORD   STDMETHODCALLTYPE D9VolumeTexture::GetPriority()                                       { return 0; }
void    STDMETHODCALLTYPE D9VolumeTexture::PreLoad()                                           {}
D3DRESOURCETYPE STDMETHODCALLTYPE D9VolumeTexture::GetType()                                   { return D3DRTYPE_VOLUMETEXTURE; }

DWORD STDMETHODCALLTYPE D9VolumeTexture::SetLOD(DWORD LODNew)
{
    if (m_pool != D3DPOOL_MANAGED) return 0;
    DWORD old = m_lod;
    m_lod = std::min<DWORD>(LODNew, m_levels - 1);
    return old;
}
DWORD STDMETHODCALLTYPE D9VolumeTexture::GetLOD()        { return m_lod; }
DWORD STDMETHODCALLTYPE D9VolumeTexture::GetLevelCount()  { return m_levels; }
HRESULT STDMETHODCALLTYPE D9VolumeTexture::SetAutoGenFilterType(D3DTEXTUREFILTERTYPE f)
{
    if (!(m_usage & D3DUSAGE_AUTOGENMIPMAP)) return D3DERR_INVALIDCALL;
    m_autoGenFilter = f;
    return D3D_OK;
}
D3DTEXTUREFILTERTYPE STDMETHODCALLTYPE D9VolumeTexture::GetAutoGenFilterType() { return m_autoGenFilter; }
void STDMETHODCALLTYPE D9VolumeTexture::GenerateMipSubLevels()
{
    if ((m_usage & D3DUSAGE_AUTOGENMIPMAP) && m_srv) {
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->GenerateMips(m_srv.Get());
        m_device->Ctx()->UnlockContext();
    }
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture::GetLevelDesc(UINT Level, D3DVOLUME_DESC* pDesc)
{
    if (!pDesc || Level >= m_levels) return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type   = D3DRTYPE_VOLUME;
    pDesc->Usage  = m_usage;
    pDesc->Pool   = m_pool;
    pDesc->Width  = MipExtent(m_width,  Level);
    pDesc->Height = MipExtent(m_height, Level);
    pDesc->Depth  = MipExtent(m_depth,  Level);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture::GetVolumeLevel(UINT, IDirect3DVolume9**)
{

    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture::LockBox(
    UINT Level, D3DLOCKED_BOX* pLockedVolume, CONST D3DBOX*  , DWORD Flags)
{
    if (!pLockedVolume || Level >= m_levels || m_lockedLevel >= 0)
        return D3DERR_INVALIDCALL;

    const UINT subresource = D3D11CalcSubresource(Level, 0, m_levels);

    if (m_pool == D3DPOOL_MANAGED || m_pool == D3DPOOL_SYSTEMMEM) {
        ID3D11Texture3D* mapTarget = m_staging ? m_staging.Get() : m_texture.Get();
        const D3D11_MAP mapType = (Flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        m_device->Ctx()->LockContext();
        HRESULT hr = m_device->Ctx()->Context()->Map(mapTarget, subresource, mapType, 0, &mapped);
        m_device->Ctx()->UnlockContext();
        if (FAILED(hr)) return D3DERR_INVALIDCALL;
        pLockedVolume->RowPitch   = static_cast<INT>(mapped.RowPitch);
        pLockedVolume->SlicePitch = static_cast<INT>(mapped.DepthPitch);
        pLockedVolume->pBits      = mapped.pData;
    } else {

        if (!m_staging) return D3DERR_INVALIDCALL;
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->CopyResource(m_staging.Get(), m_texture.Get());
        const D3D11_MAP mapType = (Flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = m_device->Ctx()->Context()->Map(m_staging.Get(), subresource, mapType, 0, &mapped);
        m_device->Ctx()->UnlockContext();
        if (FAILED(hr)) return D3DERR_INVALIDCALL;
        pLockedVolume->RowPitch   = static_cast<INT>(mapped.RowPitch);
        pLockedVolume->SlicePitch = static_cast<INT>(mapped.DepthPitch);
        pLockedVolume->pBits      = mapped.pData;
    }

    m_lockedLevel = static_cast<int>(Level);
    m_lockFlags   = Flags;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture::UnlockBox(UINT Level)
{
    if (m_lockedLevel < 0 || static_cast<UINT>(m_lockedLevel) != Level)
        return D3DERR_INVALIDCALL;

    const UINT subresource = D3D11CalcSubresource(Level, 0, m_levels);

    if (m_pool == D3DPOOL_MANAGED || m_pool == D3DPOOL_SYSTEMMEM) {
        ID3D11Texture3D* mapTarget = m_staging ? m_staging.Get() : m_texture.Get();
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->Unmap(mapTarget, subresource);
        if (m_pool == D3DPOOL_MANAGED && m_texture && !(m_lockFlags & D3DLOCK_READONLY)) {
            m_device->Ctx()->Context()->CopyResource(m_texture.Get(), mapTarget);
        }
        m_device->Ctx()->UnlockContext();
    } else {
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->Unmap(m_staging.Get(), subresource);
        if (!(m_lockFlags & D3DLOCK_READONLY)) {
            m_device->Ctx()->Context()->CopyResource(m_texture.Get(), m_staging.Get());
        }
        m_device->Ctx()->UnlockContext();
    }

    m_lockedLevel = -1;
    return D3D_OK;
}

ID3D11ShaderResourceView* D9VolumeTexture::SRV(bool srgb) const noexcept
{
    if (srgb && !m_srgbGapWarned) {
        m_srgbGapWarned = true;
        OutputDebugStringA("[dx9to11] game set D3DSAMP_SRGBTEXTURE on a volume "
                           "texture - not supported for 3-D resources; sampling "
                           "linearly\n");
    }
    return m_srv.Get();
}

HRESULT STDMETHODCALLTYPE D9VolumeTexture::AddDirtyBox(CONST D3DBOX*)
{
    return D3D_OK;
}

}
