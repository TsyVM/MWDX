// Builds ID3D11InputLayout objects from a D3D9 vertex declaration and the
// vertex shader that will consume it.
//
// The translation is mostly mechanical -- D3D9 declaration types become DXGI
// formats, usages become semantic names -- but two details are not.
//
// D3D11 validates the layout against the shader's input signature, so an
// attribute the declaration supplies but the shader does not read is fine,
// while the reverse is a creation failure. The layout therefore cannot be
// cached on the declaration alone; it is keyed on the pair.
//
// D3D9 also permits a declaration to reference a stream the game never bound,
// expecting those attributes to read as zero. D3D11 will not draw with a gap,
// so a permanently-zeroed buffer is bound to a reserved high slot and any
// unbound stream is pointed at it.
//
// A failure here must be loud. If creation fails and the caller proceeds, the
// previous draw's layout stays bound and every attribute is read from the
// wrong offset -- geometry that draws, and is completely wrong, with no error.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/InputLayoutCache.h>
#include <core/DeviceContext11.h>
#include <vector>
#include <cassert>
#include <cstring>
#include <string.h>

namespace dx9to11 {

InputLayoutCache::InputLayoutCache(DeviceContext11* ctx) noexcept
    : m_ctx(ctx)
{}

DXGI_FORMAT InputLayoutCache::DeclTypeToFormat(BYTE declType) noexcept
{

    switch (declType) {
    case D3DDECLTYPE_FLOAT1:    return DXGI_FORMAT_R32_FLOAT;
    case D3DDECLTYPE_FLOAT2:    return DXGI_FORMAT_R32G32_FLOAT;
    case D3DDECLTYPE_FLOAT3:    return DXGI_FORMAT_R32G32B32_FLOAT;
    case D3DDECLTYPE_FLOAT4:    return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DDECLTYPE_D3DCOLOR:  return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DDECLTYPE_UBYTE4:    return DXGI_FORMAT_R8G8B8A8_UINT;
    case D3DDECLTYPE_SHORT2:    return DXGI_FORMAT_R16G16_SINT;
    case D3DDECLTYPE_SHORT4:    return DXGI_FORMAT_R16G16B16A16_SINT;
    case D3DDECLTYPE_UBYTE4N:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DDECLTYPE_SHORT2N:   return DXGI_FORMAT_R16G16_SNORM;
    case D3DDECLTYPE_SHORT4N:   return DXGI_FORMAT_R16G16B16A16_SNORM;
    case D3DDECLTYPE_USHORT2N:  return DXGI_FORMAT_R16G16_UNORM;
    case D3DDECLTYPE_USHORT4N:  return DXGI_FORMAT_R16G16B16A16_UNORM;
    case D3DDECLTYPE_UDEC3:     return DXGI_FORMAT_R10G10B10A2_UINT;
    case D3DDECLTYPE_DEC3N:     return DXGI_FORMAT_R10G10B10A2_UNORM;
    case D3DDECLTYPE_FLOAT16_2: return DXGI_FORMAT_R16G16_FLOAT;
    case D3DDECLTYPE_FLOAT16_4: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                    return DXGI_FORMAT_UNKNOWN;
    }
}

const char* InputLayoutCache::DeclUsageToSemantic(BYTE usage) noexcept
{
    switch (usage) {
    case D3DDECLUSAGE_POSITION:     return "POSITION";
    case D3DDECLUSAGE_BLENDWEIGHT:  return "BLENDWEIGHT";
    case D3DDECLUSAGE_BLENDINDICES: return "BLENDINDICES";
    case D3DDECLUSAGE_NORMAL:       return "NORMAL";
    case D3DDECLUSAGE_PSIZE:        return "PSIZE";
    case D3DDECLUSAGE_TEXCOORD:     return "TEXCOORD";
    case D3DDECLUSAGE_TANGENT:      return "TANGENT";
    case D3DDECLUSAGE_BINORMAL:     return "BINORMAL";
    case D3DDECLUSAGE_TESSFACTOR:   return "TESSFACTOR";
    case D3DDECLUSAGE_POSITIONT:    return "POSITIONT";
    case D3DDECLUSAGE_COLOR:        return "COLOR";
    case D3DDECLUSAGE_FOG:          return "FOG";
    case D3DDECLUSAGE_DEPTH:        return "DEPTH";
    case D3DDECLUSAGE_SAMPLE:       return "SAMPLE";
    default:                        return "TEXCOORD";
    }
}

HRESULT InputLayoutCache::GetOrCreate(
    const D3DVERTEXELEMENT9* pElements,
    const ShaderReflection&  refl,
    ID3D11InputLayout**      ppLayout) noexcept
{
    if (!pElements || !ppLayout || refl.dxbcBlob.empty())
        return D3DERR_INVALIDCALL;

    UINT elemCount = 0;
    while (pElements[elemCount].Stream != 0xFF) {
        ++elemCount;
        if (elemCount > MAXD3DDECLLENGTH) return D3DERR_INVALIDCALL;
    }

    const uint64_t declHash   = Crc64(pElements, (elemCount + 1) * sizeof(D3DVERTEXELEMENT9));
    const uint64_t vsBlobHash = Crc64(refl.dxbcBlob.data(), refl.dxbcBlob.size());

    const InputLayoutKey key{ declHash, vsBlobHash };

    AcquireSRWLockExclusive(&m_lock);

    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        *ppLayout = it->second.Get();
        (*ppLayout)->AddRef();
        ReleaseSRWLockExclusive(&m_lock);
        return S_OK;
    }

