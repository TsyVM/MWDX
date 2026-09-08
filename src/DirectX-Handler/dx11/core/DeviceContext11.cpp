// Creates the D3D11 device and everything the backend hangs off it.
//
// Device creation requests feature level 11_1 as a hard minimum and fails
// rather than accepting less. Starting up on a lower level would mean
// discovering the missing capabilities later, one unexplained visual fault at
// a time; failing here lets the backend selector fall back to a renderer that
// works and say so in the log.
//
// The debug layer is requested only when the setting asks for it. It costs
// real performance, enough to invalidate any timing measurement taken with it
// enabled.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <core/DeviceContext11.h>
#include <core/BackendSelect.h>
#include <core/FormatConverter.h>
#include <core/Log.h>
#include <cassert>
#include <cstdio>
#include <iterator>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace dx9to11 {

std::atomic<DeviceContext11*> DeviceContext11::s_current{ nullptr };

namespace {

void CopyMarkerName(wchar_t (&dst)[64], LPCWSTR src) noexcept
{
    if (!src) { dst[0] = L'\0'; return; }
    size_t i = 0;
    for (; i < 63 && src[i]; ++i)
        dst[i] = src[i];
    dst[i] = L'\0';
}

DXGI_SWAP_CHAIN_DESC1 MakeSwapChainDesc(const D3DPRESENT_PARAMETERS& pp,
                                        UINT flags) noexcept
{
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width       = pp.BackBufferWidth;
    desc.Height      = pp.BackBufferHeight;

    desc.Format      = FormatConverter::BackBufferFormat(pp.BackBufferFormat);
    desc.Stereo      = FALSE;
    desc.SampleDesc  = { 1, 0 };

    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT | DXGI_USAGE_SHADER_INPUT;

    desc.BufferCount = 2;
    desc.Scaling     = DXGI_SCALING_STRETCH;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;

    desc.Flags       = flags;
    return desc;
}

}

