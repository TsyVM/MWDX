// The D3D11 implementation of IDirect3DDevice9.
//
// This is the largest surface in the project: every call a D3D9 game makes
// after creating its device arrives at one of these methods. The
// implementation is split across several translation units by concern rather
// than kept in one file --
//
//   D9Device.cpp           construction, teardown, device-level queries
//   D9Device_State.cpp     render, texture-stage and sampler state setters
//   D9Device_Resource.cpp  resource creation, locking, copying
//   D9Device_Draw.cpp      the draw path and everything it resolves
//   D9Device_Present.cpp   frame presentation, swap chain and reset
//
// The central design decision is that setters do almost nothing. D3D9 exposes
// around two hundred independent render states that its driver reconciles when
// a draw arrives; D3D11 wants immutable state objects decided in advance.
// Translating on every setter would rebuild those objects constantly for state
// the game may overwrite before it draws. So setters record into a shadow copy
// of D3D9 state and raise a dirty bit, and the draw path reconciles everything
// once, immediately before submitting.
//
// Two rules apply throughout, both of which exist because their absence caused
// bugs that produced no error and no log line:
//
//   Never discard work silently. Any path that drops a draw, refuses a
//   resource or falls back to an approximation must say so at least once.
//
//   Never be stricter than D3D9. Returning a failure where the original
//   runtime returned success makes a game abandon an entire render path, and
//   nothing anywhere reports a problem, because from this side nothing failed.

#pragma once

#ifndef DX9TO11_D9DEVICE_H
#define DX9TO11_D9DEVICE_H

#include <d3d9.h>
#include <d3d11_1.h>
#include <wrl/client.h>
#include <core/DeviceContext11.h>
#include <core/ResourceManager.h>
#include <core/ShaderCache.h>
#include <core/ConstantMapper.h>
#include <core/InputLayoutCache.h>
#include <core/RenderStateTracker.h>
#include <core/FFPEmulator.h>
#include <core/PSOCache.h>
#include <d3d9proxy/D9VertexShader.h>
#include <d3d9proxy/D9PixelShader.h>
#include <d3d9proxy/D9VertexDecl.h>
#include <d3d9proxy/D9SwapChain.h>
#include <d3d9proxy/D9StateBlock.h>
#include <atomic>
#include <array>
#include <memory>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace dx9to11 {

struct BindCache {
    ID3D11BlendState*        blend{};
    ID3D11RasterizerState*   rast{};
    ID3D11DepthStencilState* depthStencil{};
    UINT                     stencilRef{ 0xFFFFFFFF };

    ID3D11ShaderResourceView* srv[8]{};

    ID3D11Resource*           srvResource[8]{};
    ID3D11SamplerState*       samp[8]{};

    ID3D11Buffer* vb[16]{};
    UINT          vbStride[16]{};
    UINT          vbOffset[16]{};
    ID3D11Buffer* ib{};
    DXGI_FORMAT   ibFmt{ DXGI_FORMAT_UNKNOWN };
    UINT          ibOffset{};

    ID3D11InputLayout*       layout{};
    const void*              layoutKeyShader{};
    const void*              layoutKeyDecl{};
    DWORD                    layoutKeyFVF{};

    ID3D11VertexShader* vs{};
    ID3D11PixelShader*  ps{};
    bool shadersUnknown{ true };

    enum : uint8_t { kB1None = 0, kB1Ffp = 1, kB1Emu = 2 };
    uint8_t b1OwnerVS{ kB1None };
    uint8_t b1OwnerPS{ kB1None };

    D3D11_PRIMITIVE_TOPOLOGY topo{ D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED };
    D3D11_VIEWPORT           vp{};
    bool                     vpValid{ false };
    RECT                     scissor{};
    bool                     scissorValid{ false };

    ID3D11RenderTargetView* rtv[4]{};
    ID3D11DepthStencilView* dsv{};
    bool                    rtDirty{ true };

    void Invalidate() noexcept { *this = BindCache{}; }
};

struct PerfCounters {

    UINT drawCalls{}, drawsOk{};
    UINT draws{}, descBuilds{}, stateBinds{}, srvSets{}, sampSets{};
    UINT vbSets{}, ibSets{}, layoutSets{}, rtSets{}, topoSets{}, vpSets{};
    UINT frames{};

    long long qpcFrame{}, qpcInPresent{}, qpcInWait{};
    void ResetFrameTotals() noexcept { *this = PerfCounters{}; }
};

class D9Device final : public IDirect3DDevice9Ex {
public:

    explicit D9Device(
        std::unique_ptr<DeviceContext11> ctx,
        IDirect3D9*                      pParent,
        DWORD                            behaviorFlags,
        const D3DPRESENT_PARAMETERS&     pp,
        bool                             isEx = false) noexcept;