    std::vector<D3D11_INPUT_ELEMENT_DESC> d3d11Elems;
    d3d11Elems.reserve(refl.inputElements.size());

    for (const D3D11_INPUT_ELEMENT_DESC& sig : refl.inputElements) {
        const char* semName = sig.SemanticName;
        if (!semName) continue;

        if (_strnicmp(semName, "SV_", 3) == 0) continue;

        const D3DVERTEXELEMENT9* match = nullptr;
        for (UINT e = 0; e < elemCount; ++e) {
            const D3DVERTEXELEMENT9& el = pElements[e];
            if (el.UsageIndex == sig.SemanticIndex &&
                _stricmp(DeclUsageToSemantic(el.Usage), semName) == 0) {
                match = &el;
                break;
            }
        }

        D3D11_INPUT_ELEMENT_DESC desc{};
        desc.SemanticName         = semName;
        desc.SemanticIndex        = sig.SemanticIndex;
        desc.InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
        desc.InstanceDataStepRate = 0;

        if (match) {
            DXGI_FORMAT fmt = DeclTypeToFormat(match->Type);
            if (fmt == DXGI_FORMAT_UNKNOWN) {
                ReleaseSRWLockExclusive(&m_lock);
                return D3DERR_INVALIDCALL;
            }
            desc.Format            = fmt;
            desc.InputSlot         = match->Stream;
            desc.AlignedByteOffset = match->Offset;
        } else {

            desc.Format            = DXGI_FORMAT_R32G32B32A32_FLOAT;
            desc.InputSlot         = kZeroStreamSlot;
            desc.AlignedByteOffset = 0;
        }
        d3d11Elems.push_back(desc);
    }

    ComPtr<ID3D11InputLayout> layout;
    HRESULT hr = m_ctx->Device()->CreateInputLayout(
        d3d11Elems.data(),
        static_cast<UINT>(d3d11Elems.size()),
        refl.dxbcBlob.data(),
        refl.dxbcBlob.size(),
        layout.GetAddressOf());

    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&m_lock);
        return hr;
    }

    m_cache[key] = layout;
    *ppLayout    = layout.Get();
    (*ppLayout)->AddRef();

    ReleaseSRWLockExclusive(&m_lock);
    return S_OK;
}

}
