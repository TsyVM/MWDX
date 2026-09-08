// The DXGI swap chain, and presenting frames through it.
//
// Creates the chain from D3D9 presentation parameters, translating the ones
// that have modern equivalents and approximating the ones that do not.
// Presentation intervals become sync intervals; exclusive fullscreen becomes a
// borderless window; a multisampled back buffer, which the flip model does not
// allow, becomes a multisampled target that is resolved into the back buffer
// before each present.
//
// Resizing is stricter than under D3D9: DXGI requires every reference to a
// back buffer to be released first, which is why the render target view and
// the surface proxy are destroyed and rebuilt around a resize rather than
// updated in place.
//
// DXGI also reports the window as occluded in situations where windowed D3D9
// would not have. That is swallowed rather than reported as a lost device,
// since a game told its device is lost will tear down and rebuild everything.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>

#include <d3d9proxy/D9SwapChain.h>
#include <d3d9proxy/D9Device.h>
#include <d3d9proxy/D9Surface.h>
#include <core/FormatConverter.h>
#include <cassert>

namespace dx9to11 {

D9SwapChain::D9SwapChain(D9Device* pDevice, const D3DPRESENT_PARAMETERS& pp) noexcept
    : m_device(pDevice)
    , m_pp(pp)
{

    RebuildBackBufferRTV();
}

D9SwapChain::~D9SwapChain()
{
    ReleaseBackBufferProxy();
}

void D9SwapChain::ReleaseBackBufferProxy() noexcept
{
    if (!m_backBufferSurf)
        return;

    const ULONG remaining = m_backBufferSurf->Release();
    if (remaining != 0)
        OutputDebugStringA("[dx9to11] back-buffer proxy still referenced at "
                           "release point (game leak? Reset will fail per "
                           "D3D9 contract)\n");
    m_backBufferSurf = nullptr;
}

HRESULT D9SwapChain::RebuildBackBufferRTV() noexcept
{
    DeviceContext11* ctx = m_device->Ctx();
    IDXGISwapChain1* sc  = ctx->SwapChain();
    ID3D11Device1*   dev = ctx->Device();

    ComPtr<ID3D11Texture2D> backBuf;
    HRESULT hr = sc->GetBuffer(0, IID_PPV_ARGS(backBuf.GetAddressOf()));
    if (FAILED(hr))
        return hr;

    m_backBufferRTV.Reset();
    hr = dev->CreateRenderTargetView(backBuf.Get(), nullptr, m_backBufferRTV.GetAddressOf());
    if (FAILED(hr))
        return hr;

    m_msaaTex.Reset();
    m_msaaRTV.Reset();
    m_offscreenSamples = 1u;
    const UINT        samples = static_cast<UINT>(m_pp.MultiSampleType);
    const DXGI_FORMAT fmt     = ctx->ResolveBackBufferFormat(m_pp);
    D3D11_TEXTURE2D_DESC bd{};
    backBuf->GetDesc(&bd);
    if (samples > 1) {
        UINT levels = 0;
        if (SUCCEEDED(dev->CheckMultisampleQualityLevels(fmt, samples, &levels)) &&
            levels > 0) {
            D3D11_TEXTURE2D_DESC td = bd;
            td.Format         = fmt;
            td.SampleDesc     = { samples,
                                  (m_pp.MultiSampleQuality < levels)
                                      ? m_pp.MultiSampleQuality : levels - 1 };
            td.Usage          = D3D11_USAGE_DEFAULT;
            td.BindFlags      = D3D11_BIND_RENDER_TARGET;
            td.CPUAccessFlags = 0;
            td.MiscFlags      = 0;
            if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr,
                                               m_msaaTex.GetAddressOf()))) {
                if (FAILED(dev->CreateRenderTargetView(
                        m_msaaTex.Get(), nullptr, m_msaaRTV.GetAddressOf())))
                    m_msaaTex.Reset();
                else
                    m_offscreenSamples = samples;
            }
        }
        if (!m_msaaTex) {
            static bool s_msaaUnsupportedWarned = false;
            if (!s_msaaUnsupportedWarned) {
                s_msaaUnsupportedWarned = true;
                OutputDebugStringA("[dx9to11] requested MSAA count unsupported "
                                   "for the back-buffer format - rendering "
                                   "without MSAA\n");
            }
        } else {
            OutputDebugStringA("[dx9to11] MSAA back buffer active: offscreen "
                               "multisample target, resolved at Present\n");
        }
    }

    if (!m_msaaTex) {
        D3D11_TEXTURE2D_DESC td = bd;
        td.Format         = fmt;
        td.SampleDesc     = { 1, 0 };
        td.Usage          = D3D11_USAGE_DEFAULT;
        td.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = 0;
        td.MiscFlags      = 0;
        if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, m_msaaTex.GetAddressOf()))) {
            if (FAILED(dev->CreateRenderTargetView(
                    m_msaaTex.Get(), nullptr, m_msaaRTV.GetAddressOf())))
                m_msaaTex.Reset();
            else {
                static bool s_persistWarned = false;
                if (!s_persistWarned) {
                    s_persistWarned = true;
                    OutputDebugStringA("[dx9to11] persistent 1x back buffer active "
                                       "(flip-model persistence fix)\n");
                }
            }
        }
    }
    return S_OK;
}

