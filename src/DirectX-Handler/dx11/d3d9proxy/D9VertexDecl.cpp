// Vertex declarations.
//
// Stores a copy of the D3D9 element array; the caller is entitled to free its
// own copy immediately. No D3D11 input layout is built here, because that
// requires the vertex shader as well and the pairing is not known until a draw.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9VertexDecl.h>
#include <d3d9proxy/D9Device.h>
#include <cstring>
#include <cassert>

namespace dx9to11 {

D9VertexDecl::D9VertexDecl(D9Device* pDevice, const D3DVERTEXELEMENT9* pElements) noexcept
    : m_device(pDevice)
{
    assert(pElements);

    UINT count = 0;
    while (pElements[count].Stream != 0xFF) {
        ++count;
        if (count > MAXD3DDECLLENGTH) break;
    }
    m_elements.reserve(count + 1);
    for (UINT i = 0; i < count; ++i)
        m_elements.push_back(pElements[i]);

    m_elements.push_back(D3DDECL_END());
}

HRESULT STDMETHODCALLTYPE D9VertexDecl::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DVertexDeclaration9) {
        *ppvObj = static_cast<IDirect3DVertexDeclaration9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9VertexDecl::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG STDMETHODCALLTYPE D9VertexDecl::Release()
{
    ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
    if (ref == 0) delete this;
    return ref;
}

HRESULT STDMETHODCALLTYPE D9VertexDecl::GetDevice(IDirect3DDevice9** ppDevice)
{

    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9),
                                    reinterpret_cast<void**>(ppDevice));
}

HRESULT STDMETHODCALLTYPE D9VertexDecl::GetDeclaration(
    D3DVERTEXELEMENT9* pElements, UINT* pNumElements)
{
    if (!pNumElements) return D3DERR_INVALIDCALL;

    const UINT count = static_cast<UINT>(m_elements.size());

    if (!pElements) {
        *pNumElements = count;
        return D3D_OK;
    }

    if (*pNumElements < count) {
        *pNumElements = count;
        return D3DERR_INVALIDCALL;
    }

    std::memcpy(pElements, m_elements.data(), count * sizeof(D3DVERTEXELEMENT9));
    *pNumElements = count;
    return D3D_OK;
}

}
