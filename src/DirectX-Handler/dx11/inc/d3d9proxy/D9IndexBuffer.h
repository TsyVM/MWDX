// IDirect3DIndexBuffer9.
//
// The same design as the vertex buffer -- dynamic buffers map directly, static
// buffers keep a shadow copy that is uploaded on unlock -- with the addition of
// an index format. D3D9 carries it as D3DFMT_INDEX16 or D3DFMT_INDEX32 on the
// buffer itself, while D3D11 wants a DXGI format supplied when the buffer is
// bound, so the format is recorded here and produced on demand at bind time.

#pragma once

#ifndef DX9TO11_D9INDEXBUFFER_H
#define DX9TO11_D9INDEXBUFFER_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class D9Device;

class D9IndexBuffer final : public IDirect3DIndexBuffer9 {
public:
    D9IndexBuffer(
        D9Device*            pDevice,
        UINT                 byteWidth,
        DWORD                usage,
        D3DFORMAT            format,
        D3DPOOL              pool,
        ComPtr<ID3D11Buffer> buffer,
        ComPtr<ID3D11Buffer> staging) noexcept;

    D9IndexBuffer(const D9IndexBuffer&)            = delete;
    D9IndexBuffer& operator=(const D9IndexBuffer&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override;
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override;
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override;
    DWORD   STDMETHODCALLTYPE GetPriority() override;
    void    STDMETHODCALLTYPE PreLoad() override;
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override;

    HRESULT STDMETHODCALLTYPE Lock(UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE Unlock() override;
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* pDesc) override;

    [[nodiscard]] ID3D11Buffer*  D11Buffer()   const noexcept { return m_buffer.Get(); }
    [[nodiscard]] DXGI_FORMAT    DxgiFormat()  const noexcept;

    [[nodiscard]] UINT           BindOffset()  const noexcept { return 0; }

private:
    ~D9IndexBuffer();

    [[nodiscard]] bool IsDynamic() const noexcept
    {
        return (m_usage & D3DUSAGE_DYNAMIC) != 0 && !m_shadow.empty();
    }

    D9Device*             m_device;
    UINT                  m_byteWidth;
    DWORD                 m_usage;
    D3DFORMAT             m_format;
    D3DPOOL               m_pool;
    ComPtr<ID3D11Buffer>  m_buffer;
    ComPtr<ID3D11Buffer>  m_staging;

    std::atomic<ULONG>    m_refCount{ 1 };

    std::vector<uint8_t>  m_shadow;

    bool   m_locked{};
    UINT   m_lockOffset{};
    UINT   m_lockSize{};
    DWORD  m_lockFlags{};
};

}

#endif
