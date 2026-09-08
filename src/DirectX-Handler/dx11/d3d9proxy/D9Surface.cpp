// Surfaces -- render targets, depth buffers, back buffers, texture levels.
//
// A D3D9 surface is an object; D3D11 has only resources and views. A surface
// here is therefore a reference to one subresource of a texture plus the views
// that use requires.
//
// Back buffers need the most care. A swap chain rotates through several
// physical buffers, but D3D9 shows the game one logical back buffer and maps
// it to whichever is current. Games call GetBackBuffer once and keep the
// pointer, so a surface that captured a specific buffer at creation would be
// naming a stale one within a couple of frames -- and writing to a buffer the
// display is currently scanning out is rejected outright. The current buffer
// is therefore resolved at the point of use.
//
// GetDC and ReleaseDC are supported by converting between the surface and a
// GDI bitmap. The conversion is lossless within the precision of the formats
// involved, but a 16-bit surface round-tripped through a 32-bit bitmap is not
// bit-identical to what a real 16-bit surface would have held.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Surface.h>
#include <d3d9proxy/D9Device.h>
#include <d3d9proxy/D9CubeTexture.h>
#include <d3d9proxy/D9Texture.h>
#include <core/FormatConverter.h>
#include <cstring>
#include <algorithm>

namespace dx9to11 {

namespace {
RECT FullRect(UINT width, UINT height) noexcept
{
    RECT r{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    return r;
}

RECT UnionRect(const RECT& a, const RECT& b) noexcept
{
    RECT r;
    r.left   = std::min(a.left,   b.left);
    r.top    = std::min(a.top,    b.top);
    r.right  = std::max(a.right,  b.right);
    r.bottom = std::max(a.bottom, b.bottom);
    return r;
}
}

D9Surface::D9Surface(
    D9Device*                        pDevice,
    const Desc&                      desc,
    ComPtr<ID3D11Texture2D>          texture,
    ComPtr<ID3D11Texture2D>          staging,
    ComPtr<ID3D11RenderTargetView>   rtv,
    ComPtr<ID3D11DepthStencilView>   dsv,
    ComPtr<ID3D11ShaderResourceView> srv,
    IUnknown*                        container,
    ComPtr<ID3D11RenderTargetView>   rtvSrgb) noexcept
    : m_device(pDevice)
    , m_desc(desc)
    , m_texture(std::move(texture))
    , m_staging(std::move(staging))
    , m_rtv(std::move(rtv))
    , m_rtvSrgb(std::move(rtvSrgb))
    , m_dsv(std::move(dsv))
    , m_srv(std::move(srv))
    , m_container(container)
{

    if (m_desc.pool == D3DPOOL_MANAGED) {
        const UINT rowPitch = FormatConverter::RowPitch(m_desc.format, m_desc.width);
        const UINT rows     = FormatConverter::RowCount(m_desc.format, m_desc.height);
        m_shadow.resize(static_cast<size_t>(rowPitch) * rows);
    }
}

D9Surface::~D9Surface()
{

}

HRESULT STDMETHODCALLTYPE D9Surface::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;

