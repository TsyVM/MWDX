// Caches translated and compiled shaders for the D3D11 backend.
//
// Translating D3D9 bytecode and compiling the resulting HLSL costs
// milliseconds; shaders are bound per draw. Nothing may be translated twice.
//
// Keys are a CRC-64 of the original bytecode, which is stable across runs, so
// the compiled result can also be written to disk and reloaded next launch.
//
// Pixel shaders have variants. Alpha testing and fog were D3D9 render states
// and must be compiled into the shader, and the kind of texture bound to each
// sampler is only known once something is bound -- so one D3D9 pixel shader
// maps to several compiled blobs distinguished by a packed variant key. The
// translated HLSL is kept alongside them so that a new variant costs only a
// recompile.
//
// ShaderReflection carries what the caller needs afterwards: the input
// signature for building an input layout, and which outputs the shader writes.
// Its copy constructor exists because D3D11's input element descriptions hold
// raw pointers to semantic name strings, which have to be repointed at the
// copy's own strings or they dangle.

#pragma once

#ifndef DX9TO11_SHADER_CACHE_H
#define DX9TO11_SHADER_CACHE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <synchapi.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

struct ShaderReflection {

    std::vector<D3D11_INPUT_ELEMENT_DESC> inputElements;

    std::vector<std::string> semanticNames;

    std::vector<uint8_t> dxbcBlob;

    void RepointSemanticNames() noexcept {
        const size_t n = inputElements.size() < semanticNames.size()
                       ? inputElements.size() : semanticNames.size();
        for (size_t i = 0; i < n; ++i)
            inputElements[i].SemanticName = semanticNames[i].c_str();
    }

    ShaderReflection() = default;
    ShaderReflection(const ShaderReflection& o)
        : inputElements(o.inputElements)
        , semanticNames(o.semanticNames)
        , dxbcBlob(o.dxbcBlob)
        , outTexMask(o.outTexMask)
        , outColor0(o.outColor0)
        , outColor1(o.outColor1)
        , outFog(o.outFog)
    { RepointSemanticNames(); }

    uint32_t outTexMask = 0;
    bool     outColor0  = false;
    bool     outColor1  = false;
    bool     outFog     = false;

    ShaderReflection& operator=(const ShaderReflection& o) {
        if (this != &o) {
            inputElements = o.inputElements;
            semanticNames = o.semanticNames;
            dxbcBlob      = o.dxbcBlob;
            outTexMask    = o.outTexMask;
            outColor0     = o.outColor0;
            outColor1     = o.outColor1;
            outFog        = o.outFog;
            RepointSemanticNames();
        }
        return *this;
    }
    ShaderReflection(ShaderReflection&&) noexcept            = default;
    ShaderReflection& operator=(ShaderReflection&&) noexcept = default;
};

class ShaderCache {
public:

    explicit ShaderCache(ID3D11Device1* pDevice) noexcept;
    ~ShaderCache();

    HRESULT GetOrCreateVS(
        const DWORD*         pBytecode,
        SIZE_T               byteLen,
        ID3D11VertexShader** ppVS,
        ShaderReflection*    ppRefl) noexcept;

    HRESULT GetOrCreatePS(
        const DWORD*        pBytecode,
        SIZE_T              byteLen,
        ID3D11PixelShader** ppPS) noexcept;

    HRESULT GetPSVariant(
        const DWORD*        pBytecode,
        SIZE_T              byteLen,
        UINT                alphaFunc,
        UINT                fogMode,
        UINT                texKindMask,
        ID3D11PixelShader** ppPS) noexcept;

    [[nodiscard]] static uint32_t PackVariantKey(UINT alphaFunc, UINT fogMode,
                                                 UINT texKindMask) noexcept
    { return (alphaFunc & 0xFu) | ((fogMode & 0x7u) << 4) | ((texKindMask & 0xFFFFu) << 7); }

