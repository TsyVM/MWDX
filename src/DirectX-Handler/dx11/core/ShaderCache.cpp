
// Translated-shader cache for the DX11 backend.
//
// Translating a D3D9 shader and then running D3DCompile over the result costs
// milliseconds. The game binds shaders per draw, so nothing may be translated
// more than once. Every shader is keyed by a CRC-64 of its bytecode, which is
// also stable across runs — the same shader in the same game file always
// hashes the same — so the compiled DXBC can additionally be persisted to disk
// and reloaded on the next launch.
//
// Three layers, checked in order:
//
//   m_vsCache / m_psCache   in-memory, keyed by bytecode CRC
//   the disk cache          dx9to11_shadercache.bin, keyed by CRC + variant
//   translate and compile   the slow path
//
// Pixel shaders additionally have *variants*. Alpha test and fog were render
// states in D3D9 and have to be compiled into the shader here, and the sampler
// kinds behind each texture register are only known once something is bound.
// One D3D9 shader therefore maps to several DXBC blobs, distinguished by a
// packed variant key. PsEntry keeps the translated HLSL alongside the compiled
// variants precisely so a new variant costs a recompile and not a retranslate.
//
// Every failure path here ends at a fallback shader rather than a failed draw:
// a null vertex shader that produces no geometry, or a magenta pixel shader.
// A visibly wrong pixel that logs its shader hash is diagnosable; a silently
// skipped draw is not.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <core/ShaderCache.h>
#include <core/D3D9ShaderTranslator.h>
#include <core/BackendSelect.h>
#include <core/ConstantMapper.h>
#include <core/ShaderMods.h>
#include <core/Log.h>
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>
#include <cassert>

#if __has_include(<vkd3d_shader.h>)
#   include <vkd3d_shader.h>
#   define DX9TO11_HAS_VKD3D 1
#else
#   define DX9TO11_HAS_VKD3D 0
#endif

#pragma comment(lib, "d3dcompiler.lib")

namespace dx9to11 {

namespace {

constexpr uint64_t kCrc64Poly = 0xC96C5795D7870F42ULL;

static uint64_t BuildCrcTable(uint64_t idx) noexcept
{
    uint64_t crc = idx;
    for (int k = 0; k < 8; ++k)
        crc = (crc & 1) ? (crc >> 1) ^ kCrc64Poly : (crc >> 1);
    return crc;
}

static uint64_t gCrcTable[256];
static bool     gCrcTableInit = false;

static void EnsureCrcTable() noexcept
{
    if (gCrcTableInit) return;
    for (int i = 0; i < 256; ++i)
        gCrcTable[i] = BuildCrcTable(static_cast<uint64_t>(i));
    gCrcTableInit = true;
}

static void DbgLog(const char* msg) noexcept
{

    DXLOG_WARN("%s", msg);
}

static uint64_t DiskKey(uint64_t crc, uint32_t flags) noexcept
{
    return crc ^ (0x9E3779B97F4A7C15ULL * (static_cast<uint64_t>(flags) + 1));
}

struct DiskHeader {
    char     magic[8];
    uint32_t salt;
    uint32_t reserved;
};
constexpr char kDiskMagic[8] = { 'M','W','S','X','S','H','C','1' };

static uint32_t DiskSalt() noexcept
{
    return kSm3TranslatorVersion
         | (BackendSelect::HalfPixelFixEnabled() ? 0x100u : 0u)
         | (BackendSelect::LegacyColorDefault()  ? 0x200u : 0u);
}

static std::wstring ModuleDir() noexcept
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleDir), &self);
    wchar_t path[MAX_PATH]{};
    if (!self || GetModuleFileNameW(self, path, MAX_PATH) == 0)
        return {};
    std::wstring dir(path);
    const size_t slash = dir.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? std::wstring{} : dir.substr(0, slash + 1);
}

}

uint64_t Crc64(const void* data, size_t len) noexcept
{
    EnsureCrcTable();
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t crc = 0xFFFFFFFFFFFFFFFFULL;
    for (size_t i = 0; i < len; ++i)
        crc = gCrcTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFFFFFFFFFULL;
}

ShaderCache::ShaderCache(ID3D11Device1* pDevice) noexcept
    : m_device(pDevice)
{}

ShaderCache::~ShaderCache()
{
    if (m_diskFile && m_diskFile != INVALID_HANDLE_VALUE)
        CloseHandle(static_cast<HANDLE>(m_diskFile));
}

