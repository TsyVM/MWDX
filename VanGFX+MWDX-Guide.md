<div align="center">

<p><em>Shader modding Most Wanted with VanGFX — DirectX 9, 11 and 12</em></p>

[![VanGFX](https://img.shields.io/badge/VanGFX-Effect%20API-107C10?style=for-the-badge&labelColor=000000)](https://github.com/tsyvm/vangfx)
[![MWDX](https://img.shields.io/badge/MWDX-Backend%201%20%7C%202%20%7C%203-107C10?style=for-the-badge&labelColor=000000)](README.md)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-107C10?style=for-the-badge&labelColor=000000&logo=cplusplus&logoColor=107C10)](https://en.cppreference.com/w/cpp/20)
[![x86](https://img.shields.io/badge/Target-Win32%20x86-107C10?style=for-the-badge&labelColor=000000&logo=windows&logoColor=107C10)](#-project-setup)
[![HLSL](https://img.shields.io/badge/HLSL-SM%203.0%E2%80%936.x-107C10?style=for-the-badge&labelColor=000000)](#-writing-shaders-for-each-backend)

</div>

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

MWDX ships its own shader modding workflow — `ShaderDump` and `ShaderMods`, documented in the [README](README.md) and the [shader field guide](Index/Shaders.md). Drop an `.hlsl` file in a folder, restart, done. If that covers what you want, **use it and stop reading here**.

This guide is for the other case: driving the pipeline yourself with **[VanGFX](https://github.com/tsyvm/vangfx)**, TeamVanilla's DirectX interception library. You write a small C++ mod instead of an HLSL file, and in exchange you get things the file-based workflow cannot do — running on the DirectX 9 passthrough backend, injecting post-processing over the whole frame, driving live parameters at runtime, and filtering which draws you affect.

<div align="center">

### Contents

[Which Approach](#-which-approach) · [How They Layer](#-how-they-layer) · [The Backend Trap](#-the-backend-trap) · [Project Setup](#-project-setup) · [Getting In](#-getting-into-the-process)

[First Mod](#-your-first-mod) · [Writing Shaders](#-writing-shaders-for-each-backend) · [Targeting Draws](#-targeting-specific-draws) · [Live Parameters](#-live-parameters) · [Recipes](#-recipes)

[Interaction with MWDX](#-interaction-with-mwdxs-own-shader-feature) · [Troubleshooting](#-troubleshooting) · [Limitations](#-limitations)

</div>

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🧭 Which Approach

| | MWDX `ShaderMods` | VanGFX |
|---|---|---|
| **What you write** | An `.hlsl` file | A C++ mod (a DLL) |
| **Build step** | None | MSVC, x86 |
| **Works on Backend=1 (DX9)** | ❌ No | ✅ Yes |
| **Works on Backend=2 / 3** | ✅ Yes | ✅ Yes |
| **Replaces one game shader** | ✅ By hash, precisely | ✅ Via a filter predicate |
| **Whole-frame post-processing** | ❌ No | ✅ `inject_ps_post` |
| **Live parameters, no restart** | ❌ Restart per edit | ✅ `Uniforms` |
| **Filter by draw size / resolution** | ❌ No | ✅ `ReplaceFilter` |
| **Capture the GBuffer to disk** | ❌ No | ✅ `CaptureSession` |
| **Effort to get started** | Minutes | An afternoon |

The reason `ShaderMods` cannot work on Backend=1 is structural rather than an oversight. On Backend=1 MWDX forwards every call to the real `d3d9.dll` and never translates anything, so there is no generated HLSL to dump and nothing to substitute. VanGFX does not care — it hooks whichever device actually exists.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🥞 How They Layer

The thing to internalise before writing any code: **the game and your mod are not looking at the same API.**

```
             Backend=1                Backend=2                Backend=3
          ┌────────────┐           ┌────────────┐           ┌────────────┐
 speed.exe│    D3D9    │  speed.exe│    D3D9    │  speed.exe│    D3D9    │
          └─────┬──────┘           └─────┬──────┘           └─────┬──────┘
                │                        │                        │
          ┌─────▼──────┐           ┌─────▼──────┐           ┌─────▼──────┐
     MWDX │ passthrough│      MWDX │ translate  │      MWDX │ translate  │
          └─────┬──────┘           └─────┬──────┘           └─────┬──────┘
                │                        │                        │
          ┌─────▼──────┐           ┌─────▼──────┐           ┌─────▼──────┐
   system│    D3D9    │            │   D3D11    │            │   D3D12    │
          └────────────┘           └────────────┘           └────────────┘
                ▲                        ▲                        ▲
                └── VanGFX hooks here ───┴────────────────────────┘
```

Most Wanted always thinks it is talking to Direct3D 9. What MWDX does underneath is invisible to it. VanGFX hooks the **bottom** layer — the device that really exists — so what you intercept depends entirely on MWDX's `Backend` setting:

| MWDX `Backend` | VanGFX hooks | Draws you see | Shader model |
|---|---|---|---|
| `1` — DX9 passthrough | Direct3D 9 | The game's own D3D9 draws, unmodified | `vs_3_0` / `ps_3_0` |
| `2` — DX11 | Direct3D 11 | MWDX's translated draws | `vs_5_0` / `ps_5_0` |
| `3` — DX12 | Direct3D 12 | MWDX's translated draws | `vs_6_x` / `ps_6_x` |

On Backend=2 and 3 the shaders you are replacing are **MWDX's translated HLSL**, not the game's original D3D9 bytecode. They are functionally equivalent — same maths, same results — but the register names and structure are machine-generated. The [shader field guide](Index/Shaders.md) explains how to read them.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## ⚠️ The Backend Trap

**Do not use `BackendKind::Auto` inside an MWDX process.** It will pick the wrong backend, silently, every time.

`Auto` decides by scanning loaded modules — if `d3d12.dll` is present it targets D3D12, else D3D11, else D3D9. That heuristic is sound for a normal game. It is wrong here, because MWDX statically imports all three:

```
> dumpbin /imports d3d9.dll        (MWDX's own DLL)

    d3d11.dll
    d3d12.dll
    dxgi.dll
    D3DCOMPILER_47.dll
    KERNEL32.dll
    USER32.dll
    GDI32.dll
```

Those imports are resolved when MWDX loads, before any setting is read, because MWDX has to be *able* to create a D3D11 or D3D12 device in order to probe whether one is available. So `d3d12.dll` is in the process on Backend=1 — where nothing is using it — and `Auto` will confidently target D3D12 while the game renders through Direct3D 9.

The symptom is not an error. Your hooks install against a device that never receives a draw, and nothing happens at all.

**Always name the backend explicitly, and match it to the ini:**

```cpp
// Read MWDX.ini yourself and map it, or hard-code it for your own mod.
const int mwdx_backend = read_ini_backend();   // 1, 2 or 3

auto kind = (mwdx_backend == 1) ? vangfx::BackendKind::D3D9
          : (mwdx_backend == 2) ? vangfx::BackendKind::D3D11
                                : vangfx::BackendKind::D3D12;

auto ctx = vangfx::ContextBuilder{}
    .backend(kind)
    .log_level(vangfx::LogLevel::Info)
    .build();
```

Remember that MWDX falls back if the requested backend is unavailable — check `MWDX.log`, which records both the requested and the active renderer, rather than trusting the ini alone.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 📦 Project Setup

**Everything must be 32-bit.** `speed.exe` is a Win32 x86 binary from 2005. An x64 build will not load, and the error you get will not say so clearly.

| | |
|---|---|
| **Architecture** | Win32 / x86 — non-negotiable |
| **Toolset** | MSVC v143, `/std:c++20` (VanGFX is C++20) |
| **VanGFX library** | `libs/MSVC/win-x86/Release/vangfx.lib` |
| **Headers** | VanGFX `include/` |
| **System libs** | `d3d9.lib` `d3d11.lib` `d3d12.lib` `dxgi.lib` `d3dcompiler.lib` |
| **Runtime library** | `/MT` (Release) or `/MTd` (Debug) — static, no redistributable |

### CMake

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyMWShaderMod CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Must be built for Win32 (x86) - pass -A Win32")
endif()

set(VANGFX_ROOT "${CMAKE_SOURCE_DIR}/external/VanGFX")

add_library(vangfx STATIC IMPORTED)
set_target_properties(vangfx PROPERTIES
    IMPORTED_LOCATION_RELEASE     "${VANGFX_ROOT}/libs/MSVC/win-x86/Release/vangfx.lib"
    IMPORTED_LOCATION_DEBUG       "${VANGFX_ROOT}/libs/MSVC/win-x86/Debug/vangfx.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${VANGFX_ROOT}/include"
)

add_library(MyMod SHARED src/mod.cpp)
target_link_libraries(MyMod PRIVATE
    vangfx d3d9.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib)

if(MSVC)
    target_compile_options(MyMod PRIVATE
        $<$<CONFIG:Release>:/MT> $<$<CONFIG:Debug>:/MTd>)
endif()
```

Configure with `-A Win32`, the same as MWDX itself.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🚪 Getting Into the Process

VanGFX needs no proxy DLL of its own — it patches vtables directly — but your mod still has to be *loaded*. The obvious slot is taken: MWDX is already `d3d9.dll`. Three options, in the order most people should try them.

### 1. An ASI loader (recommended)

The Most Wanted modding scene runs on ASI plugins, and an ASI loader is just a proxy DLL that loads every `.asi` in the game folder. Build your mod as a DLL, rename it to `.asi`, drop it in beside `speed.exe`. It coexists with MWDX cleanly because they occupy different proxy slots.

```cpp
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        // Do NOT build the context here - see the timing note below.
        CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
```

### 2. A second proxy DLL

Take a system DLL the game imports that MWDX does not use — `dinput8.dll` and `winmm.dll` are the usual choices — and forward its exports. More fragile than an ASI loader and easy to get subtly wrong, but it needs no third-party loader.

### 3. Build it into MWDX

If the mod is yours and permanent, add it to MWDX's own source and call it from `DllMain`. You give up the ability to ship it separately, and you now maintain a fork.

### ⏱️ Timing — the part that catches people

**Never build the VanGFX context in `DllMain`.** Windows holds the loader lock for the duration of `DllMain`, and creating a graphics context loads and initialises driver DLLs — which deadlocks. MWDX has the same constraint and solves it the same way: it defers everything until the first `Direct3DCreate9` call.

Spawn a thread, wait for the device to exist, then build:

```cpp
static DWORD WINAPI InitThread(LPVOID)
{
    // The game has to create its device before there is anything to hook.
    // Waiting on the module is cheap and avoids a fixed sleep.
    while (!GetModuleHandleA("d3d11.dll")) Sleep(50);
    Sleep(1000);   // let MWDX finish building its device

    auto ctx = vangfx::ContextBuilder{}
        .backend(vangfx::BackendKind::D3D11)
        .build();

    if (!ctx) {
        MessageBoxA(nullptr, ctx.error().message.c_str(), "Mod", MB_OK);
        return 1;
    }
    g_ctx = std::move(*ctx);
    InstallEffects();
    return 0;
}
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🎬 Your First Mod

A whole-screen colour grade. This is the smallest useful thing and it works on all three backends unchanged, because `inject_ps_post` operates on the final pixel colour rather than on any particular shader.

```cpp
#include <vangfx/vangfx.hpp>

static std::unique_ptr<vangfx::Context> g_ctx;

void InstallEffects()
{
    // Runs after the game's pixel shader. `color` is the output, in-place.
    vangfx::inject_ps_post(*g_ctx, R"(
        // Warm the image and lift contrast slightly.
        color.rgb *= float3(1.10, 1.00, 0.92);
        color.rgb  = saturate((color.rgb - 0.5) * 1.15 + 0.5);
    )");
}
```

That is the entire mod. No shader files, no hashes, no restart-per-edit.

### What you can use inside an injected snippet

| Name | Type | Meaning |
|---|---|---|
| `color` | `float4` | The pixel output. `inject_ps_post` only — modify in place |
| `vangfx_uv` | `float2` | Screen UV, 0–1. `inject_ps_pre` can modify it to distort sampling |
| `vangfx_time` | `float` | Seconds since the context was created |
| `vangfx_delta` | `float` | Seconds since the previous frame |
| `vangfx_resolution` | `float2` | Back-buffer width and height |
| `vangfx_float4[0..7]` | `float4` | Your own live parameters — see [Live Parameters](#-live-parameters) |

`inject_ps_pre` runs *before* the game samples its textures, which is what makes screen-space distortion possible:

```cpp
// Heat haze.
vangfx::inject_ps_pre(*g_ctx,
    "vangfx_uv.x += sin(vangfx_uv.y * 40.0 + vangfx_time * 2.0) * 0.0015;");
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🎨 Writing Shaders for Each Backend

Injected snippets are backend-agnostic — VanGFX splices them into a shader it generates and compiles for whatever target is active. Full **replacements** are not, because the HLSL you supply is compiled as-is.

| MWDX `Backend` | VanGFX backend | Vertex profile | Pixel profile | Compiler |
|---|---|---|---|---|
| `1` | `D3D9` | `vs_3_0` | `ps_3_0` | `d3dcompiler_47.dll` |
| `2` | `D3D11` | `vs_5_0` | `ps_5_0` | `d3dcompiler_47.dll` |
| `3` | `D3D12` | `vs_6_0`+ | `ps_6_0`+ | `dxcompiler.dll` (set `use_dxc`) |

Leave `ReplaceOptions::profile` empty and VanGFX picks the right profile for the active backend, which is what you want most of the time.

### The Shader Model 3 gap

Direct3D 9 is where portability actually breaks. Shader Model 3 has no texture objects, no `Sample` method, and no `SV_` semantics:

```hlsl
// ── ps_3_0 (Backend=1) ──────────────────────────────
sampler2D tex0 : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR0
{
    float4 c = tex2D(tex0, uv);
    return float4(c.rgb * 1.2, c.a);
}

// ── ps_5_0 / ps_6_x (Backend=2 or 3) ────────────────
Texture2D    tex0 : register(t0);
SamplerState smp0 : register(s0);

float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    float4 c = tex0.Sample(smp0, uv);
    return float4(c.rgb * 1.2, c.a);
}
```

### Writing one source for all three

Use `ShaderBuilder::define` and branch in the HLSL. One file, three targets:

```cpp
auto shader = vangfx::ShaderBuilder{}
    .source(hlsl)
    .entry("main")
    .define("VGX_SM3", ctx->backend() == vangfx::BackendKind::D3D9 ? "1" : "0")
    .optimization(3)
    .build(*ctx);
```

```hlsl
#if VGX_SM3
    sampler2D tex0 : register(s0);
    #define SAMPLE(uv) tex2D(tex0, uv)
    #define PS_OUT COLOR0
#else
    Texture2D    tex0 : register(t0);
    SamplerState smp0 : register(s0);
    #define SAMPLE(uv) tex0.Sample(smp0, uv)
    #define PS_OUT SV_Target
#endif

float4 main(float2 uv : TEXCOORD0) : PS_OUT
{
    float4 c = SAMPLE(uv);
    return float4(c.rgb * 1.2, c.a);
}
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🎯 Targeting Specific Draws

A bare `replace_ps` hits **every** draw, which is almost never what you want. `ReplaceFilter` narrows it.

### Step one: find out what the frame contains

Before filtering you need to know what to filter on. Log the draws:

```cpp
g_ctx->on_draw([](vangfx::FrameContext& frame, vangfx::DrawEvent& ev) {
    static int n = 0;
    if (n++ > 500) return;                       // bound the log
    auto s = frame.surfaces();
    char buf[256];
    sprintf_s(buf, "draw %d verts=%u idx=%u rt=%ux%u\n",
              n, ev.vertex_count, ev.index_count, s.width, s.height);
    OutputDebugStringA(buf);
});
```

Run that under DebugView and you will see the frame's shape: a few thousand large draws for the world, a cluster of small ones at the end for the interface.

### Step two: filter

```cpp
vangfx::ReplaceFilter f;

// Cars and world geometry - big meshes only.
f.min_verts = 500;

// Interface and HUD - small draws at back-buffer resolution.
f.max_verts = 64;
f.rt_width  = 640;
f.rt_height = 480;

// Anything the simple fields cannot express.
f.predicate = [](const vangfx::FrameContext& frame, const vangfx::DrawEvent& ev) {
    return ev.type == vangfx::DrawType::DrawIndexed && ev.index_count > 2000;
};

vangfx::replace_ps(*g_ctx, hlsl, f);
```

> **Note for Backend=2 and 3.** MWDX draws the game's interface through
> `DrawIndexedPrimitiveUP`, which becomes a non-indexed draw with a small
> vertex count. Filter on vertex count and render-target size rather than
> assuming the interface is indexed.

### RAII scoping

`ScopedShaderReplace` and `ScopedInjection` restore the original when they go out of scope, which is what you want for anything toggleable:

```cpp
std::optional<vangfx::ScopedInjection> g_nightMode;

void ToggleNightMode(bool on)
{
    if (on)
        g_nightMode = vangfx::ScopedInjection::post(*g_ctx,
            "color.rgb *= float3(0.55, 0.62, 0.95);");
    else
        g_nightMode.reset();     // original restored here
}
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🎛️ Live Parameters

Eight `float4` slots, updated from C++ without recompiling anything. This is the single biggest practical advantage over editing shader files, where every change costs a restart.

```cpp
vangfx::Uniforms u(*g_ctx);

// C++ side - call whenever, e.g. from a hotkey or a VanGUI slider.
u.set(0, warmth, contrast, saturation, 0.0f);
u.set(1, vangfx::Float4(tint_r, tint_g, tint_b, 1.0f));
u.flush();
```

```cpp
vangfx::inject_ps_post(*g_ctx, R"(
    float warmth     = vangfx_float4[0].x;
    float contrast   = vangfx_float4[0].y;
    float saturation = vangfx_float4[0].z;
    float3 tint      = vangfx_float4[1].rgb;

    color.rgb *= lerp(1.0, tint, warmth);
    color.rgb  = saturate((color.rgb - 0.5) * contrast + 0.5);

    float lum  = dot(color.rgb, float3(0.2125, 0.7154, 0.0721));
    color.rgb  = lerp(lum.xxx, color.rgb, saturation);
)");
```

Pair this with [VanGUI](https://github.com/tsyvm/vangui) and you have a live grading panel over the running game.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 📖 Recipes

**Desaturate toward black and white** — `0.0` unchanged, `1.0` fully grey:

```hlsl
float lum = dot(color.rgb, float3(0.2125, 0.7154, 0.0721));
color.rgb = lerp(color.rgb, lum.xxx, 0.6);
```

**Vignette** — darken toward the edges:

```hlsl
float2 d = vangfx_uv - 0.5;
color.rgb *= 1.0 - saturate(dot(d, d) * 1.4);
```

**Filmic-ish tone curve** — tames blown highlights:

```hlsl
color.rgb = (color.rgb * (2.51 * color.rgb + 0.03))
          / (color.rgb * (2.43 * color.rgb + 0.59) + 0.14);
color.rgb = saturate(color.rgb);
```

**Scanlines**, using real resolution rather than a magic number:

```hlsl
float line = sin(vangfx_uv.y * vangfx_resolution.y * 3.14159);
color.rgb *= 0.92 + 0.08 * line;
```

**Identify a draw** — the bluntest technique, and the one that never lies. Make a filtered set of draws bright red and see what turns red:

```cpp
vangfx::ReplaceFilter f;
f.min_verts = 500;
vangfx::replace_ps(*g_ctx, "float4 main() : SV_Target { return float4(1,0,0,1); }", f);
```

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🔗 Interaction with MWDX's Own Shader Feature

The two systems work at different levels and can be used together, but the ordering matters and is worth being explicit about.

```
game shader  →  MWDX ShaderMods substitution  →  MWDX translation
                                                        ↓
                                             VanGFX replace / inject
                                                        ↓
                                                   final pixel
```

MWDX substitutes **before** it translates, so `ShaderMods` changes what MWDX generates. VanGFX operates on the result. Consequences:

- **`replace_ps` wins over `ShaderMods`.** Replacing the shader discards whatever MWDX produced, edited or not.
- **`inject_ps_post` composes with `ShaderMods`.** Your snippet runs on the output of the edited shader, so both apply.
- **`ShaderDiskCache` does not affect VanGFX.** It caches MWDX's translation, which is upstream of everything you do.
- **Turn `ShaderDump` off for performance work.** It bypasses the disk cache, which slows loading and has nothing to do with VanGFX.

For most projects, pick one. Use `ShaderMods` when you want to change how one specific material behaves; use VanGFX when you want to change the frame as a whole, or when you need Backend=1.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🔧 Troubleshooting

**Nothing happens, and there is no error.**
The backend. See [The Backend Trap](#-the-backend-trap) — `Auto` targets D3D12 in every MWDX process regardless of the ini. Set it explicitly and cross-check against `MWDX.log`, which records the renderer actually in use.

**The game hangs at startup.**
You built the context in `DllMain` and deadlocked the loader lock. Move it to a thread.

**The mod DLL will not load.**
Almost always an x64 build. `speed.exe` is 32-bit; everything in the process must be.

**`build()` fails with `NotInitialized`.**
You ran before the device existed. Wait for the game to create it, then build.

**A shader will not compile.**
`ctx.error().message` carries the full compiler diagnostic. On Backend=1, check first that you are not using `Texture2D` or `SV_Target` — Shader Model 3 has neither.

**Colours are right but everything is inside out.**
On Backend=2 and 3 you are replacing MWDX's translated shader, not the game's. Read the [shader field guide](Index/Shaders.md) for how those are structured before replacing rather than injecting.

**It works on one backend and not another.**
Expected, and the reason the profile table exists. Injection is portable; replacement is not.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 🚧 Limitations

- **Windows only.** Direct3D is a Windows API.
- **32-bit only**, because the game is.
- **No hot reload of C++.** Live *parameters* need no restart; changed shader source or C++ does.
- **VanGFX ships as a static library.** The `libs/` directory in the repository is a placeholder — obtain or build the `.lib` before linking.
- **Backend=1 gives you the game's real shaders**, which are compiled Shader Model 3 with no source available. You can replace them, but you are working from bytecode.
- **MWDX's DirectX 11 backend has a known HUD defect** (see the [README](README.md)). It is present with or without VanGFX and is not something a shader mod can work around.

<img width="100%" src="https://capsule-render.vercel.app/api?type=rect&color=0:000000,50:006400,100:000000&height=3"/>

## 📚 Further Reading

| Document | Covers |
|---|---|
| [README.md](README.md) | Installing MWDX, choosing a backend, every setting |
| [Index/Shaders.md](Index/Shaders.md) | Identifying a dumped shader, what each of the game's effects does, safe edit points |
| [VanGFX README](https://github.com/tsyvm/vangfx) | The full VanGFX feature set — GBuffer capture, D3D12 extensions, events |
| VanGFX Functions Guide | Complete API reference for every VanGFX type |

<div align="center">

<sub>Built and maintained by <a href="https://github.com/TsyVM">TsyVM</a> · <a href="https://www.teamvanilla.org/">TeamVanilla</a></sub>

<img width="100%" src="https://capsule-render.vercel.app/api?type=waving&color=0:006400,100:000000&height=80&section=footer"/>

</div>
