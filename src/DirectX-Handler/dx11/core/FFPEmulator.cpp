// Compiles, caches and binds the generated fixed-function shaders.
//
// Builds a permutation key from current render state, and compiles a vertex
// and pixel shader pair the first time each distinct key is seen. Compiling
// HLSL takes milliseconds, so a cache miss during a frame is visible; in
// practice a game reaches a small set of combinations and the cache settles
// quickly.
//
// Also owns the constant buffer these shaders read -- the transform matrices,
// the light array, material values and fog parameters -- and updates it only
// when the underlying D3D9 state has actually changed.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/FFPEmulator.h>
#include <core/FFPShaderGen.h>
#include <core/DeviceContext11.h>
#include <core/RenderStateTracker.h>
#include <core/ShaderMods.h>
#include <core/Log.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>
#include <cassert>
#include <string>
#include <type_traits>
#include <algorithm>

#pragma comment(lib, "d3dcompiler.lib")

namespace dx9to11 {

FFPEmulator::FFPEmulator(DeviceContext11* ctx) noexcept
    : m_ctx(ctx)
{

    auto identity = [](D3DMATRIX& m) {
        memset(&m, 0, sizeof(m));
        m._11 = m._22 = m._33 = m._44 = 1.0f;
    };
    for (auto& w : m_fixed.world) identity(w);
    identity(m_fixed.view); identity(m_fixed.proj);
    for (auto& t : m_fixed.texMatrix) identity(t);

    m_fixed.material.Diffuse  = { 1, 1, 1, 1 };
    m_fixed.material.Ambient  = { 0, 0, 0, 0 };
    m_fixed.material.Specular = { 0, 0, 0, 0 };
    m_fixed.material.Emissive = { 0, 0, 0, 0 };
    m_fixed.material.Power    = 0.0f;
}

FFPEmulator::~FFPEmulator() = default;

void FFPEmulator::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* pMatrix) noexcept
{
    if (!pMatrix) return;
    m_cbDirty = true;

    UINT s = static_cast<UINT>(state);
    if (s >= D3DTS_WORLDMATRIX(0) && s <= D3DTS_WORLDMATRIX(7)) {
        m_fixed.world[s - D3DTS_WORLDMATRIX(0)] = *pMatrix;
        return;
    }
    switch (s) {
    case D3DTS_VIEW:        m_fixed.view  = *pMatrix; break;
    case D3DTS_PROJECTION:  m_fixed.proj  = *pMatrix; break;
    default:
        if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7)
            m_fixed.texMatrix[state - D3DTS_TEXTURE0] = *pMatrix;
        break;
    }
}

void FFPEmulator::SetMaterial(const D3DMATERIAL9* pMat) noexcept
{
    if (!pMat) return;
    m_fixed.material = *pMat;
    m_cbDirty = true;
}

void FFPEmulator::SetLight(DWORD index, const D3DLIGHT9* pLight) noexcept
{
    if (index >= 8 || !pLight) return;
    m_fixed.lights[index] = *pLight;
    m_cbDirty = true;
}

void FFPEmulator::LightEnable(DWORD index, BOOL enable) noexcept
{
    if (index >= 8) return;
    m_fixed.lightEnabled[index] = (enable != FALSE);
    m_cbDirty = true;
}

void FFPEmulator::SetGlobalAmbient(D3DCOLOR ambient) noexcept
{

    m_cbData.GlobalAmbient[0] = ((ambient >> 16) & 0xFF) / 255.0f;
    m_cbData.GlobalAmbient[1] = ((ambient >>  8) & 0xFF) / 255.0f;
    m_cbData.GlobalAmbient[2] = ((ambient      ) & 0xFF) / 255.0f;
    m_cbData.GlobalAmbient[3] = ((ambient >> 24) & 0xFF) / 255.0f;
    m_cbDirty = true;
}

void FFPEmulator::SetViewportInfo(const D3DVIEWPORT9& vp) noexcept
{
    const float invW = vp.Width  ? 1.0f / static_cast<float>(vp.Width)  : 0.0f;
    const float invH = vp.Height ? 1.0f / static_cast<float>(vp.Height) : 0.0f;
    const float x    = static_cast<float>(vp.X);
    const float y    = static_cast<float>(vp.Y);

    if (m_cbData.ViewportInfo[0] != invW || m_cbData.ViewportInfo[1] != invH ||
        m_cbData.ViewportInfo[2] != x    || m_cbData.ViewportInfo[3] != y) {
        m_cbData.ViewportInfo[0] = invW;
        m_cbData.ViewportInfo[1] = invH;
        m_cbData.ViewportInfo[2] = x;
        m_cbData.ViewportInfo[3] = y;
        m_cbDirty = true;
    }
}

