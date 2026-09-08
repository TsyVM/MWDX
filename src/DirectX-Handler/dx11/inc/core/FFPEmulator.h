// Runs the generated fixed-function shaders.
//
// The generator turns render state into HLSL; this owns the result. It builds
// the permutation key from current state, compiles and caches the shader pair
// for each distinct key, keeps the constant buffer of matrices, lights and
// material values those shaders read, and binds all of it at draw time.
//
// Caching is essential rather than an optimisation: the key is derived from
// state that changes constantly, but the number of distinct combinations a
// game actually uses is small, and compiling HLSL mid-frame is far too slow to
// do more than once per combination.

#pragma once

#ifndef DX9TO11_FFP_EMULATOR_H
#define DX9TO11_FFP_EMULATOR_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
#include <synchapi.h>

#include "ShaderCache.h"
#include <core/FFPShaderGen.h>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class DeviceContext11;
class RenderStateTracker;

class FFPEmulator {
public:
    explicit FFPEmulator(DeviceContext11* ctx) noexcept;
    ~FFPEmulator();

    void SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* pMatrix) noexcept;
    void SetMaterial(const D3DMATERIAL9* pMat) noexcept;
    void SetLight(DWORD index, const D3DLIGHT9* pLight) noexcept;
    void LightEnable(DWORD index, BOOL enable) noexcept;
    void SetGlobalAmbient(D3DCOLOR ambient) noexcept;

    void SetViewportInfo(const D3DVIEWPORT9& vp) noexcept;

    static UINT ExpandFVF(DWORD fvf, D3DVERTEXELEMENT9* pOut) noexcept
    { return ::dx9to11::ExpandFVF(fvf, pOut); }

    HRESULT BindForDraw(const RenderStateTracker& rst, DWORD fvf,
                        const D3DVERTEXELEMENT9* pDecl) noexcept;

    HRESULT BindVSMixed(const RenderStateTracker& rst, DWORD fvf,
                        const D3DVERTEXELEMENT9* pDecl) noexcept;

    HRESULT BindPSMixed(const RenderStateTracker& rst,
                        uint32_t vsTexMask, bool vsColor0, bool vsColor1,
                        bool vsFog) noexcept;

    void ResetToDefaults() noexcept;

    void MarkCBDirty() noexcept { m_cbDirty = true; }

    [[nodiscard]] ID3D11Buffer* CBuffer() const noexcept { return m_cb.Get(); }

    [[nodiscard]] const ShaderReflection* LastReflection() const noexcept { return m_lastRefl; }

    void GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* pOut) const noexcept;

    void GetLight(DWORD index, D3DLIGHT9* pOut) const noexcept;

    [[nodiscard]] const D3DMATERIAL9& Material() const noexcept { return m_fixed.material; }

    [[nodiscard]] BOOL GetLightEnabled(DWORD index) const noexcept
    {
        return (index < 8 && m_fixed.lightEnabled[index]) ? TRUE : FALSE;
    }

    [[nodiscard]] size_t PermutationCount() const noexcept;

private:

    struct ShaderPair {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader>  ps;
        ShaderReflection           refl;
    };

    HRESULT GetOrCompile(const FFPPermKey& key, const ShaderPair** ppOut) noexcept;
    HRESULT CompilePermutation(const FFPPermKey& key, ShaderPair* pOut) noexcept;

    HRESULT EnsureCB() noexcept;
    HRESULT FlushCB(const RenderStateTracker& rst) noexcept;

    DeviceContext11* m_ctx;

    FFPConstantData  m_cbData{};
    bool             m_cbDirty{ true };
    ComPtr<ID3D11Buffer> m_cb;

    FFPFixedState m_fixed;

    std::unordered_map<FFPPermKey, ShaderPair, FFPPermKeyHash> m_cache;
    mutable SRWLOCK m_cacheLock = SRWLOCK_INIT;

    const ShaderReflection* m_lastRefl{};
};

}

#endif
