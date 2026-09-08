// Staging and upload of shader constants.
//
// Writes land in a shadow copy laid out exactly as the translated shader's
// constant arrays, so a flush is one memcpy of the whole structure with no
// per-register work. Uploads happen once per draw rather than per call,
// because games set constants in small runs and frequently rewrite the same
// values.
//
// Out-of-range writes are clamped to what fits and logged once, never
// refused. Returning an error for a partially out-of-range write is stricter
// than the original runtime, and a game that gets a failure back from a
// constant write abandons the render path that issued it -- silently, because
// from this side nothing went wrong.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/ConstantMapper.h>
#include <core/Log.h>
#include <core/DeviceContext11.h>
#include <cstring>
#include <cassert>

namespace dx9to11 {

ConstantMapper::ConstantMapper(DeviceContext11* ctx) noexcept
    : m_ctx(ctx)
{
    std::memset(&m_vsData, 0, sizeof(m_vsData));
    std::memset(&m_psData, 0, sizeof(m_psData));
}

HRESULT ConstantMapper::EnsureBuffers() noexcept
{
    if (m_vsCB && m_psCB) return S_OK;

    auto* dev = m_ctx->Device();

    if (!m_vsCB) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(VSConstantData);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = dev->CreateBuffer(&bd, nullptr, m_vsCB.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    if (!m_psCB) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(PSConstantData);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = dev->CreateBuffer(&bd, nullptr, m_psCB.GetAddressOf());
        if (FAILED(hr)) return hr;
    }

    return S_OK;
}

HRESULT ConstantMapper::SetVSConstantF(UINT start, const float* pData, UINT count) noexcept
{
    if (!pData) return D3DERR_INVALIDCALL;
    if (start >= 256) return D3D_OK;

    if (count > 256 - start) {

        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            DXLOG_WARN("[dx9to11] SetVertexShaderConstantF(start=%u count=%u) "
                       "overruns c[256]; writing the %u registers that fit "
                       "(this call previously wrote NONE of them)",
                       start, count, 256u - start);
        }
        count = 256 - start;
    }
    std::memcpy(&m_vsData.c[start], pData, count * 4 * sizeof(float));
    m_vsDirty = true;
    return D3D_OK;
}

HRESULT ConstantMapper::SetVSConstantI(UINT start, const int* pData, UINT count) noexcept
{
    if (!pData) return D3DERR_INVALIDCALL;
    if (start >= 16) return D3D_OK;

    if (count > 16 - start) count = 16 - start;
    std::memcpy(&m_vsData.i[start], pData, count * 4 * sizeof(int));
    m_vsDirty = true;
    return D3D_OK;
}

HRESULT ConstantMapper::SetVSConstantB(UINT start, const BOOL* pData, UINT count) noexcept
{
    if (!pData) return D3DERR_INVALIDCALL;
    if (start >= 16) return D3D_OK;

    if (count > 16 - start) count = 16 - start;
    for (UINT k = 0; k < count; ++k)
        m_vsData.b[start + k] = pData[k] ? 1u : 0u;
    m_vsDirty = true;
    return D3D_OK;
}

HRESULT ConstantMapper::SetPSConstantF(UINT start, const float* pData, UINT count) noexcept
{
    if (!pData) return D3DERR_INVALIDCALL;
    if (start >= 224) return D3D_OK;

    if (count > 224 - start) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            DXLOG_WARN("[dx9to11] SetPixelShaderConstantF(start=%u count=%u) "
                       "overruns c[224]; writing the %u registers that fit "
                       "(this call previously wrote NONE of them)",
                       start, count, 224u - start);
        }
        count = 224 - start;
    }
    std::memcpy(&m_psData.c[start], pData, count * 4 * sizeof(float));
    m_psDirty = true;
    return D3D_OK;
}

HRESULT ConstantMapper::SetPSConstantI(UINT start, const int* pData, UINT count) noexcept
{
    if (!pData) return D3DERR_INVALIDCALL;
    if (start >= 16) return D3D_OK;

    if (count > 16 - start) count = 16 - start;
    std::memcpy(&m_psData.i[start], pData, count * 4 * sizeof(int));
    m_psDirty = true;
    return D3D_OK;
}

