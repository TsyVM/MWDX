// IDirect3DPixelShader9 -- a handle to the game's original shader bytecode.
//
// As with vertex shaders, the bytecode is stored and translated lazily. Pixel
// shaders have a further reason to defer: alpha testing and fog were render
// states in D3D9 with no equivalent in D3D11, so they have to be compiled into
// the shader. One D3D9 pixel shader therefore becomes several compiled
// variants, selected by the state in force at each draw.

#pragma once

#ifndef DX9TO11_D9_PIXEL_SHADER_H
#define DX9TO11_D9_PIXEL_SHADER_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class ShaderCache;
class D9Device;

class D9PixelShader final : public IDirect3DPixelShader9 {
public:

    D9PixelShader(
        D9Device*                 pDevice,
        ComPtr<ID3D11PixelShader> ps11,
        std::vector<uint8_t>      d3d9Bytecode) noexcept;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetFunction(void* pData, UINT* pSizeOfData) override;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef() override;
    ULONG   STDMETHODCALLTYPE Release() override;

    [[nodiscard]] ID3D11PixelShader* PS11()         const noexcept { return m_ps11.Get(); }
    [[nodiscard]] const uint8_t*     Bytecode()     const noexcept { return m_d3d9Bytecode.data(); }
    [[nodiscard]] SIZE_T             BytecodeSize() const noexcept { return m_d3d9Bytecode.size(); }

    [[nodiscard]] bool               IsSM3()        const noexcept
    { return m_d3d9Bytecode.size() >= 4 && m_d3d9Bytecode[1] >= 3; }

    [[nodiscard]] ID3D11PixelShader* PS11ForState(UINT alphaFunc, UINT fogMode,
                                                  UINT texKindMask,
                                                  ShaderCache* cache) noexcept;

private:
    ~D9PixelShader() = default;

    LONG                      m_refCount{ 1 };
    D9Device*                 m_device;
    ComPtr<ID3D11PixelShader> m_ps11;
    std::vector<uint8_t>      m_d3d9Bytecode;

    std::unordered_map<uint32_t, ID3D11PixelShader*> m_variant;
    bool                      m_variantFailLogged = false;
};

}

#endif
