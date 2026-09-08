// D3D9 state blocks — record a set of state changes, replay them later.
//
// The critical detail is in what Apply is allowed to touch. From the D3D9
// documentation: "This will overwrite *only* the device state that has been
// captured." A block is a sparse set of recorded states, not a snapshot of the
// whole device.
//
// Implementing this as a full snapshot of the device looks equivalent and is
// not. Consider a block recorded at a moment when no index buffer happened to
// be bound: a snapshot faithfully captures "index buffer = none", and every
// later Apply then clears the index buffer of whatever draw follows it. The
// game has done nothing wrong and no call has failed, but geometry stops
// appearing after the first Apply. The sparse behaviour is what the API
// actually promises, and it is what avoids this.
//
// The m_recorded mask implements it. Each setter that runs while a block is
// open marks its own bit; ApplyCapture consults the mask and restores only
// what was marked. A state that was never recorded is never touched.
//
// One deliberate difference between the backends: while a block is open, the
// D3D11 path records each change *and* applies it to the device, whereas the
// D3D12 path only records. The original runtime's behaviour here is not
// documented unambiguously, so the StateBlockRecordOnly setting exists to
// switch between the two interpretations.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9StateBlock.h>
#include <d3d9proxy/D9Device.h>
#include <d3d9proxy/D9VertexShader.h>
#include <d3d9proxy/D9PixelShader.h>
#include <d3d9proxy/D9VertexDecl.h>
#include <core/RenderStateTracker.h>
#include <core/ConstantMapper.h>
#include <algorithm>
#include <cstring>

