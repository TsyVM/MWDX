// Query objects.
//
// A thin wrapper over the query bridge, which handles the mapping between
// D3D9's single Issue call and D3D11's separate Begin and End.
//
// Occlusion queries are the ones that matter in practice: a game uses them to
// decide whether to draw expensive effects such as lens flares, so a query
// that never produces data leaves those effects permanently disabled with
// nothing reporting a problem.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Query.h>
#include <d3d9proxy/D9Device.h>
#include <cassert>

namespace dx9to11 {

D9Query::D9Query(D9Device* pDevice, D3DQUERYTYPE type, std::unique_ptr<QueryBridge> bridge) noexcept
    : m_device(pDevice)
    , m_type(type)
    , m_bridge(std::move(bridge))
{}

HRESULT STDMETHODCALLTYPE D9Query::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DQuery9) {
        *ppvObj = static_cast<IDirect3DQuery9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Query::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG STDMETHODCALLTYPE D9Query::Release()
{
    ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
    if (ref == 0) delete this;
    return ref;
}

HRESULT STDMETHODCALLTYPE D9Query::GetDevice(IDirect3DDevice9** ppDevice)
{

    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9),
                                    reinterpret_cast<void**>(ppDevice));
}

D3DQUERYTYPE STDMETHODCALLTYPE D9Query::GetType()
{
    return m_type;
}

DWORD STDMETHODCALLTYPE D9Query::GetDataSize()
{
    return m_bridge ? m_bridge->DataSize() : 0;
}

HRESULT STDMETHODCALLTYPE D9Query::Issue(DWORD dwIssueFlags)
{
    if (!m_bridge) return D3DERR_INVALIDCALL;
    return m_bridge->Issue(dwIssueFlags);
}

HRESULT STDMETHODCALLTYPE D9Query::GetData(void* pData, DWORD dwSize, DWORD dwGetDataFlags)
{
    if (!m_bridge) return D3DERR_INVALIDCALL;
    return m_bridge->GetData(pData, dwSize, dwGetDataFlags);
}

}