HRESULT ConstantMapper::SetPSConstantB(UINT start, const BOOL* pData, UINT count) noexcept
{
    if (!pData) return D3DERR_INVALIDCALL;
    if (start >= 16) return D3D_OK;

    if (count > 16 - start) count = 16 - start;
    for (UINT k = 0; k < count; ++k)
        m_psData.b[start + k] = pData[k] ? 1u : 0u;
    m_psDirty = true;
    return D3D_OK;
}

HRESULT ConstantMapper::GetVSConstantF(UINT start, float* pData, UINT count) const noexcept
{
    if (!pData) return D3DERR_INVALIDCALL;
    if (start >= 256) return D3D_OK;
    if (count > 256 - start) count = 256 - start;
    std::memcpy(pData, &m_vsData.c[start], count * 4 * sizeof(float));
    return D3D_OK;
}

HRESULT ConstantMapper::GetVSConstantI(UINT start, int* pData, UINT count) const noexcept
{
    if (!pData || start + count > 16) return D3DERR_INVALIDCALL;
    std::memcpy(pData, &m_vsData.i[start], count * 4 * sizeof(int));
    return D3D_OK;
}

HRESULT ConstantMapper::GetVSConstantB(UINT start, BOOL* pData, UINT count) const noexcept
{
    if (!pData || start + count > 16) return D3DERR_INVALIDCALL;
    for (UINT k = 0; k < count; ++k)
        pData[k] = m_vsData.b[start + k] ? TRUE : FALSE;
    return D3D_OK;
}

HRESULT ConstantMapper::GetPSConstantF(UINT start, float* pData, UINT count) const noexcept
{
    if (!pData) return D3DERR_INVALIDCALL;
    if (start >= 224) return D3D_OK;
    if (count > 224 - start) count = 224 - start;
    std::memcpy(pData, &m_psData.c[start], count * 4 * sizeof(float));
    return D3D_OK;
}

HRESULT ConstantMapper::GetPSConstantI(UINT start, int* pData, UINT count) const noexcept
{
    if (!pData || start + count > 16) return D3DERR_INVALIDCALL;
    std::memcpy(pData, &m_psData.i[start], count * 4 * sizeof(int));
    return D3D_OK;
}

HRESULT ConstantMapper::GetPSConstantB(UINT start, BOOL* pData, UINT count) const noexcept
{
    if (!pData || start + count > 16) return D3DERR_INVALIDCALL;
    for (UINT k = 0; k < count; ++k)
        pData[k] = m_psData.b[start + k] ? TRUE : FALSE;
    return D3D_OK;
}

HRESULT ConstantMapper::FlushVS() noexcept
{
    HRESULT hr = EnsureBuffers();
    if (FAILED(hr)) return hr;

    if (m_vsDirty) {
        auto* ctx = m_ctx->Context();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = ctx->Map(m_vsCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return hr;
        std::memcpy(mapped.pData, &m_vsData, sizeof(m_vsData));
        ctx->Unmap(m_vsCB.Get(), 0);
        m_vsDirty = false;
    }

    ID3D11Buffer* cb = m_vsCB.Get();
    m_ctx->Context()->VSSetConstantBuffers(0, 1, &cb);
    return S_OK;
}

HRESULT ConstantMapper::FlushPS() noexcept
{
    HRESULT hr = EnsureBuffers();
    if (FAILED(hr)) return hr;

    if (m_psDirty) {
        auto* ctx = m_ctx->Context();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        hr = ctx->Map(m_psCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) return hr;
        std::memcpy(mapped.pData, &m_psData, sizeof(m_psData));
        ctx->Unmap(m_psCB.Get(), 0);
        m_psDirty = false;
    }

    ID3D11Buffer* cb = m_psCB.Get();
    m_ctx->Context()->PSSetConstantBuffers(0, 1, &cb);
    return S_OK;
}

}