namespace dx9to11 {

D9StateBlock::D9StateBlock(D9Device* pDevice, D3DSTATEBLOCKTYPE type,
                           bool recording) noexcept
    : m_device(pDevice)
    , m_type(type)
    , m_recording(recording)
{
    std::memset(&m_cap,      0, sizeof(m_cap));
    std::memset(&m_recorded, 0, sizeof(m_recorded));
    if (m_device) m_device->AddRef();
}

D9StateBlock::~D9StateBlock()
{
    ReleaseCapture();
    if (m_device) m_device->Release();
}

HRESULT D9StateBlock::CreatePreset(D9Device* pDevice, D3DSTATEBLOCKTYPE type,
                                   D9StateBlock** ppOut) noexcept
{
    if (!pDevice || !ppOut) return D3DERR_INVALIDCALL;
    auto* sb = new (std::nothrow) D9StateBlock(pDevice, type, false);
    if (!sb) return E_OUTOFMEMORY;
    switch (type) {
    case D3DSBT_ALL:         sb->CaptureAll();         break;
    case D3DSBT_PIXELSTATE:  sb->CapturePixelState();  break;
    case D3DSBT_VERTEXSTATE: sb->CaptureVertexState(); break;
    default:                 delete sb; return D3DERR_INVALIDCALL;
    }
    *ppOut = sb;
    return D3D_OK;
}

HRESULT D9StateBlock::CreateRecording(D9Device* pDevice,
                                      D9StateBlock** ppOut) noexcept
{
    if (!pDevice || !ppOut) return D3DERR_INVALIDCALL;
    auto* sb = new (std::nothrow) D9StateBlock(pDevice,
                                               D3DSBT_ALL,
                                               true);
    if (!sb) return E_OUTOFMEMORY;
    *ppOut = sb;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9StateBlock::QueryInterface(REFIID riid, void** ppvObj)
{
    if (!ppvObj) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirect3DStateBlock9) {
        *ppvObj = static_cast<IDirect3DStateBlock9*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObj = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE D9StateBlock::AddRef()
{
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE D9StateBlock::Release()
{
    const ULONG rc = --m_refCount;
    if (rc == 0) delete this;
    return rc;
}

HRESULT STDMETHODCALLTYPE D9StateBlock::GetDevice(IDirect3DDevice9** ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    m_device->AddRef();
    *ppDevice = m_device;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9StateBlock::Capture()
{

    if (m_recording) return D3D_OK;
    ReleaseCapture();
    std::memset(&m_cap, 0, sizeof(m_cap));
    switch (m_type) {
    case D3DSBT_ALL:         CaptureAll();         break;
    case D3DSBT_PIXELSTATE:  CapturePixelState();  break;
    case D3DSBT_VERTEXSTATE: CaptureVertexState(); break;
    default: break;
    }
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9StateBlock::Apply()
{
    ApplyCapture();
    return D3D_OK;
}

void D9StateBlock::ReleaseCapture() noexcept
{
    if (m_cap.vertexShader) { m_cap.vertexShader->Release(); m_cap.vertexShader = nullptr; }
    if (m_cap.pixelShader)  { m_cap.pixelShader->Release();  m_cap.pixelShader  = nullptr; }
    if (m_cap.vertexDecl)   { m_cap.vertexDecl->Release();   m_cap.vertexDecl   = nullptr; }
    for (int i = 0; i < 8; ++i) {
        if (m_cap.rst.textures[i]) {
            m_cap.rst.textures[i]->Release();
            m_cap.rst.textures[i] = nullptr;
        }
    }
    for (int i = 0; i < 16; ++i) {
        if (m_cap.rst.streams[i]) {
            m_cap.rst.streams[i]->Release();
            m_cap.rst.streams[i] = nullptr;
        }
    }
    if (m_cap.rst.indexBuffer) { m_cap.rst.indexBuffer->Release(); m_cap.rst.indexBuffer = nullptr; }
}

static void CopyRenderState(D9StateCapture& cap, D9Device* dev)
{
    const D9RenderState& src = dev->RST()->State();

    std::memcpy(cap.rst.rs,   src.rs,   sizeof(src.rs));
    std::memcpy(cap.rst.tss,  src.tss,  sizeof(src.tss));
    std::memcpy(cap.rst.samp, src.samp, sizeof(src.samp));

    cap.rst.indexBuffer = src.indexBuffer;
    if (cap.rst.indexBuffer) cap.rst.indexBuffer->AddRef();
    for (int i = 0; i < 8; ++i) {
        cap.rst.textures[i] = src.textures[i];
        if (cap.rst.textures[i]) cap.rst.textures[i]->AddRef();
    }
    for (int i = 0; i < 16; ++i) {
        cap.rst.streams[i]       = src.streams[i];
        cap.rst.streamStrides[i] = src.streamStrides[i];
        cap.rst.streamOffsets[i] = src.streamOffsets[i];
        if (cap.rst.streams[i]) cap.rst.streams[i]->AddRef();
    }
    cap.rst.vertexDecl    = src.vertexDecl;
    cap.rst.vertexShader  = src.vertexShader;
    cap.rst.pixelShader   = src.pixelShader;

}

void D9StateBlock::CaptureAll() noexcept
{
    CopyRenderState(m_cap, m_device);

    m_cap.vertexShader = m_cap.rst.vertexShader;
    if (m_cap.vertexShader) m_cap.vertexShader->AddRef();
    m_cap.pixelShader  = m_cap.rst.pixelShader;
    if (m_cap.pixelShader)  m_cap.pixelShader->AddRef();
    m_cap.vertexDecl   = m_cap.rst.vertexDecl;
    if (m_cap.vertexDecl)   m_cap.vertexDecl->AddRef();
    m_cap.fvf = m_device->CurrentFVF();

    ConstantMapper* cm = m_device->ConstantMapperPtr();
    cm->GetVSConstantF(0, m_cap.vsConstF, 256);
    cm->GetVSConstantI(0, m_cap.vsConstI, 16);
    cm->GetVSConstantB(0, m_cap.vsConstB, 16);
    cm->GetPSConstantF(0, m_cap.psConstF, 224);
    cm->GetPSConstantI(0, m_cap.psConstI, 16);
    cm->GetPSConstantB(0, m_cap.psConstB, 16);

    m_device->GetTransform(D3DTS_VIEW,       &m_cap.view);
    m_device->GetTransform(D3DTS_PROJECTION, &m_cap.projection);
    for (DWORD i = 0; i < 8; ++i) {
        m_device->GetTransform(D3DTS_WORLDMATRIX(i), &m_cap.world[i]);
        m_device->GetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + i),
                               &m_cap.texture[i]);
    }

    m_cap.viewport = m_device->Viewport();
    m_cap.scissorRect = m_device->ScissorRect();

    for (DWORD i = 0; i < 8; ++i) {
        m_device->GetLight(i, &m_cap.lights[i]);
        m_device->GetLightEnable(i, &m_cap.lightEnabled[i]);
    }
    m_device->GetMaterial(&m_cap.material);
}

void D9StateBlock::CapturePixelState() noexcept
{
    const D9RenderState& src = m_device->RST()->State();
    std::memcpy(m_cap.rst.rs,   src.rs,   sizeof(src.rs));
    std::memcpy(m_cap.rst.tss,  src.tss,  sizeof(src.tss));
    std::memcpy(m_cap.rst.samp, src.samp, sizeof(src.samp));
    for (int i = 0; i < 8; ++i) {
        m_cap.rst.textures[i] = src.textures[i];
        if (m_cap.rst.textures[i]) m_cap.rst.textures[i]->AddRef();
    }

    m_cap.pixelShader = m_cap.rst.pixelShader;
    if (m_cap.pixelShader) m_cap.pixelShader->AddRef();

    ConstantMapper* cm = m_device->ConstantMapperPtr();
    cm->GetPSConstantF(0, m_cap.psConstF, 224);
    cm->GetPSConstantI(0, m_cap.psConstI, 16);
    cm->GetPSConstantB(0, m_cap.psConstB, 16);
}

void D9StateBlock::CaptureVertexState() noexcept
{
    const D9RenderState& src = m_device->RST()->State();
    std::memcpy(m_cap.rst.rs, src.rs, sizeof(src.rs));
    for (int i = 0; i < 16; ++i) {
        m_cap.rst.streams[i]       = src.streams[i];
        m_cap.rst.streamStrides[i] = src.streamStrides[i];
        m_cap.rst.streamOffsets[i] = src.streamOffsets[i];
        if (m_cap.rst.streams[i]) m_cap.rst.streams[i]->AddRef();
    }
    m_cap.rst.indexBuffer = src.indexBuffer;
    if (m_cap.rst.indexBuffer) m_cap.rst.indexBuffer->AddRef();

    m_cap.vertexShader = m_cap.rst.vertexShader;
    if (m_cap.vertexShader) m_cap.vertexShader->AddRef();
    m_cap.vertexDecl   = m_cap.rst.vertexDecl;
    if (m_cap.vertexDecl)   m_cap.vertexDecl->AddRef();
    m_cap.fvf = m_device->CurrentFVF();

    ConstantMapper* cm = m_device->ConstantMapperPtr();
    cm->GetVSConstantF(0, m_cap.vsConstF, 256);
    cm->GetVSConstantI(0, m_cap.vsConstI, 16);
    cm->GetVSConstantB(0, m_cap.vsConstB, 16);

    for (DWORD i = 0; i < 8; ++i)
        m_device->GetTransform(D3DTS_WORLDMATRIX(i), &m_cap.world[i]);
    m_device->GetTransform(D3DTS_VIEW,       &m_cap.view);
    m_device->GetTransform(D3DTS_PROJECTION, &m_cap.projection);
}

void D9StateBlock::ApplyCapture() noexcept
{
    bool applyRS      = false;
    bool applyTSS     = false;
    bool applySamp    = false;
    bool applyTex     = false;
    bool applyStreams  = false;
    bool applyIB      = false;
    bool applyVS      = false;
    bool applyPS      = false;
    bool applyDecl    = false;
    bool applyFVF     = false;
    bool applyVSConstF = false, applyVSConstI = false, applyVSConstB = false;
    bool applyPSConstF = false, applyPSConstI = false, applyPSConstB = false;
    bool applyXforms  = false;
    bool applyVP      = false;
    bool applyScissor = false;
    bool applyLights  = false;
    bool applyMat     = false;

    if (m_recording) {

        applyRS       = true;
        applyTSS      = true;
        applySamp     = true;
        applyTex      = true;
        applyStreams   = true;
        applyIB       = m_recorded.indexBuffer;
        applyVS       = m_recorded.vertexShader;
        applyPS       = m_recorded.pixelShader;
        applyDecl     = m_recorded.vertexDecl;
        applyFVF      = m_recorded.fvf;
        applyVP       = m_recorded.viewport;
        applyScissor  = m_recorded.scissor;
        applyVSConstF = applyVSConstI = applyVSConstB = true;
        applyPSConstF = applyPSConstI = applyPSConstB = true;
        applyXforms   = true;
        applyLights   = true;
        (void)applyMat;

    } else {
        switch (m_type) {
        case D3DSBT_ALL:
            applyRS = applyTSS = applySamp = applyTex = applyStreams = true;
            applyIB = applyVS = applyPS = applyDecl = applyFVF = true;
            applyVSConstF = applyVSConstI = applyVSConstB = true;
            applyPSConstF = applyPSConstI = applyPSConstB = true;
            applyXforms = applyVP = applyScissor = true;
            applyLights = applyMat = true;
            break;
        case D3DSBT_PIXELSTATE:
            applyRS = applyTSS = applySamp = applyTex = true;
            applyPS = true;
            applyPSConstF = applyPSConstI = applyPSConstB = true;
            break;
        case D3DSBT_VERTEXSTATE:
            applyRS = applyStreams = applyIB = true;
            applyVS = applyDecl = applyFVF = true;
            applyVSConstF = applyVSConstI = applyVSConstB = true;
            applyXforms = true;
            break;
        default: return;
        }
    }

    if (applyRS) {
        for (int i = 0; i <= D3DRS_BLENDOPALPHA; ++i) {
            bool doit = !m_recording || m_recorded.rs[i];
            if (doit)
                m_device->SetRenderState(static_cast<D3DRENDERSTATETYPE>(i), m_cap.rst.rs[i]);
        }
    }

    if (applyTSS) {
        for (DWORD s = 0; s < 8; ++s) {
            for (int t = 0; t <= D3DTSS_CONSTANT; ++t) {
                bool doit = !m_recording || m_recorded.tss[s][t];
                if (doit)
                    m_device->SetTextureStageState(s, static_cast<D3DTEXTURESTAGESTATETYPE>(t),
                                                   m_cap.rst.tss[s][t]);
            }
        }
    }

    if (applySamp) {
        for (DWORD s = 0; s < 8; ++s) {
            for (int t = 0; t <= D3DSAMP_DMAPOFFSET; ++t) {
                bool doit = !m_recording || m_recorded.samp[s][t];
                if (doit)
                    m_device->SetSamplerState(s, static_cast<D3DSAMPLERSTATETYPE>(t),
                                              m_cap.rst.samp[s][t]);
            }
        }
    }

    if (applyTex) {
        for (DWORD s = 0; s < 8; ++s) {
            bool doit = !m_recording || m_recorded.textures[s];
            if (doit)
                m_device->SetTexture(s, m_cap.rst.textures[s]);
        }
    }

    if (applyStreams) {
        for (UINT s = 0; s < 16; ++s) {
            bool doit = !m_recording || m_recorded.streams[s];
            if (doit)
                m_device->SetStreamSource(s,
                    static_cast<IDirect3DVertexBuffer9*>(m_cap.rst.streams[s]),
                    m_cap.rst.streamOffsets[s], m_cap.rst.streamStrides[s]);
        }
    }

    if (applyIB)
        m_device->SetIndices(static_cast<IDirect3DIndexBuffer9*>(m_cap.rst.indexBuffer));

    if (applyVS) m_device->SetVertexShader(static_cast<IDirect3DVertexShader9*>(m_cap.vertexShader));
    if (applyPS) m_device->SetPixelShader (static_cast<IDirect3DPixelShader9*> (m_cap.pixelShader));

    if (applyDecl) m_device->SetVertexDeclaration(static_cast<IDirect3DVertexDeclaration9*>(m_cap.vertexDecl));
    if (applyFVF)  m_device->SetFVF(m_cap.fvf);

    if (m_recording) {

        auto replayRuns = [](const bool* flags, UINT count, auto&& apply) {
            UINT i = 0;
            while (i < count) {
                if (!flags[i]) { ++i; continue; }
                UINT start = i;
                while (i < count && flags[i]) ++i;
                apply(start, i - start);
            }
        };
        replayRuns(m_recorded.vsConstF, 256, [&](UINT s, UINT n) {
            m_device->SetVertexShaderConstantF(s, m_cap.vsConstF + s * 4, n); });
        replayRuns(m_recorded.vsConstI, 16, [&](UINT s, UINT n) {
            m_device->SetVertexShaderConstantI(s, m_cap.vsConstI + s * 4, n); });
        replayRuns(m_recorded.vsConstB, 16, [&](UINT s, UINT n) {
            m_device->SetVertexShaderConstantB(s, m_cap.vsConstB + s, n); });
        replayRuns(m_recorded.psConstF, 224, [&](UINT s, UINT n) {
            m_device->SetPixelShaderConstantF(s, m_cap.psConstF + s * 4, n); });
        replayRuns(m_recorded.psConstI, 16, [&](UINT s, UINT n) {
            m_device->SetPixelShaderConstantI(s, m_cap.psConstI + s * 4, n); });
        replayRuns(m_recorded.psConstB, 16, [&](UINT s, UINT n) {
            m_device->SetPixelShaderConstantB(s, m_cap.psConstB + s, n); });
    } else {
        if (applyVSConstF) m_device->SetVertexShaderConstantF(0, m_cap.vsConstF, 256);
        if (applyVSConstI) m_device->SetVertexShaderConstantI(0, m_cap.vsConstI, 16);
        if (applyVSConstB) m_device->SetVertexShaderConstantB(0, m_cap.vsConstB, 16);
        if (applyPSConstF) m_device->SetPixelShaderConstantF(0, m_cap.psConstF, 224);
        if (applyPSConstI) m_device->SetPixelShaderConstantI(0, m_cap.psConstI, 16);
        if (applyPSConstB) m_device->SetPixelShaderConstantB(0, m_cap.psConstB, 16);
    }

    if (applyXforms) {
        const bool rec = m_recording;
        if (!rec || m_recorded.transforms[D3DTS_VIEW])
            m_device->SetTransform(D3DTS_VIEW,       &m_cap.view);
        if (!rec || m_recorded.transforms[D3DTS_PROJECTION])
            m_device->SetTransform(D3DTS_PROJECTION, &m_cap.projection);
        for (DWORD i = 0; i < 8; ++i) {
            if (!rec || m_recorded.world[i])
                m_device->SetTransform(D3DTS_WORLDMATRIX(i), &m_cap.world[i]);
            if (!rec || m_recorded.transforms[D3DTS_TEXTURE0 + i])
                m_device->SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + i),
                                       &m_cap.texture[i]);
        }
    }

    if (applyVP)      m_device->SetViewport(&m_cap.viewport);
    if (applyScissor) m_device->SetScissorRect(&m_cap.scissorRect);

    if (applyLights) {
        const bool rec = m_recording;
        for (DWORD i = 0; i < 8; ++i) {
            if (!rec || m_recorded.lights[i])
                m_device->SetLight(i, &m_cap.lights[i]);
            if (!rec || m_recorded.lightEnable[i])
                m_device->LightEnable(i, m_cap.lightEnabled[i]);
        }
    }
    if (applyMat) m_device->SetMaterial(&m_cap.material);
}

void D9StateBlock::RecordRenderState(D3DRENDERSTATETYPE state, DWORD value) noexcept
{
    if (state > D3DRS_BLENDOPALPHA) return;
    m_cap.rst.rs[state]       = value;
    m_recorded.rs[state] = true;
}

void D9StateBlock::RecordTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type,
                                           DWORD value) noexcept
{
    if (stage >= 8 || type > D3DTSS_CONSTANT) return;
    m_cap.rst.tss[stage][type]       = value;
    m_recorded.tss[stage][type] = true;
}

void D9StateBlock::RecordSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type,
                                      DWORD value) noexcept
{
    if (sampler >= 8 || type > D3DSAMP_DMAPOFFSET) return;
    m_cap.rst.samp[sampler][type]       = value;
    m_recorded.samp[sampler][type] = true;
}

