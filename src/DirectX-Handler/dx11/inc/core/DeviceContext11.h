// Owns the D3D11 device and immediate context, and everything built on them.
//
// One of these exists per D3D9 device. It holds the objects the rest of the
// backend needs -- the shader cache, the state object cache, the input layout
// cache, the constant mapper, the upload ring -- so that they share a lifetime
// and a single point of creation.
//
// Feature level 11_1 is the minimum. Below that, device creation fails and the
// backend selector falls back to another renderer rather than starting up
// half-capable, which would surface later as unexplained missing features.
//
// The immediate context is not thread-safe and is not protected here. D3D9's
// own threading rules are similar, and the game issues its rendering from one
// thread; guarding every call would cost more than it is worth. Resource
// creation, which D3D11 does allow from multiple threads, is the exception and
// is safe.

#pragma once

#ifndef DX9TO11_DEVICE_CONTEXT_11_H
#define DX9TO11_DEVICE_CONTEXT_11_H

#include <d3d11_1.h>
#include <dxgi1_6.h>
#include <d3d9.h>
#include <wrl/client.h>
#include <synchapi.h>
#include <atomic>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

class DeviceContext11 {
public:

    static HRESULT Create(
        HWND                         hwnd,
        const D3DPRESENT_PARAMETERS& pp,
        bool                         multiThreaded,
        UINT                         adapterOrdinal,
        DeviceContext11**            ppOut) noexcept;

    ~DeviceContext11();

    void Destroy() noexcept;

    [[nodiscard]] ID3D11Device1*        Device()    const noexcept { return m_device.Get(); }
    [[nodiscard]] ID3D11DeviceContext1* Context()   const noexcept { return m_context.Get(); }
    [[nodiscard]] IDXGISwapChain1*      SwapChain() const noexcept { return m_swapChain.Get(); }
    [[nodiscard]] IDXGIFactory2*        Factory()   const noexcept { return m_factory.Get(); }
    [[nodiscard]] D3D_FEATURE_LEVEL     FeatureLevel() const noexcept { return m_featureLevel; }

    [[nodiscard]] bool                  AllowTearing() const noexcept { return m_allowTearing; }

    [[nodiscard]] UINT                  SwapChainFlags() const noexcept { return m_swapChainFlags; }

    [[nodiscard]] HWND                  TargetHwnd()     const noexcept { return m_targetHwnd; }

    [[nodiscard]] ID3DUserDefinedAnnotation* Annotation() const noexcept { return m_annotation.Get(); }

    void AnnotBegin(LPCWSTR name) noexcept;
    void AnnotEnd() noexcept;
    void AnnotMark(LPCWSTR name) noexcept;

    [[nodiscard]] static DeviceContext11* Current() noexcept;

    void WaitForFrameLatency() noexcept;

    [[nodiscard]] DXGI_FORMAT ResolveBackBufferFormat(
        const D3DPRESENT_PARAMETERS& pp) const noexcept;

    void ApplyColorSpace() noexcept;

    void ApplyWindowMode(const D3DPRESENT_PARAMETERS& pp) noexcept;

    void LockContext()   noexcept;

    void UnlockContext() noexcept;

    enum class LossPhase { Operational, Lost, NotReset };

    [[nodiscard]] bool IsDeviceLost() const noexcept;

    [[nodiscard]] HRESULT TestCooperativeLevel() noexcept;

    void NotePresentResult(HRESULT presentHr) noexcept;

    [[nodiscard]] bool InLostState() const noexcept
    { return m_lossPhase != LossPhase::Operational; }

    [[nodiscard]] bool WindowedRequested() const noexcept
    { return m_windowedRequested; }

    void ResetCompleted() noexcept;

    void LogDeviceRemoved() const noexcept;

private:
    DeviceContext11() = default;

    ComPtr<ID3D11Device1>            m_device;
    ComPtr<ID3D11DeviceContext1>     m_context;
    ComPtr<IDXGISwapChain1>          m_swapChain;
    ComPtr<IDXGIFactory2>            m_factory;
    ComPtr<ID3DUserDefinedAnnotation> m_annotation;

    D3D_FEATURE_LEVEL m_featureLevel = D3D_FEATURE_LEVEL_11_0;
    bool              m_multiThreaded = false;
    bool              m_windowedRequested = true;

    bool              m_deviceLost    = false;
    LossPhase         m_lossPhase     = LossPhase::Operational;
    bool              m_occluded      = false;
    bool              m_allowTearing  = false;
    UINT              m_swapChainFlags = 0;
    HANDLE            m_latencyWaitable = nullptr;
    HWND              m_targetHwnd     = nullptr;
    bool              m_managedWindow  = false;
    mutable bool      m_removedLogged  = false;

    wchar_t           m_lastMarker[64] = L"";

    SRWLOCK           m_ctxLock       = SRWLOCK_INIT;

    static std::atomic<DeviceContext11*> s_current;
};

}

#endif