    D9Device(const D9Device&)            = delete;
    D9Device& operator=(const D9Device&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override;
    UINT    STDMETHODCALLTYPE GetAvailableTextureMem() override;
    HRESULT STDMETHODCALLTYPE EvictManagedResources() override;
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9** ppD3D9) override;
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* pCaps) override;
    HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) override;
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) override;
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap) override;
    void    STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override;
    BOOL    STDMETHODCALLTYPE ShowCursor(BOOL bShow) override;
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) override;
    HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) override;
    UINT    STDMETHODCALLTYPE GetNumberOfSwapChains() override;
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) override;
    HRESULT STDMETHODCALLTYPE Present(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) override;
    HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) override;
    HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL bEnableDialogs) override;
    void    STDMETHODCALLTYPE SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp) override;
    void    STDMETHODCALLTYPE GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) override;
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint) override;
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture) override;
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface) override;
    HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) override;
    HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) override;
    HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) override;
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override;
    HRESULT STDMETHODCALLTYPE EndScene() override;
    HRESULT STDMETHODCALLTYPE Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) override;
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*) override;
    HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT9* pViewport) override;
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* pViewport) override;
    HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL9* pMaterial) override;
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9* pMaterial) override;
    HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, CONST D3DLIGHT9*) override;
    HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9*) override;
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override;
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL* pEnable) override;
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, CONST float* pPlane) override;
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float* pPlane) override;
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override;
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) override;
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) override;
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override;
    HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9** ppSB) override;
    HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus) override;
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9* pClipStatus) override;
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) override;
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) override;
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
    HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) override;
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* pNumPasses) override;
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries) override;
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) override;
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override;
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* PaletteNumber) override;
    HRESULT STDMETHODCALLTYPE SetScissorRect(CONST RECT* pRect) override;
    HRESULT STDMETHODCALLTYPE GetScissorRect(RECT* pRect) override;
    HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override;
    BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override;
    HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override;
    float   STDMETHODCALLTYPE GetNPatchMode() override;
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override;
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride) override;
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) override;
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) override;
    HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) override;
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override;
    HRESULT STDMETHODCALLTYPE GetFVF(DWORD* pFVF) override;
    HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* pShader) override;
    HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9** ppShader) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override;
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) override;
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override;
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) override;
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) override;
    HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override;
    HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting) override;
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9* pIndexData) override;
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9** ppIndexData) override;
    HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* pShader) override;
    HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9** ppShader) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount) override;
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount) override;
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount) override;
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo) override;
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo) override;
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override;
    HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) override;

    HRESULT STDMETHODCALLTYPE SetConvolutionMonoKernel(UINT width, UINT height, float* rows, float* columns) override;
    HRESULT STDMETHODCALLTYPE ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst, IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects, IDirect3DVertexBuffer9* pDstRectDescs, D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset) override;
    HRESULT STDMETHODCALLTYPE PresentEx(CONST RECT* pSourceRect, CONST RECT* pDestRect, HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion, DWORD dwFlags) override;
    HRESULT STDMETHODCALLTYPE GetGPUThreadPriority(INT* pPriority) override;
    HRESULT STDMETHODCALLTYPE SetGPUThreadPriority(INT Priority) override;
    HRESULT STDMETHODCALLTYPE WaitForVBlank(UINT iSwapChain) override;
    HRESULT STDMETHODCALLTYPE CheckResourceResidency(IDirect3DResource9** pResourceArray, UINT32 NumResources) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
    HRESULT STDMETHODCALLTYPE CheckDeviceState(HWND hDestinationWindow) override;
    HRESULT STDMETHODCALLTYPE CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) override;
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle, DWORD Usage) override;
    HRESULT STDMETHODCALLTYPE ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters, D3DDISPLAYMODEEX* pFullscreenDisplayMode) override;
    HRESULT STDMETHODCALLTYPE GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) override;

    [[nodiscard]] DeviceContext11* Ctx()    const noexcept { return m_ctx.get(); }
    [[nodiscard]] ResourceManager* ResMgr()       noexcept;

    [[nodiscard]] ShaderCache*      ShaderCachePtr()   const noexcept { return m_shaderCache.get(); }
    [[nodiscard]] ConstantMapper*   ConstantMapperPtr() const noexcept { return m_constantMapper.get(); }
    [[nodiscard]] InputLayoutCache* InputLayoutCachePtr() const noexcept { return m_inputLayoutCache.get(); }

    [[nodiscard]] RenderStateTracker* RST() noexcept { return &m_rst; }
    [[nodiscard]] FFPEmulator*        FFP() noexcept { return m_ffp.get(); }
    [[nodiscard]] PSOCache*           PSO() noexcept { return m_psoCache.get(); }
    [[nodiscard]] DWORD               CurrentFVF() const noexcept { return m_currentFVF; }

    [[nodiscard]] const D3DVIEWPORT9& Viewport()    const noexcept { return m_viewport; }
    [[nodiscard]] const RECT&         ScissorRect() const noexcept { return m_scissorRect; }

    [[nodiscard]] UINT                StencilRef()  const noexcept
    {
        return m_rst.State().rs[D3DRS_STENCILREF];
    }
    [[nodiscard]] D9Surface*          GetRenderTarget(int idx) const noexcept
    {
        return (idx >= 0 && idx < 4) ? m_renderTargets[idx] : nullptr;
    }
    [[nodiscard]] D9Surface*          GetDepthStencil() const noexcept { return m_depthStencil; }

    [[nodiscard]] BindCache&    Bind() noexcept { return m_bind; }
    [[nodiscard]] PerfCounters& Perf() noexcept { return m_perf; }

    void TrackDefaultPoolObject(IUnknown* obj, const char* kind) noexcept;
    void UntrackDefaultPoolObject(IUnknown* obj) noexcept;

    [[nodiscard]] bool          EmuCBDirty() const noexcept { return m_emuDirty; }
    [[nodiscard]] ID3D11Buffer* EmuCB() const noexcept { return m_emuCB.Get(); }
    void MarkEmuCBDirty() noexcept { m_emuDirty = true; }
    HRESULT FlushEmuCB() noexcept;

    [[nodiscard]] ID3D11Buffer* ZeroVB() noexcept;

