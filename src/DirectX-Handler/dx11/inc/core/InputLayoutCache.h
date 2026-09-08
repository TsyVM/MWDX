#pragma once

#ifndef DX9TO11_INPUT_LAYOUT_CACHE_H
#define DX9TO11_INPUT_LAYOUT_CACHE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <synchapi.h>
#include <unordered_map>
#include <cstdint>
#include <core/ShaderCache.h>

// Vertex declaration + vertex shader -> ID3D11InputLayout.
//
// D3D11 validates an input layout against the signature of the shader it will
// be used with, so a layout is not a property of the vertex declaration alone
// — the same declaration paired with two different shaders needs two layouts.
// Hence the two-part key.
//
// Both halves are hashes rather than the objects themselves: a declaration can
// be destroyed and recreated at the same address, and comparing shader blobs
// byte-by-byte on every draw is far too slow. Creating a layout involves
// driver validation and is expensive enough that a cache miss on a hot path is
// visible in frame time.
//
// A failure here is worth logging loudly. If GetOrCreate fails and the caller
// carries on, the *previous* draw's layout stays bound and every vertex
// attribute is read from the wrong offset — geometry that is present, drawn,
// and completely wrong, with no error anywhere.

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class DeviceContext11;

// D3D9 lets a declaration reference a stream the game never bound, and expects
// the missing attributes to read as zero. D3D11 will not draw with a gap in
// the layout, so a small permanently-zeroed buffer is bound in this high slot
// and any unbound stream is pointed at it. Slot 15 is chosen because D3D9
// itself only guarantees 16 streams, so nothing real competes for it.
static constexpr UINT kZeroStreamSlot = 15;

struct InputLayoutKey {
    uint64_t declHash   = 0;
    uint64_t vsBlobHash = 0;

    bool operator==(const InputLayoutKey& o) const noexcept {
        return declHash == o.declHash && vsBlobHash == o.vsBlobHash;
    }
};

struct InputLayoutKeyHash {
    size_t operator()(const InputLayoutKey& k) const noexcept {

        uint64_t h = k.declHash;
        h ^= k.vsBlobHash + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return static_cast<size_t>(h);
    }
};

class InputLayoutCache {
public:
    explicit InputLayoutCache(DeviceContext11* ctx) noexcept;

    HRESULT GetOrCreate(
        const D3DVERTEXELEMENT9* pElements,
        const ShaderReflection&  refl,
        ID3D11InputLayout**      ppLayout) noexcept;

    [[nodiscard]] static DXGI_FORMAT DeclTypeToFormat(BYTE declType) noexcept;

    [[nodiscard]] static const char* DeclUsageToSemantic(BYTE usage) noexcept;

private:
    DeviceContext11* m_ctx;

    std::unordered_map<InputLayoutKey, ComPtr<ID3D11InputLayout>, InputLayoutKeyHash> m_cache;
    mutable SRWLOCK m_lock = SRWLOCK_INIT;
};

}

#endif