HRESULT FFPEmulator::CompilePermutation(const FFPPermKey& key, ShaderPair* pOut) noexcept
{
    std::string vsSource = GenerateFFPVS(key);
    std::string psSource = GenerateFFPPS(key);

    std::string vsMod, psMod, vsModName, psModName;
    if (shadermods::Active()) {
        const shadermods::Id vsId =
            shadermods::MakeFixedFunctionId(&key, sizeof(key), true);
        const shadermods::Id psId =
            shadermods::MakeFixedFunctionId(&key, sizeof(key), false);
        shadermods::Dump(vsId, vsSource);
        shadermods::Dump(psId, psSource);
        (void)shadermods::Load(vsId, &vsMod, &vsModName);
        (void)shadermods::Load(psId, &psMod, &psModName);
    }

    auto compile = [&](const std::string& src, const char* target,
                        ComPtr<ID3DBlob>& blob) -> HRESULT {
        ComPtr<ID3DBlob> errBlob;

        HRESULT hr = D3DCompile(
            src.c_str(), src.size(), nullptr, nullptr, nullptr,
            "main", target,
            D3DCOMPILE_OPTIMIZATION_LEVEL1 | D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 0,
            blob.GetAddressOf(), errBlob.GetAddressOf());
        if (FAILED(hr) && errBlob) {
            OutputDebugStringA("[dx9to11] FFP compile error: ");
            OutputDebugStringA(static_cast<const char*>(errBlob->GetBufferPointer()));
            OutputDebugStringA("\n");
        }
        return hr;
    };

    auto compileWithMod = [&](const std::string& src, const std::string& mod,
                              const std::string& modName, const char* target,
                              ComPtr<ID3DBlob>& blob) -> HRESULT {
        if (!mod.empty()) {
            if (SUCCEEDED(compile(mod, target, blob))) {
                DXLOG_INFO("[shadermods] using %s.hlsl", modName.c_str());
                return S_OK;
            }
            blob.Reset();
            shadermods::ReportCompileFailure(modName);
        }
        return compile(src, target, blob);
    };

    ComPtr<ID3DBlob> vsBlob, psBlob;
    HRESULT hr = compileWithMod(vsSource, vsMod, vsModName, "vs_4_0", vsBlob);
    if (FAILED(hr)) return hr;
    hr = compileWithMod(psSource, psMod, psModName, "ps_4_0", psBlob);
    if (FAILED(hr)) return hr;

    auto* dev = m_ctx->Device();
    hr = dev->CreateVertexShader(vsBlob->GetBufferPointer(),
                                  vsBlob->GetBufferSize(),
                                  nullptr, pOut->vs.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = dev->CreatePixelShader(psBlob->GetBufferPointer(),
                                 psBlob->GetBufferSize(),
                                 nullptr, pOut->ps.GetAddressOf());
    if (FAILED(hr)) return hr;

    {
        ComPtr<ID3D11ShaderReflection> refl11;
        if (SUCCEEDED(D3DReflect(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                  __uuidof(ID3D11ShaderReflection),
                                  reinterpret_cast<void**>(refl11.GetAddressOf())))) {
            D3D11_SHADER_DESC sd{};
            refl11->GetDesc(&sd);
            pOut->refl.dxbcBlob.assign(
                static_cast<const uint8_t*>(vsBlob->GetBufferPointer()),
                static_cast<const uint8_t*>(vsBlob->GetBufferPointer()) + vsBlob->GetBufferSize());

            for (UINT i = 0; i < sd.InputParameters; ++i) {
                D3D11_SIGNATURE_PARAMETER_DESC spd{};
                refl11->GetInputParameterDesc(i, &spd);
                pOut->refl.semanticNames.emplace_back(spd.SemanticName);

                D3D11_INPUT_ELEMENT_DESC elem{};
                elem.SemanticName         = pOut->refl.semanticNames.back().c_str();
                elem.SemanticIndex        = spd.SemanticIndex;
                elem.Format               = DXGI_FORMAT_R32G32B32A32_FLOAT;
                elem.InputSlot            = 0;
                elem.AlignedByteOffset    = D3D11_APPEND_ALIGNED_ELEMENT;
                elem.InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
                elem.InstanceDataStepRate = 0;
                pOut->refl.inputElements.push_back(elem);
            }

            pOut->refl.RepointSemanticNames();
        }
    }
    return S_OK;
}

HRESULT FFPEmulator::GetOrCompile(const FFPPermKey& key, const ShaderPair** ppOut) noexcept
{
    AcquireSRWLockShared(&m_cacheLock);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        *ppOut = &it->second;
        ReleaseSRWLockShared(&m_cacheLock);
        return S_OK;
    }
    ReleaseSRWLockShared(&m_cacheLock);

    ShaderPair pair;
    HRESULT hr = CompilePermutation(key, &pair);
    if (FAILED(hr)) return hr;

    AcquireSRWLockExclusive(&m_cacheLock);
    auto [insertIt, inserted] = m_cache.emplace(key, std::move(pair));
    *ppOut = &insertIt->second;
    ReleaseSRWLockExclusive(&m_cacheLock);
    return S_OK;
}