void D9StateBlock::RecordTexture(DWORD stage, IDirect3DBaseTexture9* pTex) noexcept
{
    if (stage >= 8) return;
    if (m_cap.rst.textures[stage]) m_cap.rst.textures[stage]->Release();
    m_cap.rst.textures[stage] = pTex;
    if (pTex) pTex->AddRef();
    m_recorded.textures[stage] = true;
}

void D9StateBlock::RecordVertexShader(IDirect3DVertexShader9* pVS) noexcept
{
    if (m_cap.vertexShader) m_cap.vertexShader->Release();
    m_cap.vertexShader = pVS;
    if (pVS) pVS->AddRef();
    m_recorded.vertexShader = true;
}

void D9StateBlock::RecordPixelShader(IDirect3DPixelShader9* pPS) noexcept
{
    if (m_cap.pixelShader) m_cap.pixelShader->Release();
    m_cap.pixelShader = pPS;
    if (pPS) pPS->AddRef();
    m_recorded.pixelShader = true;
}

void D9StateBlock::RecordVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) noexcept
{
    if (m_cap.vertexDecl) m_cap.vertexDecl->Release();
    m_cap.vertexDecl = pDecl;
    if (pDecl) pDecl->AddRef();
    m_recorded.vertexDecl = true;
}

void D9StateBlock::RecordFVF(DWORD fvf) noexcept
{
    m_cap.fvf   = fvf;
    m_recorded.fvf = true;
}

