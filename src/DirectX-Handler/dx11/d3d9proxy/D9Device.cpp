// Device construction, teardown, and the device-level queries.
//
// Creates the D3D11 device context and the caches that hang off it, sets up
// the swap chain from the presentation parameters the game supplied, and
// establishes the D3D9 default render state.
//
// Also implements the device-loss protocol. D3D9 devices could be lost on a
// display mode change or a task switch, and games contain code to detect that
// and recreate their resources. Modern APIs virtualise the GPU and almost
// never lose a device, so TestCooperativeLevel reports the device as fine.
// Claiming a loss that has not happened would send the game down a recovery
// path with nothing to recover.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Device.h>
#include <d3d9proxy/D9StateBlock.h>
#include <d3d9proxy/D9Surface.h>
#include <core/BackendSelect.h>
#include <core/RenderStateTracker.h>
#include <core/FFPEmulator.h>
#include <core/CapsTable.h>
#include <core/PSOCache.h>
#include <cassert>
#include <cstring>

namespace dx9to11 {

D9Device::D9Device(
    std::unique_ptr<DeviceContext11> ctx,
    IDirect3D9*                      pParent,
    DWORD                            behaviorFlags,
    const D3DPRESENT_PARAMETERS&     pp,
    bool                             isEx) noexcept
    : m_ctx(std::move(ctx))
    , m_parent(pParent)
    , m_behaviorFlags(behaviorFlags)
    , m_isEx(isEx)
    , m_presentParams(pp)
{

    SynthesiseCaps(0, &m_caps);

    if (m_parent) m_parent->AddRef();

    m_shaderCache      = std::make_unique<ShaderCache>(m_ctx->Device());
    m_constantMapper   = std::make_unique<ConstantMapper>(m_ctx.get());
    m_inputLayoutCache = std::make_unique<InputLayoutCache>(m_ctx.get());

    m_ffp      = std::make_unique<FFPEmulator>(m_ctx.get());
    m_psoCache = std::make_unique<PSOCache>(m_ctx.get());

    InitImplicitState();
}

D9Device::~D9Device()
{

    if (m_currentVS)   m_currentVS->Release();
    if (m_currentPS)   m_currentPS->Release();
    if (m_currentDecl) m_currentDecl->Release();

    if (m_resetDepthStencil) { m_resetDepthStencil->Release(); m_resetDepthStencil = nullptr; }

    if (m_emuCursor) { ApplyEmuCursor(false); DestroyCursor(m_emuCursor); m_emuCursor = nullptr; }
}

HRESULT STDMETHODCALLTYPE D9Device::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj)
        return E_POINTER;

    if (riid == __uuidof(IUnknown)           ||
        riid == __uuidof(IDirect3DDevice9)   ||
        (riid == __uuidof(IDirect3DDevice9Ex) && m_isEx))
    {

        *ppvObj = static_cast<IDirect3DDevice9Ex*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Device::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9Device::Release()
{
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        if (m_parent) m_parent->Release();
        delete this;
    }
    return prev - 1;
}

UINT STDMETHODCALLTYPE D9Device::GetAvailableTextureMem()
{

    if (m_availTexMem == 0) {
        unsigned long long bytes = 512ull * 1024u * 1024u;
        ComPtr<IDXGIDevice>  dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC    desc{};
        if (SUCCEEDED(m_ctx->Device()->QueryInterface(
                IID_PPV_ARGS(dxgiDev.GetAddressOf()))) &&
            SUCCEEDED(dxgiDev->GetAdapter(adapter.GetAddressOf())) &&
            SUCCEEDED(adapter->GetDesc(&desc)))
            bytes = desc.DedicatedVideoMemory;

        unsigned long long mb = bytes >> 20;
        if (mb < 256u)  mb = 256u;
        if (mb > 2047u) mb = 2047u;
        m_availTexMem = static_cast<UINT>(mb << 20);
    }
    return m_availTexMem;
}