void D9SwapChain::ResolveMsaaToBackBuffer() noexcept
{
    if (!m_msaaTex)
        return;
    DeviceContext11* ctx = m_device->Ctx();
    ComPtr<ID3D11Texture2D> backBuf;
    if (FAILED(ctx->SwapChain()->GetBuffer(0, IID_PPV_ARGS(backBuf.GetAddressOf()))))
        return;

    D3D11_TEXTURE2D_DESC td{};
    m_msaaTex->GetDesc(&td);
    if (td.SampleDesc.Count > 1) {
        ctx->Context()->ResolveSubresource(
            backBuf.Get(), 0, m_msaaTex.Get(), 0,
            ctx->ResolveBackBufferFormat(m_pp));
    } else {
        ctx->Context()->CopyResource(backBuf.Get(), m_msaaTex.Get());
    }
}

HRESULT D9SwapChain::ResizeBuffers(const D3DPRESENT_PARAMETERS& pp) noexcept
{

    ReleaseBackBufferProxy();
    m_backBufferRTV.Reset();
    m_msaaTex.Reset();
    m_msaaRTV.Reset();

    IDXGISwapChain1* sc = m_device->Ctx()->SwapChain();

    const UINT flags = m_device->Ctx()->SwapChainFlags();

    HRESULT hr = sc->ResizeBuffers(
        2u,
        pp.BackBufferWidth,
        pp.BackBufferHeight,
        m_device->Ctx()->ResolveBackBufferFormat(pp),
        flags);
    if (FAILED(hr))
        return hr;

    m_pp = pp;
    hr = RebuildBackBufferRTV();

    m_device->Ctx()->ApplyColorSpace();
    return hr;
}

HRESULT STDMETHODCALLTYPE D9SwapChain::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDirect3DSwapChain9)) {
        *ppvObj = static_cast<IDirect3DSwapChain9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9SwapChain::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9SwapChain::Release()
{
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) delete this;
    return prev - 1;
}

HRESULT STDMETHODCALLTYPE D9SwapChain::Present(
    CONST RECT*    pSourceRect,
    CONST RECT*    pDestRect,
    HWND            ,
    CONST RGNDATA*  ,
    DWORD          dwFlags)
{
    DeviceContext11* ctx = m_device->Ctx();

    if ((pSourceRect || pDestRect)) {
        static bool s_rectsWarned = false;
        if (!s_rectsWarned) {
            s_rectsWarned = true;
            OutputDebugStringA("[dx9to11] Present called with non-NULL "
                               "source/dest rects - ignored (flip model)\n");
        }
    }

    UINT syncInterval = 1u;
    if (m_pp.PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE ||
        (dwFlags & D3DPRESENT_FORCEIMMEDIATE))
        syncInterval = 0u;

    const UINT presentFlags = (syncInterval == 0u && ctx->AllowTearing())
                            ? DXGI_PRESENT_ALLOW_TEARING : 0u;

    ctx->AnnotMark(L"dx9to11 Present");

    ResolveMsaaToBackBuffer();

    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);

    HRESULT hr = ctx->SwapChain()->Present(syncInterval, presentFlags);

    QueryPerformanceCounter(&t1);
    m_device->Perf().qpcInPresent += t1.QuadPart - t0.QuadPart;

    ctx->NotePresentResult(hr);

    if (hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_HUNG    ||
        hr == DXGI_ERROR_DEVICE_RESET) {
        ctx->LogDeviceRemoved();
        return D3DERR_DEVICELOST;
    }

    if (hr == DXGI_STATUS_OCCLUDED)
        return ctx->WindowedRequested() ? D3D_OK : D3DERR_DEVICELOST;

    if (SUCCEEDED(hr)) {
        ctx->WaitForFrameLatency();
        LARGE_INTEGER t2;
        QueryPerformanceCounter(&t2);
        m_device->Perf().qpcInWait += t2.QuadPart - t1.QuadPart;
    }

    return SUCCEEDED(hr) ? D3D_OK : hr;
}

