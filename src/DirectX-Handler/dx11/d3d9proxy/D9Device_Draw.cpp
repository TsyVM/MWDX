// The DX11 draw path: everything between the game calling DrawPrimitive and a
// draw reaching the D3D11 immediate context.
//
// D3D9 and D3D11 divide the same work differently. D3D9 is a bag of ~200
// independent render states that the driver reconciles at draw time; D3D11
// wants a small number of immutable state objects chosen up front. So nothing
// is translated when the game sets it — SetRenderState only marks a dirty bit
// on the RenderStateTracker — and the whole pipeline is resolved here, once,
// immediately before the draw. That is what PreDrawFlush does, in a fixed
// order: shaders, constants, input layout, vertex and index buffers,
// rasteriser, blend, depth-stencil, textures and samplers, render targets.
//
// Two rules govern this file, both learned expensively:
//
//   Never discard a draw silently. Several of the worst bugs in this project
//   were a guard here rejecting work and returning success. One `if (!ib)
//   return` discarded 100% of the game's front-end draws — its intro, menus
//   and HUD all go through DrawIndexedPrimitiveUP — and produced no error and
//   no log line anywhere. Every early-out in this file must say so at least
//   once.
//
//   Never be stricter than D3D9. Returning D3DERR_INVALIDCALL where the retail
//   runtime returns D3D_OK makes a game abandon an entire render path, and
//   again nothing logs, because from our side nothing failed. Out-of-range
//   constant writes are clamped rather than refused, stage and sampler indices
//   above 8 are accepted, and draws outside BeginScene/EndScene are honoured,
//   all for this reason.
//
// The per-draw census (DumpFrame=1) and the funnel counters (PerfLog=1) both
// live here. The funnel — count at the entry, count at the exit, compare —
// is the technique that has actually located bugs in this code; reasoning
// about which feature might be broken has repeatedly produced confident wrong
// answers.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d9proxy/D9Device.h>
#include <d3d9proxy/D9VertexDecl.h>
#include <d3d9proxy/D9VertexBuffer.h>
#include <d3d9proxy/D9IndexBuffer.h>
#include <d3d9proxy/D9Texture.h>
#include <d3d9proxy/D9CubeTexture.h>
#include <d3d9proxy/D9VolumeTexture.h>
#include <d3d9proxy/D9Surface.h>
#include <d3d9proxy/D9VertexShader.h>
#include <d3d9proxy/D9PixelShader.h>
#include <d3d9proxy/D9Query.h>
#include <d3d9proxy/D9StateBlock.h>
#include <core/QueryBridge.h>
#include <core/RenderStateTracker.h>
#include <core/FFPEmulator.h>
#include <core/PSOCache.h>
#include <core/ConstantMapper.h>
#include <core/InputLayoutCache.h>
#include <core/DynamicRingBuffer.h>
#include <core/Log.h>
#include <core/BackendSelect.h>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace dx9to11 {

static UINT PrimToVertexCount(D3DPRIMITIVETYPE pt, UINT primCount) noexcept
{
    switch (pt) {
    case D3DPT_POINTLIST:     return primCount;
    case D3DPT_LINELIST:      return primCount * 2;
    case D3DPT_LINESTRIP:     return primCount + 1;
    case D3DPT_TRIANGLELIST:  return primCount * 3;
    case D3DPT_TRIANGLESTRIP: return primCount + 2;
    case D3DPT_TRIANGLEFAN:   return primCount + 2;
    default:                   return 0;
    }
}

static D3D11_PRIMITIVE_TOPOLOGY TranslateTopology(D3DPRIMITIVETYPE pt) noexcept
{
    switch (pt) {
    case D3DPT_POINTLIST:     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_LINELIST:      return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_TRIANGLELIST:  return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;

    default:                   return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    }
}

static HRESULT ExpandFanIndices(
    ID3D11DeviceContext1* ctx,
    DynamicRingBuffer*  ring,
    UINT                startVertex,
    UINT                primCount,
    ID3D11Buffer**      ppBuffer,
    UINT*               pByteOffset,
    UINT*               pIndexCount) noexcept
{
    const UINT triCount  = primCount;
    const UINT idxCount  = triCount * 3;
    const UINT byteSize  = idxCount * sizeof(UINT);

    RingAllocation alloc{};
    HRESULT hr = ring->Map(ctx, byteSize,   false, &alloc);
    if (FAILED(hr)) return hr;

    UINT* dst = static_cast<UINT*>(alloc.pData);
    const UINT hub = startVertex;
    for (UINT t = 0; t < triCount; ++t) {
        dst[t * 3 + 0] = hub;
        dst[t * 3 + 1] = hub + 1 + t;
        dst[t * 3 + 2] = hub + 2 + t;
    }
    ring->Unmap(ctx);

    *ppBuffer    = ring->Buffer();
    *pByteOffset = alloc.offsetInBytes;
    *pIndexCount = idxCount;
    return S_OK;
}

static void BuildBlendDesc(const D9RenderState& rs, BlendDesc& out) noexcept
{
    out.BlendEnable        = rs.rs[D3DRS_ALPHABLENDENABLE] ? TRUE : FALSE;
    out.SrcBlend           = static_cast<D3DBLEND>(rs.rs[D3DRS_SRCBLEND]);
    out.DestBlend          = static_cast<D3DBLEND>(rs.rs[D3DRS_DESTBLEND]);
    out.BlendOp            = static_cast<D3DBLENDOP>(rs.rs[D3DRS_BLENDOP]);
    out.SeparateAlphaBlend = rs.rs[D3DRS_SEPARATEALPHABLENDENABLE] ? TRUE : FALSE;
    out.SrcBlendAlpha      = static_cast<D3DBLEND>(rs.rs[D3DRS_SRCBLENDALPHA]);
    out.DestBlendAlpha     = static_cast<D3DBLEND>(rs.rs[D3DRS_DESTBLENDALPHA]);
    out.BlendOpAlpha       = static_cast<D3DBLENDOP>(rs.rs[D3DRS_BLENDOPALPHA]);
    out.ColorWriteEnable   = rs.rs[D3DRS_COLORWRITEENABLE];
}

static void BuildRastDesc(const D9RenderState& rs, RastDesc& out) noexcept
{
    out.FillMode             = static_cast<D3DFILLMODE>(rs.rs[D3DRS_FILLMODE]);
    out.CullMode             = static_cast<D3DCULL>(rs.rs[D3DRS_CULLMODE]);
    out.ScissorEnable        = rs.rs[D3DRS_SCISSORTESTENABLE] ? TRUE : FALSE;
    out.MultisampleEnable    = rs.rs[D3DRS_MULTISAMPLEANTIALIAS] ? TRUE : FALSE;
    out.AntialiasedLine      = rs.rs[D3DRS_ANTIALIASEDLINEENABLE] ? TRUE : FALSE;

    out.DepthBias            = rs.rs[D3DRS_DEPTHBIAS];
    out.SlopeScaledDepthBias = *reinterpret_cast<const float*>(&rs.rs[D3DRS_SLOPESCALEDEPTHBIAS]);
}

static void BuildDepthStencilDesc(const D9RenderState& rs, DepthStencilDesc& out) noexcept
{
    out.ZEnable           = static_cast<D3DZBUFFERTYPE>(rs.rs[D3DRS_ZENABLE]) != D3DZB_FALSE;
    out.ZFunc             = static_cast<D3DCMPFUNC>(rs.rs[D3DRS_ZFUNC]);
    out.ZWriteEnable      = rs.rs[D3DRS_ZWRITEENABLE] ? TRUE : FALSE;
    out.StencilEnable     = rs.rs[D3DRS_STENCILENABLE] ? TRUE : FALSE;
    out.StencilReadMask   = static_cast<BYTE>(rs.rs[D3DRS_STENCILMASK]);
    out.StencilWriteMask  = static_cast<BYTE>(rs.rs[D3DRS_STENCILWRITEMASK]);
    out.StencilFail       = static_cast<D3DSTENCILOP>(rs.rs[D3DRS_STENCILFAIL]);
    out.StencilZFail      = static_cast<D3DSTENCILOP>(rs.rs[D3DRS_STENCILZFAIL]);
    out.StencilPass       = static_cast<D3DSTENCILOP>(rs.rs[D3DRS_STENCILPASS]);
    out.StencilFunc       = static_cast<D3DCMPFUNC>(rs.rs[D3DRS_STENCILFUNC]);
    out.TwoSidedStencil   = rs.rs[D3DRS_TWOSIDEDSTENCILMODE] ? TRUE : FALSE;
    out.CCWFail           = static_cast<D3DSTENCILOP>(rs.rs[D3DRS_CCW_STENCILFAIL]);
    out.CCWZFail          = static_cast<D3DSTENCILOP>(rs.rs[D3DRS_CCW_STENCILZFAIL]);
    out.CCWPass           = static_cast<D3DSTENCILOP>(rs.rs[D3DRS_CCW_STENCILPASS]);
    out.CCWFunc           = static_cast<D3DCMPFUNC>(rs.rs[D3DRS_CCW_STENCILFUNC]);
}

