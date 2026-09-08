// IDirect3DVertexBuffer9.
//
// Backed by an ID3D11Buffer, with the locking model being the interesting
// part. D3D9 lets a game map any buffer at any time and offers hints --
// D3DLOCK_DISCARD to say "I am replacing everything, do not wait for the GPU",
// D3DLOCK_NOOVERWRITE to say "I am only touching regions you have finished
// with". D3D11 expresses the same ideas as map types on dynamic resources.
//
// Buffers created with D3DUSAGE_DYNAMIC map straight through to those. Static
// buffers cannot be mapped at all in D3D11, so they keep a CPU-side shadow
// copy: a lock hands back the shadow, and unlocking uploads the modified
// region. That costs memory and a copy, but it is the only way to honour the
// D3D9 contract, and games do lock static buffers.

#pragma once

#ifndef DX9TO11_D9VERTEXBUFFER_H
#define DX9TO11_D9VERTEXBUFFER_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class D9Device;

class D9VertexBuffer final : public IDirect3DVertexBuffer9 {
public:
    D9VertexBuffer(
        D9Device*          pDevice,
        UINT               byteWidth,
        DWORD              usage,
        DWORD              fvf,
        D3DPOOL            pool,
        ComPtr<ID3D11Buffer> buffer,
        ComPtr<ID3D11Buffer> staging) noexcept;

    D9VertexBuffer(const D9VertexBuffer&)            = delete;
    D9VertexBuffer& operator=(const D9VertexBuffer&) = delete;

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
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* pDesc) override;

    [[nodiscard]] ID3D11Buffer* D11Buffer()   const noexcept { return m_buffer.Get(); }

    [[nodiscard]] UINT          BindOffset()  const noexcept { return 0; }

private:
    ~D9VertexBuffer();

    [[nodiscard]] bool IsDynamic() const noexcept
    {
        return (m_usage & D3DUSAGE_DYNAMIC) != 0 && !m_shadow.empty();
    }

    D9Device*             m_device;
    UINT                  m_byteWidth;
    DWORD                 m_usage;
    DWORD                 m_fvf;
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