void D9StateBlock::RecordTransform(D3DTRANSFORMSTATETYPE state,
                                   const D3DMATRIX* pMatrix) noexcept
{
    if (!pMatrix) return;
    const UINT s = static_cast<UINT>(state);
    if (state == D3DTS_VIEW)       { m_cap.view       = *pMatrix; }
    else if (state == D3DTS_PROJECTION) { m_cap.projection = *pMatrix; }
    else if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE7)
        m_cap.texture[state - D3DTS_TEXTURE0] = *pMatrix;
    else if (s >= D3DTS_WORLDMATRIX(0) && s <= D3DTS_WORLDMATRIX(7)) {
        m_cap.world[s - D3DTS_WORLDMATRIX(0)] = *pMatrix;

        m_recorded.world[s - D3DTS_WORLDMATRIX(0)] = true;
        return;
    }
    if (state <= D3DTS_TEXTURE7)
        m_recorded.transforms[state] = true;
}

void D9StateBlock::RecordLight(DWORD index, const D3DLIGHT9* pLight) noexcept
{
    if (index >= 8 || !pLight) return;
    m_cap.lights[index]      = *pLight;
    m_recorded.lights[index] = true;
}

void D9StateBlock::RecordLightEnable(DWORD index, BOOL enable) noexcept
{
    if (index >= 8) return;
    m_cap.lightEnabled[index]     = enable;
    m_recorded.lightEnable[index] = true;
}