HRESULT FFPEmulator::EnsureCB() noexcept
{
    if (m_cb) return S_OK;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = (sizeof(FFPConstantData) + 15) & ~15u;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return m_ctx->Device()->CreateBuffer(&bd, nullptr, m_cb.GetAddressOf());
}

HRESULT FFPEmulator::FlushCB(const RenderStateTracker& rst) noexcept
{
    HRESULT hr = EnsureCB();
    if (FAILED(hr)) return hr;
    if (!m_cbDirty) return S_OK;

    const auto& rs = rst.State().rs;

    for (int w = 0; w < 4; ++w)
        memcpy(m_cbData.WorldMatrix[w], &m_fixed.world[w], 64);
    memcpy(m_cbData.ViewMatrix,       &m_fixed.view,  64);
    memcpy(m_cbData.ProjectionMatrix, &m_fixed.proj,  64);
    for (int t = 0; t < 8; ++t)
        memcpy(m_cbData.TextureMatrix[t], &m_fixed.texMatrix[t], 64);

    memcpy(m_cbData.MaterialDiffuse,  &m_fixed.material.Diffuse,  16);
    memcpy(m_cbData.MaterialAmbient,  &m_fixed.material.Ambient,  16);
    memcpy(m_cbData.MaterialSpecular, &m_fixed.material.Specular, 16);
    memcpy(m_cbData.MaterialEmissive, &m_fixed.material.Emissive, 16);
    m_cbData.MaterialPower = m_fixed.material.Power;

    {
        const D3DMATRIX& V = m_fixed.view;
        m_cbData.CameraWorldPos[0] = -(V._41 * V._11 + V._42 * V._12 + V._43 * V._13);
        m_cbData.CameraWorldPos[1] = -(V._41 * V._21 + V._42 * V._22 + V._43 * V._23);
        m_cbData.CameraWorldPos[2] = -(V._41 * V._31 + V._42 * V._32 + V._43 * V._33);
    }

    m_cbData.FogStart   = *reinterpret_cast<const float*>(&rs[D3DRS_FOGSTART]);
    m_cbData.FogEnd     = *reinterpret_cast<const float*>(&rs[D3DRS_FOGEND]);
    m_cbData.FogDensity = *reinterpret_cast<const float*>(&rs[D3DRS_FOGDENSITY]);
    m_cbData.AlphaRef   = rs[D3DRS_ALPHAREF] / 255.0f;

    {
        const DWORD fc = rs[D3DRS_FOGCOLOR];
        m_cbData.FogColor[0] = ((fc >> 16) & 0xFF) / 255.0f;
        m_cbData.FogColor[1] = ((fc >>  8) & 0xFF) / 255.0f;
        m_cbData.FogColor[2] = ( fc        & 0xFF) / 255.0f;
        m_cbData.FogColor[3] = ((fc >> 24) & 0xFF) / 255.0f;
    }

    {
        const DWORD tf = rs[D3DRS_TEXTUREFACTOR];
        m_cbData.TextureFactor[0] = ((tf >> 16) & 0xFF) / 255.0f;
        m_cbData.TextureFactor[1] = ((tf >>  8) & 0xFF) / 255.0f;
        m_cbData.TextureFactor[2] = ( tf        & 0xFF) / 255.0f;
        m_cbData.TextureFactor[3] = ((tf >> 24) & 0xFF) / 255.0f;
    }

    int lcount = 0;
    for (int li = 0; li < 8; ++li) {
        if (!m_fixed.lightEnabled[li]) continue;
        auto& L = m_cbData.Lights[lcount++];
        L.Type = m_fixed.lights[li].Type;
        memcpy(L.Diffuse,    &m_fixed.lights[li].Diffuse,    16);
        memcpy(L.Specular,   &m_fixed.lights[li].Specular,   16);
        memcpy(L.Ambient,    &m_fixed.lights[li].Ambient,    16);
        memcpy(L.Position,   &m_fixed.lights[li].Position,   12);  L.Position[3] = 0;
        memcpy(L.Direction,  &m_fixed.lights[li].Direction,  12);  L.Direction[3] = 0;
        L.Range        = m_fixed.lights[li].Range;
        L.Falloff      = m_fixed.lights[li].Falloff;
        L.Attenuation0 = m_fixed.lights[li].Attenuation0;
        L.Attenuation1 = m_fixed.lights[li].Attenuation1;
        L.Attenuation2 = m_fixed.lights[li].Attenuation2;
        L.Theta        = m_fixed.lights[li].Theta;
        L.Phi          = m_fixed.lights[li].Phi;
    }
    m_cbData.ActiveLightCount = lcount;

    auto* ctx = m_ctx->Context();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    hr = ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return hr;
    memcpy(mapped.pData, &m_cbData, sizeof(m_cbData));
    ctx->Unmap(m_cb.Get(), 0);
    m_cbDirty = false;

    return S_OK;
}