    if (riid == __uuidof(IUnknown)            ||
        riid == __uuidof(IDirect3DResource9)  ||
        riid == __uuidof(IDirect3DSurface9))
    {
        *ppvObj = static_cast<IDirect3DSurface9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Surface::AddRef()
{

    if (m_container)
        return m_container->AddRef();
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9Surface::Release()
{
    if (m_container)
        return m_container->Release();
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        delete this;
    }
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9Surface::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9),
                                     reinterpret_cast<void**>(ppDevice));
}

HRESULT STDMETHODCALLTYPE D9Surface::SetPrivateData(REFGUID, CONST void*, DWORD, DWORD)
{

    return D3DERR_NOTAVAILABLE;
}
HRESULT STDMETHODCALLTYPE D9Surface::GetPrivateData(REFGUID, void*, DWORD*) { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9Surface::FreePrivateData(REFGUID)              { return D3DERR_NOTAVAILABLE; }
DWORD   STDMETHODCALLTYPE D9Surface::SetPriority(DWORD)                    { return 0; }
DWORD   STDMETHODCALLTYPE D9Surface::GetPriority()                         { return 0; }
void    STDMETHODCALLTYPE D9Surface::PreLoad()                            { }
D3DRESOURCETYPE STDMETHODCALLTYPE D9Surface::GetType()                    { return D3DRTYPE_SURFACE; }

HRESULT STDMETHODCALLTYPE D9Surface::GetContainer(REFIID riid, void** ppContainer)
{
    if (!ppContainer) return D3DERR_INVALIDCALL;

    if (m_container) {
        return m_container->QueryInterface(riid, ppContainer);
    }

    return m_device->QueryInterface(riid, ppContainer);
}

HRESULT STDMETHODCALLTYPE D9Surface::GetDesc(D3DSURFACE_DESC* pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    pDesc->Format             = m_desc.format;
    pDesc->Type               = D3DRTYPE_SURFACE;
    pDesc->Usage              = m_desc.usage;
    pDesc->Pool               = m_desc.pool;
    pDesc->MultiSampleType    = m_desc.multiSample;
    pDesc->MultiSampleQuality = 0;
    pDesc->Width              = m_desc.width;
    pDesc->Height             = m_desc.height;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Surface::LockRect(D3DLOCKED_RECT* pLockedRect, CONST RECT* pRect, DWORD Flags)
{
    if (!pLockedRect)
        return D3DERR_INVALIDCALL;
    if (m_locked)
        return D3DERR_INVALIDCALL;

    const RECT rect = pRect ? *pRect : FullRect(m_desc.width, m_desc.height);
    const UINT rowPitch = FormatConverter::RowPitch(m_desc.format, m_desc.width);
    const UINT bpp      = FormatConverter::BlockOrPixelSize(m_desc.format);

    const bool bc = FormatConverter::IsBlockCompressed(m_desc.format);
    const auto rectOrigin = [&](UINT pitch) noexcept -> size_t {
        if (bc)
            return static_cast<size_t>(rect.top / 4) * pitch
                 + static_cast<size_t>(rect.left / 4) * bpp;
        return static_cast<size_t>(rect.top) * pitch
             + static_cast<size_t>(rect.left) * bpp;
    };

    const bool rb10     = FormatConverter::NeedsRB10Swap(m_desc.format);
    const UINT rectW    = static_cast<UINT>(rect.right  - rect.left);
    const UINT rectH    = static_cast<UINT>(rect.bottom - rect.top);
    m_rb10Bits  = nullptr;
    m_rb10Pitch = 0;

    const bool lum = FormatConverter::IsCpuExpandedLuminance(m_desc.format);
    auto lumExpose = [&](void* mappedData, UINT mappedPitch, bool syncFromGpu) {
        m_lumScratch.resize(static_cast<size_t>(rowPitch) * m_desc.height);
        if (syncFromGpu) {
            FormatConverter::ContractLumRect(
                m_desc.format, mappedData, mappedPitch,
                m_lumScratch.data(), rowPitch, m_desc.width, m_desc.height);
        }
        m_lumMapped      = mappedData;
        m_lumMappedPitch = mappedPitch;
        pLockedRect->Pitch = static_cast<INT>(rowPitch);
        pLockedRect->pBits = m_lumScratch.data() + rectOrigin(rowPitch);
    };

    switch (m_desc.pool) {

        case D3DPOOL_MANAGED: {
            pLockedRect->Pitch = static_cast<INT>(rowPitch);
            pLockedRect->pBits = m_shadow.data() + rectOrigin(rowPitch);
            break;
        }

        case D3DPOOL_SYSTEMMEM: {

            ID3D11Texture2D* mapTex = m_staging ? m_staging.Get() : m_texture.Get();
            if (!mapTex) return D3DERR_INVALIDCALL;
            const UINT mapSub = m_staging ? 0u : m_desc.subresource;
            const D3D11_MAP mapType = ((Flags & D3DLOCK_READONLY) && !rb10)
                                    ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            m_device->Ctx()->LockContext();
            HRESULT hr = m_device->Ctx()->Context()->Map(mapTex, mapSub, mapType, 0, &mapped);
            m_device->Ctx()->UnlockContext();
            if (FAILED(hr)) return D3DERR_INVALIDCALL;
            if (lum) {
                lumExpose(mapped.pData, mapped.RowPitch,   true);
                break;
            }
            pLockedRect->Pitch = static_cast<INT>(mapped.RowPitch);
            pLockedRect->pBits = static_cast<uint8_t*>(mapped.pData)
                + rectOrigin(mapped.RowPitch);
            if (rb10) {
                FormatConverter::SwapRB10Rect(pLockedRect->pBits, mapped.RowPitch, rectW, rectH);
                m_rb10Bits  = pLockedRect->pBits;
                m_rb10Pitch = mapped.RowPitch;
            }
            break;
        }

        case D3DPOOL_DEFAULT: {
            if (m_desc.usage & D3DUSAGE_DYNAMIC) {

                D3D11_MAPPED_SUBRESOURCE mapped{};
                m_device->Ctx()->LockContext();
                HRESULT hr = m_device->Ctx()->Context()->Map(
                    m_texture.Get(), m_desc.subresource, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
                m_device->Ctx()->UnlockContext();
                if (FAILED(hr)) return D3DERR_INVALIDCALL;
                if (lum) {

                    lumExpose(mapped.pData, mapped.RowPitch,   false);
                    break;
                }
                pLockedRect->Pitch = static_cast<INT>(mapped.RowPitch);
                pLockedRect->pBits = static_cast<uint8_t*>(mapped.pData)
                    + rectOrigin(mapped.RowPitch);
                if (rb10) {

                    m_rb10Bits  = pLockedRect->pBits;
                    m_rb10Pitch = mapped.RowPitch;
                }
            } else {
                if (!m_staging)
                    return D3DERR_INVALIDCALL;

                m_device->Ctx()->LockContext();

                m_device->Ctx()->Context()->CopySubresourceRegion(
                    m_staging.Get(), m_desc.subresource, 0, 0, 0,
                    m_texture.Get(), m_desc.subresource, nullptr);

                const D3D11_MAP mapType = ((Flags & D3DLOCK_READONLY) && !rb10)
                                        ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
                D3D11_MAPPED_SUBRESOURCE mapped{};
                HRESULT hr = m_device->Ctx()->Context()->Map(
                    m_staging.Get(), m_desc.subresource, mapType, 0, &mapped);
                m_device->Ctx()->UnlockContext();
                if (FAILED(hr)) return D3DERR_INVALIDCALL;

                if (lum) {
                    lumExpose(mapped.pData, mapped.RowPitch,   true);
                    break;
                }
                pLockedRect->Pitch = static_cast<INT>(mapped.RowPitch);
                pLockedRect->pBits = static_cast<uint8_t*>(mapped.pData)
                    + rectOrigin(mapped.RowPitch);
                if (rb10) {
                    FormatConverter::SwapRB10Rect(pLockedRect->pBits, mapped.RowPitch, rectW, rectH);
                    m_rb10Bits  = pLockedRect->pBits;
                    m_rb10Pitch = mapped.RowPitch;
                }
            }
            break;
        }

        default:
            return D3DERR_INVALIDCALL;
    }

    m_locked     = true;
    m_lockedRect = rect;
    m_lockFlags  = Flags;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Surface::UnlockRect()
{
    if (!m_locked)
        return D3DERR_INVALIDCALL;

    const bool rb10  = FormatConverter::NeedsRB10Swap(m_desc.format);
    const UINT rectW = static_cast<UINT>(m_lockedRect.right  - m_lockedRect.left);
    const UINT rectH = static_cast<UINT>(m_lockedRect.bottom - m_lockedRect.top);
    if (m_rb10Bits) {
        FormatConverter::SwapRB10Rect(m_rb10Bits, m_rb10Pitch, rectW, rectH);
        m_rb10Bits  = nullptr;
        m_rb10Pitch = 0;
    }

    if (m_lumMapped) {
        if (!(m_lockFlags & D3DLOCK_READONLY)) {
            const UINT packedPitch =
                FormatConverter::RowPitch(m_desc.format, m_desc.width);
            FormatConverter::ExpandLumRect(
                m_desc.format, m_lumScratch.data(), packedPitch,
                m_lumMapped, m_lumMappedPitch, m_desc.width, m_desc.height);
        }
        m_lumMapped      = nullptr;
        m_lumMappedPitch = 0;
    }

    switch (m_desc.pool) {
        case D3DPOOL_MANAGED: {
            m_shadowValid = true;
            m_hasDirty    = true;
            m_dirtyRect   = m_hasDirty ? UnionRect(m_dirtyRect, m_lockedRect) : m_lockedRect;

            const UINT rowPitch = FormatConverter::RowPitch(m_desc.format, m_desc.width);
            m_device->Ctx()->LockContext();
            if (rb10) {

                FormatConverter::SwapRB10Rect(m_shadow.data(), rowPitch,
                                              m_desc.width, m_desc.height);
                m_device->Ctx()->Context()->UpdateSubresource(
                    m_texture.Get(), m_desc.subresource, nullptr,
                    m_shadow.data(), rowPitch, 0);
                FormatConverter::SwapRB10Rect(m_shadow.data(), rowPitch,
                                              m_desc.width, m_desc.height);
            } else if (FormatConverter::IsCpuExpandedLuminance(m_desc.format)) {

                const UINT expPitch = m_desc.width *
                    FormatConverter::ExpandedBytesPerPixel(m_desc.format);
                m_lumScratch.resize(static_cast<size_t>(expPitch) * m_desc.height);
                FormatConverter::ExpandLumRect(
                    m_desc.format, m_shadow.data(), rowPitch,
                    m_lumScratch.data(), expPitch, m_desc.width, m_desc.height);
                m_device->Ctx()->Context()->UpdateSubresource(
                    m_texture.Get(), m_desc.subresource, nullptr,
                    m_lumScratch.data(), expPitch, 0);
            } else {
                m_device->Ctx()->Context()->UpdateSubresource(
                    m_texture.Get(), m_desc.subresource, nullptr,
                    m_shadow.data(), rowPitch, 0);
            }
            m_device->Ctx()->UnlockContext();
            m_hasDirty = false;
            break;
        }

        case D3DPOOL_SYSTEMMEM: {

            ID3D11Texture2D* mapTex = m_staging ? m_staging.Get() : m_texture.Get();
            const UINT mapSub = m_staging ? 0u : m_desc.subresource;
            m_device->Ctx()->LockContext();
            if (mapTex) m_device->Ctx()->Context()->Unmap(mapTex, mapSub);
            m_device->Ctx()->UnlockContext();
            break;
        }

        case D3DPOOL_DEFAULT: {
            if (m_desc.usage & D3DUSAGE_DYNAMIC) {
                m_device->Ctx()->LockContext();
                m_device->Ctx()->Context()->Unmap(m_texture.Get(), m_desc.subresource);
                m_device->Ctx()->UnlockContext();
            } else {
                m_device->Ctx()->LockContext();
                m_device->Ctx()->Context()->Unmap(m_staging.Get(), m_desc.subresource);
                if (!(m_lockFlags & D3DLOCK_READONLY)) {
                    m_device->Ctx()->Context()->CopySubresourceRegion(
                        m_texture.Get(), m_desc.subresource, 0, 0, 0,
                        m_staging.Get(), m_desc.subresource, nullptr);
                }
                m_device->Ctx()->UnlockContext();
            }
            break;
        }

        default:
            return D3DERR_INVALIDCALL;
    }

    m_locked = false;
    if (!(m_lockFlags & D3DLOCK_READONLY))
        NotifyContentsChanged();
    return D3D_OK;
}

void D9Surface::NotifyContentsChanged() noexcept
{
    if (m_cubeOwner)
        m_cubeOwner->MarkCombinedDirty();

    if (m_autogenOwner)
        m_autogenOwner->GenerateMipSubLevels();
}

ID3D11ShaderResourceView* D9Surface::GetOrCreateBlitSRV() noexcept
{
    if (m_srv)     return m_srv.Get();
    if (m_blitSrv) return m_blitSrv.Get();
    if (!m_texture) return nullptr;

    D3D11_TEXTURE2D_DESC td{};
    m_texture->GetDesc(&td);
    if (!(td.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    const DXGI_FORMAT viewFmt = FormatConverter::ToLinearView(td.Format);
    const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc = nullptr;
    if (viewFmt != td.Format || td.SampleDesc.Count > 1) {
        sd.Format = viewFmt;
        if (td.SampleDesc.Count > 1) {
            sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            sd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MostDetailedMip = 0;
            sd.Texture2D.MipLevels       = td.MipLevels;
        }
        pDesc = &sd;
    }
    if (FAILED(m_device->Ctx()->Device()->CreateShaderResourceView(
            m_texture.Get(), pDesc, m_blitSrv.GetAddressOf())))
        return nullptr;
    return m_blitSrv.Get();
}

static inline uint32_t Expand16To32(uint16_t p, D3DFORMAT fmt) noexcept
{
    auto e5 = [](uint32_t v) noexcept { return (v << 3) | (v >> 2); };
    auto e6 = [](uint32_t v) noexcept { return (v << 2) | (v >> 4); };
    auto e4 = [](uint32_t v) noexcept { return (v << 4) | v; };
    uint32_t r = 0, g = 0, b = 0, a = 0xFF;
    switch (fmt) {
        case D3DFMT_R5G6B5:
            r = e5((p >> 11) & 0x1F); g = e6((p >> 5) & 0x3F); b = e5(p & 0x1F); break;
        case D3DFMT_X1R5G5B5:
            r = e5((p >> 10) & 0x1F); g = e5((p >> 5) & 0x1F); b = e5(p & 0x1F); break;
        case D3DFMT_A1R5G5B5:
            r = e5((p >> 10) & 0x1F); g = e5((p >> 5) & 0x1F); b = e5(p & 0x1F);
            a = (p & 0x8000) ? 0xFFu : 0x00u; break;
        case D3DFMT_A4R4G4B4:
            a = e4((p >> 12) & 0xF); r = e4((p >> 8) & 0xF);
            g = e4((p >> 4) & 0xF);  b = e4(p & 0xF); break;
        default: break;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}
static inline uint16_t Pack32To16(uint32_t p, D3DFORMAT fmt) noexcept
{
    const uint32_t a = (p >> 24) & 0xFF, r = (p >> 16) & 0xFF,
                   g = (p >> 8)  & 0xFF, b = p & 0xFF;
    switch (fmt) {
        case D3DFMT_R5G6B5:
            return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        case D3DFMT_X1R5G5B5:
            return static_cast<uint16_t>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        case D3DFMT_A1R5G5B5:
            return static_cast<uint16_t>(((a >= 0x80 ? 1u : 0u) << 15) |
                   ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        case D3DFMT_A4R4G4B4:
            return static_cast<uint16_t>(((a >> 4) << 12) | ((r >> 4) << 8) |
                   ((g >> 4) << 4) | (b >> 4));
        default: return 0;
    }
}
static inline bool IsGdi16bpp(D3DFORMAT f) noexcept
{
    return f == D3DFMT_R5G6B5 || f == D3DFMT_X1R5G5B5 ||
           f == D3DFMT_A1R5G5B5 || f == D3DFMT_A4R4G4B4;
}

HRESULT STDMETHODCALLTYPE D9Surface::GetDC(HDC* phdc)
{

    if (!phdc) return D3DERR_INVALIDCALL;
    if (m_hdc)  return D3DERR_INVALIDCALL;
    if (m_locked) return D3DERR_INVALIDCALL;

    switch (m_desc.format) {
        case D3DFMT_X8R8G8B8: case D3DFMT_A8R8G8B8:
        case D3DFMT_R5G6B5:   case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5:
        case D3DFMT_A4R4G4B4:
            break;
        default:
            return D3DERR_INVALIDCALL;
    }
    const bool is16bpp = IsGdi16bpp(m_desc.format);

    ID3D11Texture2D* stagingTex = m_staging ? m_staging.Get()
                                             : m_texture.Get();
    if (!stagingTex) return D3DERR_NOTAVAILABLE;

    if (m_staging && m_texture) {
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->CopySubresourceRegion(
            m_staging.Get(), 0, 0, 0, 0,
            m_texture.Get(), m_desc.subresource, nullptr);
        m_device->Ctx()->UnlockContext();
    }

    m_device->Ctx()->LockContext();
    HRESULT hr = m_device->Ctx()->Context()->Map(
        stagingTex, 0, D3D11_MAP_READ_WRITE, 0, &m_dcMapped);
    m_device->Ctx()->UnlockContext();
    if (FAILED(hr)) return D3DERR_INVALIDCALL;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = static_cast<LONG>(m_desc.width);
    bmi.bmiHeader.biHeight      = -static_cast<LONG>(m_desc.height);
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hdcScreen = ::GetDC(nullptr);
    m_hdcMem = CreateCompatibleDC(hdcScreen);
    ::ReleaseDC(nullptr, hdcScreen);

    if (!m_hdcMem) {
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->Unmap(stagingTex, 0);
        m_device->Ctx()->UnlockContext();
        m_dcMapped = {};
        return E_OUTOFMEMORY;
    }

    m_hDib = CreateDIBSection(m_hdcMem, &bmi, DIB_RGB_COLORS, &m_dibBits, nullptr, 0);
    if (!m_hDib) {
        DeleteDC(m_hdcMem); m_hdcMem = nullptr;
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->Unmap(stagingTex, 0);
        m_device->Ctx()->UnlockContext();
        m_dcMapped = {};
        return E_OUTOFMEMORY;
    }

    const UINT rowBytes = m_desc.width * 4u;
    for (UINT row = 0; row < m_desc.height; ++row) {
        const uint8_t* src = static_cast<const uint8_t*>(m_dcMapped.pData)
                             + row * m_dcMapped.RowPitch;
        uint8_t*       dst = static_cast<uint8_t*>(m_dibBits) + row * rowBytes;
        if (is16bpp) {
            const uint16_t* s16 = reinterpret_cast<const uint16_t*>(src);
            uint32_t*       d32 = reinterpret_cast<uint32_t*>(dst);
            for (UINT x = 0; x < m_desc.width; ++x)
                d32[x] = Expand16To32(s16[x], m_desc.format);
        } else {
            std::memcpy(dst, src, rowBytes);
        }
    }

    SelectObject(m_hdcMem, m_hDib);
    m_hdc  = m_hdcMem;
    *phdc  = m_hdc;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Surface::ReleaseDC(HDC hdc)
{
    if (!m_hdc || hdc != m_hdc) return D3DERR_INVALIDCALL;

    ID3D11Texture2D* stagingTex = m_staging ? m_staging.Get()
                                             : m_texture.Get();

    if (stagingTex && m_dibBits) {
        const UINT rowBytes  = m_desc.width * 4u;
        const bool is16bpp   = IsGdi16bpp(m_desc.format);
        for (UINT row = 0; row < m_desc.height; ++row) {
            const uint8_t* src = static_cast<const uint8_t*>(m_dibBits) + row * rowBytes;
            uint8_t*       dst = static_cast<uint8_t*>(m_dcMapped.pData)
                                 + row * m_dcMapped.RowPitch;
            if (is16bpp) {
                const uint32_t* s32 = reinterpret_cast<const uint32_t*>(src);
                uint16_t*       d16 = reinterpret_cast<uint16_t*>(dst);
                for (UINT x = 0; x < m_desc.width; ++x)
                    d16[x] = Pack32To16(s32[x], m_desc.format);
            } else {
                std::memcpy(dst, src, rowBytes);
            }
        }
    }

    if (stagingTex) {
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->Unmap(stagingTex, 0);

        if (m_staging && m_texture) {
            m_device->Ctx()->Context()->CopySubresourceRegion(
                m_texture.Get(), m_desc.subresource, 0, 0, 0,
                m_staging.Get(), 0, nullptr);
        }
        m_device->Ctx()->UnlockContext();
    }
    m_dcMapped = {};

    DeleteObject(m_hDib);  m_hDib    = nullptr; m_dibBits = nullptr;
    DeleteDC(m_hdcMem);    m_hdcMem  = nullptr;
    m_hdc = nullptr;
    NotifyContentsChanged();
    return D3D_OK;
}

}