void D9StateBlock::RecordViewport(const D3DVIEWPORT9* pVP) noexcept
{
    if (!pVP) return;
    m_cap.viewport      = *pVP;
    m_recorded.viewport = true;
}

void D9StateBlock::RecordScissorRect(const RECT* pRect) noexcept
{
    if (!pRect) return;
    m_cap.scissorRect   = *pRect;
    m_recorded.scissor  = true;
}

void D9StateBlock::RecordVertexShaderConstantF(UINT start, const float* pData,
                                               UINT count) noexcept
{
    if (!pData || start + count > 256) return;
    std::memcpy(m_cap.vsConstF + start * 4, pData, count * 4 * sizeof(float));
    for (UINT i = start; i < start + count && i < 256; ++i)
        m_recorded.vsConstF[i] = true;
}

void D9StateBlock::RecordVertexShaderConstantI(UINT start, const int* pData,
                                               UINT count) noexcept
{
    if (!pData || start + count > 16) return;
    std::memcpy(m_cap.vsConstI + start * 4, pData, count * 4 * sizeof(int));
    for (UINT i = start; i < start + count && i < 16; ++i)
        m_recorded.vsConstI[i] = true;
}

void D9StateBlock::RecordVertexShaderConstantB(UINT start, const BOOL* pData,
                                               UINT count) noexcept
{
    if (!pData || start + count > 16) return;
    std::memcpy(m_cap.vsConstB + start, pData, count * sizeof(BOOL));
    for (UINT i = start; i < start + count && i < 16; ++i)
        m_recorded.vsConstB[i] = true;
}