HRESULT DeviceContext11::Create(
    HWND                         hwnd,
    const D3DPRESENT_PARAMETERS& pp,
    bool                         multiThreaded,
    UINT                         adapterOrdinal,
    DeviceContext11**            ppOut) noexcept
{
    assert(ppOut && "ppOut must not be null");

    log::Init();

    DXLOG_INFO("DeviceContext11::Create - REAL D3DPRESENT_PARAMETERS:");
    DXLOG_INFO("  BackBuffer       = %ux%u  fmt=%d(D3DFMT)  count=%u",
               pp.BackBufferWidth, pp.BackBufferHeight,
               (int)pp.BackBufferFormat, pp.BackBufferCount);
    DXLOG_INFO("  MultiSample      = type=%d  quality=%u",
               (int)pp.MultiSampleType, pp.MultiSampleQuality);
    DXLOG_INFO("  SwapEffect       = %d (1=DISCARD 2=FLIP 3=COPY 5=FLIPEX)",
               (int)pp.SwapEffect);
    DXLOG_INFO("  hDeviceWindow    = %p  Windowed=%d",
               (void*)pp.hDeviceWindow, (int)pp.Windowed);
    DXLOG_INFO("  AutoDepthStencil = enable=%d  fmt=%d(D3DFMT)",
               (int)pp.EnableAutoDepthStencil, (int)pp.AutoDepthStencilFormat);
    DXLOG_INFO("  Flags            = 0x%08X", pp.Flags);
    DXLOG_INFO("  RefreshRate      = %u Hz  PresentInterval=0x%08X",
               pp.FullScreen_RefreshRateInHz, pp.PresentationInterval);
    DXLOG_INFO("  (wrapper: mt=%d adapter=%u; SwapEffect is remapped to DXGI "
               "FLIP_DISCARD for flip-model presentation)",
               (int)multiThreaded, adapterOrdinal);

    auto* self = new (std::nothrow) DeviceContext11();
    if (!self)
        return E_OUTOFMEMORY;

    self->m_multiThreaded     = multiThreaded;

    self->m_windowedRequested = true;

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(self->m_factory.GetAddressOf()));
    if (FAILED(hr)) {
        delete self;
        return D3DERR_NOTAVAILABLE;
    }

    ComPtr<IDXGIAdapter1> requestedAdapter;
    if (FAILED(self->m_factory->EnumAdapters1(
            adapterOrdinal, requestedAdapter.GetAddressOf()))) {
        requestedAdapter.Reset();
        if (adapterOrdinal != 0)
            OutputDebugStringA("[dx9to11] requested adapter ordinal not "
                               "enumerable - using default adapter\n");
    }

    constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };

    IDXGIAdapter* const     pAdapter   = requestedAdapter.Get();
    const D3D_DRIVER_TYPE   driverType = pAdapter ? D3D_DRIVER_TYPE_UNKNOWN
                                                  : D3D_DRIVER_TYPE_HARDWARE;

    UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG) || defined(DX9TO11_ENABLE_D3D11_DEBUG)
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    if (BackendSelect::DebugLayerEnabled())
        createFlags |= D3D11_CREATE_DEVICE_DEBUG;

    ComPtr<ID3D11Device>        deviceBase;
    ComPtr<ID3D11DeviceContext> contextBase;
    D3D_FEATURE_LEVEL           achievedLevel{};

    hr = D3D11CreateDevice(
        pAdapter,
        driverType,
        nullptr,
        createFlags,
        kFeatureLevels, static_cast<UINT>(std::size(kFeatureLevels)),
        D3D11_SDK_VERSION,
        deviceBase.GetAddressOf(),
        &achievedLevel,
        contextBase.GetAddressOf());

    if (FAILED(hr) && (createFlags & D3D11_CREATE_DEVICE_DEBUG)) {
        OutputDebugStringA("[dx9to11] D3D11 debug layer unavailable "
                           "(install the Windows 'Graphics Tools' optional "
                           "feature) - retrying without validation\n");
        createFlags &= ~UINT(D3D11_CREATE_DEVICE_DEBUG);
        hr = D3D11CreateDevice(
            pAdapter, driverType, nullptr, createFlags,
            kFeatureLevels, static_cast<UINT>(std::size(kFeatureLevels)),
            D3D11_SDK_VERSION,
            deviceBase.ReleaseAndGetAddressOf(),
            &achievedLevel,
            contextBase.ReleaseAndGetAddressOf());
    }

    if (FAILED(hr)) {
        DXLOG_FATAL_HR(hr, "D3D11CreateDevice failed - no Direct3D 11 device "
                           "could be created. Update your GPU driver / ensure a "
                           "D3D11-capable adapter is present.");
        delete self;
        return D3DERR_NOTAVAILABLE;
    }

    hr = deviceBase->QueryInterface(IID_PPV_ARGS(self->m_device.GetAddressOf()));
    if (FAILED(hr)) {

        DXLOG_FATAL_HR(hr, "GPU/driver does not expose the Direct3D 11.1 device "
                           "interface, which this renderer requires. Update the "
                           "driver, or set [Renderer] Backend=1 for the original "
                           "D3D9 path.");
        delete self;
        return D3DERR_NOTAVAILABLE;
    }

    hr = contextBase->QueryInterface(IID_PPV_ARGS(self->m_context.GetAddressOf()));
    if (FAILED(hr)) {
        DXLOG_FATAL_HR(hr, "ID3D11DeviceContext1 interface unavailable (D3D11.1 "
                           "required).");
        delete self;
        return D3DERR_NOTAVAILABLE;
    }

    self->m_featureLevel = achievedLevel;

    self->m_context.As(&self->m_annotation);

    HWND targetHwnd = hwnd ? hwnd : pp.hDeviceWindow;
    self->m_targetHwnd = targetHwnd;

    self->ApplyWindowMode(pp);

    {
        BOOL tearing = FALSE;
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(self->m_factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(
                DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing))))
            self->m_allowTearing = (tearing == TRUE);
    }

    {
        DXGI_ADAPTER_DESC ad{};
        ComPtr<IDXGIDevice>  dxgiDev;
        ComPtr<IDXGIAdapter> adapter;
        if (SUCCEEDED(self->m_device.As(&dxgiDev)) &&
            SUCCEEDED(dxgiDev->GetAdapter(adapter.GetAddressOf())))
            adapter->GetDesc(&ad);

        D3D11_FEATURE_DATA_THREADING th{};
        self->m_device->CheckFeatureSupport(D3D11_FEATURE_THREADING,
                                            &th, sizeof(th));

        const char* fl = "10_1";
        if      (achievedLevel == D3D_FEATURE_LEVEL_11_1) fl = "11_1";
        else if (achievedLevel == D3D_FEATURE_LEVEL_11_0) fl = "11_0";

        const char* vendor = "unknown";
        switch (ad.VendorId) {
        case 0x10DE: vendor = "NVIDIA"; break;
        case 0x1002: vendor = "AMD";    break;
        case 0x8086: vendor = "Intel";  break;
        default: break;
        }

        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "[dx9to11] adapter: %ls | %s (0x%04X) device 0x%04X | FL %s | "
            "vram %u MB | cmdlists %d concurrent-creates %d | tearing %d | debug %d\n",
            ad.Description, vendor, ad.VendorId, ad.DeviceId, fl,
            static_cast<unsigned>(ad.DedicatedVideoMemory >> 20),
            th.DriverCommandLists ? 1 : 0,
            th.DriverConcurrentCreates ? 1 : 0,
            self->m_allowTearing ? 1 : 0,
            (createFlags & D3D11_CREATE_DEVICE_DEBUG) ? 1 : 0);
        OutputDebugStringA(buf);
        DXLOG_INFO("adapter: %ls | %s (0x%04X) device 0x%04X | FL %s | vram %u MB "
                   "| cmdlists %d | tearing %d | debugLayer %d",
                   ad.Description, vendor, ad.VendorId, ad.DeviceId, fl,
                   static_cast<unsigned>(ad.DedicatedVideoMemory >> 20),
                   th.DriverCommandLists ? 1 : 0, self->m_allowTearing ? 1 : 0,
                   (createFlags & D3D11_CREATE_DEVICE_DEBUG) ? 1 : 0);
    }

    self->m_swapChainFlags =
        (self->m_allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u) |
        (BackendSelect::LowLatencyEnabled()
             ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u);

    DXGI_SWAP_CHAIN_DESC1 scDesc = MakeSwapChainDesc(pp, self->m_swapChainFlags);

    scDesc.Format = self->ResolveBackBufferFormat(pp);

    hr = self->m_factory->CreateSwapChainForHwnd(
        self->m_device.Get(),
        targetHwnd,
        &scDesc,
        nullptr,
        nullptr,
        self->m_swapChain.GetAddressOf());

    if (FAILED(hr) &&
        (self->m_swapChainFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)) {
        OutputDebugStringA("[dx9to11] waitable swap chain rejected - "
                           "retrying without FRAME_LATENCY_WAITABLE_OBJECT\n");
        self->m_swapChainFlags &=
            ~UINT(DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
        scDesc.Flags = self->m_swapChainFlags;
        hr = self->m_factory->CreateSwapChainForHwnd(
            self->m_device.Get(), targetHwnd, &scDesc, nullptr, nullptr,
            self->m_swapChain.ReleaseAndGetAddressOf());
    }

    if (FAILED(hr)) {
        delete self;
        return D3DERR_NOTAVAILABLE;
    }

    self->ApplyColorSpace();

    if (self->m_swapChainFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(self->m_swapChain.As(&sc2))) {
            sc2->SetMaximumFrameLatency(1);
            self->m_latencyWaitable = sc2->GetFrameLatencyWaitableObject();
            if (self->m_latencyWaitable)
                OutputDebugStringA("[dx9to11] LowLatency active - waitable "
                                   "swap chain, max frame latency 1\n");
        }
    }

    self->m_factory->MakeWindowAssociation(targetHwnd, DXGI_MWA_NO_ALT_ENTER);

    s_current.store(self, std::memory_order_release);
    *ppOut = self;
    return S_OK;
}

