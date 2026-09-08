// IDirect3DQuery9 -- occlusion counts, timestamps, pipeline statistics.
//
// A thin object over the QueryBridge, which does the real work of mapping
// D3D9's single Issue call with begin/end flags onto D3D11's separate Begin and
// End, and of reshaping result data into the layout D3D9 defines.
//
// Occlusion queries matter most in practice: games use them to decide whether
// to draw expensive effects such as lens flares, so a query that never returns
// data leaves those effects permanently switched off, with nothing failing.

#pragma once

#ifndef DX9TO11_D9_QUERY_H
#define DX9TO11_D9_QUERY_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <core/QueryBridge.h>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class DeviceContext11;
class D9Device;

class D9Query final : public IDirect3DQuery9 {
public:

    D9Query(D9Device* pDevice, D3DQUERYTYPE type, std::unique_ptr<QueryBridge> bridge) noexcept;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override;
    DWORD   STDMETHODCALLTYPE GetDataSize() override;
    HRESULT STDMETHODCALLTYPE Issue(DWORD dwIssueFlags) override;
    HRESULT STDMETHODCALLTYPE GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags) override;

private:
    ~D9Query() = default;

    LONG                         m_refCount{ 1 };
    D9Device*                    m_device;
    D3DQUERYTYPE                 m_type;
    std::unique_ptr<QueryBridge> m_bridge;
};

}

#endif
