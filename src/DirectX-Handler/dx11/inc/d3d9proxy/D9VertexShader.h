// IDirect3DVertexShader9 -- a handle to the game's original shader bytecode.
//
// The bytecode is kept verbatim rather than translated on creation. Translation
// is deferred to first use and then cached, for two reasons: a game may create
// shaders it never draws with, and the translation of a pixel shader depends on
// render state that is not known until a draw is issued. Keeping the original
// bytecode also means it is available as the cache key.
//
// D3D9 requires GetFunction to return the original bytecode on request, which
// is another reason it cannot be discarded.

#pragma once

#ifndef DX9TO11_D9_VERTEX_SHADER_H
#define DX9TO11_D9_VERTEX_SHADER_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <core/ShaderCache.h>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class D9Device;

class D9VertexShader final : public IDirect3DVertexShader9 {
public:

    D9VertexShader(
        D9Device*                  pDevice,
        ComPtr<ID3D11VertexShader> vs11,
        ShaderReflection           refl,
        std::vector<uint8_t>       d3d9Bytecode) noexcept;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSizeOfData) override;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    [[nodiscard]] ID3D11VertexShader*     VS11()     const noexcept { return m_vs11.Get(); }
    [[nodiscard]] const ShaderReflection& Reflection() const noexcept { return m_refl; }
    [[nodiscard]] const uint8_t*          Bytecode()  const noexcept { return m_d3d9Bytecode.data(); }
    [[nodiscard]] SIZE_T                  BytecodeSize() const noexcept { return m_d3d9Bytecode.size(); }

private:
    ~D9VertexShader() = default;

    LONG                       m_refCount{ 1 };
    D9Device*                  m_device;
    ComPtr<ID3D11VertexShader> m_vs11;
    ShaderReflection           m_refl;
    std::vector<uint8_t>       m_d3d9Bytecode;
};

}

#endif