#if DX9TO11_HAS_VKD3D
namespace {

struct Vkd3dRuntime {
    HMODULE                           dll           = nullptr;
    PFN_vkd3d_shader_compile          compile       = nullptr;
    PFN_vkd3d_shader_free_shader_code free_code     = nullptr;
    PFN_vkd3d_shader_free_messages    free_messages = nullptr;
};

const Vkd3dRuntime& Vkd3d() noexcept
{
    static const Vkd3dRuntime rt = [] {
        Vkd3dRuntime r{};
        std::wstring dir = ModuleDir();
        if (!dir.empty()) {
            dir += L"libvkd3d-shader-1.dll";
            r.dll = LoadLibraryExW(dir.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }
        if (!r.dll)
            r.dll = LoadLibraryW(L"libvkd3d-shader-1.dll");
        if (r.dll) {
            r.compile = reinterpret_cast<PFN_vkd3d_shader_compile>(
                GetProcAddress(r.dll, "vkd3d_shader_compile"));
            r.free_code = reinterpret_cast<PFN_vkd3d_shader_free_shader_code>(
                GetProcAddress(r.dll, "vkd3d_shader_free_shader_code"));
            r.free_messages = reinterpret_cast<PFN_vkd3d_shader_free_messages>(
                GetProcAddress(r.dll, "vkd3d_shader_free_messages"));
            if (!r.compile || !r.free_code || !r.free_messages) {
                DbgLog("libvkd3d-shader-1.dll loaded but missing expected exports - ignoring it.");
                FreeLibrary(r.dll);
                r = {};
            } else {
                DbgLog("vkd3d-shader runtime loaded - available as translation fallback.");
            }
        }
        return r;
    }();
    return rt;
}

}
#endif

bool ShaderCache::TranslatorAvailable() noexcept
{

    return true;
}

HRESULT ShaderCache::TranslateWithVkd3d(
    const DWORD*          pBytecode,
    SIZE_T                byteLen,
    bool                  isVertexShader,
    std::vector<uint8_t>* pDXBC) noexcept
{
    assert(pBytecode && byteLen > 0 && pDXBC);

#if DX9TO11_HAS_VKD3D
    const Vkd3dRuntime& vk = Vkd3d();
    if (!vk.compile) {
        (void)isVertexShader;
        return D3DERR_INVALIDCALL;
    }

    vkd3d_shader_compile_info info = {};
    info.type        = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO;
    info.source.code = pBytecode;
    info.source.size = byteLen;
    info.source_type = VKD3D_SHADER_SOURCE_D3D_BYTECODE;
    info.target_type = VKD3D_SHADER_TARGET_DXBC_TPF;
    info.log_level   = VKD3D_SHADER_LOG_WARNING;

    vkd3d_shader_code out = {};
    char* messages = nullptr;
    int rc = vk.compile(&info, &out, &messages);

    if (messages) {
        DbgLog("vkd3d-shader:");
        OutputDebugStringA(messages);
        vk.free_messages(messages);
    }
    if (rc != VKD3D_OK)
        return D3DERR_INVALIDCALL;

    pDXBC->assign(
        static_cast<const uint8_t*>(out.code),
        static_cast<const uint8_t*>(out.code) + out.size);
    vk.free_code(&out);
    return S_OK;
#else
    (void)pBytecode; (void)byteLen; (void)isVertexShader; (void)pDXBC;
    return D3DERR_INVALIDCALL;
#endif
}

HRESULT ShaderCache::CompileHlsl(
    const std::string&    hlsl,
    bool                  isVertexShader,
    UINT                  alphaFunc,
    UINT                  fogMode,
    UINT                  texKindMask,
    std::vector<uint8_t>* pDXBC) noexcept
{
    std::string defs;
    if (alphaFunc > 0 || fogMode > 0) {
        char def[96];
        std::snprintf(def, sizeof(def),
                      "#define DX9_ATEST %u\n#define DX9_FOG %u\n",
                      alphaFunc, fogMode);
        defs += def;
    }

    if (texKindMask != 0 && !isVertexShader) {
        for (unsigned i = 0; i < 8; ++i) {
            const unsigned kind = (texKindMask >> (i * 2)) & 0x3u;
            if (kind == 0) continue;
            char def[48];
            std::snprintf(def, sizeof(def), "#define DX9_TEXKIND%u %u\n", i, kind);
            defs += def;
        }
    }
    const std::string src = defs.empty() ? hlsl : (defs + hlsl);
    const std::string& body = defs.empty() ? hlsl : src;

    ComPtr<ID3DBlob> blob, errs;
    const HRESULT hr = D3DCompile(
        body.data(), body.size(), "dx9to11_translated",
        nullptr, nullptr, "main",
        isVertexShader ? "vs_4_0" : "ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        blob.GetAddressOf(), errs.GetAddressOf());

    if (FAILED(hr)) {
        DbgLog("D3DCompile of translated HLSL FAILED:");
        if (errs)
            DXLOG_ERROR("D3DCompile errors: %s", static_cast<const char*>(errs->GetBufferPointer()));

        DXLOG_ERROR("D3DCompile source dump:\n%s", body.c_str());
        return hr;
    }

    pDXBC->assign(
        static_cast<const uint8_t*>(blob->GetBufferPointer()),
        static_cast<const uint8_t*>(blob->GetBufferPointer()) + blob->GetBufferSize());
    return S_OK;
}

// Compiles `hlsl`, giving a user-supplied replacement first refusal.
//
// This is the hook for the ShaderDump / ShaderMods workflow. When either key
// is on, the generated HLSL is written out for the user to work from, and any
// edited file matching this shader is compiled in its place. A mod that fails
// to compile is reported once and the original is used, so a mistake in a
// shader mod can never take the game down.
//
// Note the ordering: the original text is dumped before the replacement is
// loaded, so the dump is always the pristine shader and a user editing in
// place cannot end up dumping their own work back over itself.
HRESULT ShaderCache::CompileWithMods(
    const DWORD*          pBytecode,
    SIZE_T                byteLen,
    bool                  isVertexShader,
    UINT                  alphaFunc,
    UINT                  fogMode,
    UINT                  texKindMask,
    const std::string&    hlsl,
    std::vector<uint8_t>* pDXBC) noexcept
{
    if (!shadermods::Active())
        return CompileHlsl(hlsl, isVertexShader, alphaFunc, fogMode,
                           texKindMask, pDXBC);

    const shadermods::Id id = shadermods::MakeId(
        pBytecode, byteLen, isVertexShader, alphaFunc, fogMode, texKindMask);

    shadermods::Dump(id, hlsl);

    std::string modded, name;
    if (shadermods::Load(id, &modded, &name)) {
        const HRESULT hr = CompileHlsl(modded, isVertexShader, alphaFunc,
                                       fogMode, texKindMask, pDXBC);
        if (SUCCEEDED(hr)) {
            DXLOG_INFO("[shadermods] using %s.hlsl", name.c_str());
            return hr;
        }
        shadermods::ReportCompileFailure(name);
    }

    return CompileHlsl(hlsl, isVertexShader, alphaFunc, fogMode,
                       texKindMask, pDXBC);
}

HRESULT ShaderCache::TranslateBuiltin(
    const DWORD*          pBytecode,
    SIZE_T                byteLen,
    bool                  isVertexShader,
    UINT                  alphaFunc,
    UINT                  fogMode,
    UINT                  texKindMask,
    std::vector<uint8_t>* pDXBC,
    std::string*          pHlslOut) noexcept
{
    Sm3TranslateOptions opt{};
    opt.halfPixelFix = BackendSelect::HalfPixelFixEnabled();
    opt.legacyColorDefault = BackendSelect::LegacyColorDefault();

    Sm3TranslateResult res{};
    if (!TranslateD3D9Shader(reinterpret_cast<const uint32_t*>(pBytecode),
                             byteLen, opt, res)) {
        std::string msg = "built-in shader translation failed: " + res.error;
        DbgLog(msg.c_str());
        return D3DERR_INVALIDCALL;
    }
    if (res.isVertexShader != isVertexShader) {
        DbgLog("shader stage mismatch (VS bytecode passed to PS create or vice versa)");
        return D3DERR_INVALIDCALL;
    }
    if (res.usesVSTexture)
        DbgLog("note: vs_3_0 vertex texturing used - VS SRV binding not wired yet");

    const HRESULT hr = CompileWithMods(pBytecode, byteLen, isVertexShader,
                                       alphaFunc, fogMode, texKindMask,
                                       res.hlsl, pDXBC);
    if (SUCCEEDED(hr) && pHlslOut)
        *pHlslOut = std::move(res.hlsl);
    return hr;
}

void ShaderCache::DiskCacheLoad() noexcept
{
    if (m_diskLoaded) return;
    m_diskLoaded = true;

    if (!BackendSelect::ShaderDiskCacheEnabled())
        return;

    const std::wstring path = ModuleDir() + L"dx9to11_shadercache.bin";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DbgLog("shader disk cache: cannot open dx9to11_shadercache.bin (disabled this run)");
        return;
    }

    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);

