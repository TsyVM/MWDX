#pragma once

#ifndef DX9TO11_QUERY_BRIDGE_H
#define DX9TO11_QUERY_BRIDGE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>

// D3D9 queries on top of D3D11 queries.
//
// The two models differ in an important way: D3D9's Issue takes a flag saying
// whether this is the beginning or the end of the measured range, whereas
// D3D11 has separate Begin and End calls. Mapping one onto the other is most
// of what this class does.
//
// Two D3D9 query types have no D3D11 equivalent at all — RESOURCEMANAGER and
// VCACHE — and are answered as stubs: success, with zero-filled data. That is
// a considered choice. Games use these for diagnostics and rarely act on the
// result, and returning an error instead risks a game treating the failure as
// fatal.
//
// Unsupported types return D3DERR_NOTAVAILABLE and specifically not E_NOTIMPL.
// D3D9 games treat NOTAVAILABLE as "this feature is absent, take the other
// path" and E_NOTIMPL as a hard error worth aborting over.

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class DeviceContext11;

class QueryBridge {
public:

    static HRESULT Create(
        DeviceContext11* ctx,
        D3DQUERYTYPE     type,
        QueryBridge**    ppOut) noexcept;

    HRESULT Issue(DWORD dwIssueFlags) noexcept;

    HRESULT GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) noexcept;

    [[nodiscard]] DWORD DataSize() const noexcept { return m_dataSize; }

    [[nodiscard]] D3DQUERYTYPE Type() const noexcept { return m_type; }

private:
    QueryBridge() = default;

    DeviceContext11*         m_ctx{};
    D3DQUERYTYPE             m_type{};
    DWORD                    m_dataSize{};

    ComPtr<ID3D11Query>      m_query;

    bool m_isStub{ false };

    HRESULT FillD3D9Data(const void* pD11Data, void* pOut, DWORD outSize) noexcept;
};

}

#endif
