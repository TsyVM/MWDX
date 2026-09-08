// Creation of D3D11 resources from D3D9 descriptions.
//
// Sits between the resource-creation entry points and the device, and holds
// the rules for translating one API's idea of a resource into the other's:
//
//   Pools. D3D9's D3DPOOL says where a resource lives and who may touch it.
//   MANAGED means the runtime keeps a system-memory copy and re-uploads after
//   a device loss; DEFAULT is GPU-only; SYSTEMMEM is CPU-only. D3D11 has no
//   pools, so these become usage flags plus, where the game may lock the
//   resource, a staging companion.
//
//   Usage flags. D3DUSAGE_RENDERTARGET, _DEPTHSTENCIL and _DYNAMIC map onto
//   bind and CPU-access flags. A resource that is both a render target and a
//   shader input needs both bind flags requested up front, since D3D11 will
//   not add one later.
//
//   Formats. Some D3D9 formats have no DXGI equivalent and must be expanded on
//   the CPU during upload.
//
// Failing to create a resource is worth logging loudly. A game that cannot
// create a texture usually carries on and draws without it, so the visible
// result is a missing or untextured object rather than an error.

#pragma once

#ifndef DX9TO11_RESOURCE_MANAGER_H
#define DX9TO11_RESOURCE_MANAGER_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <core/DeviceContext11.h>
#include <core/DynamicRingBuffer.h>
#include <memory>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

struct PoolPolicy {
    D3D11_USAGE usage          = D3D11_USAGE_DEFAULT;
    UINT        cpuAccessFlags = 0;
    bool        needsShadowCopy = false;
    bool        needsStaging    = false;
    bool        gpuVisible      = true;
};

class ResourceManager {
public:

    explicit ResourceManager(DeviceContext11* ctx) noexcept;

    [[nodiscard]] static PoolPolicy MapPool(D3DPOOL pool, DWORD usage) noexcept;

    HRESULT CreateTexture2D(
        UINT                       width,
        UINT                       height,
        UINT                       levels,
        DWORD                      usage,
        D3DFORMAT                  format,
        D3DPOOL                    pool,
        ID3D11Texture2D**          ppTexture,
        ID3D11ShaderResourceView** ppSRV,
        ID3D11Texture2D**          ppStaging,
        ID3D11ShaderResourceView** ppSRVSrgb = nullptr) noexcept;

    HRESULT CreateTexture3D(
        UINT                       width,
        UINT                       height,
        UINT                       depth,
        UINT                       levels,
        DWORD                      usage,
        D3DFORMAT                  format,
        D3DPOOL                    pool,
        ID3D11Texture3D**          ppTexture,
        ID3D11ShaderResourceView** ppSRV,
        ID3D11Texture3D**          ppStaging) noexcept;

    HRESULT CreateRenderTargetTexture(
        UINT                      width,
        UINT                      height,
        D3DFORMAT                 format,
        D3DMULTISAMPLE_TYPE       multiSample,
        DWORD                     multiSampleQuality,
        BOOL                      lockable,
        ID3D11Texture2D**         ppTexture,
        ID3D11RenderTargetView**  ppRTV,
        ID3D11Texture2D**         ppStaging,
        ID3D11RenderTargetView**  ppRTVSrgb = nullptr) noexcept;

    HRESULT CreateDepthStencilTexture(
        UINT                      width,
        UINT                      height,
        D3DFORMAT                 format,
        D3DMULTISAMPLE_TYPE       multiSample,
        DWORD                     multiSampleQuality,
        BOOL                      discard,
        ID3D11Texture2D**         ppTexture,
        ID3D11DepthStencilView**  ppDSV,
        ID3D11ShaderResourceView** ppSRV,
        ID3D11Texture2D**         ppStaging) noexcept;

    HRESULT CreateOffscreenPlainSurface(
        UINT               width,
        UINT               height,
        D3DFORMAT          format,
        D3DPOOL            pool,
        ID3D11Texture2D**  ppStaging) noexcept;

    HRESULT CreateStaticBuffer(
        UINT             byteWidth,
        UINT             bindFlags,
        D3DPOOL          pool,
        ID3D11Buffer**   ppBuffer,
        ID3D11Buffer**   ppStaging) noexcept;

    HRESULT CreateDynamicBuffer(
        UINT             byteWidth,
        UINT             bindFlags,
        ID3D11Buffer**   ppBuffer) noexcept;

    [[nodiscard]] DynamicRingBuffer* GetOrCreateDynamicVertexRing() noexcept;

    [[nodiscard]] DynamicRingBuffer* GetOrCreateDynamicIndexRing() noexcept;

    [[nodiscard]] DynamicRingBuffer* VertexRing()    noexcept { return GetOrCreateDynamicVertexRing(); }

    [[nodiscard]] DynamicRingBuffer* IndexRing()     noexcept { return GetOrCreateDynamicIndexRing(); }

    [[nodiscard]] DynamicRingBuffer* FanIndexRing()  noexcept { return GetOrCreateFanIndexRing(); }

private:
    [[nodiscard]] DynamicRingBuffer* GetOrCreateFanIndexRing() noexcept;

private:
    DeviceContext11* m_ctx;

    struct RingDeleter { void operator()(DynamicRingBuffer* p) const noexcept { delete p; } };
    using RingPtr = std::unique_ptr<DynamicRingBuffer, RingDeleter>;

    RingPtr m_dynamicVertexRing;
    RingPtr m_dynamicIndexRing;
    RingPtr m_fanIndexRing;
};

}

#endif