    DiskHeader hdr{};
    DWORD read = 0;
    bool valid = size.QuadPart >= static_cast<LONGLONG>(sizeof(DiskHeader))
              && ReadFile(h, &hdr, sizeof(hdr), &read, nullptr)
              && read == sizeof(hdr)
              && std::memcmp(hdr.magic, kDiskMagic, 8) == 0
              && hdr.salt == DiskSalt();

    if (!valid) {

        SetFilePointer(h, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
        DiskHeader nh{};
        std::memcpy(nh.magic, kDiskMagic, 8);
        nh.salt = DiskSalt();
        DWORD written = 0;
        WriteFile(h, &nh, sizeof(nh), &written, nullptr);
        m_diskFile = h;
        m_diskWritable = (written == sizeof(nh));
        return;
    }

    size_t entries = 0;
    for (;;) {
        struct { uint64_t key; uint32_t len; } eh{};
        if (!ReadFile(h, &eh, sizeof(eh), &read, nullptr) || read != sizeof(eh))
            break;
        if (eh.len == 0 || eh.len > (8u << 20)) break;
        std::vector<uint8_t> blob(eh.len);
        if (!ReadFile(h, blob.data(), eh.len, &read, nullptr) || read != eh.len)
            break;
        m_disk[eh.key] = std::move(blob);
        ++entries;
    }

    if (entries) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "shader disk cache: %zu DXBC blobs loaded", entries);
        DbgLog(msg);
    }
    m_diskFile = h;
    m_diskWritable = true;

    SetFilePointer(h, 0, nullptr, FILE_END);
}

