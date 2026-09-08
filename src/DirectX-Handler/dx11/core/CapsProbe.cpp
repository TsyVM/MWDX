// Answers format and multisample support questions by asking D3D11.
//
// D3D9's CheckDeviceFormat asks "can this format be used this way" -- as a
// texture, a render target, a depth buffer, a vertex texture. D3D11 answers
// the same question through CheckFormatSupport, which returns a bit field, so
// most of the work here is deciding which support bits a given D3D9 usage
// actually requires.
//
// Requiring too many is the mistake to avoid. These are query APIs: a game
// told a format is unsupported does not fail, it silently drops the feature
// that needed it, and no error is raised anywhere to trace. So each usage maps
// to the minimum set of bits that genuinely matter for it.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <core/CapsProbe.h>
#include <core/FormatConverter.h>

namespace dx9to11 {
namespace CapsProbe {
namespace {

UINT RequiredSupportBits(DWORD usage, D3DRESOURCETYPE rtype) noexcept
{
    UINT req = 0;

    switch (rtype) {
    case D3DRTYPE_TEXTURE:        req |= D3D11_FORMAT_SUPPORT_TEXTURE2D;          break;
    case D3DRTYPE_CUBETEXTURE:    req |= D3D11_FORMAT_SUPPORT_TEXTURECUBE;        break;
    case D3DRTYPE_VOLUMETEXTURE:  req |= D3D11_FORMAT_SUPPORT_TEXTURE3D;          break;
    case D3DRTYPE_VERTEXBUFFER:   req |= D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER;   break;
    case D3DRTYPE_INDEXBUFFER:    req |= D3D11_FORMAT_SUPPORT_IA_INDEX_BUFFER;    break;
    case D3DRTYPE_SURFACE:
    default:

        req |= D3D11_FORMAT_SUPPORT_TEXTURE2D;
        break;
    }

    if (usage & D3DUSAGE_RENDERTARGET)
        req = (req & ~UINT(D3D11_FORMAT_SUPPORT_TEXTURE2D))
            | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    if (usage & D3DUSAGE_DEPTHSTENCIL)
        req = (req & ~UINT(D3D11_FORMAT_SUPPORT_TEXTURE2D))
            | D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
    if (usage & D3DUSAGE_AUTOGENMIPMAP)
        req |= D3D11_FORMAT_SUPPORT_MIP_AUTOGEN;
    if (usage & D3DUSAGE_QUERY_FILTER)
        req |= D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (usage & D3DUSAGE_QUERY_VERTEXTEXTURE)
        req |= D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (usage & D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING)
        req |= D3D11_FORMAT_SUPPORT_BLENDABLE;
    if (usage & D3DUSAGE_QUERY_WRAPANDMIP)
        req |= D3D11_FORMAT_SUPPORT_MIP;

    return req;
}

}

bool FormatSupportsUsage(ID3D11Device*   dev,
                         D3DFORMAT       fmt,
                         DWORD           usage,
                         D3DRESOURCETYPE rtype) noexcept
{

    if (!dev)
        return true;

    const FormatMapping m = FormatConverter::ToDxgi(fmt);
    if (!m.IsValid())
        return false;

    UINT support = 0;
    if (FAILED(dev->CheckFormatSupport(m.dxgiFormat, &support)))
        return false;

    const UINT req = RequiredSupportBits(usage, rtype);
    return (support & req) == req;
}

UINT MultisampleQualityLevels(ID3D11Device* dev,
                              D3DFORMAT     fmt,
                              UINT          sampleCount) noexcept
{
    if (!dev || sampleCount == 0)
        return 0;

    const FormatMapping m = FormatConverter::ToDxgi(fmt);
    if (!m.IsValid())
        return 0;

    UINT quality = 0;
    if (FAILED(dev->CheckMultisampleQualityLevels(m.dxgiFormat, sampleCount, &quality)))
        return 0;
    return quality;
}

bool DepthStencilSupported(ID3D11Device* dev, D3DFORMAT dsFmt) noexcept
{
    return FormatSupportsUsage(dev, dsFmt, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE);
}

}
}
