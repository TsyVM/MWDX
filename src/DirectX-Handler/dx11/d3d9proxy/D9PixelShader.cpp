// Pixel shader objects.
//
// As with vertex shaders, the original bytecode is kept and translated lazily.
// Pixel shaders have a further reason to defer: alpha testing and fog were
// D3D9 render states that must be compiled into the shader, so one D3D9 pixel
// shader becomes several compiled variants chosen by the state at each draw.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9PixelShader.h>
#include <d3d9proxy/D9Device.h>
#include <core/ShaderCache.h>
#include <core/Log.h>
#include <cstring>
#include <cassert>

namespace dx9to11 {

D9PixelShader::D9PixelShader(
    D9Device*                 pDevice,
    ComPtr<ID3D11PixelShader> ps11,
    std::vector<uint8_t>      d3d9Bytecode) noexcept
    : m_device(pDevice)
    , m_ps11(std::move(ps11))
    , m_d3d9Bytecode(std::move(d3d9Bytecode))
{}

HRESULT STDMETHODCALLTYPE D9PixelShader::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DPixelShader9) {
        *ppvObj = static_cast<IDirect3DPixelShader9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9PixelShader::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&m_refCount));
}

ULONG STDMETHODCALLTYPE D9PixelShader::Release()
{
    ULONG ref = static_cast<ULONG>(InterlockedDecrement(&m_refCount));
    if (ref == 0) delete this;
    return ref;
}

HRESULT STDMETHODCALLTYPE D9PixelShader::GetDevice(IDirect3DDevice9** ppDevice)
{

    if (!ppDevice) return D3DERR_INVALIDCALL;
    return m_device->QueryInterface(__uuidof(IDirect3DDevice9),
                                    reinterpret_cast<void**>(ppDevice));
}

HRESULT STDMETHODCALLTYPE D9PixelShader::GetFunction(void* pData, UINT* pSizeOfData)
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

ID3D11PixelShader* D9PixelShader::PS11ForState(UINT alphaFunc, UINT fogMode,
                                               UINT texKindMask,
                                               ShaderCache* cache) noexcept
{

    const UINT af = (alphaFunc == 0 || alphaFunc >= 8) ? 0u : alphaFunc;
    const UINT fm = (fogMode <= 4) ? fogMode : 0u;
    const UINT tk = texKindMask & 0xFFFFu;
    if ((af == 0 && fm == 0 && tk == 0) || !cache)
        return m_ps11.Get();

    const uint32_t key = ShaderCache::PackVariantKey(af, fm, tk);

    auto it = m_variant.find(key);
    if (it != m_variant.end())
        return it->second;

    ID3D11PixelShader* ps = nullptr;
    const HRESULT hr = cache->GetPSVariant(
        reinterpret_cast<const DWORD*>(m_d3d9Bytecode.data()),
        m_d3d9Bytecode.size(), af, fm, tk, &ps);

    if (SUCCEEDED(hr) && ps) {

        ps->Release();
        m_variant.emplace(key, ps);
        return ps;
    }

    if (!m_variantFailLogged) {
        m_variantFailLogged = true;

        DXLOG_WARN("PS11ForState variant fallback: af=%u fm=%u tk=0x%04X - "
                   "requested variant unavailable, drawing with the BASE "
                   "shader for this bytecode (check for a D3DCompile FAILED "
                   "line above for this shader).",
                   af, fm, tk);
    }

    m_variant.emplace(key, m_ps11.Get());
    return m_ps11.Get();
}

}