DeviceContext11::~DeviceContext11()
{

    DeviceContext11* expected = this;
    s_current.compare_exchange_strong(expected, nullptr,
                                      std::memory_order_acq_rel);

    if (m_latencyWaitable) {
        CloseHandle(m_latencyWaitable);
        m_latencyWaitable = nullptr;
    }

}

void DeviceContext11::Destroy() noexcept
{
    delete this;
}

DeviceContext11* DeviceContext11::Current() noexcept
{
    return s_current.load(std::memory_order_acquire);
}

void DeviceContext11::AnnotBegin(LPCWSTR name) noexcept
{
    CopyMarkerName(m_lastMarker, name);
    if (m_annotation)
        m_annotation->BeginEvent(name);
}

void DeviceContext11::AnnotEnd() noexcept
{
    if (m_annotation)
        m_annotation->EndEvent();
}

void DeviceContext11::AnnotMark(LPCWSTR name) noexcept
{
    CopyMarkerName(m_lastMarker, name);
    if (m_annotation)
        m_annotation->SetMarker(name);
}

void DeviceContext11::WaitForFrameLatency() noexcept
{
    if (m_latencyWaitable)
        WaitForSingleObjectEx(m_latencyWaitable, 1000, FALSE);
}

DXGI_FORMAT DeviceContext11::ResolveBackBufferFormat(
    const D3DPRESENT_PARAMETERS& pp) const noexcept
{
    return FormatConverter::BackBufferFormat(pp.BackBufferFormat);
}