HRESULT FFPEmulator::BindForDraw(const RenderStateTracker& rst,
                                  DWORD fvf,
                                  const D3DVERTEXELEMENT9* pDecl) noexcept
{

    FFPPermKey key = BuildFFPKey(rst.State(), m_fixed, fvf, pDecl);

    const ShaderPair* pair = nullptr;
    HRESULT hr = GetOrCompile(key, &pair);
    if (FAILED(hr)) return hr;

    auto* ctx = m_ctx->Context();
    ctx->VSSetShader(pair->vs.Get(), nullptr, 0);
    ctx->PSSetShader(pair->ps.Get(), nullptr, 0);

    m_lastRefl = &pair->refl;

    return FlushCB(rst);
}

HRESULT FFPEmulator::BindVSMixed(const RenderStateTracker& rst,
                                 DWORD fvf,
                                 const D3DVERTEXELEMENT9* pDecl) noexcept
{
    FFPPermKey key = BuildFFPKey(rst.State(), m_fixed, fvf, pDecl);
    key.mixedFullTexOutputs = 1;

    const ShaderPair* pair = nullptr;
    HRESULT hr = GetOrCompile(key, &pair);
    if (FAILED(hr)) return hr;

    m_ctx->Context()->VSSetShader(pair->vs.Get(), nullptr, 0);
    m_lastRefl = &pair->refl;
    return FlushCB(rst);
}

HRESULT FFPEmulator::BindPSMixed(const RenderStateTracker& rst,
                                 uint32_t vsTexMask, bool vsColor0,
                                 bool vsColor1, bool vsFog) noexcept
{

    FFPPermKey key = BuildFFPKey(rst.State(), m_fixed, 0, nullptr);
    key.mixedPsLink = 1;
    key.linkTexMask = vsTexMask & 0xFFu;
    key.linkColor0  = vsColor0 ? 1 : 0;
    key.linkColor1  = vsColor1 ? 1 : 0;
    key.linkFog     = vsFog ? 1 : 0;

    const ShaderPair* pair = nullptr;
    HRESULT hr = GetOrCompile(key, &pair);
    if (FAILED(hr)) return hr;

    m_ctx->Context()->PSSetShader(pair->ps.Get(), nullptr, 0);
    return FlushCB(rst);
}

void FFPEmulator::ResetToDefaults() noexcept
{
    auto identity = [](D3DMATRIX& m) {
        memset(&m, 0, sizeof(m));
        m._11 = m._22 = m._33 = m._44 = 1.0f;
    };
    for (auto& w : m_fixed.world) identity(w);
    identity(m_fixed.view); identity(m_fixed.proj);
    for (auto& t : m_fixed.texMatrix) identity(t);

    m_fixed.material = D3DMATERIAL9{};
    m_fixed.material.Diffuse  = { 1, 1, 1, 1 };

    for (auto& l : m_fixed.lights)      l = D3DLIGHT9{};
    for (auto& e : m_fixed.lightEnabled) e = false;

    m_cbData = FFPConstantData{};
    m_cbDirty = true;
}

size_t FFPEmulator::PermutationCount() const noexcept
{
    AcquireSRWLockShared(&m_cacheLock);
    size_t n = m_cache.size();
    ReleaseSRWLockShared(&m_cacheLock);
    return n;
}

void FFPEmulator::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* pOut) const noexcept
{
    if (!pOut) return;

    UINT s = static_cast<UINT>(state);
    if (s >= D3DTS_WORLDMATRIX(0) && s <= D3DTS_WORLDMATRIX(7)) {
        *pOut = m_fixed.world[s - D3DTS_WORLDMATRIX(0)];
        return;
    }
    switch (s) {
    case D3DTS_VIEW:        *pOut = m_fixed.view;  break;
    case D3DTS_PROJECTION:  *pOut = m_fixed.proj;  break;
    default:
        if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7)
            *pOut = m_fixed.texMatrix[state - D3DTS_TEXTURE0];
        else {

            std::memset(pOut, 0, sizeof(D3DMATRIX));
            pOut->_11 = pOut->_22 = pOut->_33 = pOut->_44 = 1.f;
        }
        break;
    }
}

void FFPEmulator::GetLight(DWORD index, D3DLIGHT9* pOut) const noexcept
{
    if (!pOut || index >= 8) return;
    *pOut = m_fixed.lights[index];
}

}
