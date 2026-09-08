// Creates and caches the D3D11 blend, rasteriser and depth-stencil state
// objects, and translates D3D9 render state into them.
//
// Most of the translation is a direct mapping of enumerations. The parts that
// are not:
//
//   D3DBLEND_BOTHSRCALPHA and BOTHINVSRCALPHA are deprecated dual-source modes
//   that set the colour and alpha blends together. D3D11 has no equivalent, so
//   they map to the closest single-source mode. A draw using one composites
//   slightly wrongly rather than not at all.
//
//   Depth bias is a float in D3D9 and an integer in D3D11, scaled by the depth
//   buffer's precision. The conversion assumes 24-bit depth, which is exact for
//   D24 targets and approximate for anything else.
//
//   Sampler LOD range is clamped so that D3D11 does not select a mip level
//   where the original runtime would not have.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/PSOCache.h>
#include <core/DeviceContext11.h>
#include <algorithm>
#include <cassert>
#include <cmath>

namespace dx9to11 {

static D3D11_BLEND TranslateBlend(D3DBLEND b) noexcept
{
    switch (b) {
    case D3DBLEND_ZERO:            return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE:             return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR:        return D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:     return D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA:        return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:     return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:       return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA:    return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR:       return D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR:    return D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT:     return D3D11_BLEND_SRC_ALPHA_SAT;
    case D3DBLEND_BOTHSRCALPHA:    return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_BOTHINVSRCALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_BLENDFACTOR:     return D3D11_BLEND_BLEND_FACTOR;
    case D3DBLEND_INVBLENDFACTOR:  return D3D11_BLEND_INV_BLEND_FACTOR;
    case D3DBLEND_SRCCOLOR2:       return D3D11_BLEND_SRC1_COLOR;
    case D3DBLEND_INVSRCCOLOR2:    return D3D11_BLEND_INV_SRC1_COLOR;
    default:                        return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND SanitizeAlphaBlend(D3D11_BLEND b) noexcept
{
    switch (b) {
    case D3D11_BLEND_SRC_COLOR:      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:     return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3D11_BLEND_SRC1_COLOR:     return D3D11_BLEND_SRC1_ALPHA;
    case D3D11_BLEND_INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_ALPHA;
    default:                         return b;
    }
}

static D3D11_BLEND_OP TranslateBlendOp(D3DBLENDOP op) noexcept
{
    switch (op) {
    case D3DBLENDOP_ADD:         return D3D11_BLEND_OP_ADD;
    case D3DBLENDOP_SUBTRACT:    return D3D11_BLEND_OP_SUBTRACT;
    case D3DBLENDOP_REVSUBTRACT: return D3D11_BLEND_OP_REV_SUBTRACT;
    case D3DBLENDOP_MIN:         return D3D11_BLEND_OP_MIN;
    case D3DBLENDOP_MAX:         return D3D11_BLEND_OP_MAX;
    default:                      return D3D11_BLEND_OP_ADD;
    }
}

static D3D11_FILL_MODE TranslateFillMode(D3DFILLMODE m) noexcept
{
    switch (m) {
    case D3DFILL_WIREFRAME: return D3D11_FILL_WIREFRAME;
    case D3DFILL_SOLID:     return D3D11_FILL_SOLID;
    default:                 return D3D11_FILL_SOLID;
    }
}

static D3D11_CULL_MODE TranslateCullMode(D3DCULL c) noexcept
{
    switch (c) {
    case D3DCULL_NONE: return D3D11_CULL_NONE;
    case D3DCULL_CW:   return D3D11_CULL_FRONT;
    case D3DCULL_CCW:  return D3D11_CULL_BACK;
    default:            return D3D11_CULL_NONE;
    }
}

static D3D11_COMPARISON_FUNC TranslateCmpFunc(D3DCMPFUNC f) noexcept
{
    switch (f) {
    case D3DCMP_NEVER:        return D3D11_COMPARISON_NEVER;
    case D3DCMP_LESS:         return D3D11_COMPARISON_LESS;
    case D3DCMP_EQUAL:        return D3D11_COMPARISON_EQUAL;
    case D3DCMP_LESSEQUAL:    return D3D11_COMPARISON_LESS_EQUAL;
    case D3DCMP_GREATER:      return D3D11_COMPARISON_GREATER;
    case D3DCMP_NOTEQUAL:     return D3D11_COMPARISON_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
    case D3DCMP_ALWAYS:       return D3D11_COMPARISON_ALWAYS;
    default:                   return D3D11_COMPARISON_ALWAYS;
    }
}

static D3D11_STENCIL_OP TranslateStencilOp(D3DSTENCILOP op) noexcept
{
    switch (op) {
    case D3DSTENCILOP_KEEP:    return D3D11_STENCIL_OP_KEEP;
    case D3DSTENCILOP_ZERO:    return D3D11_STENCIL_OP_ZERO;
    case D3DSTENCILOP_REPLACE: return D3D11_STENCIL_OP_REPLACE;
    case D3DSTENCILOP_INCRSAT: return D3D11_STENCIL_OP_INCR_SAT;
    case D3DSTENCILOP_DECRSAT: return D3D11_STENCIL_OP_DECR_SAT;
    case D3DSTENCILOP_INVERT:  return D3D11_STENCIL_OP_INVERT;
    case D3DSTENCILOP_INCR:    return D3D11_STENCIL_OP_INCR;
    case D3DSTENCILOP_DECR:    return D3D11_STENCIL_OP_DECR;
    default:                    return D3D11_STENCIL_OP_KEEP;
    }
}

static D3D11_TEXTURE_ADDRESS_MODE TranslateAddressMode(D3DTEXTUREADDRESS a) noexcept
{
    switch (a) {
    case D3DTADDRESS_WRAP:       return D3D11_TEXTURE_ADDRESS_WRAP;
    case D3DTADDRESS_MIRROR:     return D3D11_TEXTURE_ADDRESS_MIRROR;
    case D3DTADDRESS_CLAMP:      return D3D11_TEXTURE_ADDRESS_CLAMP;
    case D3DTADDRESS_BORDER:     return D3D11_TEXTURE_ADDRESS_BORDER;
    case D3DTADDRESS_MIRRORONCE: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default:                      return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static D3D11_FILTER TranslateFilter(D3DTEXTUREFILTERTYPE min,
                                    D3DTEXTUREFILTERTYPE mag,
                                    D3DTEXTUREFILTERTYPE mip) noexcept
{

    if (min == D3DTEXF_ANISOTROPIC || mag == D3DTEXF_ANISOTROPIC)
        return D3D11_FILTER_ANISOTROPIC;

    bool minLinear  = (min == D3DTEXF_LINEAR);
    bool magLinear  = (mag == D3DTEXF_LINEAR);
    bool mipLinear  = (mip == D3DTEXF_LINEAR);
    bool mipNone    = (mip == D3DTEXF_NONE);

    UINT enc = 0;
    if (minLinear)          enc |= D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    if (magLinear)          enc |= D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    if (mipLinear && !mipNone) enc |= D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;

    switch (enc) {
    case 0:
        return D3D11_FILTER_MIN_MAG_MIP_POINT;
    case D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT:
        return D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    case D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT:
        return D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;
    case D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR:
        return D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
    case (D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT | D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR):
        return D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
    case (D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT | D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR):
        return D3D11_FILTER_MIN_POINT_MAG_MIP_LINEAR;
    case (D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT | D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT):
        return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    default:
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    }
}

static UINT8 TranslateColorWriteEnable(DWORD d3d9) noexcept
{
    UINT8 mask = 0;
    if (d3d9 & D3DCOLORWRITEENABLE_RED)   mask |= D3D11_COLOR_WRITE_ENABLE_RED;
    if (d3d9 & D3DCOLORWRITEENABLE_GREEN) mask |= D3D11_COLOR_WRITE_ENABLE_GREEN;
    if (d3d9 & D3DCOLORWRITEENABLE_BLUE)  mask |= D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (d3d9 & D3DCOLORWRITEENABLE_ALPHA) mask |= D3D11_COLOR_WRITE_ENABLE_ALPHA;
    return mask;
}

PSOCache::PSOCache(DeviceContext11* ctx) noexcept
    : m_ctx(ctx)
{}

ID3D11BlendState1* PSOCache::GetOrCreateBlendState(const BlendDesc& desc) noexcept
{

    {
        AcquireSRWLockShared(&m_blendLock);
        auto it = m_blendCache.find(desc);
        if (it != m_blendCache.end()) {
            auto* p = it->second.Get();
            ReleaseSRWLockShared(&m_blendLock);
            return p;
        }
        ReleaseSRWLockShared(&m_blendLock);
    }

    D3D11_BLEND_DESC1 bd{};
    bd.AlphaToCoverageEnable  = FALSE;
    bd.IndependentBlendEnable = FALSE;

    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable           = desc.BlendEnable;
    rt.SrcBlend              = TranslateBlend(desc.SrcBlend);
    rt.DestBlend             = TranslateBlend(desc.DestBlend);
    rt.BlendOp               = TranslateBlendOp(desc.BlendOp);
    rt.LogicOpEnable         = FALSE;
    rt.LogicOp               = D3D11_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = TranslateColorWriteEnable(desc.ColorWriteEnable);

    if (desc.SeparateAlphaBlend) {
        rt.SrcBlendAlpha  = SanitizeAlphaBlend(TranslateBlend(desc.SrcBlendAlpha));
        rt.DestBlendAlpha = SanitizeAlphaBlend(TranslateBlend(desc.DestBlendAlpha));
        rt.BlendOpAlpha   = TranslateBlendOp(desc.BlendOpAlpha);
    } else {
        rt.SrcBlendAlpha  = SanitizeAlphaBlend(rt.SrcBlend);
        rt.DestBlendAlpha = SanitizeAlphaBlend(rt.DestBlend);
        rt.BlendOpAlpha   = rt.BlendOp;
    }

    for (int i = 1; i < 8; ++i)
        bd.RenderTarget[i] = rt;

    ComPtr<ID3D11BlendState1> state;
    auto* dev11 = static_cast<ID3D11Device1*>(m_ctx->Device());
    HRESULT hr = dev11->CreateBlendState1(&bd, state.GetAddressOf());
    if (FAILED(hr)) {
        OutputDebugStringA("[dx9to11] PSOCache: CreateBlendState1 failed\n");
        return nullptr;
    }

    AcquireSRWLockExclusive(&m_blendLock);
    auto [it, inserted] = m_blendCache.emplace(desc, std::move(state));
    auto* p = it->second.Get();
    ReleaseSRWLockExclusive(&m_blendLock);
    return p;
}

ID3D11RasterizerState1* PSOCache::GetOrCreateRasterizerState(const RastDesc& desc) noexcept
{
    {
        AcquireSRWLockShared(&m_rastLock);
        auto it = m_rastCache.find(desc);
        if (it != m_rastCache.end()) {
            auto* p = it->second.Get();
            ReleaseSRWLockShared(&m_rastLock);
            return p;
        }
        ReleaseSRWLockShared(&m_rastLock);
    }

    float d3d9Bias = *reinterpret_cast<const float*>(&desc.DepthBias);

    INT d3d11Bias = static_cast<INT>(d3d9Bias * (1 << 24));

    D3D11_RASTERIZER_DESC1 rd{};
    rd.FillMode              = TranslateFillMode(desc.FillMode);
    rd.CullMode              = TranslateCullMode(desc.CullMode);
    rd.FrontCounterClockwise = FALSE;

    rd.DepthBias             = d3d11Bias;
    rd.DepthBiasClamp        = 0.0f;
    rd.SlopeScaledDepthBias  = desc.SlopeScaledDepthBias;
    rd.DepthClipEnable       = TRUE;
    rd.ScissorEnable         = desc.ScissorEnable;
    rd.MultisampleEnable     = desc.MultisampleEnable;
    rd.AntialiasedLineEnable = desc.AntialiasedLine;
    rd.ForcedSampleCount     = 0;

    ComPtr<ID3D11RasterizerState1> state;
    auto* dev11 = static_cast<ID3D11Device1*>(m_ctx->Device());
    HRESULT hr = dev11->CreateRasterizerState1(&rd, state.GetAddressOf());
    if (FAILED(hr)) {
        OutputDebugStringA("[dx9to11] PSOCache: CreateRasterizerState1 failed\n");
        return nullptr;
    }

    AcquireSRWLockExclusive(&m_rastLock);
    auto [it, inserted] = m_rastCache.emplace(desc, std::move(state));
    auto* p = it->second.Get();
    ReleaseSRWLockExclusive(&m_rastLock);
    return p;
}

ID3D11DepthStencilState* PSOCache::GetOrCreateDepthStencilState(const DepthStencilDesc& desc) noexcept
{
    {
        AcquireSRWLockShared(&m_dsLock);
        auto it = m_dsCache.find(desc);
        if (it != m_dsCache.end()) {
            auto* p = it->second.Get();
            ReleaseSRWLockShared(&m_dsLock);
            return p;
        }
        ReleaseSRWLockShared(&m_dsLock);
    }

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable    = desc.ZEnable;
    dd.DepthWriteMask = desc.ZWriteEnable ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc      = TranslateCmpFunc(desc.ZFunc);

    dd.StencilEnable    = desc.StencilEnable;
    dd.StencilReadMask  = desc.StencilReadMask;
    dd.StencilWriteMask = desc.StencilWriteMask;

    dd.FrontFace.StencilFailOp      = TranslateStencilOp(desc.StencilFail);
    dd.FrontFace.StencilDepthFailOp = TranslateStencilOp(desc.StencilZFail);
    dd.FrontFace.StencilPassOp      = TranslateStencilOp(desc.StencilPass);
    dd.FrontFace.StencilFunc        = TranslateCmpFunc(desc.StencilFunc);

    if (desc.TwoSidedStencil) {

        dd.BackFace.StencilFailOp      = TranslateStencilOp(desc.CCWFail);
        dd.BackFace.StencilDepthFailOp = TranslateStencilOp(desc.CCWZFail);
        dd.BackFace.StencilPassOp      = TranslateStencilOp(desc.CCWPass);
        dd.BackFace.StencilFunc        = TranslateCmpFunc(desc.CCWFunc);
    } else {
        dd.BackFace = dd.FrontFace;
    }

    ComPtr<ID3D11DepthStencilState> state;
    HRESULT hr = m_ctx->Device()->CreateDepthStencilState(&dd, state.GetAddressOf());
    if (FAILED(hr)) {
        OutputDebugStringA("[dx9to11] PSOCache: CreateDepthStencilState failed\n");
        return nullptr;
    }

    AcquireSRWLockExclusive(&m_dsLock);
    auto [it, inserted] = m_dsCache.emplace(desc, std::move(state));
    auto* p = it->second.Get();
    ReleaseSRWLockExclusive(&m_dsLock);
    return p;
}

ID3D11SamplerState* PSOCache::GetOrCreateSamplerState(const SamplerDesc& desc) noexcept
{
    {
        AcquireSRWLockShared(&m_sampLock);
        auto it = m_sampCache.find(desc);
        if (it != m_sampCache.end()) {
            auto* p = it->second.Get();
            ReleaseSRWLockShared(&m_sampLock);
            return p;
        }
        ReleaseSRWLockShared(&m_sampLock);
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = TranslateFilter(desc.MinFilter, desc.MagFilter, desc.MipFilter);
    sd.AddressU       = TranslateAddressMode(desc.AddressU);
    sd.AddressV       = TranslateAddressMode(desc.AddressV);
    sd.AddressW       = TranslateAddressMode(desc.AddressW);
    sd.MipLODBias     = desc.MipLODBias;

    sd.MaxAnisotropy  = (sd.Filter == D3D11_FILTER_ANISOTROPIC)
                        ? std::min<UINT>(std::max<UINT>(desc.MaxAnisotropy, 1u), 16u)
                        : 1u;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;

    sd.MinLOD         = static_cast<float>(desc.MaxMipLevel);
    sd.MaxLOD         = (desc.MipFilter == D3DTEXF_NONE)
                            ? static_cast<float>(desc.MaxMipLevel)
                            : D3D11_FLOAT32_MAX;

    sd.BorderColor[0] = ((desc.BorderColor >> 16) & 0xFF) / 255.0f;
    sd.BorderColor[1] = ((desc.BorderColor >>  8) & 0xFF) / 255.0f;
    sd.BorderColor[2] = ((desc.BorderColor      ) & 0xFF) / 255.0f;
    sd.BorderColor[3] = ((desc.BorderColor >> 24) & 0xFF) / 255.0f;

    ComPtr<ID3D11SamplerState> state;
    HRESULT hr = m_ctx->Device()->CreateSamplerState(&sd, state.GetAddressOf());
    if (FAILED(hr)) {
        OutputDebugStringA("[dx9to11] PSOCache: CreateSamplerState failed\n");
        return nullptr;
    }

    AcquireSRWLockExclusive(&m_sampLock);
    auto [it, inserted] = m_sampCache.emplace(desc, std::move(state));
    auto* p = it->second.Get();
    ReleaseSRWLockExclusive(&m_sampLock);
    return p;
}

size_t PSOCache::BlendCacheSize() const noexcept {
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&m_blendLock));
    auto n = m_blendCache.size();
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&m_blendLock));
    return n;
}
size_t PSOCache::RastCacheSize() const noexcept {
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&m_rastLock));
    auto n = m_rastCache.size();
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&m_rastLock));
    return n;
}
size_t PSOCache::DepthStencilCacheSize() const noexcept {
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&m_dsLock));
    auto n = m_dsCache.size();
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&m_dsLock));
    return n;
}
size_t PSOCache::SamplerCacheSize() const noexcept {
    AcquireSRWLockShared(const_cast<SRWLOCK*>(&m_sampLock));
    auto n = m_sampCache.size();
    ReleaseSRWLockShared(const_cast<SRWLOCK*>(&m_sampLock));
    return n;
}

}