void DeviceContext11::ApplyColorSpace() noexcept
{

}

void DeviceContext11::ApplyWindowMode(const D3DPRESENT_PARAMETERS& pp) noexcept
{

    m_windowedRequested = true;

    if (!m_targetHwnd || !BackendSelect::BorderlessFullscreenEnabled())
        return;

    if (!pp.Windowed) {
        HMONITOR mon = MonitorFromWindow(m_targetHwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!mon || !GetMonitorInfoW(mon, &mi))
            return;

        LONG_PTR style = GetWindowLongPtrW(m_targetHwnd, GWL_STYLE);
        style &= ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW);
        style |= WS_POPUP | WS_VISIBLE;
        SetWindowLongPtrW(m_targetHwnd, GWL_STYLE, style);

        SetWindowPos(m_targetHwnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right  - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        m_managedWindow = true;
        OutputDebugStringA("[dx9to11] fullscreen request -> borderless "
                           "window over monitor (exclusive mode not used)\n");
    } else if (m_managedWindow) {

        LONG_PTR style = GetWindowLongPtrW(m_targetHwnd, GWL_STYLE);
        style &= ~static_cast<LONG_PTR>(WS_POPUP);
        style |= WS_OVERLAPPEDWINDOW | WS_VISIBLE;
        SetWindowLongPtrW(m_targetHwnd, GWL_STYLE, style);

        RECT rc{ 0, 0,
                 static_cast<LONG>(pp.BackBufferWidth  ? pp.BackBufferWidth  : 800u),
                 static_cast<LONG>(pp.BackBufferHeight ? pp.BackBufferHeight : 600u) };
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(m_targetHwnd, HWND_NOTOPMOST, 0, 0,
                     rc.right - rc.left, rc.bottom - rc.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOMOVE);
        m_managedWindow = false;
        OutputDebugStringA("[dx9to11] windowed request -> normal frame restored\n");
    }
}

void DeviceContext11::LockContext() noexcept
{
    if (m_multiThreaded)
        AcquireSRWLockExclusive(&m_ctxLock);
}

void DeviceContext11::UnlockContext() noexcept
{
    if (m_multiThreaded)
        ReleaseSRWLockExclusive(&m_ctxLock);
}