static void BuildSamplerDesc(const D9RenderState& rs, DWORD stage, SamplerDesc& out) noexcept
{
    out.MinFilter     = static_cast<D3DTEXTUREFILTERTYPE>(rs.samp[stage][D3DSAMP_MINFILTER]);
    out.MagFilter     = static_cast<D3DTEXTUREFILTERTYPE>(rs.samp[stage][D3DSAMP_MAGFILTER]);
    out.MipFilter     = static_cast<D3DTEXTUREFILTERTYPE>(rs.samp[stage][D3DSAMP_MIPFILTER]);
    out.AddressU      = static_cast<D3DTEXTUREADDRESS>(rs.samp[stage][D3DSAMP_ADDRESSU]);
    out.AddressV      = static_cast<D3DTEXTUREADDRESS>(rs.samp[stage][D3DSAMP_ADDRESSV]);
    out.AddressW      = static_cast<D3DTEXTUREADDRESS>(rs.samp[stage][D3DSAMP_ADDRESSW]);
    DWORD biasRaw     = rs.samp[stage][D3DSAMP_MIPMAPLODBIAS];
    out.MipLODBias    = *reinterpret_cast<const float*>(&biasRaw);
    out.MaxAnisotropy = rs.samp[stage][D3DSAMP_MAXANISOTROPY];
    out.MaxMipLevel   = rs.samp[stage][D3DSAMP_MAXMIPLEVEL];
    out.BorderColor   = rs.samp[stage][D3DSAMP_BORDERCOLOR];
}

static ID3D11ShaderResourceView* GetSRVFromBaseTexture(IDirect3DBaseTexture9* pTex, bool srgb) noexcept
{
    if (!pTex) return nullptr;
    D3DRESOURCETYPE rt = pTex->GetType();
    if (rt == D3DRTYPE_TEXTURE)
        return static_cast<D9Texture*>(pTex)->SRV(srgb);
    if (rt == D3DRTYPE_CUBETEXTURE)
        return static_cast<D9CubeTexture*>(pTex)->SRV(srgb);
    if (rt == D3DRTYPE_VOLUMETEXTURE)
        return static_cast<D9VolumeTexture*>(pTex)->SRV(srgb);
    return nullptr;
}

