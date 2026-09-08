// Frame presentation, device reset, and window handling.
//
// Present ends the frame and hands the back buffer to the display. Beyond the
// swap itself this is where per-frame bookkeeping happens: statistics
// counters, the periodic performance log, and releasing anything that was kept
// alive only for the duration of the frame.
//
// Reset recreates the swap chain after a resolution or mode change. DXGI is
// stricter than D3D9 here: every outstanding reference to a back buffer must
// be released before the chain can be resized, so the surface proxy is torn
// down and rebuilt rather than kept across the reset.
//
// Fullscreen is served as a borderless window sized to the display rather than
// a true mode change. It alt-tabs instantly, avoids mode-switch stalls, and
// behaves predictably alongside modern window management.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Device.h>
#include <core/BackendSelect.h>
#include <cstdio>
#include <d3d9proxy/D9SwapChain.h>
#include <d3d9proxy/D9Surface.h>
#include <core/DeviceContext11.h>
#include <core/ResourceManager.h>
#include <core/Log.h>
#include <core/FormatConverter.h>
#include <cassert>
#include <cstring>

namespace dx9to11 {

void D9Device::PostPresentBookkeeping() noexcept
{
    m_bind.Invalidate();
    m_rst.MarkAllDirty();

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (m_lastPresentQpc != 0)
        m_perf.qpcFrame += now.QuadPart - m_lastPresentQpc;
    m_lastPresentQpc = now.QuadPart;

    m_perf.frames++;
    if (m_perf.frames >= 120) {
        if (BackendSelect::PerfLogEnabled()) {
            static const double qpcToMs = [] {
                LARGE_INTEGER f;
                QueryPerformanceFrequency(&f);
                return 1000.0 / static_cast<double>(f.QuadPart);
            }();
            char buf[384];
            const float  n  = static_cast<float>(m_perf.frames);
            const double dn = static_cast<double>(m_perf.frames);
            std::snprintf(buf, sizeof(buf),
                "[dx9to11] perf/frame: Draw* calls %.0f -> submitted %.0f | draws %.0f | desc %.0f state %.0f | srv %.0f samp %.0f | "
                "vb %.0f ib %.0f layout %.0f topo %.0f vp %.0f rt %.0f | "
                "frame %.2fms (present %.2f, wait %.2f)\n",
                m_perf.drawCalls / n, m_perf.drawsOk / n,
                m_perf.draws / n, m_perf.descBuilds / n, m_perf.stateBinds / n,
                m_perf.srvSets / n, m_perf.sampSets / n,
                m_perf.vbSets / n, m_perf.ibSets / n, m_perf.layoutSets / n,
                m_perf.topoSets / n, m_perf.vpSets / n, m_perf.rtSets / n,
                m_perf.qpcFrame     * qpcToMs / dn,
                m_perf.qpcInPresent * qpcToMs / dn,
                m_perf.qpcInWait    * qpcToMs / dn);
            OutputDebugStringA(buf);

            DXLOG_INFO("%s", buf);
        }
        m_perf.ResetFrameTotals();
    }
}

HRESULT STDMETHODCALLTYPE D9Device::Present(
    CONST RECT*    pSourceRect,
    CONST RECT*    pDestRect,
    HWND           hDestWindowOverride,
    CONST RGNDATA* pDirtyRegion)
{
    HRESULT hr;

    if (m_ctx->InLostState())
        return D3DERR_DEVICELOST;

    if (m_primarySwapChain) {
        hr = m_primarySwapChain->Present(pSourceRect, pDestRect,
                                         hDestWindowOverride, pDirtyRegion, 0);
    } else {

        UINT syncInterval = 1u;
        if (m_presentParams.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE)
            syncInterval = 0u;

        const UINT presentFlags = (syncInterval == 0u && m_ctx->AllowTearing())
                                ? DXGI_PRESENT_ALLOW_TEARING : 0u;
        m_ctx->AnnotMark(L"dx9to11 Present");

        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        hr = m_ctx->SwapChain()->Present(syncInterval, presentFlags);
        QueryPerformanceCounter(&t1);
        m_perf.qpcInPresent += t1.QuadPart - t0.QuadPart;

        m_ctx->NotePresentResult(hr);

        if (hr == DXGI_ERROR_DEVICE_REMOVED ||
            hr == DXGI_ERROR_DEVICE_HUNG    ||
            hr == DXGI_ERROR_DEVICE_RESET) {
            m_ctx->LogDeviceRemoved();
            hr = D3DERR_DEVICELOST;
        } else if (hr == DXGI_STATUS_OCCLUDED) {

            hr = m_ctx->WindowedRequested() ? D3D_OK : D3DERR_DEVICELOST;
        } else {
            if (SUCCEEDED(hr)) {
                m_ctx->WaitForFrameLatency();
                LARGE_INTEGER t2;
                QueryPerformanceCounter(&t2);
                m_perf.qpcInWait += t2.QuadPart - t1.QuadPart;
            }
            hr = SUCCEEDED(hr) ? D3D_OK : hr;
        }
    }

    if (BackendSelect::DebugLayerEnabled())
        log::DrainInfoQueue(m_ctx->Device());

    PostPresentBookkeeping();
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Device::PresentEx(
    CONST RECT*    pSourceRect,
    CONST RECT*    pDestRect,
    HWND           hDestWindowOverride,
    CONST RGNDATA* pDirtyRegion,
    DWORD           )
{
    return Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT STDMETHODCALLTYPE D9Device::TestCooperativeLevel()
{
    return m_ctx->TestCooperativeLevel();
}

HRESULT STDMETHODCALLTYPE D9Device::CheckDeviceState(HWND  )
{
    return m_ctx->TestCooperativeLevel();
}

HRESULT STDMETHODCALLTYPE D9Device::Reset(D3DPRESENT_PARAMETERS* pPP)
{
    if (!pPP) return D3DERR_INVALIDCALL;

    m_ctx->AnnotBegin(L"dx9to11 Reset");

    ReportDefaultPoolSurvivors();

    m_ctx->ApplyWindowMode(*pPP);

    m_ctx->Context()->ClearState();
    m_ctx->Context()->Flush();

    m_bind.Invalidate();
    m_rst.MarkAllDirty();

    for (auto& rt : m_renderTargets) {
        if (rt) { rt->Release(); rt = nullptr; }
    }
    if (m_depthStencil) { m_depthStencil->Release(); m_depthStencil = nullptr; }

    {
        auto& st = m_rst.State();
        for (auto& t : st.textures)  { if (t) { t->Release(); t = nullptr; } }
        for (auto& s : st.streams)   { if (s) { s->Release(); s = nullptr; } }
        if (st.indexBuffer) { st.indexBuffer->Release(); st.indexBuffer = nullptr; }
        st.vertexDecl   = nullptr;
        st.vertexShader = nullptr;
        st.pixelShader  = nullptr;
    }
    if (m_currentVS)   { m_currentVS->Release();   m_currentVS = nullptr; }
    if (m_currentPS)   { m_currentPS->Release();   m_currentPS = nullptr; }
    if (m_currentDecl) { m_currentDecl->Release(); m_currentDecl = nullptr; }
    m_currentFVF      = 0;
    m_inputLayoutDirty = true;

    m_rst.ResetToDefaults();
    m_constantMapper->ResetToDefaults();
    m_ffp->ResetToDefaults();
    std::memset(m_clipPlanes, 0, sizeof(m_clipPlanes));
    m_clipPlaneEnable = 0;
    m_clipStatus      = D3DCLIPSTATUS9{ 0, 0xFFFFFFFFu };
    m_nPatchMode      = 0.0f;

    m_emuDirty        = true;

    if (!m_primarySwapChain)
        m_primarySwapChain.reset(new (std::nothrow) D9SwapChain(this, *pPP));
    if (!m_primarySwapChain) {
        m_ctx->AnnotEnd();
        return E_OUTOFMEMORY;
    }

    HRESULT hr = m_primarySwapChain->ResizeBuffers(*pPP);
    if (FAILED(hr)) {

        DXLOG_INFO("[dx9to11] Reset: ResizeBuffers failed (0x%08lX) - "
                   "outstanding back-buffer/default-pool references? "
                   "Returning D3DERR_INVALIDCALL per the D3D9 contract.",
                   static_cast<unsigned long>(hr));
        m_ctx->AnnotEnd();
        return D3DERR_INVALIDCALL;
    }

    m_presentParams = *pPP;

    BindDefaultTargets(*pPP);

    m_ctx->ResetCompleted();

    m_ctx->AnnotEnd();
    return D3D_OK;
}

void D9Device::BindDefaultTargets(const D3DPRESENT_PARAMETERS& pp) noexcept
{
    const UINT w = pp.BackBufferWidth;
    const UINT h = pp.BackBufferHeight;

    if (m_resetDepthStencil) {
        m_resetDepthStencil->Release();
        m_resetDepthStencil = nullptr;
    }
    if (pp.EnableAutoDepthStencil && w > 0 && h > 0) {

        const UINT activeSamples =
            m_primarySwapChain ? m_primarySwapChain->ActiveSampleCount() : 1u;
        IDirect3DSurface9* pDS = nullptr;
        HRESULT hr = CreateDepthStencilSurface(w, h,
            pp.AutoDepthStencilFormat,
            activeSamples > 1 ? pp.MultiSampleType : D3DMULTISAMPLE_NONE,
            activeSamples > 1 ? pp.MultiSampleQuality : 0,
            FALSE, &pDS, nullptr);
        if (SUCCEEDED(hr) && pDS) {
            m_resetDepthStencil = static_cast<D9Surface*>(pDS);
            SetDepthStencilSurface(pDS);
        }
    }

    if (m_primarySwapChain) {
        IDirect3DSurface9* pBB = nullptr;
        m_primarySwapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pBB);
        if (pBB) {
            SetRenderTarget(0, pBB);
            pBB->Release();
        }
    }

    D3DVIEWPORT9 vp{};
    vp.X = 0; vp.Y = 0;
    vp.Width  = w;
    vp.Height = h;
    vp.MinZ   = 0.0f;
    vp.MaxZ   = 1.0f;
    SetViewport(&vp);
}

void D9Device::InitImplicitState() noexcept
{
    if (!m_primarySwapChain)
        m_primarySwapChain.reset(new (std::nothrow) D9SwapChain(this, m_presentParams));
    BindDefaultTargets(m_presentParams);
}

void D9Device::TrackDefaultPoolObject(IUnknown* obj, const char* kind) noexcept
{
    if (!obj) return;
    AcquireSRWLockExclusive(&m_defaultPoolLock);
    m_defaultPoolObjects[obj] = kind;
    ReleaseSRWLockExclusive(&m_defaultPoolLock);
}

void D9Device::UntrackDefaultPoolObject(IUnknown* obj) noexcept
{
    if (!obj) return;
    AcquireSRWLockExclusive(&m_defaultPoolLock);
    m_defaultPoolObjects.erase(obj);
    ReleaseSRWLockExclusive(&m_defaultPoolLock);
}

void D9Device::ReportDefaultPoolSurvivors() noexcept
{
    AcquireSRWLockExclusive(&m_defaultPoolLock);
    const size_t n = m_defaultPoolObjects.size();
    if (n) {
        DXLOG_WARN("Reset: %zu D3DPOOL_DEFAULT resource(s) still alive at Reset "
                   "- D3D9 requires them released first (leak / missing "
                   "OnLostDevice). Listing up to 16:", n);
        unsigned i = 0;
        for (const auto& kv : m_defaultPoolObjects) {
            if (i >= 16u) break;
            DXLOG_WARN("  survivor[%u] %s @ %p", i, kv.second ? kv.second : "?",
                       static_cast<void*>(kv.first));
            ++i;
        }
    }
    ReleaseSRWLockExclusive(&m_defaultPoolLock);
}

HRESULT STDMETHODCALLTYPE D9Device::ResetEx(
    D3DPRESENT_PARAMETERS*  pPP,
    D3DDISPLAYMODEEX*        )
{
    return Reset(pPP);
}

HRESULT STDMETHODCALLTYPE D9Device::CreateAdditionalSwapChain(
    D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DSwapChain9**  pSwapChain)
{

    (void)pPresentationParameters;
    if (pSwapChain) *pSwapChain = nullptr;
    static bool warned = false;
    if (!warned) {
        warned = true;
        OutputDebugStringA("[dx9to11] CreateAdditionalSwapChain requested - "
                           "not implemented (single swap chain design)\n");
    }
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** ppSC)
{
    if (!ppSC) return D3DERR_INVALIDCALL;
    *ppSC = nullptr;
    if (iSwapChain != 0) return D3DERR_INVALIDCALL;

    if (!m_primarySwapChain)
        m_primarySwapChain.reset(new (std::nothrow) D9SwapChain(this, m_presentParams));
    if (!m_primarySwapChain) return E_OUTOFMEMORY;

    *ppSC = m_primarySwapChain.get();
    (*ppSC)->AddRef();
    return D3D_OK;
}

UINT STDMETHODCALLTYPE D9Device::GetNumberOfSwapChains()
{
    return 1u;
}

HRESULT STDMETHODCALLTYPE D9Device::GetBackBuffer(
    UINT               iSwapChain,
    UINT               iBackBuffer,
    D3DBACKBUFFER_TYPE Type,
    IDirect3DSurface9** ppSurface)
{
    if (!ppSurface) return D3DERR_INVALIDCALL;
    *ppSurface = nullptr;
    if (iSwapChain != 0) return D3DERR_INVALIDCALL;

    if (!m_primarySwapChain)
        m_primarySwapChain.reset(new (std::nothrow) D9SwapChain(this, m_presentParams));
    if (!m_primarySwapChain) return E_OUTOFMEMORY;

    return m_primarySwapChain->GetBackBuffer(iBackBuffer, Type, ppSurface);
}

HRESULT STDMETHODCALLTYPE D9Device::GetRasterStatus(UINT  , D3DRASTER_STATUS* pStatus)
{
    if (!pStatus) return D3DERR_INVALIDCALL;
    pStatus->InVBlank = FALSE;
    pStatus->ScanLine = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetDisplayMode(UINT  , D3DDISPLAYMODE* pMode)
{
    if (!pMode) return D3DERR_INVALIDCALL;

    DeviceContext11* ctx = m_ctx.get();
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(ctx->SwapChain()->GetContainingOutput(output.GetAddressOf()))) {
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(output->GetDesc(&od))) {
            const LONG w = od.DesktopCoordinates.right  - od.DesktopCoordinates.left;
            const LONG h = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
            pMode->Width       = w > 0 ? static_cast<UINT>(w) : m_presentParams.BackBufferWidth;
            pMode->Height      = h > 0 ? static_cast<UINT>(h) : m_presentParams.BackBufferHeight;
            pMode->RefreshRate = 60u;
            pMode->Format      = m_presentParams.BackBufferFormat;
            return D3D_OK;
        }
    }

    pMode->Width       = m_presentParams.BackBufferWidth;
    pMode->Height      = m_presentParams.BackBufferHeight;
    pMode->RefreshRate = 60u;
    pMode->Format      = m_presentParams.BackBufferFormat;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetDisplayModeEx(
    UINT                iSwapChain,
    D3DDISPLAYMODEEX*   pMode,
    D3DDISPLAYROTATION* pRotation)
{
    if (!pMode) return D3DERR_INVALIDCALL;
    if (pRotation) *pRotation = D3DDISPLAYROTATION_IDENTITY;

    D3DDISPLAYMODE dm{};
    HRESULT hr = GetDisplayMode(iSwapChain, &dm);
    if (SUCCEEDED(hr)) {
        pMode->Size             = sizeof(D3DDISPLAYMODEEX);
        pMode->Width            = dm.Width;
        pMode->Height           = dm.Height;
        pMode->RefreshRate      = dm.RefreshRate;
        pMode->Format           = dm.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Device::GetFrontBufferData(UINT  , IDirect3DSurface9* pDest)
{
    if (!m_primarySwapChain)
        m_primarySwapChain.reset(new (std::nothrow) D9SwapChain(this, m_presentParams));
    if (!m_primarySwapChain) return E_OUTOFMEMORY;
    return m_primarySwapChain->GetFrontBufferData(pDest);
}

void STDMETHODCALLTYPE D9Device::SetGammaRamp(UINT  , DWORD  , CONST D3DGAMMARAMP* pRamp)
{
    if (!pRamp) return;
    ComPtr<IDXGIOutput> output;
    if (FAILED(m_ctx->SwapChain()->GetContainingOutput(output.GetAddressOf()))) return;

    DXGI_GAMMA_CONTROL_CAPABILITIES caps{};
    if (FAILED(output->GetGammaControlCapabilities(&caps)) || caps.NumGammaControlPoints == 0) return;

    DXGI_GAMMA_CONTROL gc{};
    gc.Scale.Red   = gc.Scale.Green  = gc.Scale.Blue   = 1.0f;
    gc.Offset.Red  = gc.Offset.Green = gc.Offset.Blue  = 0.0f;

    for (UINT i = 0; i < 1025; ++i) {
        float t  = i / 1024.0f;
        UINT  lo = static_cast<UINT>(t * 255.0f);
        UINT  hi = lo < 255 ? lo + 1 : 255;
        float f  = (t * 255.0f) - lo;
        gc.GammaCurve[i].Red   = (pRamp->red[lo]   + f * (pRamp->red[hi]   - pRamp->red[lo]))   / 65535.0f;
        gc.GammaCurve[i].Green = (pRamp->green[lo] + f * (pRamp->green[hi] - pRamp->green[lo])) / 65535.0f;
        gc.GammaCurve[i].Blue  = (pRamp->blue[lo]  + f * (pRamp->blue[hi]  - pRamp->blue[lo]))  / 65535.0f;
    }
    output->SetGammaControl(&gc);
}

void STDMETHODCALLTYPE D9Device::GetGammaRamp(UINT  , D3DGAMMARAMP* pRamp)
{
    if (!pRamp) return;
    std::memset(pRamp, 0, sizeof(*pRamp));

    ComPtr<IDXGIOutput> output;
    if (FAILED(m_ctx->SwapChain()->GetContainingOutput(output.GetAddressOf()))) {

        for (UINT i = 0; i < 256; ++i) {
            WORD v = static_cast<WORD>((i * 65535u) / 255u);
            pRamp->red[i] = pRamp->green[i] = pRamp->blue[i] = v;
        }
        return;
    }

    DXGI_GAMMA_CONTROL gc{};
    if (FAILED(output->GetGammaControl(&gc))) {
        for (UINT i = 0; i < 256; ++i) {
            WORD v = static_cast<WORD>((i * 65535u) / 255u);
            pRamp->red[i] = pRamp->green[i] = pRamp->blue[i] = v;
        }
        return;
    }

    for (UINT i = 0; i < 256; ++i) {
        UINT idx = (i * 1024u) / 255u;
        pRamp->red[i]   = static_cast<WORD>(gc.GammaCurve[idx].Red   * 65535.0f);
        pRamp->green[i] = static_cast<WORD>(gc.GammaCurve[idx].Green * 65535.0f);
        pRamp->blue[i]  = static_cast<WORD>(gc.GammaCurve[idx].Blue  * 65535.0f);
    }
}

HRESULT STDMETHODCALLTYPE D9Device::SetDialogBoxMode(BOOL  )
{

    return D3D_OK;
}

}