HRESULT STDMETHODCALLTYPE D9Device::EvictManagedResources()
{

    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetDirect3D(IDirect3D9** ppD3D9)
{
    if (!ppD3D9)
        return D3DERR_INVALIDCALL;
    m_parent->AddRef();
    *ppD3D9 = m_parent;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetDeviceCaps(D3DCAPS9* pCaps)
{
    if (!pCaps)
        return D3DERR_INVALIDCALL;
    std::memcpy(pCaps, &m_caps, sizeof(D3DCAPS9));
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetCreationParameters(
    D3DDEVICE_CREATION_PARAMETERS* pParameters)
{
    if (!pParameters)
        return D3DERR_INVALIDCALL;
    std::memset(pParameters, 0, sizeof(*pParameters));
    pParameters->AdapterOrdinal = 0;
    pParameters->DeviceType     = D3DDEVTYPE_HAL;
    pParameters->BehaviorFlags  = m_behaviorFlags;

    pParameters->hFocusWindow   = m_ctx->TargetHwnd();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetCursorProperties(
    UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap)
{
    if (!pCursorBitmap) return D3DERR_INVALIDCALL;

    D3DSURFACE_DESC sd{};
    if (FAILED(pCursorBitmap->GetDesc(&sd)) || sd.Format != D3DFMT_A8R8G8B8)
        return D3DERR_INVALIDCALL;

    D3DLOCKED_RECT lr{};
    HRESULT hr = pCursorBitmap->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr))
        return D3DERR_INVALIDCALL;

    BITMAPV5HEADER bh{};
    bh.bV5Size        = sizeof(bh);
    bh.bV5Width       = static_cast<LONG>(sd.Width);
    bh.bV5Height      = -static_cast<LONG>(sd.Height);
    bh.bV5Planes      = 1;
    bh.bV5BitCount    = 32;
    bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask     = 0x00FF0000u;
    bh.bV5GreenMask   = 0x0000FF00u;
    bh.bV5BlueMask    = 0x000000FFu;
    bh.bV5AlphaMask   = 0xFF000000u;

    void* dibBits = nullptr;
    HBITMAP hColor = CreateDIBSection(nullptr,
        reinterpret_cast<const BITMAPINFO*>(&bh), DIB_RGB_COLORS,
        &dibBits, nullptr, 0);
    if (!hColor || !dibBits) {
        pCursorBitmap->UnlockRect();
        if (hColor) DeleteObject(hColor);
        return E_OUTOFMEMORY;
    }

    const auto* src = static_cast<const uint8_t*>(lr.pBits);
    auto*       dst = static_cast<uint8_t*>(dibBits);
    const size_t rowBytes = static_cast<size_t>(sd.Width) * 4u;
    for (UINT y = 0; y < sd.Height; ++y)
        std::memcpy(dst + rowBytes * y,
                    src + static_cast<size_t>(lr.Pitch) * y, rowBytes);
    pCursorBitmap->UnlockRect();

    HBITMAP hMask = CreateBitmap(static_cast<int>(sd.Width),
                                 static_cast<int>(sd.Height), 1, 1, nullptr);
    if (!hMask) {
        DeleteObject(hColor);
        return E_OUTOFMEMORY;
    }

    ICONINFO ii{};
    ii.fIcon    = FALSE;
    ii.xHotspot = XHotSpot;
    ii.yHotspot = YHotSpot;
    ii.hbmMask  = hMask;
    ii.hbmColor = hColor;
    HCURSOR hCur = CreateIconIndirect(&ii);
    DeleteObject(hMask);
    DeleteObject(hColor);
    if (!hCur)
        return D3DERR_INVALIDCALL;

    if (m_emuCursor)
        DestroyCursor(m_emuCursor);
    m_emuCursor = hCur;
    if (m_emuCursorVisible)
        ApplyEmuCursor(true);
    return D3D_OK;
}

void STDMETHODCALLTYPE D9Device::SetCursorPosition(int X, int Y, DWORD  )
{

    SetCursorPos(X, Y);
}

BOOL STDMETHODCALLTYPE D9Device::ShowCursor(BOOL bShow)
{
    const BOOL prev = m_emuCursorVisible ? TRUE : FALSE;
    m_emuCursorVisible = (bShow != FALSE);
    ApplyEmuCursor(m_emuCursorVisible);
    return prev;
}

void D9Device::ApplyEmuCursor(bool show) noexcept
{

    HWND hwnd = m_ctx->TargetHwnd();
    if (!hwnd) hwnd = m_presentParams.hDeviceWindow;

    HCURSOR cur = (show && m_emuCursor) ? m_emuCursor : nullptr;
    if (hwnd)
        SetClassLongPtrW(hwnd, GCLP_HCURSOR,
                         reinterpret_cast<LONG_PTR>(cur));
    ::SetCursor(cur);
}

HRESULT STDMETHODCALLTYPE D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
{
    if (!pMatrix) return D3DERR_INVALIDCALL;
    if (m_recordingBlock) {
        m_recordingBlock->RecordTransform(State, pMatrix);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    m_ffp->SetTransform(State, pMatrix);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetViewport(CONST D3DVIEWPORT9* pViewport)
{
    if (!pViewport) return D3DERR_INVALIDCALL;
    if (m_recordingBlock) {
        m_recordingBlock->RecordViewport(pViewport);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    if (m_viewport.Width != pViewport->Width || m_viewport.Height != pViewport->Height)
        m_emuDirty = true;
    m_viewport = *pViewport;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::GetViewport(D3DVIEWPORT9* pViewport)
{
    if (!pViewport) return D3DERR_INVALIDCALL;
    *pViewport = m_viewport;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::SetMaterial(CONST D3DMATERIAL9* pMaterial)
{
    if (!pMaterial) return D3DERR_INVALIDCALL;
    m_ffp->SetMaterial(pMaterial);
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::GetMaterial(D3DMATERIAL9* pMaterial)
{
    if (!pMaterial) return D3DERR_INVALIDCALL;
    *pMaterial = m_ffp->Material();
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::SetLight(DWORD Index, CONST D3DLIGHT9* pLight)
{
    if (!pLight) return D3DERR_INVALIDCALL;
    if (m_recordingBlock) {
        m_recordingBlock->RecordLight(Index, pLight);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    m_ffp->SetLight(Index, pLight);
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::LightEnable(DWORD Index, BOOL Enable)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordLightEnable(Index, Enable);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    m_ffp->LightEnable(Index, Enable);
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordRenderState(State, Value);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    m_rst.SetRenderState(State, Value);

    if (State == D3DRS_AMBIENT)
        m_ffp->SetGlobalAmbient(Value);
    else if (State == D3DRS_FOGSTART || State == D3DRS_FOGEND ||
             State == D3DRS_FOGDENSITY || State == D3DRS_ALPHAREF ||
             State == D3DRS_ALPHATESTENABLE || State == D3DRS_ALPHAFUNC ||
             State == D3DRS_FOGTABLEMODE || State == D3DRS_FOGVERTEXMODE ||
             State == D3DRS_TEXTUREFACTOR)
        m_ffp->MarkCBDirty();

    if (State == D3DRS_ALPHAREF   || State == D3DRS_FOGSTART ||
        State == D3DRS_FOGEND     || State == D3DRS_FOGDENSITY ||
        State == D3DRS_FOGCOLOR   || State == D3DRS_CLIPPLANEENABLE)
        m_emuDirty = true;

    if (State == D3DRS_SRGBWRITEENABLE && Value != 0 && !m_srgbWarned) {
        m_srgbWarned = true;
        OutputDebugStringA("[dx9to11] game enabled D3DRS_SRGBWRITEENABLE\n");
    }

    if (State == D3DRS_CLIPPLANEENABLE && Value != 0 && !m_clipPlaneWarned &&
        m_rst.State().vertexShader == nullptr) {
        m_clipPlaneWarned = true;
        OutputDebugStringA("[dx9to11] D3DRS_CLIPPLANEENABLE on the FIXED-"
                           "FUNCTION path - FFP clip planes are not applied "
                           "(translated-shader planes are)\n");
    }
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    if (static_cast<UINT>(State) < static_cast<UINT>(D3DRS_BLENDOPALPHA + 1))
        *pValue = m_rst.State().rs[State];
    else
        *pValue = 0;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
{
    if (!ppTexture) return D3DERR_INVALIDCALL;
    if (Stage < 8) {
        *ppTexture = m_rst.State().textures[Stage];
        if (*ppTexture) (*ppTexture)->AddRef();
    } else {
        *ppTexture = nullptr;
    }
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture)
{
    if (Stage >= 8) {

        if (pTexture) {
            static bool s_hiStageWarned = false;
            if (!s_hiStageWarned) {
                s_hiStageWarned = true;
                OutputDebugStringA("[dx9to11] SetTexture on stage >= 8 (or a "
                                   "vertex-texture sampler) - not tracked; "
                                   "texture dropped\n");
            }
        }
        return D3D_OK;
    }
    if (m_recordingBlock) {
        m_recordingBlock->RecordTexture(Stage, pTexture);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    if (pTexture) pTexture->AddRef();
    auto& slot = m_rst.State().textures[Stage];
    if (slot) slot->Release();
    slot = pTexture;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    if (Stage < 8 && static_cast<UINT>(Type) < static_cast<UINT>(D3DTSS_CONSTANT + 1))
        *pValue = m_rst.State().tss[Stage][Type];
    else
        *pValue = 0;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordTextureStageState(Stage, Type, Value);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    m_rst.SetTextureStageState(Stage, Type, Value);

    if (Type == D3DTSS_BUMPENVMAT00 || Type == D3DTSS_BUMPENVMAT01 ||
        Type == D3DTSS_BUMPENVMAT10 || Type == D3DTSS_BUMPENVMAT11 ||
        Type == D3DTSS_BUMPENVLSCALE || Type == D3DTSS_BUMPENVLOFFSET)
        m_emuDirty = true;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    if (Sampler < 8 && static_cast<UINT>(Type) < static_cast<UINT>(D3DSAMP_DMAPOFFSET + 1))
        *pValue = m_rst.State().samp[Sampler][Type];
    else
        *pValue = 0;
    return D3D_OK;
}
HRESULT STDMETHODCALLTYPE D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordSamplerState(Sampler, Type, Value);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    m_rst.SetSamplerState(Sampler, Type, Value);

    if (Type == D3DSAMP_SRGBTEXTURE && Value != 0 && !m_srgbWarned) {
        m_srgbWarned = true;
        OutputDebugStringA("[dx9to11] game enabled D3DSAMP_SRGBTEXTURE\n");
    }
    return D3D_OK;
}

namespace {
struct Dx9EmuCBData {
    float misc[4];
    float bumpMat[8][4];
    float bumpScaleOff[8][4];
    float fogParams[4];
    float fogColor[4];
    float clipPlanes[6][4];
};
static_assert(sizeof(Dx9EmuCBData) == 400, "Dx9EmuCBData must match the HLSL cbuffer");

inline float AsFloat(DWORD v) noexcept
{
    float f;
    std::memcpy(&f, &v, sizeof(f));
    return f;
}
}

HRESULT D9Device::FlushEmuCB() noexcept
{
    if (!m_emuCB) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(Dx9EmuCBData);
        bd.Usage          = D3D11_USAGE_DEFAULT;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        const HRESULT hr = m_ctx->Device()->CreateBuffer(&bd, nullptr, m_emuCB.GetAddressOf());
        if (FAILED(hr)) return hr;
    }
    if (!m_emuDirty) return S_OK;

    Dx9EmuCBData d{};
    d.misc[0] = m_viewport.Width  ? 1.0f / static_cast<float>(m_viewport.Width)  : 0.0f;
    d.misc[1] = m_viewport.Height ? 1.0f / static_cast<float>(m_viewport.Height) : 0.0f;
    d.misc[2] = static_cast<float>(m_rst.State().rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
    d.misc[3] = 0.0f;

    const auto& tss = m_rst.State().tss;
    for (int s = 0; s < 8; ++s) {
        d.bumpMat[s][0] = AsFloat(tss[s][D3DTSS_BUMPENVMAT00]);
        d.bumpMat[s][1] = AsFloat(tss[s][D3DTSS_BUMPENVMAT10]);
        d.bumpMat[s][2] = AsFloat(tss[s][D3DTSS_BUMPENVMAT01]);
        d.bumpMat[s][3] = AsFloat(tss[s][D3DTSS_BUMPENVMAT11]);
        d.bumpScaleOff[s][0] = AsFloat(tss[s][D3DTSS_BUMPENVLSCALE]);
        d.bumpScaleOff[s][1] = AsFloat(tss[s][D3DTSS_BUMPENVLOFFSET]);
    }

    {
        const auto& rs = m_rst.State().rs;
        d.fogParams[0] = AsFloat(rs[D3DRS_FOGSTART]);
        d.fogParams[1] = AsFloat(rs[D3DRS_FOGEND]);
        d.fogParams[2] = AsFloat(rs[D3DRS_FOGDENSITY]);
        d.fogParams[3] = 0.0f;
        const DWORD fc = rs[D3DRS_FOGCOLOR];
        d.fogColor[0] = ((fc >> 16) & 0xFF) / 255.0f;
        d.fogColor[1] = ((fc >>  8) & 0xFF) / 255.0f;
        d.fogColor[2] = ( fc        & 0xFF) / 255.0f;
        d.fogColor[3] = ((fc >> 24) & 0xFF) / 255.0f;

        const DWORD enable = rs[D3DRS_CLIPPLANEENABLE];
        for (int p = 0; p < 6; ++p) {
            if (enable & (1u << p))
                std::memcpy(d.clipPlanes[p], m_clipPlanes[p], sizeof(float) * 4);

        }
    }

    m_ctx->Context()->UpdateSubresource(m_emuCB.Get(), 0, nullptr, &d, 0, 0);
    m_emuDirty = false;
    return S_OK;
}

ID3D11Buffer* D9Device::ZeroVB() noexcept
{
    if (!m_zeroVB) {
        static const float kZeros[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(kZeros);
        bd.Usage     = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem = kZeros;
        (void)m_ctx->Device()->CreateBuffer(&bd, &srd, m_zeroVB.GetAddressOf());
    }
    return m_zeroVB.Get();
}

HRESULT STDMETHODCALLTYPE D9Device::SetConvolutionMonoKernel(UINT,UINT,float*,float*) { return E_NOTIMPL; }
HRESULT STDMETHODCALLTYPE D9Device::ComposeRects(IDirect3DSurface9*,IDirect3DSurface9*,IDirect3DVertexBuffer9*,UINT,IDirect3DVertexBuffer9*,D3DCOMPOSERECTSOP,int,int) { return E_NOTIMPL; }

HRESULT STDMETHODCALLTYPE D9Device::GetGPUThreadPriority(INT*p)                    { if(p)*p=0; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::SetGPUThreadPriority(INT)                      { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::WaitForVBlank(UINT)                            { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::CheckResourceResidency(IDirect3DResource9**,UINT32) { return S_OK; }
HRESULT STDMETHODCALLTYPE D9Device::SetMaximumFrameLatency(UINT)                   { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::GetMaximumFrameLatency(UINT*p)                 { if(p)*p=3; return D3D_OK; }

}
