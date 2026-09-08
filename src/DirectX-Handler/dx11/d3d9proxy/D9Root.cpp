// Adapter enumeration, format and capability queries, device creation.
//
// The adapter and mode lists come from DXGI. Format queries are answered by
// asking D3D11 what the hardware supports rather than from a fixed table, so
// the answers reflect the machine in front of the user.
//
// These are all query APIs, and the failure mode of a query API is silence: a
// game told a format is unavailable simply stops asking and does without the
// feature. Answering conservatively is therefore not the safe option it
// appears to be.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Root.h>
#include <d3d9proxy/D9Device.h>
#include <core/DeviceContext11.h>
#include <core/CapsProbe.h>
#include <core/FormatConverter.h>

#include <core/CapsTable.h>

#include <algorithm>
#include <cstring>
#include <float.h>

namespace dx9to11 {

D9Root::D9Root(UINT sdkVersion) noexcept
    : m_sdkVersion(sdkVersion)
{}

D9Root::~D9Root() = default;

ID3D11Device* D9Root::EnsureProbeDevice(UINT adapter) noexcept
{
    if (FAILED(EnsureAdapters()) || adapter >= m_adapters.size())
        return nullptr;
    if (m_probeDevice && m_probeAdapter == adapter)
        return m_probeDevice.Get();

    m_probeDevice.Reset();
    m_probeAdapter = 0xFFFFFFFFu;

    constexpr D3D_FEATURE_LEVEL kFL[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };
    ComPtr<ID3D11Device> dev;
    const HRESULT hr = D3D11CreateDevice(
        m_adapters[adapter].Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kFL, static_cast<UINT>(std::size(kFL)),
        D3D11_SDK_VERSION,
        dev.GetAddressOf(), nullptr, nullptr);
    if (FAILED(hr))
        return nullptr;

    m_probeDevice  = dev;
    m_probeAdapter = adapter;
    return m_probeDevice.Get();
}

HRESULT STDMETHODCALLTYPE D9Root::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj)
        return E_POINTER;

    if (riid == __uuidof(IUnknown)      ||
        riid == __uuidof(IDirect3D9)    ||
        riid == __uuidof(IDirect3D9Ex))
    {
        *ppvObj = static_cast<IDirect3D9Ex*>(this);
        AddRef();
        return S_OK;
    }

    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9Root::AddRef()
{
    return m_refCount.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE D9Root::Release()
{
    ULONG prev = m_refCount.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1)
        delete this;
    return prev - 1;
}

