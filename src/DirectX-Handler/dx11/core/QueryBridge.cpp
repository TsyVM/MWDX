// Maps D3D9 queries onto D3D11 queries.
//
// The models differ in shape: D3D9 has one Issue call with a flag saying
// whether this is the start or the end of the measured range, while D3D11 has
// separate Begin and End. Result data also has to be reshaped, since the two
// APIs define different structures for the same measurement.
//
// Pipeline timing is approximated. D3D11 reports invocation counts per stage
// where D3D9 expects percentages of frame time, so the counts are converted
// into proportions. It is indicative rather than exact, which is what games
// use it for.
//
// Query types with no equivalent return D3DERR_NOTAVAILABLE and specifically
// not E_NOTIMPL: games read the former as "absent, take another path" and the
// latter as a hard error.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/QueryBridge.h>
#include <core/DeviceContext11.h>
#include <cstring>
#include <cassert>

namespace dx9to11 {

struct D3DPIPELINETIMINGS {
    float VertexProcessingTimePercent;
    float PixelProcessingTimePercent;
    float OtherGPUProcessingTimePercent;
    float GPUIdleTimePercent;
};
static_assert(sizeof(D3DPIPELINETIMINGS) == 16, "");

struct D3DDEVINFO_VCACHE_LOCAL {
    DWORD Pattern;
    DWORD OptMethod;
    DWORD CacheSize;
    DWORD MagicNumber;
};

struct D3DDEVINFO_D3DVERTEXSTATS_LOCAL {
    DWORD NumRenderedTriangles;
    DWORD NumExtraClippingTriangles;
};

namespace {

DWORD QueryDataSize(D3DQUERYTYPE type) noexcept
{
    switch (type) {
    case D3DQUERYTYPE_VCACHE:           return static_cast<DWORD>(sizeof(D3DDEVINFO_VCACHE_LOCAL));
    case D3DQUERYTYPE_RESOURCEMANAGER:  return 7 * sizeof(DWORD);
    case D3DQUERYTYPE_VERTEXSTATS:      return static_cast<DWORD>(sizeof(D3DDEVINFO_D3DVERTEXSTATS_LOCAL));
    case D3DQUERYTYPE_EVENT:            return sizeof(BOOL);
    case D3DQUERYTYPE_OCCLUSION:        return sizeof(DWORD);
    case D3DQUERYTYPE_TIMESTAMP:        return sizeof(UINT64);
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:return sizeof(BOOL);
    case D3DQUERYTYPE_TIMESTAMPFREQ:    return sizeof(UINT64);
    case D3DQUERYTYPE_PIPELINETIMINGS:  return static_cast<DWORD>(sizeof(D3DPIPELINETIMINGS));
    default:                            return 0;
    }
}

bool MapToD11Query(D3DQUERYTYPE type, D3D11_QUERY& out) noexcept
{
    switch (type) {
    case D3DQUERYTYPE_EVENT:              out = D3D11_QUERY_EVENT;                    return true;
    case D3DQUERYTYPE_OCCLUSION:          out = D3D11_QUERY_OCCLUSION;               return true;
    case D3DQUERYTYPE_TIMESTAMP:          out = D3D11_QUERY_TIMESTAMP;               return true;
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:  out = D3D11_QUERY_TIMESTAMP_DISJOINT;      return true;
    case D3DQUERYTYPE_TIMESTAMPFREQ:      out = D3D11_QUERY_TIMESTAMP_DISJOINT;      return true;
    case D3DQUERYTYPE_PIPELINETIMINGS:    out = D3D11_QUERY_PIPELINE_STATISTICS;     return true;
    case D3DQUERYTYPE_VERTEXSTATS:        out = D3D11_QUERY_PIPELINE_STATISTICS;     return true;
    default:                              return false;
    }
}

}

HRESULT QueryBridge::Create(
    DeviceContext11* ctx,
    D3DQUERYTYPE     type,
    QueryBridge**    ppOut) noexcept
{
    if (!ppOut) return E_POINTER;
    *ppOut = nullptr;

    switch (type) {
    case D3DQUERYTYPE_VCACHE:
    case D3DQUERYTYPE_RESOURCEMANAGER:
    case D3DQUERYTYPE_VERTEXSTATS:
    case D3DQUERYTYPE_EVENT:
    case D3DQUERYTYPE_OCCLUSION:
    case D3DQUERYTYPE_TIMESTAMP:
    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    case D3DQUERYTYPE_TIMESTAMPFREQ:
    case D3DQUERYTYPE_PIPELINETIMINGS:
        break;
    default:

        return D3DERR_NOTAVAILABLE;
    }

    auto* bridge = new (std::nothrow) QueryBridge();
    if (!bridge) return E_OUTOFMEMORY;

    bridge->m_ctx      = ctx;
    bridge->m_type     = type;
    bridge->m_dataSize = QueryDataSize(type);

    D3D11_QUERY d11type{};
    if (!MapToD11Query(type, d11type)) {
        bridge->m_isStub = true;
        *ppOut = bridge;
        return S_OK;
    }

    D3D11_QUERY_DESC desc{};
    desc.Query     = d11type;
    desc.MiscFlags = 0;

    HRESULT hr = ctx->Device()->CreateQuery(&desc, bridge->m_query.GetAddressOf());
    if (FAILED(hr)) {
        delete bridge;
        return hr;
    }

    *ppOut = bridge;
    return S_OK;
}

HRESULT QueryBridge::Issue(DWORD dwIssueFlags) noexcept
{
    if (m_isStub) return D3D_OK;
    if (!m_query) return D3DERR_INVALIDCALL;

    ID3D11DeviceContext1* ctx = m_ctx->Context();

    if (dwIssueFlags & D3DISSUE_BEGIN) {
        ctx->Begin(m_query.Get());
    }
    if (dwIssueFlags & D3DISSUE_END) {
        ctx->End(m_query.Get());
    }
    return D3D_OK;
}

HRESULT QueryBridge::GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) noexcept
{

    if (m_isStub) {
        if (pData && dwSize > 0)
            std::memset(pData, 0, dwSize);
        return S_OK;
    }

    if (!m_query) return D3DERR_INVALIDCALL;

    const BOOL flush = (dwGetDataFlags & D3DGETDATA_FLUSH) ? TRUE : FALSE;
    ID3D11DeviceContext1* ctx11 = m_ctx->Context();

    switch (m_type)
    {

    case D3DQUERYTYPE_EVENT:
    {
        BOOL signalled = FALSE;
        HRESULT hr = ctx11->GetData(m_query.Get(), &signalled, sizeof(BOOL), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return S_FALSE;
        if (FAILED(hr))    return D3DERR_INVALIDCALL;
        if (pData && dwSize >= sizeof(BOOL))
            *static_cast<BOOL*>(pData) = signalled;
        return S_OK;
    }

    case D3DQUERYTYPE_OCCLUSION:
    {
        UINT64 pixels = 0;
        HRESULT hr = ctx11->GetData(m_query.Get(), &pixels, sizeof(UINT64), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return S_FALSE;
        if (FAILED(hr))    return D3DERR_INVALIDCALL;
        if (pData && dwSize >= sizeof(DWORD)) {
            DWORD clamped = (pixels > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : static_cast<DWORD>(pixels);
            *static_cast<DWORD*>(pData) = clamped;
        }
        return S_OK;
    }

    case D3DQUERYTYPE_TIMESTAMP:
    {
        UINT64 ts = 0;
        HRESULT hr = ctx11->GetData(m_query.Get(), &ts, sizeof(UINT64), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return S_FALSE;
        if (FAILED(hr))    return D3DERR_INVALIDCALL;
        if (pData && dwSize >= sizeof(UINT64))
            *static_cast<UINT64*>(pData) = ts;
        return S_OK;
    }

    case D3DQUERYTYPE_TIMESTAMPDISJOINT:
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
        HRESULT hr = ctx11->GetData(m_query.Get(), &data, sizeof(data), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return S_FALSE;
        if (FAILED(hr))    return D3DERR_INVALIDCALL;
        if (pData && dwSize >= sizeof(BOOL))
            *static_cast<BOOL*>(pData) = data.Disjoint;
        return S_OK;
    }

    case D3DQUERYTYPE_TIMESTAMPFREQ:
    {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT data{};
        HRESULT hr = ctx11->GetData(m_query.Get(), &data, sizeof(data), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return S_FALSE;
        if (FAILED(hr))    return D3DERR_INVALIDCALL;
        if (pData && dwSize >= sizeof(UINT64))
            *static_cast<UINT64*>(pData) = data.Frequency;
        return S_OK;
    }

    case D3DQUERYTYPE_PIPELINETIMINGS:
    {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
        HRESULT hr = ctx11->GetData(m_query.Get(), &stats, sizeof(stats), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return S_FALSE;
        if (FAILED(hr))    return D3DERR_INVALIDCALL;
        if (pData && dwSize >= sizeof(D3DPIPELINETIMINGS)) {
            D3DPIPELINETIMINGS* out = static_cast<D3DPIPELINETIMINGS*>(pData);
            UINT64 total = stats.VSInvocations + stats.PSInvocations + 1;
            out->VertexProcessingTimePercent        = static_cast<float>(stats.VSInvocations * 100.0 / total);
            out->PixelProcessingTimePercent         = static_cast<float>(stats.PSInvocations * 100.0 / total);
            out->OtherGPUProcessingTimePercent      = 0.0f;
            out->GPUIdleTimePercent                 = 0.0f;
        }
        return S_OK;
    }

    case D3DQUERYTYPE_VERTEXSTATS:
    {
        D3D11_QUERY_DATA_PIPELINE_STATISTICS stats{};
        HRESULT hr = ctx11->GetData(m_query.Get(), &stats, sizeof(stats), flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_FALSE) return S_FALSE;
        if (FAILED(hr))    return D3DERR_INVALIDCALL;
        if (pData && dwSize >= sizeof(D3DDEVINFO_D3DVERTEXSTATS_LOCAL)) {
            D3DDEVINFO_D3DVERTEXSTATS_LOCAL* out = static_cast<D3DDEVINFO_D3DVERTEXSTATS_LOCAL*>(pData);

            out->NumRenderedTriangles       = static_cast<DWORD>(stats.CInvocations & 0xFFFFFFFF);
            out->NumExtraClippingTriangles  = 0;
        }
        return S_OK;
    }

    default:
        return E_NOTIMPL;
    }
}

}