static HRESULT PreDrawFlush(
    D9Device*          dev,
    D3DPRIMITIVETYPE   primType,
    UINT                ,
    UINT                ,
    bool               isIndexed,
    UINT                ,
    INT                 ,
    UINT                ,

    ID3D11Buffer*      overrideVB,
    UINT               overrideVBOffset,
    UINT               overrideVBStride,
    ID3D11Buffer*      overrideIB,
    UINT               overrideIBOffset,
    DXGI_FORMAT        overrideIBFmt) noexcept
{
    auto*  ctx  = dev->Ctx()->Context();
    auto&  rst  = *dev->RST();
    auto&  rs   = rst.State();
    auto&  bind = dev->Bind();
    auto&  perf = dev->Perf();
    perf.draws++;

    const bool hasGameVS = (rs.vertexShader != nullptr);
    const bool hasGamePS = (rs.pixelShader  != nullptr);
    const bool ffpVS     = !hasGameVS;
    const bool ffpPS     = !hasGamePS;
    const bool isFFP     = ffpVS;
    bool       isPosTDraw = false;

    if (ffpVS) {

        DWORD fvf = dev->CurrentFVF();
        const D3DVERTEXELEMENT9* pDecl = nullptr;
        D3DVERTEXELEMENT9 fvfElems[MAXD3DDECLLENGTH + 1];

        if (rs.vertexDecl) {
            auto* d9decl = static_cast<D9VertexDecl*>(rs.vertexDecl);
            pDecl = d9decl->Elements();
        } else if (fvf) {
            FFPEmulator::ExpandFVF(fvf, fvfElems);
            pDecl = fvfElems;
        }

        const bool fvfIsPosT = fvf && ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
        const bool declIsPosT = pDecl && !fvf && [&]() noexcept {
            for (const D3DVERTEXELEMENT9* e = pDecl; e->Stream != 0xFF; ++e)
                if (e->Usage == D3DDECLUSAGE_POSITIONT) return true;
            return false;
        }();
        isPosTDraw = fvfIsPosT || declIsPosT;
        if (isPosTDraw) {
            static unsigned s_posTTriageCount = 0;
            if (s_posTTriageCount < 500u) {
                ++s_posTTriageCount;
                D9Surface* ds = dev->GetDepthStencil();
                D9Surface* rt = dev->GetRenderTarget(0);
                unsigned dsw = 0, dsh = 0, rtw = 0, rth = 0;
                if (ds) { dsw = ds->Info().width; dsh = ds->Info().height; }
                if (rt) { rtw = rt->Info().width; rth = rt->Info().height; }
                DXLOG_TRACE("posT-triage#%u: ZEnable=%u ZWriteEnable=%u ZFunc=%u "
                            "RT0=%ux%u DSV=%s%ux%u",
                            s_posTTriageCount,
                            (unsigned)(static_cast<D3DZBUFFERTYPE>(rs.rs[D3DRS_ZENABLE]) != D3DZB_FALSE ? 1 : 0),
                            (unsigned)(rs.rs[D3DRS_ZWRITEENABLE] ? 1 : 0),
                            (unsigned)rs.rs[D3DRS_ZFUNC],
                            rtw, rth, ds ? "" : "none ", dsw, dsh);
            }
        }

        dev->FFP()->SetViewportInfo(dev->Viewport());
        HRESULT hr = ffpPS
            ? dev->FFP()->BindForDraw(rst, fvf, pDecl)
            : dev->FFP()->BindVSMixed(rst, fvf, pDecl);
        if (FAILED(hr)) {

            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                DXLOG_WARN("[dx9to11] DRAW DROPPED: fixed-function vertex stage "
                           "could not be bound (fvf=0x%08X decl=%p) - this "
                           "geometry does not reach the screen", fvf, (void*)pDecl);
            }
            return hr;
        }
        bind.shadersUnknown = true;

        if (BackendSelect::NoBindCache() || bind.b1OwnerVS != BindCache::kB1Ffp) {
            ID3D11Buffer* ffpCB = dev->FFP()->CBuffer();
            if (ffpCB) ctx->VSSetConstantBuffers(1, 1, &ffpCB);
            bind.b1OwnerVS = BindCache::kB1Ffp;
        }
        if (ffpPS && (BackendSelect::NoBindCache() || bind.b1OwnerPS != BindCache::kB1Ffp)) {
            ID3D11Buffer* ffpCB = dev->FFP()->CBuffer();
            if (ffpCB) ctx->PSSetConstantBuffers(1, 1, &ffpCB);
            bind.b1OwnerPS = BindCache::kB1Ffp;
        }
    }

    if (hasGameVS) {

        auto* d9vs = static_cast<D9VertexShader*>(rs.vertexShader);
        ID3D11VertexShader* vs11 = d9vs ? d9vs->VS11() : nullptr;
        if (bind.shadersUnknown || vs11 != bind.vs) {
            if (vs11) ctx->VSSetShader(vs11, nullptr, 0);
            bind.vs = vs11;
        }

        if (ffpPS) {

            const ShaderReflection& vr = d9vs->Reflection();
            HRESULT hr = dev->FFP()->BindPSMixed(rst, vr.outTexMask,
                                                 vr.outColor0, vr.outColor1,
                                                 vr.outFog);
            if (FAILED(hr)) {
                static bool s_warned = false;
                if (!s_warned) {
                    s_warned = true;
                    DXLOG_WARN("[dx9to11] DRAW DROPPED: fixed-function pixel stage "
                               "could not be bound for a programmable VS - this "
                               "geometry does not reach the screen");
                }
                return hr;
            }
            bind.ps = nullptr;
            bind.shadersUnknown = true;
            if (BackendSelect::NoBindCache() || bind.b1OwnerPS != BindCache::kB1Ffp) {
                ID3D11Buffer* ffpCB = dev->FFP()->CBuffer();
                if (ffpCB) ctx->PSSetConstantBuffers(1, 1, &ffpCB);
                bind.b1OwnerPS = BindCache::kB1Ffp;
            }
        }
    }

    if (hasGamePS) {

        auto* d9ps = static_cast<D9PixelShader*>(rs.pixelShader);

        const UINT atestFunc = rs.rs[D3DRS_ALPHATESTENABLE]
                             ? (rs.rs[D3DRS_ALPHAFUNC] & 0xFu) : 0u;

        UINT fogMode = 0u;
        if (rs.rs[D3DRS_FOGENABLE]) {
            const UINT tbl = rs.rs[D3DRS_FOGTABLEMODE]  & 3u;
            const UINT vtx = rs.rs[D3DRS_FOGVERTEXMODE] & 3u;
            fogMode = tbl ? tbl : (vtx ? 4u : 0u);
        }

        UINT texKindMask = 0;
        for (DWORD s = 0; s < 8; ++s) {
            if (!rs.textures[s]) continue;
            const D3DRESOURCETYPE trt = rs.textures[s]->GetType();
            unsigned kind = 0;
            if (trt == D3DRTYPE_CUBETEXTURE)        kind = 1;
            else if (trt == D3DRTYPE_VOLUMETEXTURE) kind = 2;
            if (kind) texKindMask |= (kind << (s * 2));
        }

        if (texKindMask) {
            static bool s_texKindOverrideSeen = false;
            if (!s_texKindOverrideSeen) {
                s_texKindOverrideSeen = true;
                uint32_t verTag = 0;
                if (d9ps->BytecodeSize() >= 4)
                    std::memcpy(&verTag, d9ps->Bytecode(), 4);

                DXLOG_INFO("texKindMask override ACTIVE: mask=0x%04X on ps_%08X - "
                           "if SRV-dimension errors persist after this line, the "
                           "variant compile itself is failing (check for a "
                           "D3DCompile FAILED line right after).",
                           texKindMask, verTag);
            }
        }

        ID3D11PixelShader* ps11 =
            d9ps->PS11ForState(atestFunc, fogMode, texKindMask, dev->ShaderCachePtr());

        if (bind.shadersUnknown || ps11 != bind.ps) {
            if (ps11) ctx->PSSetShader(ps11, nullptr, 0);
            bind.ps = ps11;
        }
    }
    if (hasGameVS && hasGamePS)
        bind.shadersUnknown = false;

    if (hasGamePS && rs.rs[D3DRS_FOGENABLE] &&
        (rs.rs[D3DRS_FOGTABLEMODE] & 3u) == 0 &&
        (rs.rs[D3DRS_FOGVERTEXMODE] & 3u) != 0 &&
        static_cast<D9PixelShader*>(rs.pixelShader)->IsSM3()) {
        static bool s_fogGapWarned = false;
        if (!s_fogGapWarned) {
            s_fogGapWarned = true;
            OutputDebugStringA("[dx9to11] vertex fog on an SM3 PS is not "
                               "emulated (no oFog interpolant); rare\n");
        }
    }

    const DWORD dirty = rst.Dirty();

    if ((dirty & RenderStateTracker::kDirtyBlend) || !bind.blend) {
        BlendDesc bd{};
        BuildBlendDesc(rs, bd);
        perf.descBuilds++;
        auto* bs = dev->PSO()->GetOrCreateBlendState(bd);
        if (!bs) {

            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                DXLOG_WARN("[dx9to11] blend state could not be created - this "
                           "draw inherits the previous draw's blend mode");
            }
        }
        if (bs && (BackendSelect::NoBindCache() || bs != bind.blend)) {
            const FLOAT blendFactor[4] = { 1.f, 1.f, 1.f, 1.f };
            ctx->OMSetBlendState(bs, blendFactor, 0xFFFFFFFF);
            bind.blend = bs;
            perf.stateBinds++;
        }
        rst.ClearDirty(RenderStateTracker::kDirtyBlend);
    }
    if ((dirty & RenderStateTracker::kDirtyRast) || !bind.rast) {
        RastDesc rd{};
        BuildRastDesc(rs, rd);
        perf.descBuilds++;
        auto* rs11 = dev->PSO()->GetOrCreateRasterizerState(rd);
        if (!rs11) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                DXLOG_WARN("[dx9to11] rasterizer state could not be created - this "
                           "draw inherits the previous draw's rasterizer state");
            }
        }
        if (rs11 && (BackendSelect::NoBindCache() || rs11 != bind.rast)) {
            ctx->RSSetState(rs11);
            bind.rast = rs11;
            perf.stateBinds++;
        }
        rst.ClearDirty(RenderStateTracker::kDirtyRast);
    }
    if ((dirty & RenderStateTracker::kDirtyDepthStencil) || !bind.depthStencil ||
        dev->StencilRef() != bind.stencilRef) {
        DepthStencilDesc dsd{};
        BuildDepthStencilDesc(rs, dsd);
        perf.descBuilds++;
        auto* ds = dev->PSO()->GetOrCreateDepthStencilState(dsd);
        if (!ds) {
            static bool s_warned = false;
            if (!s_warned) {
                s_warned = true;
                DXLOG_WARN("[dx9to11] depth-stencil state could not be created - this "
                           "draw inherits the previous draw's depth/stencil state");
            }
        }
        if (ds && (BackendSelect::NoBindCache() ||
                   ds != bind.depthStencil || dev->StencilRef() != bind.stencilRef)) {
            ctx->OMSetDepthStencilState(ds, dev->StencilRef());
            bind.depthStencil = ds;
            bind.stencilRef   = dev->StencilRef();
            perf.stateBinds++;
        }
        rst.ClearDirty(RenderStateTracker::kDirtyDepthStencil);
    }

    {
        ID3D11RenderTargetView* rtvs[4] = {};
        ID3D11DepthStencilView* dsv = nullptr;

        const bool srgbWrite = rs.rs[D3DRS_SRGBWRITEENABLE] != 0;
        for (int i = 0; i < 4; ++i) {
            D9Surface* rt = dev->GetRenderTarget(i);
            if (rt) rtvs[i] = rt->RTV(srgbWrite);
        }
        D9Surface* ds = dev->GetDepthStencil();
        if (ds) dsv = ds->DSV();

        // ── Depth-stencil detachment for 2-D overlay draws ──────────────────
        //
        // A D3D9 game leaves the depth-stencil surface bound for its entire
        // frame and turns depth testing off with a render state when it draws
        // its interface on top. That works there because the render states are
        // what the driver consults; the binding on its own does nothing.
        //
        // Under D3D11 a bound depth-stencil view is not inert in the same way.
        // Leaving the world's depth buffer attached while drawing a heads-up
        // display exposes that draw to whatever is still in the buffer:
        // fragments get rejected by depth values from geometry drawn minutes
        // earlier in the frame, or by stencil bits left behind by an earlier
        // shadow pass. The four rules below detect draws that provably cannot
        // be using the buffer and unbind it for the duration.
        //
        // Each rule looks for a different way of saying "this draw does not
        // use depth", because games express it differently — some disable
        // testing outright, some leave it enabled but set a comparison that
        // always passes, some rely on the geometry already being in screen
        // space. The last rule is different in kind: it works around an API
        // incompatibility rather than a game idiom.
        const bool depthTests   = static_cast<D3DZBUFFERTYPE>(rs.rs[D3DRS_ZENABLE]) != D3DZB_FALSE;
        const bool depthWrites  = rs.rs[D3DRS_ZWRITEENABLE] != 0;

        // A stencil op left at KEEP on every face cannot modify the buffer, so
        // an "enabled" stencil that only KEEPs is not a write for our purposes.
        auto stencilKeeps = [&](D3DRENDERSTATETYPE op) noexcept {
            return static_cast<D3DSTENCILOP>(rs.rs[op]) == D3DSTENCILOP_KEEP;
        };
        const bool stencilWrites =
            rs.rs[D3DRS_STENCILENABLE] &&
            (!stencilKeeps(D3DRS_STENCILFAIL)  ||
             !stencilKeeps(D3DRS_STENCILZFAIL) ||
             !stencilKeeps(D3DRS_STENCILPASS)  ||
             (rs.rs[D3DRS_TWOSIDEDSTENCILMODE] &&
              (!stencilKeeps(D3DRS_CCW_STENCILFAIL)  ||
               !stencilKeeps(D3DRS_CCW_STENCILZFAIL) ||
               !stencilKeeps(D3DRS_CCW_STENCILPASS))));

        // Rule 1: depth testing off, depth writes off, and stencil either
        // disabled or set to KEEP everywhere. The draw cannot read or write
        // the buffer by any route, so binding it can only do harm.
        if (dsv && !depthTests && !depthWrites && !stencilWrites)
            dsv = nullptr;

        // Rule 2: the same situation written differently. Depth testing is
        // nominally enabled, but the comparison is ALWAYS and writes are off,
        // so every fragment passes and nothing is recorded. The depth buffer
        // contributes nothing — yet leaving it bound still subjects the draw
        // to stencil state left over from earlier passes.
        if (dsv && depthTests && !depthWrites && !stencilWrites) {
            const auto zfunc = static_cast<D3DCMPFUNC>(rs.rs[D3DRS_ZFUNC]);
            if (zfunc == D3DCMP_ALWAYS)
                dsv = nullptr;
        }

        // Rule 3: pre-transformed geometry (D3DFVF_XYZRHW) that does not write
        // depth. Vertices like these are already in screen space and carry no
        // meaningful depth — in practice their z lands at the far plane. A
        // LESSEQUAL test against a buffer that already contains sky or foliage
        // therefore rejects the entire quad, which is how a perfectly correct
        // interface draw ends up invisible.
        if (dsv && isPosTDraw && !depthWrites)
            dsv = nullptr;

        // Rule 4 is not a heuristic but an API incompatibility.
        //
        // D3D9 allows the depth-stencil surface to be larger than the render
        // target and simply ignores the excess. D3D11 requires them to match,
        // and — importantly — rejects the *whole* OMSetRenderTargets call when
        // they do not, binding neither the colour target nor the depth target.
        // The consequence is out of all proportion to the cause: one
        // mismatched pair leaves nothing bound, and every draw from that point
        // on renders into nowhere. Dropping the oversized depth view keeps the
        // colour target bound, which is much closer to what D3D9 would do.
        if (dsv) {
            D9Surface* rt0dim = dev->GetRenderTarget(0);
            D9Surface* dsdim  = dev->GetDepthStencil();
            if (rt0dim && dsdim) {
                const auto& rd = rt0dim->Info();
                const auto& dd = dsdim->Info();
                if (rd.width != dd.width || rd.height != dd.height) {
                    dsv = nullptr;
                    static bool s_mismatchWarned = false;
                    if (!s_mismatchWarned) {
                        s_mismatchWarned = true;
                        DXLOG_WARN("RTV/DSV size mismatch (RT %ux%u vs DS %ux%u) - "
                                   "detaching oversized DSV so the offscreen target "
                                   "renders; D3D9 allowed DS >= RT, D3D11 does not",
                                   rd.width, rd.height, dd.width, dd.height);
                    }
                }
            }
        }

        if (bind.rtDirty ||
            dsv != bind.dsv ||
            std::memcmp(rtvs, bind.rtv, sizeof(rtvs)) != 0) {

            for (int i = 0; i < 4; ++i) {
                if (!rtvs[i]) continue;
                ID3D11Resource* rtRes = nullptr;
                rtvs[i]->GetResource(&rtRes);
                if (!rtRes) continue;
                for (DWORD s = 0; s < 8; ++s) {
                    if (!bind.srv[s]) continue;
                    ID3D11Resource* srvRes = nullptr;
                    bind.srv[s]->GetResource(&srvRes);
                    if (srvRes == rtRes) {
                        ID3D11ShaderResourceView* nullSrv = nullptr;
                        ctx->PSSetShaderResources(s, 1, &nullSrv);
                        bind.srv[s] = nullptr;
                        bind.srvResource[s] = nullptr;
                    }
                    if (srvRes) srvRes->Release();
                }
                rtRes->Release();
            }

            {

                static unsigned s_compositeTriageCount = 0;
                bool anySrvBound = false;
                for (DWORD s = 0; s < 8; ++s) {
                    if (bind.srv[s]) { anySrvBound = true; break; }
                }
                if (anySrvBound && s_compositeTriageCount < 500u) {
                    ++s_compositeTriageCount;

                DXLOG_TRACE("composite-triage#%u: ZEnable=%u ZWriteEnable=%u ZFunc=%u | "
                            "AlphaBlend=%u Src=%u Dst=%u SrcA=%u DstA=%u | CW=%u",
                            s_compositeTriageCount,
                            (unsigned)(static_cast<D3DZBUFFERTYPE>(rs.rs[D3DRS_ZENABLE]) != D3DZB_FALSE ? 1 : 0),
                            (unsigned)(rs.rs[D3DRS_ZWRITEENABLE] ? 1 : 0),
                            (unsigned)rs.rs[D3DRS_ZFUNC],
                            (unsigned)rs.rs[D3DRS_ALPHABLENDENABLE],
                            (unsigned)rs.rs[D3DRS_SRCBLEND],
                            (unsigned)rs.rs[D3DRS_DESTBLEND],
                            (unsigned)rs.rs[D3DRS_SRCBLENDALPHA],
                            (unsigned)rs.rs[D3DRS_DESTBLENDALPHA],
                            (unsigned)rs.rs[D3DRS_COLORWRITEENABLE]);
                unsigned rt0w = 0, rt0h = 0;
                if (rtvs[0]) {
                    ID3D11Resource* r0 = nullptr;
                    rtvs[0]->GetResource(&r0);
                    if (r0) {
                        ComPtr<ID3D11Texture2D> t2d0;
                        if (SUCCEEDED(r0->QueryInterface(IID_PPV_ARGS(t2d0.GetAddressOf())))) {
                            D3D11_TEXTURE2D_DESC td0{};
                            t2d0->GetDesc(&td0);
                            rt0w = td0.Width; rt0h = td0.Height;
                        }
                        r0->Release();
                    }
                }
                {

                    if (dsv) {
                        ID3D11Resource* dsRes = nullptr;
                        dsv->GetResource(&dsRes);
                        if (dsRes) {
                            D3D11_TEXTURE2D_DESC dtd{};
                            ComPtr<ID3D11Texture2D> dt2d;
                            if (SUCCEEDED(dsRes->QueryInterface(IID_PPV_ARGS(dt2d.GetAddressOf()))))
                                dt2d->GetDesc(&dtd);
                            DXLOG_TRACE("composite-triage#%u: RT0=%ux%u DSV=%ux%u fmt=%u",
                                        s_compositeTriageCount, rt0w, rt0h, dtd.Width, dtd.Height, (unsigned)dtd.Format);
                            dsRes->Release();
                        }
                    } else {
                        DXLOG_TRACE("composite-triage#%u: RT0=%ux%u DSV=none", s_compositeTriageCount, rt0w, rt0h);
                    }
                    for (DWORD s = 0; s < 8; ++s) {
                        if (!bind.srv[s]) continue;
                        ID3D11Resource* r = nullptr;
                        bind.srv[s]->GetResource(&r);
                        if (!r) continue;
                        D3D11_TEXTURE2D_DESC td{};
                        ComPtr<ID3D11Texture2D> t2d;
                        if (SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(t2d.GetAddressOf()))))
                            t2d->GetDesc(&td);
                        DXLOG_TRACE("composite-triage#%u: RT0=%ux%u slot %u bound %ux%u fmt=%u",
                                    s_compositeTriageCount, rt0w, rt0h, (unsigned)s, td.Width, td.Height, (unsigned)td.Format);
                        r->Release();
                    }
                }
                    if (rt0w == 0 && rt0h == 0) {
                        DXLOG_TRACE("composite-triage: RT0=none (all slots null after unbind)");
                    }
                }
            }

            ctx->OMSetRenderTargets(4, rtvs, dsv);
            std::memcpy(bind.rtv, rtvs, sizeof(rtvs));
            bind.dsv     = dsv;
            bind.rtDirty = false;
            perf.rtSets++;
        }
    }

    {
        const UINT8 sampDirty = rst.SamplerDirty();
        for (DWORD s = 0; s < 8; ++s) {
            const bool srgbSample = rs.samp[s][D3DSAMP_SRGBTEXTURE] != 0;
            ID3D11ShaderResourceView* srv = GetSRVFromBaseTexture(rs.textures[s], srgbSample);
            if (BackendSelect::NoBindCache() || srv != bind.srv[s]) {
                ctx->PSSetShaderResources(s, 1, &srv);
                bind.srv[s] = srv;

                ID3D11Resource* srvRes = nullptr;
                if (srv) { srv->GetResource(&srvRes); if (srvRes) srvRes->Release(); }
                bind.srvResource[s] = srvRes;
                perf.srvSets++;
            }

            if (hasGamePS && rs.textures[s]) {
                const D3DRESOURCETYPE trt = rs.textures[s]->GetType();
                if (trt == D3DRTYPE_CUBETEXTURE || trt == D3DRTYPE_VOLUMETEXTURE) {
                    static uint8_t s_srvDimSeen = 0;
                    auto* d9psDiag = static_cast<D9PixelShader*>(rs.pixelShader);
                    uint32_t ver = 0;
                    if (d9psDiag && d9psDiag->BytecodeSize() >= 4)
                        std::memcpy(&ver, d9psDiag->Bytecode(), 4);
                    const unsigned major = (ver >> 8) & 0xFu;
                    const uint8_t bit = static_cast<uint8_t>(
                        1u << (((major & 3u) * 2u) +
                               (trt == D3DRTYPE_VOLUMETEXTURE ? 1u : 0u)));
                    if (!(s_srvDimSeen & bit)) {
                        s_srvDimSeen = static_cast<uint8_t>(s_srvDimSeen | bit);
                        DXLOG_INFO("SRV-dim triage: %s texture bound to slot %u under "
                                   "ps_%u_%u (token 0x%08X) - shader likely declares Texture2D here",
                                   (trt == D3DRTYPE_CUBETEXTURE ? "CUBE" : "VOLUME"),
                                   (unsigned)s, major, (ver & 0xFu), ver);
                    }
                }
            }

            if (rs.textures[s] && ((sampDirty & (1u << s)) || !bind.samp[s])) {
                SamplerDesc sd{};
                BuildSamplerDesc(rs, s, sd);
                auto* samp = dev->PSO()->GetOrCreateSamplerState(sd);
                if (samp && (BackendSelect::NoBindCache() || samp != bind.samp[s])) {
                    ctx->PSSetSamplers(s, 1, &samp);
                    bind.samp[s] = samp;
                    perf.sampSets++;
                }
            }
        }
        rst.ClearSamplerDirty();
    }

    if (hasGameVS || hasGamePS)
        dev->FlushEmuCB();
    if (hasGameVS) {
        dev->ConstantMapperPtr()->FlushVS();
        if (BackendSelect::NoBindCache() || bind.b1OwnerVS != BindCache::kB1Emu) {
            ID3D11Buffer* emuCB = dev->EmuCB();
            if (emuCB) ctx->VSSetConstantBuffers(1, 1, &emuCB);
            bind.b1OwnerVS = BindCache::kB1Emu;
        }
    }
    if (hasGamePS) {
        dev->ConstantMapperPtr()->FlushPS();
        if (BackendSelect::NoBindCache() || bind.b1OwnerPS != BindCache::kB1Emu) {
            ID3D11Buffer* emuCB = dev->EmuCB();
            if (emuCB) ctx->PSSetConstantBuffers(1, 1, &emuCB);
            bind.b1OwnerPS = BindCache::kB1Emu;
        }
    }

    if (overrideVB) {

        UINT stride = overrideVBStride;
        UINT offset = overrideVBOffset;
        ctx->IASetVertexBuffers(0, 1, &overrideVB, &stride, &offset);
        bind.vb[0] = overrideVB; bind.vbStride[0] = stride; bind.vbOffset[0] = offset;
        perf.vbSets++;
    } else {
        for (UINT i = 0; i < 16; ++i) {
            if (!rs.streams[i]) continue;
            auto* vb = static_cast<D9VertexBuffer*>(rs.streams[i]);
            ID3D11Buffer* buf = vb->D11Buffer();
            UINT stride = rs.streamStrides[i];
            UINT offset = rs.streamOffsets[i] + vb->BindOffset();
            if (BackendSelect::NoBindCache() ||
                buf != bind.vb[i] || stride != bind.vbStride[i] || offset != bind.vbOffset[i]) {
                ctx->IASetVertexBuffers(i, 1, &buf, &stride, &offset);
                bind.vb[i] = buf; bind.vbStride[i] = stride; bind.vbOffset[i] = offset;
                perf.vbSets++;
            }
        }
    }

    if (!rs.streams[kZeroStreamSlot]) {
        ID3D11Buffer* zvb = dev->ZeroVB();
        if (zvb && (BackendSelect::NoBindCache() ||
                    zvb != bind.vb[kZeroStreamSlot] ||
                    bind.vbStride[kZeroStreamSlot] != 0 ||
                    bind.vbOffset[kZeroStreamSlot] != 0)) {
            UINT zstride = 0, zoffset = 0;
            ctx->IASetVertexBuffers(kZeroStreamSlot, 1, &zvb, &zstride, &zoffset);
            bind.vb[kZeroStreamSlot]       = zvb;
            bind.vbStride[kZeroStreamSlot] = 0;
            bind.vbOffset[kZeroStreamSlot] = 0;
            perf.vbSets++;
        }
    }

    if (isIndexed) {
        ID3D11Buffer* ibuf = nullptr; DXGI_FORMAT ifmt = DXGI_FORMAT_UNKNOWN; UINT ioff = 0;
        if (overrideIB) {
            ibuf = overrideIB; ifmt = overrideIBFmt; ioff = overrideIBOffset;
        } else if (rs.indexBuffer) {
            auto* ib = static_cast<D9IndexBuffer*>(rs.indexBuffer);
            ibuf = ib->D11Buffer(); ifmt = ib->DxgiFormat(); ioff = ib->BindOffset();
        }
        if (ibuf && (BackendSelect::NoBindCache() ||
                     ibuf != bind.ib || ifmt != bind.ibFmt || ioff != bind.ibOffset)) {
            ctx->IASetIndexBuffer(ibuf, ifmt, ioff);
            bind.ib = ibuf; bind.ibFmt = ifmt; bind.ibOffset = ioff;
            perf.ibSets++;
        }
    }

    {
        const ShaderReflection* refl = nullptr;
        const void*             shaderKey = nullptr;

        if (isFFP) {
            refl      = dev->FFP()->LastReflection();
            shaderKey = refl;
        } else if (rs.vertexShader) {
            auto* d9vs = static_cast<D9VertexShader*>(rs.vertexShader);
            refl      = &d9vs->Reflection();
            shaderKey = d9vs;
        }

        const bool forceBind = BackendSelect::NoBindCache();
        const bool keyHit = !forceBind && bind.layout &&
                            shaderKey == bind.layoutKeyShader &&
                            rs.vertexDecl == bind.layoutKeyDecl &&
                            dev->CurrentFVF() == bind.layoutKeyFVF;

        if (!keyHit && refl && refl->dxbcBlob.empty()) {
            static bool s_emptyBlobWarned = false;
            if (!s_emptyBlobWarned) {
                s_emptyBlobWarned = true;
                DXLOG_WARN("Step8: dxbcBlob EMPTY for %s shader - input layout not bound (black screen root cause)",
                           isFFP ? "FFP" : "programmatic");
            }
        }
        if (!keyHit && refl && !refl->dxbcBlob.empty()) {
            const D3DVERTEXELEMENT9* pElems = nullptr;
            D3DVERTEXELEMENT9 fvfBuf[MAXD3DDECLLENGTH + 1];

            if (rs.vertexDecl) {
                pElems = static_cast<D9VertexDecl*>(rs.vertexDecl)->Elements();
            } else {
                DWORD fvf = dev->CurrentFVF();
                if (fvf) {
                    FFPEmulator::ExpandFVF(fvf, fvfBuf);
                    pElems = fvfBuf;
                }
            }

            static const D3DVERTEXELEMENT9 kEndOnlyDecl = D3DDECL_END();
            if (!pElems) pElems = &kEndOnlyDecl;

            {
                ID3D11InputLayout* layout = nullptr;
                HRESULT hr = dev->InputLayoutCachePtr()->GetOrCreate(pElems, *refl, &layout);
                if (FAILED(hr) || !layout) {

                    static unsigned s_ilFails = 0;
                    if (s_ilFails < 8u) {
                        ++s_ilFails;
                        UINT declCount = 0;
                        while (pElems[declCount].Stream != 0xFF &&
                               declCount <= MAXD3DDECLLENGTH) ++declCount;
                        DXLOG_WARN("[dx9to11] INPUT LAYOUT FAILED (hr=0x%08X) #%u: "
                                   "%s shader, %u decl elements, %zu signature inputs "
                                   "- this draw keeps the PREVIOUS draw's input layout, "
                                   "so its vertex attributes are read at the wrong offsets",
                                   (unsigned)hr, s_ilFails,
                                   isFFP ? "FFP" : "programmable",
                                   declCount, refl->inputElements.size());
                        for (UINT e = 0; e < declCount && e < 12u; ++e)
                            DXLOG_WARN("[dx9to11]   decl[%u] stream=%u offset=%u "
                                       "type=%u usage=%u usageIndex=%u",
                                       e, pElems[e].Stream, pElems[e].Offset,
                                       pElems[e].Type, pElems[e].Usage,
                                       pElems[e].UsageIndex);
                    }
                }
                if (SUCCEEDED(hr) && layout) {
                    if (forceBind || layout != bind.layout) {
                        ctx->IASetInputLayout(layout);
                        perf.layoutSets++;
                    }
                    bind.layout          = layout;
                    bind.layoutKeyShader = shaderKey;
                    bind.layoutKeyDecl   = rs.vertexDecl;
                    bind.layoutKeyFVF    = dev->CurrentFVF();
                    layout->Release();
                }
            }
        }
    }

    {
        const D3D11_PRIMITIVE_TOPOLOGY topo = TranslateTopology(primType);
        if (BackendSelect::NoBindCache() || topo != bind.topo) {
            ctx->IASetPrimitiveTopology(topo);
            bind.topo = topo;
            perf.topoSets++;
        }
    }

    {
        const D3DVIEWPORT9& vp9 = dev->Viewport();
        D3D11_VIEWPORT vp11{};
        vp11.TopLeftX = static_cast<float>(vp9.X);
        vp11.TopLeftY = static_cast<float>(vp9.Y);
        vp11.Width    = static_cast<float>(vp9.Width);
        vp11.Height   = static_cast<float>(vp9.Height);
        vp11.MinDepth = vp9.MinZ;
        vp11.MaxDepth = vp9.MaxZ;
        if (!bind.vpValid || std::memcmp(&vp11, &bind.vp, sizeof(vp11)) != 0) {
            ctx->RSSetViewports(1, &vp11);
            bind.vp = vp11;
            bind.vpValid = true;
            perf.vpSets++;

            if (D9Surface* rt0vp = dev->GetRenderTarget(0)) {
                const auto& d = rt0vp->Info();
                DXLOG_TRACE("VP-triage: viewport %.0f,%.0f %ux%u z[%.2f..%.2f] | RT0 %ux%u",
                            vp11.TopLeftX, vp11.TopLeftY,
                            (unsigned)vp11.Width, (unsigned)vp11.Height,
                            vp11.MinDepth, vp11.MaxDepth, d.width, d.height);
            } else {
                DXLOG_TRACE("VP-triage: viewport %.0f,%.0f %ux%u z[%.2f..%.2f] | RT0 unbound",
                            vp11.TopLeftX, vp11.TopLeftY,
                            (unsigned)vp11.Width, (unsigned)vp11.Height,
                            vp11.MinDepth, vp11.MaxDepth);
            }
        }

        if (rs.rs[D3DRS_SCISSORTESTENABLE]) {
            const RECT& sr = dev->ScissorRect();
            if (BackendSelect::NoBindCache() || !bind.scissorValid ||
                std::memcmp(&sr, &bind.scissor, sizeof(sr)) != 0) {
                D3D11_RECT r = { sr.left, sr.top, sr.right, sr.bottom };
                ctx->RSSetScissorRects(1, &r);
                bind.scissor = sr;
                bind.scissorValid = true;

                DXLOG_TRACE("VP-triage: scissor [%ld,%ld .. %ld,%ld] ENABLED",
                            (long)sr.left, (long)sr.top, (long)sr.right, (long)sr.bottom);
            }
        }
    }

    {
        static const bool     s_census    = BackendSelect::DumpFrameEnabled();
        static const unsigned s_censusCap = BackendSelect::CensusLimit();
        if (s_census) {
            static uint32_t s_frame      = 0xFFFFFFFFu;
            static unsigned s_worldDraws = 0;
            static unsigned s_lineN      = 0;
            static unsigned s_framesDone = 0;
            const uint32_t  f = perf.frames;
            if (f != s_frame) {
                if (s_frame != 0xFFFFFFFFu && s_lineN) ++s_framesDone;
                s_frame = f; s_worldDraws = 0; s_lineN = 0;
            }
            const bool zOn =
                static_cast<D3DZBUFFERTYPE>(rs.rs[D3DRS_ZENABLE]) != D3DZB_FALSE;
            if (zOn) ++s_worldDraws;

            static bool s_gameplay = false;
            if (s_worldDraws >= 100u) s_gameplay = true;
            if (s_gameplay && s_framesDone < 6u && s_lineN < s_censusCap) {
                ++s_lineN;
                auto kind = [](IDirect3DBaseTexture9* t) -> const char* {
                    if (!t) return "none";
                    switch (t->GetType()) {
                        case D3DRTYPE_CUBETEXTURE:   return "CUBE";
                        case D3DRTYPE_VOLUMETEXTURE: return "VOL";
                        default:                     return "2D";
                    }
                };
                uint32_t vtok = 0, ptok = 0;
                if (auto* v = static_cast<D9VertexShader*>(rs.vertexShader))
                    if (v->BytecodeSize() >= 4) std::memcpy(&vtok, v->Bytecode(), 4);
                if (auto* p = static_cast<D9PixelShader*>(rs.pixelShader))
                    if (p->BytecodeSize() >= 4) std::memcpy(&ptok, p->Bytecode(), 4);
                unsigned rtw = 0, rth = 0; bool rt0BB = false;
                if (D9Surface* rt0 = dev->GetRenderTarget(0)) {
                    const auto& d = rt0->Info(); rtw = d.width; rth = d.height;

                    IDirect3DSurface9* bb = nullptr;
                    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
                        rt0BB = (static_cast<D9Surface*>(bb)->Texture() == rt0->Texture());
                        bb->Release();
                    }
                }
                unsigned dsw = 0, dsh = 0;
                if (auto* ds0 = dev->GetDepthStencil()) {
                    const auto& d = ds0->Info(); dsw = d.width; dsh = d.height;
                }

                void*    s0tex = static_cast<void*>(rs.textures[0]);
                unsigned s0w = 0, s0h = 0, s0fmt = 0;
                if (bind.srv[0]) {
                    ID3D11Resource* r = nullptr;
                    bind.srv[0]->GetResource(&r);
                    if (r) {
                        ComPtr<ID3D11Texture2D> t2d;
                        if (SUCCEEDED(r->QueryInterface(IID_PPV_ARGS(t2d.GetAddressOf())))) {
                            D3D11_TEXTURE2D_DESC td{};
                            t2d->GetDesc(&td);
                            s0w = td.Width; s0h = td.Height; s0fmt = (unsigned)td.Format;
                        }
                        r->Release();
                    }
                }
                DXLOG_WARN("census#%u f%u %s vs=0x%08X ps=0x%08X s0=%s s1=%s "
                           "s0tex=%p s0dim=%ux%u s0fmt=%u "
                           "RT0=%ux%u RT0bb=%u blend=%u cw=0x%X atest=%u "
                           "zen=%u zw=%u zfunc=%u sten=%u sref=%u sfunc=%u DS=%ux%u",
                           s_lineN, f, (rs.vertexShader ? "prog" : "FFP"), vtok, ptok,
                           kind(rs.textures[0]), kind(rs.textures[1]),
                           s0tex, s0w, s0h, s0fmt, rtw, rth,
                           (unsigned)(rt0BB ? 1 : 0),
                           (unsigned)(rs.rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0),
                           (unsigned)rs.rs[D3DRS_COLORWRITEENABLE],
                           (unsigned)(rs.rs[D3DRS_ALPHATESTENABLE] ? 1 : 0),
                           (unsigned)(zOn ? 1 : 0),
                           (unsigned)(rs.rs[D3DRS_ZWRITEENABLE] ? 1 : 0),
                           (unsigned)rs.rs[D3DRS_ZFUNC],
                           (unsigned)(rs.rs[D3DRS_STENCILENABLE] ? 1 : 0),
                           (unsigned)rs.rs[D3DRS_STENCILREF],
                           (unsigned)rs.rs[D3DRS_STENCILFUNC],
                           dsw, dsh);
            }
        }
    }

    perf.drawsOk++;
    return S_OK;
}