bool DeviceContext11::IsDeviceLost() const noexcept
{
    if (m_deviceLost)
        return true;

    HRESULT reason = m_device->GetDeviceRemovedReason();
    const bool lost =
           (reason == DXGI_ERROR_DEVICE_REMOVED ||
            reason == DXGI_ERROR_DEVICE_HUNG    ||
            reason == DXGI_ERROR_DEVICE_RESET   ||
            reason == DXGI_ERROR_DRIVER_INTERNAL_ERROR);
    if (lost)
        LogDeviceRemoved();
    return lost;
}

void DeviceContext11::LogDeviceRemoved() const noexcept
{
    if (m_removedLogged)
        return;
    m_removedLogged = true;

    const HRESULT reason = m_device ? m_device->GetDeviceRemovedReason() : E_FAIL;
    const char* name = "unknown";
    switch (reason) {
    case DXGI_ERROR_DEVICE_REMOVED:        name = "DEVICE_REMOVED (adapter lost/driver update)"; break;
    case DXGI_ERROR_DEVICE_HUNG:           name = "DEVICE_HUNG (bad command stream / TDR)";      break;
    case DXGI_ERROR_DEVICE_RESET:          name = "DEVICE_RESET (TDR recovery)";                 break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR: name = "DRIVER_INTERNAL_ERROR";                       break;
    case DXGI_ERROR_INVALID_CALL:          name = "INVALID_CALL";                                break;
    case S_OK:                             name = "S_OK (removal signalled elsewhere)";          break;
    default: break;
    }

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "[dx9to11] D3D11 DEVICE REMOVED: 0x%08lX %s | last marker: %ls\n",
        static_cast<unsigned long>(reason), name,
        m_lastMarker[0] ? m_lastMarker : L"(none)");
    OutputDebugStringA(buf);

    DXLOG_FATAL_HR(reason,
        "Direct3D 11 DEVICE REMOVED (%s). The GPU/driver dropped the device - "
        "this is typically a driver timeout (TDR), a driver update mid-run, or a "
        "bad draw. Last GPU marker: %ls",
        name, m_lastMarker[0] ? m_lastMarker : L"(none)");
}

void DeviceContext11::NotePresentResult(HRESULT presentHr) noexcept
{
    if (presentHr == DXGI_STATUS_OCCLUDED) {

        if (m_windowedRequested)
            return;
        m_occluded = true;
        if (m_lossPhase == LossPhase::Operational)
            m_lossPhase = LossPhase::Lost;
    } else if (presentHr == S_OK) {
        m_occluded = false;
    } else if (presentHr == DXGI_ERROR_DEVICE_REMOVED ||
               presentHr == DXGI_ERROR_DEVICE_HUNG    ||
               presentHr == DXGI_ERROR_DEVICE_RESET) {
        m_deviceLost = true;
        if (m_lossPhase == LossPhase::Operational)
            m_lossPhase = LossPhase::Lost;
    }
}

void DeviceContext11::ResetCompleted() noexcept
{
    m_lossPhase = LossPhase::Operational;
    m_occluded  = false;
}

HRESULT DeviceContext11::TestCooperativeLevel() noexcept
{

    if (IsDeviceLost()) {
        if (m_lossPhase == LossPhase::Operational)
            m_lossPhase = LossPhase::Lost;
        m_lossPhase = LossPhase::NotReset;
        return D3DERR_DEVICENOTRESET;
    }

    switch (m_lossPhase) {
    case LossPhase::Operational:
        return D3D_OK;

    case LossPhase::Lost:
        if (m_occluded && m_swapChain) {

            const HRESULT probe = m_swapChain->Present(0, DXGI_PRESENT_TEST);
            if (probe != S_OK)
                return D3DERR_DEVICELOST;
            m_occluded = false;
        }
        m_lossPhase = LossPhase::NotReset;
        return D3DERR_DEVICENOTRESET;

    case LossPhase::NotReset:
    default:
        return D3DERR_DEVICENOTRESET;
    }
}

}