HRESULT D9Root::EnsureAdapters() noexcept
{
    if (m_factory)
        return S_OK;

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(m_factory.GetAddressOf()));
    if (FAILED(hr))
        return hr;

    for (UINT i = 0; ; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        hr = m_factory->EnumAdapters1(i, adapter.GetAddressOf());
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        if (FAILED(hr))
            return hr;
        m_adapters.push_back(std::move(adapter));
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9Root::RegisterSoftwareDevice(void*  )
{

    return D3DERR_INVALIDCALL;
}

UINT STDMETHODCALLTYPE D9Root::GetAdapterCount()
{
    if (FAILED(EnsureAdapters()))
        return 0;
    return static_cast<UINT>(m_adapters.size());
}

HRESULT STDMETHODCALLTYPE D9Root::GetAdapterIdentifier(
    UINT Adapter, DWORD  , D3DADAPTER_IDENTIFIER9* pIdentifier)
{
    if (!pIdentifier)
        return D3DERR_INVALIDCALL;
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;

    DXGI_ADAPTER_DESC1 desc{};
    HRESULT hr = m_adapters[Adapter]->GetDesc1(&desc);
    if (FAILED(hr))
        return D3DERR_INVALIDCALL;

    std::memset(pIdentifier, 0, sizeof(*pIdentifier));

    WideCharToMultiByte(CP_ACP, 0, desc.Description, -1,
                        pIdentifier->Driver, MAX_DEVICE_IDENTIFIER_STRING, nullptr, nullptr);
    strncpy_s(pIdentifier->Description, MAX_DEVICE_IDENTIFIER_STRING,
              pIdentifier->Driver, _TRUNCATE);

    pIdentifier->VendorId     = desc.VendorId;
    pIdentifier->DeviceId     = desc.DeviceId;
    pIdentifier->SubSysId     = desc.SubSysId;
    pIdentifier->Revision     = desc.Revision;

    LUID luid{};
    m_adapters[Adapter]->GetDesc1(&desc);
    luid = desc.AdapterLuid;
    std::memcpy(&pIdentifier->DeviceIdentifier, &luid, std::min(sizeof(luid), sizeof(GUID)));

    pIdentifier->WHQLLevel = 1;

    return D3D_OK;
}

UINT STDMETHODCALLTYPE D9Root::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
{
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return 0;

    if (Format != D3DFMT_A8R8G8B8 && Format != D3DFMT_X8R8G8B8 &&
        Format != D3DFMT_R5G6B5   && Format != D3DFMT_A2R10G10B10)
        return 0;

    ComPtr<IDXGIOutput> output;
    if (FAILED(m_adapters[Adapter]->EnumOutputs(0, output.GetAddressOf())))
        return 0;

    DXGI_FORMAT dxgiFormat;
    switch (Format) {
        case D3DFMT_R5G6B5:      dxgiFormat = DXGI_FORMAT_B5G6R5_UNORM;      break;
        case D3DFMT_A2R10G10B10: dxgiFormat = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        default:                 dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;    break;
    }

    UINT count = 0;
    output->GetDisplayModeList(dxgiFormat, 0, &count, nullptr);
    return count;
}

HRESULT STDMETHODCALLTYPE D9Root::EnumAdapterModes(
    UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
{
    if (!pMode || FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;

    ComPtr<IDXGIOutput> output;
    if (FAILED(m_adapters[Adapter]->EnumOutputs(0, output.GetAddressOf())))
        return D3DERR_INVALIDCALL;

    DXGI_FORMAT dxgiFormat;
    switch (Format) {
        case D3DFMT_R5G6B5:      dxgiFormat = DXGI_FORMAT_B5G6R5_UNORM;      break;
        case D3DFMT_A2R10G10B10: dxgiFormat = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        default:                 dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;    break;
    }

    UINT count = 0;
    output->GetDisplayModeList(dxgiFormat, 0, &count, nullptr);
    if (Mode >= count)
        return D3DERR_INVALIDCALL;

    std::vector<DXGI_MODE_DESC> modes(count);
    output->GetDisplayModeList(dxgiFormat, 0, &count, modes.data());

    pMode->Width       = modes[Mode].Width;
    pMode->Height      = modes[Mode].Height;
    pMode->RefreshRate = modes[Mode].RefreshRate.Numerator /
                         std::max(1u, modes[Mode].RefreshRate.Denominator);
    pMode->Format      = Format;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Root::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
{
    if (!pMode || FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;

    ComPtr<IDXGIOutput> output;
    if (FAILED(m_adapters[Adapter]->EnumOutputs(0, output.GetAddressOf())))
        return D3DERR_INVALIDCALL;

    DXGI_OUTPUT_DESC outputDesc{};
    output->GetDesc(&outputDesc);

    pMode->Width       = outputDesc.DesktopCoordinates.right  - outputDesc.DesktopCoordinates.left;
    pMode->Height      = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
    pMode->RefreshRate = 60;
    pMode->Format      = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Root::CheckDeviceType(
    UINT  , D3DDEVTYPE DevType,
    D3DFORMAT  , D3DFORMAT  , BOOL  )
{

    return (DevType == D3DDEVTYPE_HAL) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE D9Root::CheckDeviceFormat(
    UINT Adapter, D3DDEVTYPE DevType,
    D3DFORMAT  , DWORD Usage,
    D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
{
    if (DevType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;

    if (FormatConverter::IsFourCC(CheckFormat)) {

        if (FormatConverter::IsNullSurfaceFormat(CheckFormat))
            return (Usage & D3DUSAGE_RENDERTARGET) ? D3D_OK : D3DERR_NOTAVAILABLE;

        FormatConverter::DepthFormatViews dfv{};
        if (FormatConverter::GetDepthFormatViews(CheckFormat, dfv)) {
            ID3D11Device* probe = EnsureProbeDevice(Adapter);
            if (!probe)
                return D3DERR_NOTAVAILABLE;
            UINT dsSup = 0, srSup = 0;
            if (FAILED(probe->CheckFormatSupport(dfv.dsv, &dsSup)) ||
                FAILED(probe->CheckFormatSupport(dfv.srv, &srSup)))
                return D3DERR_NOTAVAILABLE;
            const bool ok = (dsSup & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) &&
                            (srSup & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE);
            return ok ? D3D_OK : D3DERR_NOTAVAILABLE;
        }

    }

    if (Usage & (D3DUSAGE_QUERY_SRGBREAD | D3DUSAGE_QUERY_SRGBWRITE)) {
        const FormatMapping mapping = FormatConverter::ToDxgi(CheckFormat);
        if (!mapping.IsValid() || !FormatConverter::HasSRGBVariant(mapping.dxgiFormat))
            return D3DERR_NOTAVAILABLE;
    }

    if (ID3D11Device* probe = EnsureProbeDevice(Adapter))
        return CapsProbe::FormatSupportsUsage(probe, CheckFormat, Usage, RType)
             ? D3D_OK : D3DERR_NOTAVAILABLE;

    switch (CheckFormat) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R5G6B5:
    case D3DFMT_A1R5G5B5: case D3DFMT_A8: case D3DFMT_L8:
    case D3DFMT_V8U8: case D3DFMT_DXT1: case D3DFMT_DXT3: case D3DFMT_DXT5:
    case D3DFMT_D24S8: case D3DFMT_D32F_LOCKABLE: case D3DFMT_R32F:
    case D3DFMT_A16B16G16R16F: case D3DFMT_A32B32G32R32F:
    case D3DFMT_INDEX16: case D3DFMT_INDEX32:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE:
    case D3DFMT_A2B10G10R10: case D3DFMT_G16R16F: case D3DFMT_R16F:
    case D3DFMT_G32R32F: case D3DFMT_A8B8G8R8:
    case D3DFMT_A2R10G10B10:
        return D3D_OK;
    default:
        return D3DERR_NOTAVAILABLE;
    }
}

HRESULT STDMETHODCALLTYPE D9Root::CheckDeviceMultiSampleType(
    UINT Adapter, D3DDEVTYPE DevType,
    D3DFORMAT SurfaceFormat, BOOL  ,
    D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
{
    if (DevType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;

    if (MultiSampleType == D3DMULTISAMPLE_NONE) {
        if (pQualityLevels) *pQualityLevels = 1;
        return D3D_OK;
    }

    const UINT sampleCount = static_cast<UINT>(MultiSampleType);
    if (sampleCount < 2 || sampleCount > 16)
        return D3DERR_NOTAVAILABLE;

    if (ID3D11Device* probe = EnsureProbeDevice(Adapter)) {
        const UINT q = CapsProbe::MultisampleQualityLevels(probe, SurfaceFormat, sampleCount);
        if (q == 0)
            return D3DERR_NOTAVAILABLE;

        if (pQualityLevels) *pQualityLevels = q;
        return D3D_OK;
    }

    if (sampleCount == 2 || sampleCount == 4 || sampleCount == 8) {
        if (pQualityLevels) *pQualityLevels = 1;
        return D3D_OK;
    }
    return D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE D9Root::CheckDepthStencilMatch(
    UINT Adapter, D3DDEVTYPE DevType,
    D3DFORMAT  , D3DFORMAT  , D3DFORMAT DepthStencilFormat)
{
    if (DevType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;

    if (ID3D11Device* probe = EnsureProbeDevice(Adapter))
        return CapsProbe::DepthStencilSupported(probe, DepthStencilFormat)
             ? D3D_OK : D3DERR_NOTAVAILABLE;

    switch (DepthStencilFormat) {
    case D3DFMT_D24S8: case D3DFMT_D32F_LOCKABLE: case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE: case D3DFMT_D24X8:
        return D3D_OK;
    default:
        return D3DERR_NOTAVAILABLE;
    }
}

HRESULT STDMETHODCALLTYPE D9Root::CheckDeviceFormatConversion(
    UINT  , D3DDEVTYPE DevType,
    D3DFORMAT  , D3DFORMAT  )
{
    return (DevType == D3DDEVTYPE_HAL) ? D3D_OK : D3DERR_NOTAVAILABLE;
}

HRESULT STDMETHODCALLTYPE D9Root::GetDeviceCaps(
    UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
{
    if (!pCaps)
        return D3DERR_INVALIDCALL;
    if (DeviceType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;

    constexpr D3D_FEATURE_LEVEL kFL[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    ComPtr<ID3D11Device>        tempDevBase;
    ComPtr<ID3D11DeviceContext> tempCtxBase;
    D3D_FEATURE_LEVEL           fl{};

    HRESULT hr = D3D11CreateDevice(
        m_adapters[Adapter].Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        kFL, static_cast<UINT>(std::size(kFL)),
        D3D11_SDK_VERSION,
        tempDevBase.GetAddressOf(), &fl, tempCtxBase.GetAddressOf());

    if (FAILED(hr))
        return D3DERR_NOTAVAILABLE;

    ComPtr<ID3D11Device1> tempDev1;
    hr = tempDevBase->QueryInterface(IID_PPV_ARGS(tempDev1.GetAddressOf()));
    if (FAILED(hr))
        return D3DERR_NOTAVAILABLE;

    SynthesiseCaps(Adapter, pCaps);
    return D3D_OK;
}

HMONITOR STDMETHODCALLTYPE D9Root::GetAdapterMonitor(UINT Adapter)
{
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return nullptr;

    ComPtr<IDXGIOutput> output;
    if (FAILED(m_adapters[Adapter]->EnumOutputs(0, output.GetAddressOf())))
        return nullptr;

    DXGI_OUTPUT_DESC desc{};
    output->GetDesc(&desc);
    return desc.Monitor;
}

HRESULT D9Root::CreateDeviceInternal(
    UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPP, bool isEx,
    IDirect3DDevice9**     ppDevice) noexcept
{
    if (!ppDevice || !pPP)
        return D3DERR_INVALIDCALL;
    if (DeviceType != D3DDEVTYPE_HAL)
        return D3DERR_NOTAVAILABLE;
    if (FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;

    bool mt = (BehaviorFlags & D3DCREATE_MULTITHREADED) != 0;
    HWND hwnd = hFocusWindow ? hFocusWindow : pPP->hDeviceWindow;

    if (pPP->Windowed && (pPP->BackBufferWidth == 0 || pPP->BackBufferHeight == 0)) {
        RECT rc{};
        if (hwnd && GetClientRect(hwnd, &rc)) {
            if (pPP->BackBufferWidth  == 0)
                pPP->BackBufferWidth  = static_cast<UINT>(rc.right - rc.left);
            if (pPP->BackBufferHeight == 0)
                pPP->BackBufferHeight = static_cast<UINT>(rc.bottom - rc.top);
        }
        if (pPP->BackBufferWidth  == 0) pPP->BackBufferWidth  = 1;
        if (pPP->BackBufferHeight == 0) pPP->BackBufferHeight = 1;
    }
    if (pPP->BackBufferCount == 0)
        pPP->BackBufferCount = 1;
    if (pPP->BackBufferFormat == D3DFMT_UNKNOWN && pPP->Windowed)
        pPP->BackBufferFormat = D3DFMT_X8R8G8B8;

#if defined(_M_IX86)
    if (!(BehaviorFlags & D3DCREATE_FPU_PRESERVE)) {
        unsigned int prev = 0;
        _controlfp_s(&prev, _PC_24, _MCW_PC);
        _controlfp_s(&prev, _RC_NEAR, _MCW_RC);
    }
#endif

    DeviceContext11* ctx{};
    HRESULT hr = DeviceContext11::Create(hwnd, *pPP, mt, Adapter, &ctx);
    if (FAILED(hr))
        return hr;

    auto* dev = new (std::nothrow) D9Device(
        std::unique_ptr<DeviceContext11>(ctx),
        this,
        BehaviorFlags,
        *pPP,
        isEx);

    if (!dev) {
        ctx->Destroy();
        return E_OUTOFMEMORY;
    }

    *ppDevice = dev;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Root::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pPP,
    IDirect3DDevice9**     ppDevice)
{
    return CreateDeviceInternal(Adapter, DeviceType, hFocusWindow,
                                BehaviorFlags, pPP,   false, ppDevice);
}

HRESULT STDMETHODCALLTYPE D9Root::CreateDeviceEx(
    UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS*  pPP,
    D3DDISPLAYMODEEX*        ,
    IDirect3DDevice9Ex**    ppDevice)
{
    if (!ppDevice || !pPP)
        return D3DERR_INVALIDCALL;

    IDirect3DDevice9* pBase{};
    HRESULT hr = CreateDeviceInternal(Adapter, DeviceType, hFocusWindow,
                                      BehaviorFlags, pPP,   true, &pBase);
    if (FAILED(hr))
        return hr;

    hr = pBase->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                               reinterpret_cast<void**>(ppDevice));
    pBase->Release();
    return hr;
}

UINT STDMETHODCALLTYPE D9Root::GetAdapterModeCountEx(UINT Adapter, CONST D3DDISPLAYMODEFILTER*)
{
    return GetAdapterModeCount(Adapter, D3DFMT_X8R8G8B8);
}

HRESULT STDMETHODCALLTYPE D9Root::EnumAdapterModesEx(
    UINT Adapter, CONST D3DDISPLAYMODEFILTER*, UINT Mode, D3DDISPLAYMODEEX* pMode)
{
    if (!pMode) return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE basic{};
    HRESULT hr = EnumAdapterModes(Adapter, D3DFMT_X8R8G8B8, Mode, &basic);
    if (SUCCEEDED(hr)) {
        pMode->Size         = sizeof(D3DDISPLAYMODEEX);
        pMode->Width        = basic.Width;
        pMode->Height       = basic.Height;
        pMode->RefreshRate  = basic.RefreshRate;
        pMode->Format       = basic.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Root::GetAdapterDisplayModeEx(
    UINT Adapter, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation)
{
    if (!pMode) return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE basic{};
    HRESULT hr = GetAdapterDisplayMode(Adapter, &basic);
    if (SUCCEEDED(hr)) {
        pMode->Size         = sizeof(D3DDISPLAYMODEEX);
        pMode->Width        = basic.Width;
        pMode->Height       = basic.Height;
        pMode->RefreshRate  = basic.RefreshRate;
        pMode->Format       = basic.Format;
        pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
    }
    if (pRotation) *pRotation = D3DDISPLAYROTATION_IDENTITY;
    return hr;
}

HRESULT STDMETHODCALLTYPE D9Root::GetAdapterLUID(UINT Adapter, LUID* pLUID)
{
    if (!pLUID || FAILED(EnsureAdapters()) || Adapter >= m_adapters.size())
        return D3DERR_INVALIDCALL;

    DXGI_ADAPTER_DESC1 desc{};
    HRESULT hr = m_adapters[Adapter]->GetDesc1(&desc);
    if (SUCCEEDED(hr))
        *pLUID = desc.AdapterLuid;
    return SUCCEEDED(hr) ? D3D_OK : D3DERR_INVALIDCALL;
}

}