void D9StateBlock::RecordPixelShaderConstantF(UINT start, const float* pData,
                                              UINT count) noexcept
{
    if (!pData || start + count > 224) return;
    std::memcpy(m_cap.psConstF + start * 4, pData, count * 4 * sizeof(float));
    for (UINT i = start; i < start + count && i < 224; ++i)
        m_recorded.psConstF[i] = true;
}

void D9StateBlock::RecordPixelShaderConstantI(UINT start, const int* pData,
                                              UINT count) noexcept
{
    if (!pData || start + count > 16) return;
    std::memcpy(m_cap.psConstI + start * 4, pData, count * 4 * sizeof(int));
    for (UINT i = start; i < start + count && i < 16; ++i)
        m_recorded.psConstI[i] = true;
}

void D9StateBlock::RecordPixelShaderConstantB(UINT start, const BOOL* pData,
                                              UINT count) noexcept
{
    if (!pData || start + count > 16) return;
    std::memcpy(m_cap.psConstB + start, pData, count * sizeof(BOOL));
    for (UINT i = start; i < start + count && i < 16; ++i)
        m_recorded.psConstB[i] = true;
}

void D9StateBlock::RecordStreamSource(UINT stream, IDirect3DVertexBuffer9* pVB,
                                      UINT offsetBytes, UINT stride) noexcept
{
    if (stream >= 16) return;
    if (m_cap.rst.streams[stream]) m_cap.rst.streams[stream]->Release();
    m_cap.rst.streams[stream]       = pVB;
    m_cap.rst.streamOffsets[stream] = offsetBytes;
    m_cap.rst.streamStrides[stream] = stride;
    if (pVB) pVB->AddRef();
    m_recorded.streams[stream] = true;
}

void D9StateBlock::RecordIndices(IDirect3DIndexBuffer9* pIB) noexcept
{
    if (m_cap.rst.indexBuffer) m_cap.rst.indexBuffer->Release();
    m_cap.rst.indexBuffer    = pIB;
    if (pIB) pIB->AddRef();
    m_recorded.indexBuffer = true;
}

}
