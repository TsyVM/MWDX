// The render state, texture stage state and sampler state setters.
//
// These do almost nothing by design: each records into the shadow state and
// raises a dirty bit. Nothing is translated here, because D3D9 games set state
// far more often than they draw, and frequently overwrite a state several
// times before issuing the draw that uses it.
//
// Stage and sampler indices beyond the eight D3D9 guarantees are accepted
// rather than rejected. The original runtime tolerates them, and returning an
// error where it returned success makes a game abandon whatever render path
// issued the call, with nothing logged anywhere to explain it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Device.h>
#include <d3d9proxy/D9StateBlock.h>
#include <d3d9proxy/D9VertexShader.h>
#include <d3d9proxy/D9PixelShader.h>
#include <d3d9proxy/D9VertexDecl.h>
#include <core/BackendSelect.h>
#include <core/ShaderCache.h>
#include <core/ConstantMapper.h>
#include <core/DeviceContext11.h>
#include <core/D3D9ShaderTranslator.h>

#include <cassert>
#include <vector>
#include <cstring>

namespace dx9to11 {

namespace {

constexpr size_t kMaxShaderBlobBytes = 4u << 20;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateVertexShader(
    const DWORD* pFunction, IDirect3DVertexShader9** ppShader)
{
    if (!pFunction || !ppShader) return D3DERR_INVALIDCALL;
    *ppShader = nullptr;

    const SIZE_T byteLen = MeasureD3D9Shader(
        reinterpret_cast<const uint32_t*>(pFunction), kMaxShaderBlobBytes);
    if (byteLen == 0) return D3DERR_INVALIDCALL;

    ComPtr<ID3D11VertexShader> vs11;
    ShaderReflection refl;
    HRESULT hr = m_shaderCache->GetOrCreateVS(
        pFunction, byteLen, vs11.GetAddressOf(), &refl);
    if (FAILED(hr)) return hr;

    std::vector<uint8_t> blob(
        reinterpret_cast<const uint8_t*>(pFunction),
        reinterpret_cast<const uint8_t*>(pFunction) + byteLen);

    auto* obj = new (std::nothrow) D9VertexShader(
        this, std::move(vs11), std::move(refl), std::move(blob));
    if (!obj) return E_OUTOFMEMORY;

    *ppShader = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetVertexShader(IDirect3DVertexShader9* pShader)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordVertexShader(pShader);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    auto* vs = static_cast<D9VertexShader*>(pShader);

    if (m_currentVS == pShader) return D3D_OK;

    if (m_currentVS) m_currentVS->Release();
    m_currentVS = vs;
    if (m_currentVS) m_currentVS->AddRef();

    m_rst.State().vertexShader = pShader;

    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader)
{
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_currentVS;
    if (*ppShader) (*ppShader)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreatePixelShader(
    const DWORD* pFunction, IDirect3DPixelShader9** ppShader)
{
    if (!pFunction || !ppShader) return D3DERR_INVALIDCALL;
    *ppShader = nullptr;

    const SIZE_T byteLen = MeasureD3D9Shader(
        reinterpret_cast<const uint32_t*>(pFunction), kMaxShaderBlobBytes);
    if (byteLen == 0) return D3DERR_INVALIDCALL;

    ComPtr<ID3D11PixelShader> ps11;
    HRESULT hr = m_shaderCache->GetOrCreatePS(
        pFunction, byteLen, ps11.GetAddressOf());
    if (FAILED(hr)) return hr;

    std::vector<uint8_t> blob(
        reinterpret_cast<const uint8_t*>(pFunction),
        reinterpret_cast<const uint8_t*>(pFunction) + byteLen);

    auto* obj = new (std::nothrow) D9PixelShader(
        this, std::move(ps11), std::move(blob));
    if (!obj) return E_OUTOFMEMORY;

    *ppShader = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetPixelShader(IDirect3DPixelShader9* pShader)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordPixelShader(pShader);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    auto* ps = static_cast<D9PixelShader*>(pShader);

    if (m_currentPS == pShader) return D3D_OK;

    if (m_currentPS) m_currentPS->Release();
    m_currentPS = ps;
    if (m_currentPS) m_currentPS->AddRef();

    m_rst.State().pixelShader = pShader;

    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader)
{
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_currentPS;
    if (*ppShader) (*ppShader)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateVertexDeclaration(
    const D3DVERTEXELEMENT9* pElements,
    IDirect3DVertexDeclaration9** ppDecl)
{
    if (!pElements || !ppDecl) return D3DERR_INVALIDCALL;

    auto* obj = new (std::nothrow) D9VertexDecl(this, pElements);
    if (!obj) return E_OUTOFMEMORY;

    *ppDecl = obj;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetVertexDeclaration(
    IDirect3DVertexDeclaration9* pDecl)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordVertexDeclaration(pDecl);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    auto* decl = static_cast<D9VertexDecl*>(pDecl);

    if (m_currentDecl == pDecl) return D3D_OK;

    if (m_currentDecl) m_currentDecl->Release();
    m_currentDecl = decl;
    if (m_currentDecl) m_currentDecl->AddRef();

    m_rst.State().vertexDecl = pDecl;

    m_currentFVF = 0;

    m_inputLayoutDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetVertexDeclaration(
    IDirect3DVertexDeclaration9** ppDecl)
{
    if (!ppDecl) return D3DERR_INVALIDCALL;
    *ppDecl = m_currentDecl;
    if (*ppDecl) (*ppDecl)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetFVF(DWORD FVF)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordFVF(FVF);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    m_currentFVF = FVF;

    if (FVF != 0 && m_currentDecl) {
        m_currentDecl->Release();
        m_currentDecl = nullptr;
        m_rst.State().vertexDecl = nullptr;
    }

    m_inputLayoutDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetFVF(DWORD* pFVF)
{
    if (!pFVF) return D3DERR_INVALIDCALL;
    *pFVF = m_currentFVF;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetVertexShaderConstantF(
    UINT StartRegister, const float* pConstantData, UINT Vector4fCount)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    return m_constantMapper->SetVSConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D9Device::GetVertexShaderConstantF(
    UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{
    return m_constantMapper->GetVSConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D9Device::SetVertexShaderConstantI(
    UINT StartRegister, const int* pConstantData, UINT Vector4iCount)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    return m_constantMapper->SetVSConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT STDMETHODCALLTYPE D9Device::GetVertexShaderConstantI(
    UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{
    return m_constantMapper->GetVSConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT STDMETHODCALLTYPE D9Device::SetVertexShaderConstantB(
    UINT StartRegister, const BOOL* pConstantData, UINT BoolCount)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordVertexShaderConstantB(StartRegister, pConstantData, BoolCount);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    return m_constantMapper->SetVSConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE D9Device::GetVertexShaderConstantB(
    UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{
    return m_constantMapper->GetVSConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE D9Device::SetPixelShaderConstantF(
    UINT StartRegister, const float* pConstantData, UINT Vector4fCount)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    return m_constantMapper->SetPSConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D9Device::GetPixelShaderConstantF(
    UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{
    return m_constantMapper->GetPSConstantF(StartRegister, pConstantData, Vector4fCount);
}

HRESULT STDMETHODCALLTYPE D9Device::SetPixelShaderConstantI(
    UINT StartRegister, const int* pConstantData, UINT Vector4iCount)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    return m_constantMapper->SetPSConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT STDMETHODCALLTYPE D9Device::GetPixelShaderConstantI(
    UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{
    return m_constantMapper->GetPSConstantI(StartRegister, pConstantData, Vector4iCount);
}

HRESULT STDMETHODCALLTYPE D9Device::SetPixelShaderConstantB(
    UINT StartRegister, const BOOL* pConstantData, UINT BoolCount)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordPixelShaderConstantB(StartRegister, pConstantData, BoolCount);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    return m_constantMapper->SetPSConstantB(StartRegister, pConstantData, BoolCount);
}

HRESULT STDMETHODCALLTYPE D9Device::GetPixelShaderConstantB(
    UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{
    return m_constantMapper->GetPSConstantB(StartRegister, pConstantData, BoolCount);
}

}