HRESULT STDMETHODCALLTYPE D9SwapChain::GetBackBuffer(
    UINT                iBackBuffer,
    D3DBACKBUFFER_TYPE   ,
    IDirect3DSurface9** ppBackBuffer)
{
    if (!ppBackBuffer) return D3DERR_INVALIDCALL;
    *ppBackBuffer = nullptr;

    if (iBackBuffer != 0) {
        const UINT declared = (m_pp.BackBufferCount > 0) ? m_pp.BackBufferCount : 1u;
        if (iBackBuffer >= declared)
            return D3DERR_INVALIDCALL;
        static bool warned = false;
        if (!warned) {
            warned = true;
            OutputDebugStringA("[dx9to11] GetBackBuffer(i>0) clamped to buffer 0 "
                               "(flip model exposes one writable buffer)\n");
        }
        iBackBuffer = 0;
    }
    (void)iBackBuffer;

    if (m_backBufferSurf) {
        m_backBufferSurf->AddRef();
        *ppBackBuffer = m_backBufferSurf;
        return D3D_OK;
    }

    DeviceContext11* ctx = m_device->Ctx();
    ComPtr<ID3D11Texture2D> backBuf;
    HRESULT hr = ctx->SwapChain()->GetBuffer(0, IID_PPV_ARGS(backBuf.GetAddressOf()));
    if (FAILED(hr)) return hr;

    const bool haveOffscreen = (m_msaaTex != nullptr);
    const bool multisampled  = (m_offscreenSamples > 1u);
    D9Surface::Desc desc{};
    desc.width       = m_pp.BackBufferWidth;
    desc.height      = m_pp.BackBufferHeight;
    desc.format      = m_pp.BackBufferFormat;
    desc.pool        = D3DPOOL_DEFAULT;
    desc.usage       = D3DUSAGE_RENDERTARGET;
    desc.multiSample = multisampled ? m_pp.MultiSampleType : D3DMULTISAMPLE_NONE;
    desc.subresource = 0;
    desc.lockable    = FALSE;

    ComPtr<ID3D11Texture2D>        tex = haveOffscreen ? m_msaaTex : backBuf;
    ComPtr<ID3D11RenderTargetView> rtv = haveOffscreen ? m_msaaRTV : m_backBufferRTV;

    auto* surf = new (std::nothrow) D9Surface(
        m_device, desc,
        tex,
        nullptr,
        rtv,
        nullptr,
        nullptr,
        nullptr);

    if (!surf) return E_OUTOFMEMORY;

    m_backBufferSurf = surf;
    surf->AddRef();
    *ppBackBuffer = surf;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9SwapChain::GetFrontBufferData(IDirect3DSurface9* pDestSurface)
{
    if (!pDestSurface) return D3DERR_INVALIDCALL;

    auto* dest = static_cast<D9Surface*>(pDestSurface);

    DeviceContext11* ctx = m_device->Ctx();

    ResolveMsaaToBackBuffer();
    ComPtr<ID3D11Texture2D> backBuf;
    HRESULT hr = ctx->SwapChain()->GetBuffer(0, IID_PPV_ARGS(backBuf.GetAddressOf()));
    if (FAILED(hr)) return hr;

    ID3D11Texture2D* stagingDst = dest->Staging();
    if (!stagingDst) return D3DERR_INVALIDCALL;

    ctx->Context()->CopySubresourceRegion(
        stagingDst, 0, 0, 0, 0,
        backBuf.Get(), 0, nullptr);

    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9SwapChain::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus)
{
    if (!pRasterStatus) return D3DERR_INVALIDCALL;
    pRasterStatus->InVBlank   = FALSE;
    pRasterStatus->ScanLine   = 0;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9SwapChain::GetDisplayMode(D3DDISPLAYMODE* pMode)
{
    if (!pMode) return D3DERR_INVALIDCALL;

    DeviceContext11* ctx = m_device->Ctx();
    ComPtr<IDXGIOutput> output;
    if (SUCCEEDED(ctx->SwapChain()->GetContainingOutput(output.GetAddressOf()))) {
        DXGI_OUTPUT_DESC odesc{};
        if (SUCCEEDED(output->GetDesc(&odesc))) {
            const LONG w = odesc.DesktopCoordinates.right  - odesc.DesktopCoordinates.left;
            const LONG h = odesc.DesktopCoordinates.bottom - odesc.DesktopCoordinates.top;
            pMode->Width       = w > 0 ? static_cast<UINT>(w) : m_pp.BackBufferWidth;
            pMode->Height      = h > 0 ? static_cast<UINT>(h) : m_pp.BackBufferHeight;
            pMode->RefreshRate = 60u;
            pMode->Format      = m_pp.BackBufferFormat;
            return D3D_OK;
        }
    }

    pMode->Width       = m_pp.BackBufferWidth;
    pMode->Height      = m_pp.BackBufferHeight;
    pMode->RefreshRate = 60u;
    pMode->Format      = m_pp.BackBufferFormat;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9SwapChain::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = static_cast<IDirect3DDevice9*>(m_device);
    m_device->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9SwapChain::GetPresentParameters(D3DPRESENT_PARAMETERS* pPP)
{
    if (!pPP) return D3DERR_INVALIDCALL;
    *pPP = m_pp;
    return D3D_OK;
}

}