static void IssueDraw(
    ID3D11DeviceContext1* ctx,
    bool                  isIndexed,
    UINT                  vertexOrIndexCount,
    UINT                  startVertexOrIndex,
    INT                   baseVertex) noexcept
{
    if (isIndexed)
        ctx->DrawIndexed(vertexOrIndexCount, startVertexOrIndex, baseVertex);
    else
        ctx->Draw(vertexOrIndexCount, startVertexOrIndex);
}

HRESULT STDMETHODCALLTYPE D9Device::BeginScene() { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::EndScene()   { return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::Clear(
    DWORD Count, CONST D3DRECT* pRects,
    DWORD Flags, D3DCOLOR Color,
    float Z, DWORD Stencil)
{

    if ((Count == 0) != (pRects == nullptr))
        return D3DERR_INVALIDCALL;

    if ((Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && !m_depthStencil)
        return D3DERR_INVALIDCALL;

    auto* ctx = m_ctx->Context();

    const LONG vpL = static_cast<LONG>(m_viewport.X);
    const LONG vpT = static_cast<LONG>(m_viewport.Y);
    const LONG vpR = static_cast<LONG>(m_viewport.X + m_viewport.Width);
    const LONG vpB = static_cast<LONG>(m_viewport.Y + m_viewport.Height);

    const auto coversSurface = [&](const D9Surface* s) noexcept -> bool {
        const auto& d = s->Info();
        return vpL <= 0 && vpT <= 0 &&
               vpR >= static_cast<LONG>(d.width) &&
               vpB >= static_cast<LONG>(d.height);
    };

    D3D11_RECT rects[16];
    const auto buildRects = [&](DWORD first, DWORD maxN) noexcept -> UINT {
        UINT n = 0;
        if (Count == 0) {
            rects[n++] = { vpL, vpT, vpR, vpB };
            return n;
        }
        for (DWORD i = first; i < Count && n < maxN; ++i) {
            const D3DRECT& r = pRects[i];
            D3D11_RECT out{
                (r.x1 > vpL) ? r.x1 : vpL,
                (r.y1 > vpT) ? r.y1 : vpT,
                (r.x2 < vpR) ? r.x2 : vpR,
                (r.y2 < vpB) ? r.y2 : vpB,
            };
            if (out.left < out.right && out.top < out.bottom)
                rects[n++] = out;
        }
        return n;
    };

    if (Flags & D3DCLEAR_TARGET) {

        float rgba[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,
            ((Color >>  8) & 0xFF) / 255.0f,
            ((Color      ) & 0xFF) / 255.0f,
            ((Color >> 24) & 0xFF) / 255.0f,
        };

        for (int i = 0; i < 4; ++i) {
            D9Surface* rt = GetRenderTarget(i);
            if (!rt || !rt->RTV())
                continue;
            if (Count == 0 && coversSurface(rt)) {
                ctx->ClearRenderTargetView(rt->RTV(), rgba);
                continue;
            }

            for (DWORD first = 0; ; first += 16) {
                const UINT n = buildRects(first, 16);
                if (n > 0)
                    ctx->ClearView(rt->RTV(), rgba, rects, n);
                if (Count == 0 || first + 16 >= Count)
                    break;
            }
        }
    }

    if (Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) {
        D9Surface* ds = GetDepthStencil();
        if (ds && ds->DSV()) {
            UINT clearFlags = 0;
            if (Flags & D3DCLEAR_ZBUFFER)  clearFlags |= D3D11_CLEAR_DEPTH;
            if (Flags & D3DCLEAR_STENCIL)  clearFlags |= D3D11_CLEAR_STENCIL;

            if (!m_partialDepthClearWarned &&
                (Count != 0 || !coversSurface(ds))) {
                m_partialDepthClearWarned = true;
                OutputDebugStringA("[dx9to11] Clear: partial depth/stencil "
                                   "clear approximated as full-surface clear "
                                   "(D3D11 limitation) - watch for artefacts\n");
            }
            ctx->ClearDepthStencilView(ds->DSV(), clearFlags, Z, static_cast<UINT8>(Stencil));
        }
    }

    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetTransform(D3DTRANSFORMSTATETYPE state, D3DMATRIX* p)
{
    if (!p) return D3DERR_INVALIDCALL;
    m_ffp->GetTransform(state, p);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE, CONST D3DMATRIX*)
{ return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::GetLight(DWORD idx, D3DLIGHT9* p)
{

    if (!p || idx >= 8) return D3DERR_INVALIDCALL;
    m_ffp->GetLight(idx, p);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetLightEnable(DWORD idx, BOOL* p)
{ if (!p) return D3DERR_INVALIDCALL; *p = m_ffp->GetLightEnabled(idx); return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::SetClipPlane(DWORD Index, CONST float* pPlane)
{
    if (Index >= 6 || !pPlane) return D3DERR_INVALIDCALL;
    std::memcpy(m_clipPlanes[Index], pPlane, sizeof(float) * 4);
    m_emuDirty = true;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetClipPlane(DWORD Index, float* pPlane)
{
    if (Index >= 6 || !pPlane) return D3DERR_INVALIDCALL;
    std::memcpy(pPlane, m_clipPlanes[Index], sizeof(float) * 4);
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type,
                                                      IDirect3DStateBlock9** ppSB)
{
    if (!ppSB) return D3DERR_INVALIDCALL;
    if (m_recordingBlock) return D3DERR_INVALIDCALL;
    D9StateBlock* sb = nullptr;
    HRESULT hr = D9StateBlock::CreatePreset(this, Type, &sb);
    if (FAILED(hr)) return hr;
    *ppSB = sb;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::BeginStateBlock()
{
    if (m_recordingBlock) return D3DERR_INVALIDCALL;
    return D9StateBlock::CreateRecording(this, &m_recordingBlock);
}

HRESULT STDMETHODCALLTYPE D9Device::EndStateBlock(IDirect3DStateBlock9** ppSB)
{
    if (!m_recordingBlock) return D3DERR_INVALIDCALL;
    if (!ppSB) { m_recordingBlock->Release(); m_recordingBlock = nullptr; return D3DERR_INVALIDCALL; }
    m_recordingBlock->EndRecording();
    *ppSB            = m_recordingBlock;
    m_recordingBlock = nullptr;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::ValidateDevice(DWORD* pNumPasses)
{ if (pNumPasses) *pNumPasses = 1; return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::SetPaletteEntries(UINT, CONST PALETTEENTRY*) { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::GetPaletteEntries(UINT, PALETTEENTRY* p)
{ if (p) std::memset(p, 0, sizeof(PALETTEENTRY) * 256); return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::SetCurrentTexturePalette(UINT n) { m_currentPalette = n; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::GetCurrentTexturePalette(UINT* p) { if (p) *p = m_currentPalette; return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::SetScissorRect(CONST RECT* pRect)
{ if (!pRect) return D3DERR_INVALIDCALL; if (m_recordingBlock) m_recordingBlock->RecordScissorRect(pRect); m_scissorRect = *pRect; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::GetScissorRect(RECT* pRect)
{ if (!pRect) return D3DERR_INVALIDCALL; *pRect = m_scissorRect; return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::SetSoftwareVertexProcessing(BOOL b) { m_softwareVP = b; return D3D_OK; }
BOOL    STDMETHODCALLTYPE D9Device::GetSoftwareVertexProcessing()        { return m_softwareVP; }
HRESULT STDMETHODCALLTYPE D9Device::SetNPatchMode(float n) { m_nPatchMode = n; return D3D_OK; }
float   STDMETHODCALLTYPE D9Device::GetNPatchMode()        { return m_nPatchMode; }

HRESULT STDMETHODCALLTYPE D9Device::SetClipStatus(CONST D3DCLIPSTATUS9* p) { if (p) m_clipStatus = *p; return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::GetClipStatus(D3DCLIPSTATUS9* p) { if (p) *p = m_clipStatus; return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::SetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData,
    UINT OffsetInBytes, UINT Stride)
{
    if (StreamNumber >= 16) return D3DERR_INVALIDCALL;
    if (StreamNumber == kZeroStreamSlot && pStreamData) {

        static bool warnedZeroSlot = false;
        if (!warnedZeroSlot) {
            warnedZeroSlot = true;
            DXLOG_WARN("game bound a real vertex stream to slot %u, reserved as "
                       "the zero stream for absent VS inputs; padded inputs on "
                       "affected draws will read this buffer, not zero",
                       kZeroStreamSlot);
        }
    }
    if (m_recordingBlock) {
        m_recordingBlock->RecordStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    if (pStreamData) pStreamData->AddRef();
    auto& slot = m_rst.State().streams[StreamNumber];
    if (slot) slot->Release();
    slot = pStreamData;
    m_rst.State().streamStrides[StreamNumber] = Stride;
    m_rst.State().streamOffsets[StreamNumber] = OffsetInBytes;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetStreamSource(
    UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData,
    UINT* pOffset, UINT* pStride)
{
    if (StreamNumber >= 16 || !ppStreamData) return D3DERR_INVALIDCALL;
    *ppStreamData = m_rst.State().streams[StreamNumber];
    if (*ppStreamData) (*ppStreamData)->AddRef();
    if (pOffset) *pOffset = m_rst.State().streamOffsets[StreamNumber];
    if (pStride) *pStride = m_rst.State().streamStrides[StreamNumber];
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::SetStreamSourceFreq(UINT, UINT) { return D3D_OK; }
HRESULT STDMETHODCALLTYPE D9Device::GetStreamSourceFreq(UINT, UINT* p) { if (p) *p = 1; return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData)
{
    if (m_recordingBlock) {
        m_recordingBlock->RecordIndices(pIndexData);

        if (BackendSelect::StateBlockRecordOnly()) return D3D_OK;
    }
    if (pIndexData) pIndexData->AddRef();
    if (m_rst.State().indexBuffer) m_rst.State().indexBuffer->Release();
    m_rst.State().indexBuffer = pIndexData;
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
{
    if (!ppIndexData) return D3DERR_INVALIDCALL;
    *ppIndexData = m_rst.State().indexBuffer;
    if (*ppIndexData) (*ppIndexData)->AddRef();
    return D3D_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::DrawPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    m_perf.drawCalls++;
    const UINT vertexCount = PrimToVertexCount(PrimitiveType, PrimitiveCount);
    if (vertexCount == 0) return D3DERR_INVALIDCALL;

    if (PrimitiveType == D3DPT_TRIANGLEFAN) {

        DynamicRingBuffer* ring = ResMgr()->FanIndexRing();
        if (!ring) return E_OUTOFMEMORY;

        ID3D11Buffer* fanIB     = nullptr;
        UINT          fanOffset = 0;
        UINT          fanIdxCnt = 0;

        HRESULT hr = ExpandFanIndices(m_ctx->Context(), ring, StartVertex, PrimitiveCount,
                                      &fanIB, &fanOffset, &fanIdxCnt);
        if (FAILED(hr)) return hr;

        hr = PreDrawFlush(this, PrimitiveType,
                          StartVertex, vertexCount,
                            true, 0, 0, fanIdxCnt,
                          nullptr, 0, 0,
                          fanIB, fanOffset, DXGI_FORMAT_R32_UINT);
        if (FAILED(hr)) return hr;

        m_ctx->Context()->DrawIndexed(fanIdxCnt, 0, 0);
        return S_OK;
    }

    HRESULT hr = PreDrawFlush(this, PrimitiveType,
                               StartVertex, vertexCount,
                               false, 0, 0, 0,
                               nullptr, 0, 0, nullptr, 0, DXGI_FORMAT_UNKNOWN);
    if (FAILED(hr)) return hr;
    IssueDraw(m_ctx->Context(), false, vertexCount, StartVertex, 0);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::DrawIndexedPrimitive(
    D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
    UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimCount)
{
    m_perf.drawCalls++;
    (void)MinVertexIndex; (void)NumVertices;

    const UINT idxCount = PrimToVertexCount(PrimitiveType, PrimCount);
    if (idxCount == 0) return D3DERR_INVALIDCALL;

    HRESULT hr = PreDrawFlush(this, PrimitiveType,
                               0, NumVertices,
                               true, StartIndex, BaseVertexIndex, idxCount,
                               nullptr, 0, 0, nullptr, 0, DXGI_FORMAT_UNKNOWN);
    if (FAILED(hr)) return hr;
    IssueDraw(m_ctx->Context(), true, idxCount, StartIndex, BaseVertexIndex);
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::DrawPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
    CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    m_perf.drawCalls++;
    if (!pVertexStreamZeroData) return D3DERR_INVALIDCALL;

    const UINT vCount   = PrimToVertexCount(PrimitiveType, PrimitiveCount);
    if (vCount == 0) return D3DERR_INVALIDCALL;
    const UINT byteSize = vCount * VertexStreamZeroStride;

    DynamicRingBuffer* ring = ResMgr()->VertexRing();
    if (!ring) return E_OUTOFMEMORY;

    RingAllocation vAlloc{};
    HRESULT hr = ring->Map(m_ctx->Context(), byteSize,   false, &vAlloc);
    if (FAILED(hr)) return hr;
    std::memcpy(vAlloc.pData, pVertexStreamZeroData, byteSize);
    ring->Unmap(m_ctx->Context());

    ID3D11Buffer* upVB     = ring->Buffer();
    const UINT    upOffset = vAlloc.offsetInBytes;

    if (PrimitiveType == D3DPT_TRIANGLEFAN) {
        DynamicRingBuffer* fanRing = ResMgr()->FanIndexRing();
        if (!fanRing) return E_OUTOFMEMORY;
        ID3D11Buffer* fanIB     = nullptr;
        UINT          fanOffset = 0;
        UINT          fanIdxCnt = 0;
        hr = ExpandFanIndices(m_ctx->Context(), fanRing, 0, PrimitiveCount, &fanIB, &fanOffset, &fanIdxCnt);
        if (FAILED(hr)) return hr;

        hr = PreDrawFlush(this, PrimitiveType,
                          0, vCount, true, 0, 0, fanIdxCnt,
                          upVB, upOffset, VertexStreamZeroStride,
                          fanIB, fanOffset, DXGI_FORMAT_R32_UINT);
        if (FAILED(hr)) return hr;
        m_ctx->Context()->DrawIndexed(fanIdxCnt, 0, 0);
    } else {
        hr = PreDrawFlush(this, PrimitiveType,
                          0, vCount, false, 0, 0, 0,
                          upVB, upOffset, VertexStreamZeroStride,
                          nullptr, 0, DXGI_FORMAT_UNKNOWN);
        if (FAILED(hr)) return hr;
        IssueDraw(m_ctx->Context(), false, vCount, 0, 0);
    }

    {
        auto& st = m_rst.State();
        if (st.streams[0]) { st.streams[0]->Release(); st.streams[0] = nullptr; }
        st.streamStrides[0] = 0;
        st.streamOffsets[0] = 0;
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::DrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices,
    UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat,
    CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{
    m_perf.drawCalls++;
    if (!pVertexStreamZeroData || !pIndexData) return D3DERR_INVALIDCALL;

    const UINT idxCount  = PrimToVertexCount(PrimitiveType, PrimitiveCount);
    if (idxCount == 0) return D3DERR_INVALIDCALL;
    const UINT idxStride = (IndexDataFormat == D3DFMT_INDEX32) ? 4u : 2u;
    const UINT idxBytes  = idxCount * idxStride;

    const UINT vtxBytes  = NumVertices * VertexStreamZeroStride;
    const auto* vtxSrc   = static_cast<const uint8_t*>(pVertexStreamZeroData)
                         + static_cast<size_t>(MinVertexIndex) * VertexStreamZeroStride;

    DynamicRingBuffer* vring = ResMgr()->VertexRing();
    DynamicRingBuffer* iring = ResMgr()->IndexRing();
    if (!vring || !iring) return E_OUTOFMEMORY;

    RingAllocation vAlloc{};
    HRESULT hr = vring->Map(m_ctx->Context(), vtxBytes,   false, &vAlloc);
    if (FAILED(hr)) return hr;
    std::memcpy(vAlloc.pData, vtxSrc, vtxBytes);
    vring->Unmap(m_ctx->Context());
    ID3D11Buffer* upVB    = vring->Buffer();
    const UINT    upVBOff = vAlloc.offsetInBytes;

    RingAllocation iAlloc{};
    hr = iring->Map(m_ctx->Context(), (idxBytes + 3u) & ~3u,   false, &iAlloc);
    if (FAILED(hr)) return hr;
    std::memcpy(iAlloc.pData, pIndexData, idxBytes);
    iring->Unmap(m_ctx->Context());
    ID3D11Buffer* upIB    = iring->Buffer();
    const UINT    upIBOff = iAlloc.offsetInBytes;

    DXGI_FORMAT ibFmt = (IndexDataFormat == D3DFMT_INDEX32)
                        ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;

    hr = PreDrawFlush(this, PrimitiveType,
                      0, NumVertices, true, 0, static_cast<INT>(MinVertexIndex), idxCount,
                      upVB, upVBOff, VertexStreamZeroStride,
                      upIB, upIBOff, ibFmt);
    if (FAILED(hr)) return hr;

    IssueDraw(m_ctx->Context(), true, idxCount, 0, -static_cast<INT>(MinVertexIndex));

    {
        auto& st = m_rst.State();
        if (st.streams[0]) { st.streams[0]->Release(); st.streams[0] = nullptr; }
        st.streamStrides[0] = 0;
        st.streamOffsets[0] = 0;
        if (st.indexBuffer) { st.indexBuffer->Release(); st.indexBuffer = nullptr; }
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE D9Device::ProcessVertices(
    UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
    IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags)
{

    static bool s_warned = false;
    if (!s_warned) {
        s_warned = true;
        DXLOG_WARN("ProcessVertices CALLED but NOT IMPLEMENTED - "
                   "src=%u dst=%u count=%u flags=0x%lX. If the speedo/minimap "
                   "depend on CPU vertex processing, THIS is the missing function.",
                   SrcStartIndex, DestIndex, VertexCount, (unsigned long)Flags);
    }
    (void)pDestBuffer; (void)pVertexDecl;
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE D9Device::DrawRectPatch(UINT, CONST float*, CONST D3DRECTPATCH_INFO*)
{
    static bool s_w = false;
    if (!s_w) { s_w = true; DXLOG_WARN("DrawRectPatch CALLED but NOT IMPLEMENTED"); }
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE D9Device::DrawTriPatch(UINT, CONST float*, CONST D3DTRIPATCH_INFO*)
{
    static bool s_w = false;
    if (!s_w) { s_w = true; DXLOG_WARN("DrawTriPatch CALLED but NOT IMPLEMENTED"); }
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE D9Device::DeletePatch(UINT) { return D3D_OK; }

HRESULT STDMETHODCALLTYPE D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
{

    QueryBridge* bridge = nullptr;
    HRESULT hr = QueryBridge::Create(m_ctx.get(), Type, &bridge);
    if (FAILED(hr)) return hr;

    if (!ppQuery) {

        delete bridge;
        return S_OK;
    }

    auto* q = new (std::nothrow) D9Query(this, Type, std::unique_ptr<QueryBridge>(bridge));
    if (!q) { delete bridge; return E_OUTOFMEMORY; }

    *ppQuery = q;
    return S_OK;
}

}