bool ShaderCache::DiskCacheGet(uint64_t crc, uint32_t flags, std::vector<uint8_t>* out) noexcept
{
    // The disk cache stores compiled DXBC, which is downstream of both the
    // dump and the mod substitution. A cache hit therefore skips translation
    // entirely — nothing would be dumped, and an edited shader would never be
    // read. Bypassing the cache whenever either key is on costs some load time
    // and is the only way the workflow can work at all.
    if (shadermods::Active())
        return false;

    DiskCacheLoad();
    auto it = m_disk.find(DiskKey(crc, flags));
    if (it == m_disk.end()) return false;
    *out = it->second;
    return true;
}

void ShaderCache::DiskCachePut(uint64_t crc, uint32_t flags, const std::vector<uint8_t>& dxbc) noexcept
{
    // Writing while mods are active would persist the user's edited shader
    // under the original's key and keep serving it after they turn ShaderMods
    // back off.
    if (shadermods::Active())
        return;

    DiskCacheLoad();
    if (!m_diskWritable || !m_diskFile || m_diskFile == INVALID_HANDLE_VALUE)
        return;
    const uint64_t key = DiskKey(crc, flags);
    if (m_disk.count(key)) return;
    m_disk[key] = dxbc;

    HANDLE h = static_cast<HANDLE>(m_diskFile);
    struct { uint64_t key; uint32_t len; } eh{ key, static_cast<uint32_t>(dxbc.size()) };
    DWORD written = 0;
    SetFilePointer(h, 0, nullptr, FILE_END);
    if (!WriteFile(h, &eh, sizeof(eh), &written, nullptr) || written != sizeof(eh) ||
        !WriteFile(h, dxbc.data(), eh.len, &written, nullptr) || written != eh.len) {
        DbgLog("shader disk cache: append failed - disabling writes");
        m_diskWritable = false;
    }
}

