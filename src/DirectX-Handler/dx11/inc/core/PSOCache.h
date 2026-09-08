#pragma once

#ifndef DX9TO11_PSO_CACHE_H
#define DX9TO11_PSO_CACHE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <unordered_map>
#include <synchapi.h>
#include <cstdint>
#include <cstring>

// Cache of D3D11 immutable state objects, keyed by the D3D9 state that
// produced them.
//
// Blend, rasteriser and depth-stencil state are created objects in D3D11, and
// creating one involves driver work. A D3D9 game changes the underlying render
// states constantly but only ever uses a small number of distinct
// combinations, so each unique combination is built once and reused.
//
// The keys below are the D3D9 states verbatim rather than the translated D3D11
// descriptions. That is deliberate: translation is lossy in places — the
// deprecated BOTHSRCALPHA blend modes have no D3D11 equivalent and are
// approximated — so two different D3D9 states could translate to the same
// D3D11 description, and keying on the translation would silently merge them.
//
// Comparison is by memcmp, which is why every field is a fixed-width POD with
// no padding holes and every member is explicitly initialised. Adding a field
// of a type the compiler will pad around breaks equality in a way that
// presents as a random state object being reused for the wrong draw.

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class DeviceContext11;

struct BlendDesc {
    BOOL       BlendEnable         = FALSE;
    D3DBLEND   SrcBlend            = D3DBLEND_ONE;
    D3DBLEND   DestBlend           = D3DBLEND_ZERO;
    D3DBLENDOP BlendOp             = D3DBLENDOP_ADD;
    BOOL       SeparateAlphaBlend  = FALSE;
    D3DBLEND   SrcBlendAlpha       = D3DBLEND_ONE;
    D3DBLEND   DestBlendAlpha      = D3DBLEND_ZERO;
    D3DBLENDOP BlendOpAlpha        = D3DBLENDOP_ADD;
    DWORD      ColorWriteEnable    = 0x0F;

    bool operator==(const BlendDesc& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
};

struct RastDesc {
    D3DFILLMODE FillMode             = D3DFILL_SOLID;
    D3DCULL     CullMode             = D3DCULL_CCW;
    BOOL        ScissorEnable        = FALSE;
    BOOL        MultisampleEnable    = FALSE;
    BOOL        AntialiasedLine      = FALSE;
    INT         DepthBias            = 0;
    FLOAT       SlopeScaledDepthBias = 0.0f;

    bool operator==(const RastDesc& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
};

struct DepthStencilDesc {
    BOOL         ZEnable           = TRUE;
    D3DCMPFUNC   ZFunc             = D3DCMP_LESSEQUAL;
    BOOL         ZWriteEnable      = TRUE;
    BOOL         StencilEnable     = FALSE;
    BYTE         StencilReadMask   = 0xFF;
    BYTE         StencilWriteMask  = 0xFF;
    BYTE         _pad0[2]          = { 0, 0 };

    D3DSTENCILOP StencilFail       = D3DSTENCILOP_KEEP;
    D3DSTENCILOP StencilZFail      = D3DSTENCILOP_KEEP;
    D3DSTENCILOP StencilPass       = D3DSTENCILOP_KEEP;
    D3DCMPFUNC   StencilFunc       = D3DCMP_ALWAYS;
    BOOL         TwoSidedStencil   = FALSE;
    D3DSTENCILOP CCWFail           = D3DSTENCILOP_KEEP;
    D3DSTENCILOP CCWZFail          = D3DSTENCILOP_KEEP;
    D3DSTENCILOP CCWPass           = D3DSTENCILOP_KEEP;
    D3DCMPFUNC   CCWFunc           = D3DCMP_ALWAYS;

    bool operator==(const DepthStencilDesc& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
};

struct SamplerDesc {
    D3DTEXTUREFILTERTYPE MinFilter    = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE MagFilter    = D3DTEXF_POINT;
    D3DTEXTUREFILTERTYPE MipFilter    = D3DTEXF_NONE;
    D3DTEXTUREADDRESS    AddressU     = D3DTADDRESS_WRAP;
    D3DTEXTUREADDRESS    AddressV     = D3DTADDRESS_WRAP;
    D3DTEXTUREADDRESS    AddressW     = D3DTADDRESS_WRAP;
    FLOAT                MipLODBias   = 0.0f;
    DWORD                MaxAnisotropy = 1;
    DWORD                MaxMipLevel  = 0;
    D3DCOLOR             BorderColor  = 0;

    bool operator==(const SamplerDesc& o) const noexcept {
        return std::memcmp(this, &o, sizeof(*this)) == 0;
    }
};

template<typename T>
struct PodHash {
    size_t operator()(const T& v) const noexcept {

        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        uint64_t h = 14695981039346656037ULL;
        for (size_t i = 0; i < sizeof(T); ++i) {
            h ^= p[i];
            h *= 1099511628211ULL;
        }
        return static_cast<size_t>(h);
    }
};

class PSOCache {
public:
    explicit PSOCache(DeviceContext11* ctx) noexcept;

    ID3D11BlendState1*        GetOrCreateBlendState(const BlendDesc& desc) noexcept;
    ID3D11RasterizerState1*   GetOrCreateRasterizerState(const RastDesc& desc) noexcept;
    ID3D11DepthStencilState*  GetOrCreateDepthStencilState(const DepthStencilDesc& desc) noexcept;
    ID3D11SamplerState*       GetOrCreateSamplerState(const SamplerDesc& desc) noexcept;

    [[nodiscard]] size_t BlendCacheSize()        const noexcept;
    [[nodiscard]] size_t RastCacheSize()         const noexcept;
    [[nodiscard]] size_t DepthStencilCacheSize() const noexcept;
    [[nodiscard]] size_t SamplerCacheSize()      const noexcept;

private:
    DeviceContext11* m_ctx;

    std::unordered_map<BlendDesc,        ComPtr<ID3D11BlendState1>,
                       PodHash<BlendDesc>>        m_blendCache;
    std::unordered_map<RastDesc,         ComPtr<ID3D11RasterizerState1>,
                       PodHash<RastDesc>>         m_rastCache;
    std::unordered_map<DepthStencilDesc, ComPtr<ID3D11DepthStencilState>,
                       PodHash<DepthStencilDesc>> m_dsCache;
    std::unordered_map<SamplerDesc,      ComPtr<ID3D11SamplerState>,
                       PodHash<SamplerDesc>>      m_sampCache;

    SRWLOCK m_blendLock  = SRWLOCK_INIT;
    SRWLOCK m_rastLock   = SRWLOCK_INIT;
    SRWLOCK m_dsLock     = SRWLOCK_INIT;
    SRWLOCK m_sampLock   = SRWLOCK_INIT;
};

}

#endif