private:
    ~D9Device();

    void PostPresentBookkeeping() noexcept;

    void BindDefaultTargets(const D3DPRESENT_PARAMETERS& pp) noexcept;

    void InitImplicitState() noexcept;

    std::atomic<ULONG>               m_refCount{ 1 };
    std::unique_ptr<DeviceContext11> m_ctx;
    std::unique_ptr<ResourceManager> m_resMgr;
    IDirect3D9*                      m_parent{};
    DWORD                            m_behaviorFlags{};
    bool                             m_isEx{ false };
    D3DCAPS9                         m_caps{};

    std::array<class D9Surface*, 4> m_renderTargets{};
    class D9Surface*                m_depthStencil{};

    std::unique_ptr<ShaderCache>      m_shaderCache;
    std::unique_ptr<ConstantMapper>   m_constantMapper;
    std::unique_ptr<InputLayoutCache> m_inputLayoutCache;

    D9VertexShader* m_currentVS{};
    D9PixelShader*  m_currentPS{};
    D9VertexDecl*   m_currentDecl{};
    DWORD           m_currentFVF{};

    bool m_inputLayoutDirty{ true };

    RenderStateTracker           m_rst;
    std::unique_ptr<FFPEmulator> m_ffp;
    std::unique_ptr<PSOCache>    m_psoCache;
    D3DVIEWPORT9                 m_viewport{};
    RECT                         m_scissorRect{};
    bool                         m_partialDepthClearWarned{ false };
    float                        m_clipPlanes[6][4]{};

    void ApplyEmuCursor(bool show) noexcept;
    HCURSOR                      m_emuCursor{ nullptr };
    bool                         m_emuCursorVisible{ false };
    bool                         m_srgbWarned{ false };
    bool                         m_clipPlaneWarned{ false };
    UINT                         m_availTexMem{ 0 };
    DWORD                        m_clipPlaneEnable{};
    BOOL                         m_softwareVP{ FALSE };
    float                        m_nPatchMode{ 0.0f };
    D3DCLIPSTATUS9               m_clipStatus{};
    UINT                         m_currentPalette{ 0 };

    BindCache    m_bind;
    PerfCounters m_perf;
    long long    m_lastPresentQpc{ 0 };

    ComPtr<ID3D11Buffer> m_emuCB;
    bool                 m_emuDirty{ true };

    ComPtr<ID3D11Buffer> m_zeroVB;

    SRWLOCK                                    m_defaultPoolLock = SRWLOCK_INIT;
    std::unordered_map<IUnknown*, const char*> m_defaultPoolObjects;

    void ReportDefaultPoolSurvivors() noexcept;

    HRESULT EnsureBlitPass() noexcept;
    HRESULT ShaderBlit(class D9Surface* src, const RECT& srcRect,
                       class D9Surface* dst, const RECT& dstRect,
                       D3DTEXTUREFILTERTYPE filter) noexcept;
    ComPtr<ID3D11VertexShader> m_blitVS;
    ComPtr<ID3D11PixelShader>  m_blitPS;
    ComPtr<ID3D11Buffer>       m_blitCB;
    ComPtr<ID3D11SamplerState> m_blitSampPoint;
    ComPtr<ID3D11SamplerState> m_blitSampLinear;

    D9StateBlock*                 m_recordingBlock{};

    D3DPRESENT_PARAMETERS         m_presentParams{};

    struct SwapChainReleaser { void operator()(D9SwapChain* p) const noexcept { if (p) p->Release(); } };
    std::unique_ptr<D9SwapChain, SwapChainReleaser> m_primarySwapChain;
    D9Surface*                    m_resetDepthStencil{};
};

}

#endif