static void ValidateTranslatedLayout(const std::vector<uint8_t>& dxbc,
                                     bool isVS, uint64_t crc) noexcept
{
    ComPtr<ID3D11ShaderReflection> refl;
    if (FAILED(D3DReflect(dxbc.data(), dxbc.size(),
                          IID_PPV_ARGS(refl.GetAddressOf()))))
        return;

    D3D11_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd)))
        return;

    const UINT   cBytes    = (isVS ? 256u : 224u) * 16u;
    const size_t mirrorCap = isVS ? sizeof(VSConstantData) : sizeof(PSConstantData);

    for (UINT i = 0; i < sd.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC bind{};
        if (FAILED(refl->GetResourceBindingDesc(i, &bind)))
            continue;
        if (bind.Type != D3D_SIT_CBUFFER || bind.BindPoint != 0)
            continue;

        ID3D11ShaderReflectionConstantBuffer* cb =
            refl->GetConstantBufferByName(bind.Name);
        D3D11_SHADER_BUFFER_DESC cbd{};
        if (!cb || FAILED(cb->GetDesc(&cbd)))
            return;

        bool ok = (cbd.Size <= mirrorCap);

        for (UINT v = 0; ok && v < cbd.Variables; ++v) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D11_SHADER_VARIABLE_DESC vd{};
            if (!var || FAILED(var->GetDesc(&vd))) { ok = false; break; }
            if      (std::strcmp(vd.Name, "c")  == 0) ok = (vd.StartOffset == 0);
            else if (std::strcmp(vd.Name, "ic") == 0) ok = (vd.StartOffset == cBytes);
            else if (std::strcmp(vd.Name, "bc") == 0) ok = (vd.StartOffset == cBytes + 256u);
        }

        if (!ok) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "[dx9to11] WARNING: translated %s %016llX cbuffer b0 layout "
                "drifted from ConstantMapper (size %u, mirror %u) - constants "
                "would read garbage; report this shader hash\n",
                isVS ? "VS" : "PS",
                static_cast<unsigned long long>(crc),
                cbd.Size, static_cast<unsigned>(mirrorCap));
            OutputDebugStringA(buf);
        }
        return;
    }
}

HRESULT ShaderCache::TranslateToDxbc(
    const DWORD*          pBytecode,
    SIZE_T                byteLen,
    bool                  isVertexShader,
    UINT                  alphaFunc,
    UINT                  fogMode,
    UINT                  texKindMask,
    uint64_t              crc,
    std::vector<uint8_t>* pDXBC,
    std::string*          pHlslOut) noexcept
{
    const uint32_t flags = DiskKeyFlags(isVertexShader, alphaFunc, fogMode, texKindMask);

    if (DiskCacheGet(crc, flags, pDXBC))
        return S_OK;

    char env[8]{};
    const bool forceVkd3d =
        GetEnvironmentVariableA("DX9TO11_FORCE_VKD3D", env, sizeof(env)) > 0
        && env[0] == '1';

    HRESULT hr = E_FAIL;
    if (forceVkd3d && alphaFunc == 0 && fogMode == 0 && texKindMask == 0) {
        hr = TranslateWithVkd3d(pBytecode, byteLen, isVertexShader, pDXBC);
        if (SUCCEEDED(hr)) {
            ValidateTranslatedLayout(*pDXBC, isVertexShader, crc);
            DiskCachePut(crc, flags, *pDXBC);
            return hr;
        }
    }

    hr = TranslateBuiltin(pBytecode, byteLen, isVertexShader, alphaFunc, fogMode,
                          texKindMask, pDXBC, pHlslOut);
    if (SUCCEEDED(hr)) {
        ValidateTranslatedLayout(*pDXBC, isVertexShader, crc);
        DiskCachePut(crc, flags, *pDXBC);
        return hr;
    }

    if (alphaFunc == 0 && fogMode == 0 && texKindMask == 0) {
        hr = TranslateWithVkd3d(pBytecode, byteLen, isVertexShader, pDXBC);
        if (SUCCEEDED(hr)) {
            DbgLog("vkd3d-shader fallback translated a shader the built-in path rejected");
            ValidateTranslatedLayout(*pDXBC, isVertexShader, crc);
            DiskCachePut(crc, flags, *pDXBC);
        }
    }
    return hr;
}

