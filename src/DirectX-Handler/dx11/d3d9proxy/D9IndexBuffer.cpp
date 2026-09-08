// Index buffers.
//
// The same dynamic and shadowed-static handling as vertex buffers, plus the
// index format. D3D9 records 16- or 32-bit indices on the buffer; D3D11 wants
// a DXGI format supplied at bind time, so it is stored here and produced on
// demand.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9IndexBuffer.h>
#include <d3d9proxy/D9Device.h>
#include <core/FormatConverter.h>
#include <cstring>

namespace dx9to11 {

D9IndexBuffer::D9IndexBuffer(
    D9Device*            pDevice,
    UINT                 byteWidth,
    DWORD                usage,
    D3DFORMAT            format,
    D3DPOOL              pool,
    ComPtr<ID3D11Buffer> buffer,
    ComPtr<ID3D11Buffer> staging) noexcept
    : m_device(pDevice)
    , m_byteWidth(byteWidth)
    , m_usage(usage)
    , m_format(format)
    , m_pool(pool)
    , m_buffer(std::move(buffer))
    , m_staging(std::move(staging))
{
    if ((m_usage & D3DUSAGE_DYNAMIC) && m_buffer)
        m_shadow.assign(m_byteWidth, 0u);
    if (m_pool == D3DPOOL_DEFAULT)
        m_device->TrackDefaultPoolObject(this, "IndexBuffer");
}

D9IndexBuffer::~D9IndexBuffer()
{
    m_device->UntrackDefaultPoolObject(this);
}

HRESULT STDMETHODCALLTYPE D9IndexBuffer::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == __uuidof(IUnknown)              ||
        riid == __uuidof(IDirect3DResource9)    ||
        riid == __uuidof(IDirect3DIndexBuffer9))
    {
        *ppvObj = static_cast<IDirect3DIndexBuffer9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9IndexBuffer::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9IndexBuffer::Release()
{
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9IndexBuffer::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9),
                                    reinterpret_cast<void**>(ppDevice));
}

HRESULT STDMETHODCALLTYPE D9IndexBuffer::SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9IndexBuffer::GetPrivateData(REFGUID, void*, DWORD*)              { return D3DERR_NOTAVAILABLE; }
HRESULT STDMETHODCALLTYPE D9IndexBuffer::FreePrivateData(REFGUID)                            { return D3DERR_NOTAVAILABLE; }
DWORD   STDMETHODCALLTYPE D9IndexBuffer::SetPriority(DWORD)                                  { return 0; }
DWORD   STDMETHODCALLTYPE D9IndexBuffer::GetPriority()                                       { return 0; }
void    STDMETHODCALLTYPE D9IndexBuffer::PreLoad()                                           {}
D3DRESOURCETYPE STDMETHODCALLTYPE D9IndexBuffer::GetType()                                   { return D3DRTYPE_INDEXBUFFER; }

HRESULT STDMETHODCALLTYPE D9IndexBuffer::Lock(
    UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags)
{
    if (!ppbData) return D3DERR_INVALIDCALL;
    if (m_locked)  return D3DERR_INVALIDCALL;

    if (OffsetToLock > m_byteWidth) return D3DERR_INVALIDCALL;
    const UINT lockSize = (SizeToLock == 0) ? (m_byteWidth - OffsetToLock)
                                            : SizeToLock;
    if (lockSize > m_byteWidth - OffsetToLock) return D3DERR_INVALIDCALL;

    if (IsDynamic()) {
        *ppbData     = m_shadow.data() + OffsetToLock;
        m_locked     = true;
        m_lockOffset = OffsetToLock;
        m_lockSize   = lockSize;
        m_lockFlags  = Flags;
        return S_OK;
    }

    if (m_pool == D3DPOOL_DEFAULT) {
        if (!m_staging) return D3DERR_INVALIDCALL;
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->CopyResource(m_staging.Get(), m_buffer.Get());
        const D3D11_MAP mapType = (Flags & D3DLOCK_READONLY)
                                   ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = m_device->Ctx()->Context()->Map(m_staging.Get(), 0, mapType, 0, &mapped);
        m_device->Ctx()->UnlockContext();
        if (FAILED(hr)) return D3DERR_INVALIDCALL;
        *ppbData     = static_cast<uint8_t*>(mapped.pData) + OffsetToLock;
        m_locked     = true;
        m_lockFlags  = Flags;
        return S_OK;
    }

    if (!m_staging) return D3DERR_INVALIDCALL;
    const D3D11_MAP mapType = (Flags & D3DLOCK_READONLY) ? D3D11_MAP_READ : D3D11_MAP_READ_WRITE;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    m_device->Ctx()->LockContext();
    HRESULT hr = m_device->Ctx()->Context()->Map(m_staging.Get(), 0, mapType, 0, &mapped);
    m_device->Ctx()->UnlockContext();
    if (FAILED(hr)) return D3DERR_INVALIDCALL;
    *ppbData     = static_cast<uint8_t*>(mapped.pData) + OffsetToLock;
    m_locked     = true;
    m_lockFlags  = Flags;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9IndexBuffer::Unlock()
{
    if (!m_locked) return D3DERR_INVALIDCALL;

    if (IsDynamic()) {
        if (!(m_lockFlags & D3DLOCK_READONLY)) {
            const bool noOverwrite = (m_lockFlags & D3DLOCK_NOOVERWRITE) != 0 &&
                                     !(m_lockFlags & D3DLOCK_DISCARD);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            m_device->Ctx()->LockContext();
            HRESULT hr = m_device->Ctx()->Context()->Map(
                m_buffer.Get(), 0,
                noOverwrite ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,
                0, &mapped);
            if (SUCCEEDED(hr)) {
                auto* dst = static_cast<uint8_t*>(mapped.pData);
                if (noOverwrite)
                    std::memcpy(dst + m_lockOffset, m_shadow.data() + m_lockOffset,
                                m_lockSize);
                else
                    std::memcpy(dst, m_shadow.data(), m_byteWidth);
                m_device->Ctx()->Context()->Unmap(m_buffer.Get(), 0);
            }
            m_device->Ctx()->UnlockContext();
        }
    } else if (m_pool == D3DPOOL_DEFAULT) {
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->Unmap(m_staging.Get(), 0);
        if (!(m_lockFlags & D3DLOCK_READONLY)) {
            m_device->Ctx()->Context()->CopyResource(m_buffer.Get(), m_staging.Get());
        }
        m_device->Ctx()->UnlockContext();
    } else {
        m_device->Ctx()->LockContext();
        m_device->Ctx()->Context()->Unmap(m_staging.Get(), 0);
        if (m_pool == D3DPOOL_MANAGED && m_buffer && !(m_lockFlags & D3DLOCK_READONLY)) {
            m_device->Ctx()->Context()->CopyResource(m_buffer.Get(), m_staging.Get());
        }
        m_device->Ctx()->UnlockContext();
    }

    m_locked = false;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9IndexBuffer::GetDesc(D3DINDEXBUFFER_DESC* pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    pDesc->Format = m_format;
    pDesc->Type   = D3DRTYPE_INDEXBUFFER;
    pDesc->Usage  = m_usage;
    pDesc->Pool   = m_pool;
    pDesc->Size   = m_byteWidth;
    return D3D_OK;
}

DXGI_FORMAT D9IndexBuffer::DxgiFormat() const noexcept
{
    return (m_format == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
}

}
