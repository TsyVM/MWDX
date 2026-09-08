// IDirect3DSwapChain9 -- the back buffers and the act of presenting them.
//
// Wraps a DXGI swap chain. Most of the complexity here is in the differences
// between how the two APIs handle the display:
//
//   Fullscreen. D3D9's exclusive fullscreen takes over the display mode. The
//   modern equivalent is a borderless window sized to the screen, which alt-tabs
//   instantly and avoids mode-switch stalls. That is what is used by default.
//
//   Presentation model. DXGI's flip model has no true equivalent of D3D9's
//   presentation intervals, so those are mapped onto sync intervals.
//
//   Multisampling. D3D9 could present a multisampled back buffer directly; the
//   flip model cannot, so a multisampled target has to be resolved into the
//   back buffer before presenting.
//
// Buffer count and resize handling are also stricter here: DXGI requires every
// outstanding reference to a back buffer to be released before the chain can
// be resized, which is why the surface proxy is torn down and rebuilt around a
// reset rather than kept.

#pragma once

#ifndef DX9TO11_D9SWAPCHAIN_H
#define DX9TO11_D9SWAPCHAIN_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <core/DeviceContext11.h>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class D9Device;
class D9Surface;

class D9SwapChain final : public IDirect3DSwapChain9 {
public:

    D9SwapChain(
        D9Device*                    pDevice,
        const D3DPRESENT_PARAMETERS& pp) noexcept;

    D9SwapChain(const D9SwapChain&)            = delete;
    D9SwapChain& operator=(const D9SwapChain&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE Present(
        CONST RECT* pSourceRect,
        CONST RECT* pDestRect,
        HWND        hDestWindowOverride,
        CONST RGNDATA* pDirtyRegion,
        DWORD          dwFlags) override;

    HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9* pDestSurface) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                                            IDirect3DSurface9** ppBackBuffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* pMode) override;
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS* pPresentationParameters) override;

    HRESULT ResizeBuffers(const D3DPRESENT_PARAMETERS& pp) noexcept;

    [[nodiscard]] ID3D11RenderTargetView* BackBufferRTV() const noexcept { return m_backBufferRTV.Get(); }
    [[nodiscard]] const D3DPRESENT_PARAMETERS& PresentParams() const noexcept { return m_pp; }

    [[nodiscard]] UINT ActiveSampleCount() const noexcept
    { return m_offscreenSamples; }

private:
    ~D9SwapChain();

    HRESULT RebuildBackBufferRTV() noexcept;

    void ReleaseBackBufferProxy() noexcept;

    void ResolveMsaaToBackBuffer() noexcept;

    D9Device*                      m_device;
    D3DPRESENT_PARAMETERS          m_pp;
    ComPtr<ID3D11RenderTargetView> m_backBufferRTV;

    ComPtr<ID3D11Texture2D>        m_msaaTex;
    ComPtr<ID3D11RenderTargetView> m_msaaRTV;

    UINT                           m_offscreenSamples = 1u;

    class D9Surface*               m_backBufferSurf = nullptr;
    std::atomic<ULONG>             m_refCount{ 1 };
};

}

#endif
