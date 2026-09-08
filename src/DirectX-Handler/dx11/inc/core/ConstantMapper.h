#pragma once

#ifndef DX9TO11_CONSTANT_MAPPER_H
#define DX9TO11_CONSTANT_MAPPER_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <cstring>
#include <cstdint>

// Shader constants: D3D9's flat register file onto D3D11 constant buffers.
//
// A D3D9 game writes constants by register index — SetVertexShaderConstantF(12,
// ptr, 4) means "registers c12 through c15". D3D11 has no register file; it
// has constant buffers. The translator resolves this by emitting the register
// file literally as arrays (c[], ic[], bc[]), so the shadow copies below have
// exactly the layout the shader expects and a flush is a straight memcpy of
// the whole structure. No translation, no per-register bookkeeping.
//
// The sizes are D3D9's limits, not D3D11's: 256 float registers for a vertex
// shader, 224 for a pixel shader. The trailing padding keeps each structure a
// multiple of 16 bytes, which D3D11 requires of a constant buffer.
//
// Writes are staged into these copies and uploaded once per draw rather than
// per call, because games set constants in small runs and often set the same
// values repeatedly.
//
// Out-of-range writes are clamped, never refused. Returning D3DERR_INVALIDCALL
// for a partially out-of-range write is stricter than the retail D3D9 runtime,
// and a game that gets an error back from a constant write abandons the whole
// render path that issued it — silently, since from our side nothing failed.
// An overrun logs once and writes what fits.

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class DeviceContext11;

#pragma pack(push, 16)

struct VSConstantData {
    float  c[256][4];
    int    i[16][4];
    UINT   b[16];
    UINT   _pad[16];
};
static_assert(sizeof(VSConstantData) == 4416 + 64, "VSConstantData size mismatch");

struct PSConstantData {
    float  c[224][4];
    int    i[16][4];
    UINT   b[16];
    UINT   _pad[16];
};
static_assert(sizeof(PSConstantData) == 3904 + 64, "PSConstantData size mismatch");

#pragma pack(pop)

class ConstantMapper {
public:
    explicit ConstantMapper(DeviceContext11* ctx) noexcept;

    HRESULT SetVSConstantF(UINT start, const float* pData, UINT count) noexcept;
    HRESULT SetVSConstantI(UINT start, const int*   pData, UINT count) noexcept;
    HRESULT SetVSConstantB(UINT start, const BOOL*  pData, UINT count) noexcept;

    HRESULT SetPSConstantF(UINT start, const float* pData, UINT count) noexcept;
    HRESULT SetPSConstantI(UINT start, const int*   pData, UINT count) noexcept;
    HRESULT SetPSConstantB(UINT start, const BOOL*  pData, UINT count) noexcept;

    HRESULT GetVSConstantF(UINT start, float* pData, UINT count) const noexcept;
    HRESULT GetVSConstantI(UINT start, int*   pData, UINT count) const noexcept;
    HRESULT GetVSConstantB(UINT start, BOOL*  pData, UINT count) const noexcept;

    HRESULT GetPSConstantF(UINT start, float* pData, UINT count) const noexcept;
    HRESULT GetPSConstantI(UINT start, int*   pData, UINT count) const noexcept;
    HRESULT GetPSConstantB(UINT start, BOOL*  pData, UINT count) const noexcept;

    HRESULT FlushVS() noexcept;

    HRESULT FlushPS() noexcept;

    [[nodiscard]] ID3D11Buffer* VSConstantBuffer() const noexcept { return m_vsCB.Get(); }
    [[nodiscard]] ID3D11Buffer* PSConstantBuffer() const noexcept { return m_psCB.Get(); }

    void ResetToDefaults() noexcept
    {
        std::memset(&m_vsData, 0, sizeof(m_vsData));
        std::memset(&m_psData, 0, sizeof(m_psData));
        m_vsDirty = m_psDirty = true;
    }

private:
    HRESULT EnsureBuffers() noexcept;

    DeviceContext11* m_ctx;

    VSConstantData   m_vsData{};
    PSConstantData   m_psData{};
    bool             m_vsDirty{ true };
    bool             m_psDirty{ true };

    ComPtr<ID3D11Buffer> m_vsCB;
    ComPtr<ID3D11Buffer> m_psCB;
};

}

#endif