    HRESULT GetVSInputSignature(
        const DWORD*      pBytecode,
        SIZE_T            byteLen,
        ShaderReflection* pReflOut) noexcept;

    [[nodiscard]] size_t VSCacheSize() const noexcept;
    [[nodiscard]] size_t PSCacheSize() const noexcept;

    [[nodiscard]] static bool TranslatorAvailable() noexcept;

private:
    struct PsEntry {

        std::unordered_map<uint32_t, ComPtr<ID3D11PixelShader>> variants;
        std::string               hlsl;
    };

    HRESULT TranslateBuiltin(
        const DWORD*          pBytecode,
        SIZE_T                byteLen,
        bool                  isVertexShader,
        UINT                  alphaFunc,
        UINT                  fogMode,
        UINT                  texKindMask,
        std::vector<uint8_t>* pDXBC,
        std::string*          pHlslOut) noexcept;

    HRESULT CompileHlsl(
        const std::string&    hlsl,
        bool                  isVertexShader,
        UINT                  alphaFunc,
        UINT                  fogMode,
        UINT                  texKindMask,
        std::vector<uint8_t>* pDXBC) noexcept;

    HRESULT CompileWithMods(
        const DWORD*          pBytecode,
        SIZE_T                byteLen,
        bool                  isVertexShader,
        UINT                  alphaFunc,
        UINT                  fogMode,
        UINT                  texKindMask,
        const std::string&    hlsl,
        std::vector<uint8_t>* pDXBC) noexcept;

    HRESULT TranslateWithVkd3d(
        const DWORD*          pBytecode,
        SIZE_T                byteLen,
        bool                  isVertexShader,
        std::vector<uint8_t>* pDXBC) noexcept;

    HRESULT TranslateToDxbc(
        const DWORD*          pBytecode,
        SIZE_T                byteLen,
        bool                  isVertexShader,
        UINT                  alphaFunc,
        UINT                  fogMode,
        UINT                  texKindMask,
        uint64_t              crc,
        std::vector<uint8_t>* pDXBC,
        std::string*          pHlslOut) noexcept;

    HRESULT ReflectVSInputSignature(
        const std::vector<uint8_t>& dxbc,
        ShaderReflection*           pReflOut) noexcept;

    HRESULT EnsureFallbackVS() noexcept;
    HRESULT EnsureFallbackPS() noexcept;

    static uint32_t DiskKeyFlags(bool isVS, UINT alphaFunc, UINT fogMode,
                                 UINT texKindMask) noexcept
    { return (isVS ? 1u : 0u) | ((alphaFunc & 0xFu) << 1) |
             ((fogMode & 0x3u) << 5) | ((texKindMask & 0xFFFFu) << 7); }

    void DiskCacheLoad() noexcept;
    bool DiskCacheGet(uint64_t crc, uint32_t flags, std::vector<uint8_t>* out) noexcept;
    void DiskCachePut(uint64_t crc, uint32_t flags, const std::vector<uint8_t>& dxbc) noexcept;

    ID3D11Device1* m_device;

    std::unordered_map<uint64_t, ComPtr<ID3D11VertexShader>> m_vsCache;
    std::unordered_map<uint64_t, PsEntry>                    m_psCache;
    std::unordered_map<uint64_t, ShaderReflection>           m_reflCache;

    ComPtr<ID3D11VertexShader> m_fallbackVS;
    ComPtr<ID3D11PixelShader>  m_fallbackPS;
    ShaderReflection           m_fallbackVSRefl;

    bool   m_diskLoaded = false;
    bool   m_diskWritable = false;
    void*  m_diskFile = nullptr;
    std::unordered_map<uint64_t, std::vector<uint8_t>> m_disk;

    mutable SRWLOCK m_lock = SRWLOCK_INIT;
};

[[nodiscard]] uint64_t Crc64(const void* data, size_t len) noexcept;

}

#endif