HRESULT ShaderCache::ReflectVSInputSignature(
    const std::vector<uint8_t>& dxbc,
    ShaderReflection*           pReflOut) noexcept
{
    assert(pReflOut);

    ComPtr<ID3D11ShaderReflection> refl;
    HRESULT hr = D3DReflect(
        dxbc.data(), dxbc.size(),
        __uuidof(ID3D11ShaderReflection),
        reinterpret_cast<void**>(refl.GetAddressOf()));
    if (FAILED(hr)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "[dx9to11] ReflectVSInputSignature: D3DReflect FAILED hr=0x%08lX (dxbc size=%zu)\n",
            static_cast<unsigned long>(hr), dxbc.size());
        OutputDebugStringA(buf);
        DXLOG_WARN("ReflectVSInputSignature D3DReflect FAILED hr=0x%08lX - input layout will not be bound", static_cast<unsigned long>(hr));
        return hr;
    }

    D3D11_SHADER_DESC shDesc{};
    refl->GetDesc(&shDesc);

    pReflOut->inputElements.clear();
    pReflOut->semanticNames.clear();
    pReflOut->dxbcBlob = dxbc;

    for (UINT i = 0; i < shDesc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC paramDesc{};
        refl->GetInputParameterDesc(i, &paramDesc);

        pReflOut->semanticNames.push_back(paramDesc.SemanticName);

        D3D11_INPUT_ELEMENT_DESC elem{};
        elem.SemanticName         = pReflOut->semanticNames.back().c_str();
        elem.SemanticIndex        = paramDesc.SemanticIndex;
        elem.Format               = DXGI_FORMAT_UNKNOWN;
        elem.InputSlot            = 0;
        elem.AlignedByteOffset    = D3D11_APPEND_ALIGNED_ELEMENT;
        elem.InputSlotClass       = D3D11_INPUT_PER_VERTEX_DATA;
        elem.InstanceDataStepRate = 0;
        pReflOut->inputElements.push_back(elem);
    }

    pReflOut->RepointSemanticNames();

    for (UINT i = 0; i < shDesc.OutputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC op{};
        if (FAILED(refl->GetOutputParameterDesc(i, &op)) || !op.SemanticName)
            continue;
        if (_stricmp(op.SemanticName, "TEXCOORD") == 0 && op.SemanticIndex < 8)
            pReflOut->outTexMask |= (1u << op.SemanticIndex);
        else if (_stricmp(op.SemanticName, "COLOR") == 0) {
            if (op.SemanticIndex == 0) pReflOut->outColor0 = true;
            if (op.SemanticIndex == 1) pReflOut->outColor1 = true;
        } else if (_stricmp(op.SemanticName, "FOG") == 0 && op.SemanticIndex == 0) {
            pReflOut->outFog = true;
        }
    }
    return S_OK;
}

HRESULT ShaderCache::EnsureFallbackVS() noexcept
{
    if (m_fallbackVS) return S_OK;

    static const char kHlsl[] =
        "struct VSIn  { float4 pos : POSITION; };\n"
        "struct VSOut { float4 pos : SV_Position; };\n"
        "VSOut main(VSIn i) {\n"
        "    VSOut o;\n"
        "    o.pos = float4(2.0, 2.0, 2.0, 1.0);\n"
        "    o.pos.x += i.pos.x * 1e-20;   // keep POSITION in the signature\n"
        "    return o;\n"
        "}\n";

    std::vector<uint8_t> dxbc;
    HRESULT hr = CompileHlsl(std::string(kHlsl),  true,  0,  0,  0, &dxbc);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11VertexShader> vs;
    hr = m_device->CreateVertexShader(dxbc.data(), dxbc.size(), nullptr, vs.GetAddressOf());
    if (FAILED(hr)) return hr;

    (void)ReflectVSInputSignature(dxbc, &m_fallbackVSRefl);
    m_fallbackVS = vs;
    return S_OK;
}

HRESULT ShaderCache::EnsureFallbackPS() noexcept
{
    if (m_fallbackPS) return S_OK;

    static const char kHlsl[] =
        "float4 main() : SV_Target { return float4(1.0, 0.0, 1.0, 1.0); }\n";

    std::vector<uint8_t> dxbc;
    HRESULT hr = CompileHlsl(std::string(kHlsl),  false,  0,  0,  0, &dxbc);
    if (FAILED(hr)) return hr;

    ComPtr<ID3D11PixelShader> ps;
    hr = m_device->CreatePixelShader(dxbc.data(), dxbc.size(), nullptr, ps.GetAddressOf());
    if (FAILED(hr)) return hr;

    m_fallbackPS = ps;
    return S_OK;
}

