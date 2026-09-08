// The D3D11 implementation of IDirect3D9 -- the factory the game starts from.
//
// Direct3DCreate9 hands the game one of these. Its job is to answer questions
// about the adapter (how many, what modes, what formats, what capabilities)
// and then create the device.
//
// Answering those questions honestly is more important than it looks. They are
// query APIs, and a query API that under-reports fails silently: a game told
// that a format is unsupported does not error, it simply stops asking, and the
// feature is absent from the screen with no failure anywhere in the stack to
// find. The reported capabilities come from a shared table for this reason,
// and the format checks below are deliberately permissive.

#pragma once

#ifndef DX9TO11_D9ROOT_H
#define DX9TO11_D9ROOT_H

#include <d3d9.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>

using Microsoft::WRL::ComPtr;

struct ID3D11Device;

namespace dx9to11 {

class D9Root final : public IDirect3D9Ex {
public:

    explicit D9Root(UINT sdkVersion) noexcept;
    D9Root(const D9Root&)            = delete;
    D9Root& operator=(const D9Root&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* pInitializeFunction) override;
    UINT    STDMETHODCALLTYPE GetAdapterCount() override;
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags,
                                                   D3DADAPTER_IDENTIFIER9* pIdentifier) override;
    UINT    STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode,
                                               D3DDISPLAYMODE* pMode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter,
                                                    D3DDISPLAYMODE* pMode) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType,
                                              D3DFORMAT AdapterFormat,
                                              D3DFORMAT BackBufferFormat,
                                              BOOL bWindowed) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType,
                                                D3DFORMAT AdapterFormat, DWORD Usage,
                                                D3DRESOURCETYPE RType,
                                                D3DFORMAT CheckFormat) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType,
                                                         D3DFORMAT SurfaceFormat,
                                                         BOOL Windowed,
                                                         D3DMULTISAMPLE_TYPE MultiSampleType,
                                                         DWORD* pQualityLevels) override;
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType,
                                                     D3DFORMAT AdapterFormat,
                                                     D3DFORMAT RenderTargetFormat,
                                                     D3DFORMAT DepthStencilFormat) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType,
                                                          D3DFORMAT SourceFormat,
                                                          D3DFORMAT TargetFormat) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType,
                                            D3DCAPS9* pCaps) override;
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override;
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType,
                                           HWND hFocusWindow, DWORD BehaviorFlags,
                                           D3DPRESENT_PARAMETERS* pPresentationParameters,
                                           IDirect3DDevice9** ppReturnedDeviceInterface) override;

    UINT    STDMETHODCALLTYPE GetAdapterModeCountEx(UINT Adapter,
                                                    CONST D3DDISPLAYMODEFILTER* pFilter) override;
    HRESULT STDMETHODCALLTYPE EnumAdapterModesEx(UINT Adapter,
                                                 CONST D3DDISPLAYMODEFILTER* pFilter,
                                                 UINT Mode,
                                                 D3DDISPLAYMODEEX* pMode) override;
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayModeEx(UINT Adapter,
                                                      D3DDISPLAYMODEEX* pMode,
                                                      D3DDISPLAYROTATION* pRotation) override;
    HRESULT STDMETHODCALLTYPE CreateDeviceEx(UINT Adapter, D3DDEVTYPE DeviceType,
                                             HWND hFocusWindow, DWORD BehaviorFlags,
                                             D3DPRESENT_PARAMETERS* pPresentationParameters,
                                             D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                             IDirect3DDevice9Ex** ppReturnedDeviceInterface) override;
    HRESULT STDMETHODCALLTYPE GetAdapterLUID(UINT Adapter, LUID* pLUID) override;

private:
    ~D9Root();

    HRESULT CreateDeviceInternal(UINT Adapter, D3DDEVTYPE DeviceType,
                                 HWND hFocusWindow, DWORD BehaviorFlags,
                                 D3DPRESENT_PARAMETERS* pPP, bool isEx,
                                 IDirect3DDevice9** ppDevice) noexcept;

    HRESULT EnsureAdapters() noexcept;

    ID3D11Device* EnsureProbeDevice(UINT adapter) noexcept;

    std::atomic<ULONG> m_refCount{ 1 };
    UINT               m_sdkVersion;

    ComPtr<IDXGIFactory2>              m_factory;
    std::vector<ComPtr<IDXGIAdapter1>> m_adapters;
    ComPtr<ID3D11Device>               m_probeDevice;
    UINT                               m_probeAdapter{ 0xFFFFFFFFu };
};

}

#endif
