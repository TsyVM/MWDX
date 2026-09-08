// IDirect3DStateBlock9 -- record a set of state changes and replay them later.
//
// The subtlety is in what Apply is permitted to touch. A state block is a
// sparse set of the states that were recorded, not a snapshot of the device,
// and applying one must overwrite only those. The recorded-state mask in this
// class is what enforces that: each setter running inside an open block marks
// its own bit, and applying consults the mask.
//
// A full-snapshot implementation looks equivalent and is not. A block recorded
// while no index buffer happened to be bound would capture "no index buffer"
// and, on every later apply, clear the index buffer of the draws that follow.

#pragma once

#ifndef DX9TO11_D9STATE_BLOCK_H
#define DX9TO11_D9STATE_BLOCK_H

#include <d3d9.h>
#include <atomic>
#include <array>
#include <vector>
#include <core/RenderStateTracker.h>

namespace dx9to11 {

class D9Device;

struct D9StateCapture {

    D9RenderState rst;

    IDirect3DVertexShader9* vertexShader{};
    IDirect3DPixelShader9*  pixelShader{};

    IDirect3DVertexDeclaration9* vertexDecl{};
    DWORD                        fvf{};

    float   vsConstF[256 * 4]{};
    int     vsConstI[16 * 4]{};
    BOOL    vsConstB[16]{};
    float   psConstF[224 * 4]{};
    int     psConstI[16 * 4]{};
    BOOL    psConstB[16]{};

    D3DMATRIX world[8]{};
    D3DMATRIX view{};
    D3DMATRIX projection{};
    D3DMATRIX texture[8]{};

    D3DVIEWPORT9 viewport{};
    RECT         scissorRect{};

    D3DLIGHT9 lights[8]{};
    BOOL      lightEnabled[8]{};

    D3DMATERIAL9 material{};

    float clipPlanes[6][4]{};
    DWORD clipPlaneEnable{};
};

class D9StateBlock final : public IDirect3DStateBlock9 {
public:

    static HRESULT CreatePreset(D9Device* pDevice,
                                D3DSTATEBLOCKTYPE type,
                                D9StateBlock** ppOut) noexcept;

    static HRESULT CreateRecording(D9Device* pDevice,
                                   D9StateBlock** ppOut) noexcept;

    D9StateBlock(const D9StateBlock&)            = delete;
    D9StateBlock& operator=(const D9StateBlock&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG   STDMETHODCALLTYPE AddRef()  override;
    ULONG   STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9** ppDevice) override;
    HRESULT STDMETHODCALLTYPE Capture() override;
    HRESULT STDMETHODCALLTYPE Apply()   override;

    void RecordRenderState(D3DRENDERSTATETYPE state, DWORD value) noexcept;
    void RecordTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) noexcept;
    void RecordSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) noexcept;
    void RecordTexture(DWORD stage, IDirect3DBaseTexture9* pTex) noexcept;
    void RecordVertexShader(IDirect3DVertexShader9* pVS) noexcept;
    void RecordPixelShader(IDirect3DPixelShader9* pPS) noexcept;
    void RecordVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) noexcept;
    void RecordFVF(DWORD fvf) noexcept;
    void RecordTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* pMatrix) noexcept;
    void RecordLight(DWORD index, const D3DLIGHT9* pLight) noexcept;
    void RecordLightEnable(DWORD index, BOOL enable) noexcept;
    void RecordViewport(const D3DVIEWPORT9* pVP) noexcept;
    void RecordScissorRect(const RECT* pRect) noexcept;
    void RecordVertexShaderConstantF(UINT start, const float* pData, UINT count) noexcept;
    void RecordVertexShaderConstantI(UINT start, const int* pData, UINT count) noexcept;
    void RecordVertexShaderConstantB(UINT start, const BOOL* pData, UINT count) noexcept;
    void RecordPixelShaderConstantF(UINT start, const float* pData, UINT count) noexcept;
    void RecordPixelShaderConstantI(UINT start, const int* pData, UINT count) noexcept;
    void RecordPixelShaderConstantB(UINT start, const BOOL* pData, UINT count) noexcept;
    void RecordStreamSource(UINT stream, IDirect3DVertexBuffer9* pVB,
                            UINT offsetBytes, UINT stride) noexcept;
    void RecordIndices(IDirect3DIndexBuffer9* pIB) noexcept;

    [[nodiscard]] bool IsRecording() const noexcept { return m_recording; }
    void EndRecording() noexcept { m_recording = false; }

private:
    explicit D9StateBlock(D9Device* pDevice, D3DSTATEBLOCKTYPE type,
                          bool recording) noexcept;
    ~D9StateBlock();

    void CaptureAll()         noexcept;
    void CapturePixelState()  noexcept;
    void CaptureVertexState() noexcept;
    void ApplyCapture()       noexcept;

    void ReleaseCapture() noexcept;

    struct RecordedSets {
        bool viewport    = false;
        bool scissor     = false;
        bool fvf         = false;
        bool vertexDecl  = false;
        bool vertexShader = false;
        bool pixelShader  = false;
        bool indexBuffer  = false;
        bool rs[D3DRS_BLENDOPALPHA + 1]{};
        bool tss[8][D3DTSS_CONSTANT + 1]{};
        bool samp[8][D3DSAMP_DMAPOFFSET + 1]{};
        bool textures[8]{};
        bool streams[16]{};
        bool transforms[D3DTS_TEXTURE7 + 1]{};
        bool world[8]{};

        bool lights[8]{};
        bool lightEnable[8]{};
        bool vsConstF[256]{};
        bool psConstF[224]{};
        bool vsConstI[16]{};
        bool psConstI[16]{};
        bool vsConstB[16]{};
        bool psConstB[16]{};
    };

    D9Device*           m_device;
    D3DSTATEBLOCKTYPE   m_type;
    bool                m_recording;
    D9StateCapture      m_cap;
    RecordedSets        m_recorded;
    std::atomic<ULONG>  m_refCount{ 1 };
};

}

#endif