HRESULT ShaderCache::GetOrCreateVS(
    const DWORD*         pBytecode,
    SIZE_T               byteLen,
    ID3D11VertexShader** ppVS,
    ShaderReflection*    pReflOut) noexcept
{
    if (!pBytecode || byteLen == 0 || !ppVS)
        return D3DERR_INVALIDCALL;

    const uint64_t key = Crc64(pBytecode, byteLen);

    AcquireSRWLockExclusive(&m_lock);

    auto vsIt = m_vsCache.find(key);
    if (vsIt != m_vsCache.end()) {
        *ppVS = vsIt->second.Get();
        (*ppVS)->AddRef();
        if (pReflOut) {
            auto rIt = m_reflCache.find(key);
            if (rIt != m_reflCache.end())
                *pReflOut = rIt->second;
        }
        ReleaseSRWLockExclusive(&m_lock);
        return S_OK;
    }

    std::vector<uint8_t> dxbc;
    HRESULT hr = TranslateToDxbc(pBytecode, byteLen,  true,
                                  0,  0,  0, key, &dxbc, nullptr);

    ComPtr<ID3D11VertexShader> vs;
    if (SUCCEEDED(hr))
        hr = m_device->CreateVertexShader(dxbc.data(), dxbc.size(), nullptr, vs.GetAddressOf());

    if (FAILED(hr)) {

        if (SUCCEEDED(EnsureFallbackVS())) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "[dx9to11] VS %016llX could not be translated (hr=0x%08lX) - "
                "using null fallback (geometry skipped); report this hash\n",
                static_cast<unsigned long long>(key), static_cast<unsigned long>(hr));
            OutputDebugStringA(buf);

            m_vsCache[key]   = m_fallbackVS;
            m_reflCache[key] = m_fallbackVSRefl;
            *ppVS = m_fallbackVS.Get();
            (*ppVS)->AddRef();
            if (pReflOut) *pReflOut = m_fallbackVSRefl;
            ReleaseSRWLockExclusive(&m_lock);
            return S_OK;
        }
        ReleaseSRWLockExclusive(&m_lock);
        return hr;
    }

    ShaderReflection refl{};
    (void)ReflectVSInputSignature(dxbc, &refl);

    m_vsCache[key]   = vs;
    m_reflCache[key] = std::move(refl);

    *ppVS = vs.Get();
    (*ppVS)->AddRef();
    if (pReflOut)
        *pReflOut = m_reflCache[key];

    ReleaseSRWLockExclusive(&m_lock);
    return S_OK;
}

HRESULT ShaderCache::GetOrCreatePS(
    const DWORD*        pBytecode,
    SIZE_T              byteLen,
    ID3D11PixelShader** ppPS) noexcept
{
    if (!pBytecode || byteLen == 0 || !ppPS)
        return D3DERR_INVALIDCALL;

    const uint64_t key = Crc64(pBytecode, byteLen);

    AcquireSRWLockExclusive(&m_lock);

    auto it = m_psCache.find(key);
    if (it != m_psCache.end()) {
        auto vIt = it->second.variants.find(0u);
        if (vIt != it->second.variants.end() && vIt->second) {
            *ppPS = vIt->second.Get();
            (*ppPS)->AddRef();
            ReleaseSRWLockExclusive(&m_lock);
            return S_OK;
        }
    }

    std::vector<uint8_t> dxbc;
    std::string hlsl;
    HRESULT hr = TranslateToDxbc(pBytecode, byteLen,  false,
                                  0,  0,  0,
                                 key, &dxbc, &hlsl);

    ComPtr<ID3D11PixelShader> ps;
    if (SUCCEEDED(hr))
        hr = m_device->CreatePixelShader(dxbc.data(), dxbc.size(), nullptr, ps.GetAddressOf());

    if (FAILED(hr)) {

        if (SUCCEEDED(EnsureFallbackPS())) {
            DXLOG_ERROR("PS %016llX could not be translated (hr=0x%08lX) - "
                       "using magenta fallback; report this hash",
                       static_cast<unsigned long long>(key), static_cast<unsigned long>(hr));

            PsEntry& fe = m_psCache[key];
            fe.variants[0u] = m_fallbackPS;
            *ppPS = m_fallbackPS.Get();
            (*ppPS)->AddRef();
            ReleaseSRWLockExclusive(&m_lock);
            return S_OK;
        }
        ReleaseSRWLockExclusive(&m_lock);
        return hr;
    }

    PsEntry& e = m_psCache[key];
    e.variants[0u] = ps;
    if (!hlsl.empty()) e.hlsl = std::move(hlsl);

    *ppPS = ps.Get();
    (*ppPS)->AddRef();

    ReleaseSRWLockExclusive(&m_lock);
    return S_OK;
}

