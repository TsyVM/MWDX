// Vertex shader objects.
//
// Keeps the game's original bytecode. Translation happens on first use and is
// cached, because a game may create shaders it never draws with, and because
// GetFunction is required to hand the original bytecode back on request.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9VertexShader.h>
#include <d3d9proxy/D9Device.h>
#include <cstring>
#include <cassert>

namespace dx9to11 {

D9VertexShader::D9VertexShader(
    D9Device*                  pDevice,
    ComPtr<ID3D11VertexShader> vs11,
    ShaderReflection           refl,
    std::vector<uint8_t>       d3d9Bytecode) noexcept
    : m_device(pDevice)
    , m_vs11(std::move(vs11))
    , m_refl(std::move(refl))
    , m_d3d9Bytecode(std::move(d3d9Bytecode))
{}

HRESULT STDMETHODCALLTYPE D9VertexShader::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DVertexShader9) {
        *ppvObj = static_cast<IDirect3DVertexShader9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9VertexShader::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG STDMETHODCALLTYPE D9VertexShader::Release()
{
    ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
    if (ref == 0) delete this;
    return ref;
}

HRESULT STDMETHODCALLTYPE D9VertexShader::GetDevice(IDirect3DDevice9** ppDevice)
{

    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9),
                                    reinterpret_cast<void**>(ppDevice));
}

HRESULT STDMETHODCALLTYPE D9VertexShader::GetFunction(void* pData, UINT* pSizeOfData)
{

    if (!pSizeOfData) return D3DERR_INVALIDCALL;

    const UINT byteLen = static_cast<UINT>(m_d3d9Bytecode.size());

    if (!pData) {

        *pSizeOfData = byteLen;
        return D3D_OK;
    }

    if (*pSizeOfData < byteLen) {
        *pSizeOfData = byteLen;
        return D3DERR_INVALIDCALL;
    }

    std::memcpy(pData, m_d3d9Bytecode.data(), byteLen);
    *pSizeOfData = byteLen;
    return D3D_OK;
}

}