HRESULT ShaderCache::GetPSVariant(
    const DWORD*        pBytecode,
    SIZE_T              byteLen,
    UINT                alphaFunc,
    UINT                fogMode,
    UINT                texKindMask,
    ID3D11PixelShader** ppPS) noexcept
{
    if (!pBytecode || byteLen == 0 || !ppPS)
        return D3DERR_INVALIDCALL;

    const UINT af = (alphaFunc == 0 || alphaFunc >= 8) ? 0u : alphaFunc;
    const UINT fm = (fogMode <= 3) ? fogMode : 0u;
    const UINT tk = texKindMask & 0xFFFFu;
    if (af == 0 && fm == 0 && tk == 0)
        return GetOrCreatePS(pBytecode, byteLen, ppPS);

    const uint64_t key = Crc64(pBytecode, byteLen);
    const uint32_t vkey = PackVariantKey(af, fm, tk);

    AcquireSRWLockExclusive(&m_lock);

    PsEntry& e = m_psCache[key];
    auto vIt = e.variants.find(vkey);
    if (vIt != e.variants.end() && vIt->second) {
        *ppPS = vIt->second.Get();
        (*ppPS)->AddRef();
        ReleaseSRWLockExclusive(&m_lock);
        return S_OK;
    }

    std::vector<uint8_t> dxbc;
    const uint32_t flags = DiskKeyFlags(false, af, fm, tk);
    bool haveDxbc = DiskCacheGet(key, flags, &dxbc);

    if (!haveDxbc) {
        if (e.hlsl.empty()) {
            Sm3TranslateOptions opt{};
            opt.halfPixelFix = BackendSelect::HalfPixelFixEnabled();
            opt.legacyColorDefault = BackendSelect::LegacyColorDefault();
            Sm3TranslateResult res{};
            if (TranslateD3D9Shader(reinterpret_cast<const uint32_t*>(pBytecode),
                                    byteLen, opt, res) && !res.isVertexShader) {
                e.hlsl = std::move(res.hlsl);
            }
        }
        if (!e.hlsl.empty()) {
            haveDxbc = SUCCEEDED(CompileWithMods(pBytecode, byteLen,  false,
                                                 af, fm, tk, e.hlsl, &dxbc));
            if (haveDxbc) DiskCachePut(key, flags, dxbc);
        }
    }

    if (haveDxbc) {
        ComPtr<ID3D11PixelShader> ps;
        if (SUCCEEDED(m_device->CreatePixelShader(dxbc.data(), dxbc.size(),
                                                  nullptr, ps.GetAddressOf()))) {
            e.variants[vkey] = ps;
            *ppPS = ps.Get();
            (*ppPS)->AddRef();
            ReleaseSRWLockExclusive(&m_lock);
            return S_OK;
        }
    }

    ReleaseSRWLockExclusive(&m_lock);

    return GetOrCreatePS(pBytecode, byteLen, ppPS);
}

HRESULT ShaderCache::GetVSInputSignature(
    const DWORD*      pBytecode,
    SIZE_T            byteLen,
    ShaderReflection* pReflOut) noexcept
{
    if (!pBytecode || byteLen == 0 || !pReflOut)
        return D3DERR_INVALIDCALL;

    const uint64_t key = Crc64(pBytecode, byteLen);

    AcquireSRWLockExclusive(&m_lock);

    auto it = m_reflCache.find(key);
    if (it != m_reflCache.end()) {
        *pReflOut = it->second;
        ReleaseSRWLockExclusive(&m_lock);
        return S_OK;
    }

    std::vector<uint8_t> dxbc;
    HRESULT hr = TranslateToDxbc(pBytecode, byteLen,  true,
                                  0,  0,  0, key, &dxbc, nullptr);
    if (FAILED(hr)) {
        ReleaseSRWLockExclusive(&m_lock);
        return hr;
    }

    ShaderReflection refl{};
    hr = ReflectVSInputSignature(dxbc, &refl);
    if (SUCCEEDED(hr)) {
        m_reflCache[key] = std::move(refl);
        *pReflOut = m_reflCache[key];
    }

    ReleaseSRWLockExclusive(&m_lock);
    return hr;
}

size_t ShaderCache::VSCacheSize() const noexcept
{
    AcquireSRWLockShared(&m_lock);
    size_t n = m_vsCache.size();
    ReleaseSRWLockShared(&m_lock);
    return n;
}

size_t ShaderCache::PSCacheSize() const noexcept
{
    AcquireSRWLockShared(&m_lock);
    size_t n = m_psCache.size();
    ReleaseSRWLockShared(&m_lock);
    return n;
}

}
