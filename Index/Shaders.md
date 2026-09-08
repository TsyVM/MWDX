# Shader Index

A field guide to the shaders you'll find in `MWDX\Shaders\Dumped`.

Turning on `ShaderDump` gives you a folder full of files named after hashes —
`ps_07f85658a15e74a6.hlsl` and so on. Nothing in the filename tells you whether
you're looking at car paint, the sky, or the speedometer. This page is how you
work that out, and what's worth changing once you have.

You do not need to be a shader programmer to use this. The last two sections are
step-by-step.

---

## First: what you're actually looking at

The files in `Dumped` are **not** the game's original shader source. Most Wanted
shipped its shaders already compiled, and MWDX translates that compiled form into
HLSL so a modern API can use it. What comes out is correct and editable, but it's
machine-written — no comments, no meaningful names.

So a dumped pixel shader looks roughly like this:

```hlsl
// dx9to11 translated ps_3_0
#ifndef DX9_ATEST
#define DX9_ATEST 0
#endif
...
cbuffer Dx9PSConst : register(b0) {
    float4 c[224];          // the game's shader constants
    int4   ic[16];
    uint4  bc[4];
};
cbuffer Dx9Emu : register(b1) { ... fog, alpha ref, clip planes ... };

#define DX9_TEXKIND0 0      // one of these per texture the shader uses
#if DX9_TEXKIND0 == 1
TextureCube tex0 : register(t0);
#elif DX9_TEXKIND0 == 2
Texture3D   tex0 : register(t0);
#else
Texture2D   tex0 : register(t0);
#endif
SamplerState smp0 : register(s0);
...
struct PSIn { ... };

float4 main(PSIn In) : SV_Target0 {
    float4 oC0 = 0.0;
    float4 r0, r1, r2;      // temporaries

    ... the shader's actual work, writing into oC0 ...

#if DX9_ATEST == 1
    discard;                                          // NEVER
#elif DX9_ATEST == 2
    if (!(oC0.a <  g_emuMisc.z)) discard;             // LESS
...
#endif
#if DX9_FOG == 3
    { ... fog blend into oC0.rgb ... }
#endif
    return oC0;
}
```

Two things follow from that, and they're the whole trick to editing safely:

- **`oC0` is the final colour.** Everything the shader does ends up there.
- **The original parameter names are gone.** What the source called
  `SpecularPower` is now just some `c[17].x`. You generally can't tell which
  constant is which without experimenting.

Which is why the recommended way to mod is to leave the body alone and change
`oC0` at the end. That works on every shader, whether or not you understand it.

---

## The safe edit point

Scroll to the bottom and find the line `#if DX9_ATEST == 1`. (Not the
`#ifndef DX9_ATEST` at the top — that's just a default.) **Insert your change
immediately above it**, after the shader has done its work and before
alpha-testing and fog.

```hlsl
    ... shader body ends ...

    oC0.rgb *= float3(1.2, 1.0, 0.9);   // <-- your line goes here

#if DX9_ATEST == 1
```

Rules that keep a mod from breaking things:

| Do | Don't |
|---|---|
| Change `oC0.rgb` | Change `oC0.a` — alpha drives alpha-testing and blending; touching it makes things vanish or turn into solid blocks |
| Keep the function named `main` | Rename it, or change what it returns |
| Keep every `cbuffer`, `Texture`, `SamplerState` and `register(...)` line exactly as-is | Delete a sampler you think is unused — the register numbers must line up with what the game binds |
| Add new local variables freely | Remove the `#if DX9_ATEST` / `#if DX9_FOG` blocks |

If you get it wrong, nothing dramatic happens: MWDX logs the compile error to
`MWDX-render.log`, uses the original shader for that one, and the game carries on.

Vertex shaders work the same way but the output is a struct — `O.oPos` is the
screen position, `O.oT[0..7]` the texture coordinates. Leave vertex shaders alone
until you're comfortable; almost everything visually interesting lives in the
pixel shaders.

---

## Working out which shader you've got

Dump a scene, then narrow it down. Two methods, both cheap:

**By elimination.** Delete everything in `Dumped`, load the exact scene you care
about, quit, and look at what came back. Only shaders actually drawn get written.
A garage scene and a highway scene share very little.

**By fingerprint.** How many textures a shader samples, and of what kind, is a
strong tell — few effects in this game use the same combination.

Don't read this off the `Texture2D` / `TextureCube` / `Texture3D` lines. Every
sampler emits all three of those wrapped in `#if` blocks, so they tell you
nothing. **Read the `#define DX9_TEXKIND` lines near the top instead.** There is
exactly one per sampler the shader actually uses, and the number is the kind:

```hlsl
#define DX9_TEXKIND0 0      // 0 = 2D
#define DX9_TEXKIND1 2      // 2 = volume (3D)
#define DX9_TEXKIND2 1      // 1 = cube
#define DX9_TEXKIND3 0
```

That example is the real thing: four samplers — two 2D, one volume, one cube.
It's `car.fx` (diffuse, metallic-flake noise, reflection cube, shadow map).

If there are no `DX9_TEXKIND` lines at all, the shader samples nothing. **Every
vertex shader in this game is in that category**, so any `DX9_TEXKIND` line means
you're holding a pixel shader.

| Sampler mix | Almost certainly |
|---|---|
| Any volume (`2`) at all | **Car bodywork.** 2×2D + volume + cube is `car.fx`; 3×2D + volume + cube is `carnormalmap.fx`. Nothing else in the game uses a volume texture. |
| A cube (`1`), no volume | `worldbone.fx` — skinned world objects |
| 6 × 2D | **`visualtreatment.fx`** — the full-screen colour grade. The one most people are after. |
| 4 × 2D | `sky.fx` |
| 5 × 2D | `worldreflect.fx`, or the non-branching variant of `visualtreatment.fx` |
| 3 × 2D | `particles.fx`, `waterseashader.fx`, `world.fx`, `worldnormalmap.fx`, or one `hdr.fx` pass |
| 2 × 2D | `standard.fx`, `glassreflectshader.fx`, or a `debugPoly.fx` blit |
| 1 × 2D | `trees.fx`, or most `hdr.fx` and `debugPoly.fx` passes |
| No samplers | `shadowvolume.fx`, `shadowscreenfilter.fx`, or a depth/lighting-only pass |

Where a row is ambiguous, size settles it: a bloom or blur pass from `hdr.fx` is
a dozen lines that tap one sampler repeatedly at nearby coordinates, while
`world.fx` is long and full of lighting maths.

Full-screen effects (`visualtreatment.fx`, `hdr.fx`) are also easy to spot
because their vertex shader does almost nothing — it copies position and texture
coordinates and stops.

> **How this table was produced.** The 2005 sources were compiled to PC Direct3D 9
> bytecode and run through MWDX's own translator — 77 shaders, the same code path
> the game uses. The mixes above are measured from that output, not guessed. Five
> effects (`world.fx`, `worldnormalmap.fx`, `worldreflect.fx`,
> `glassreflectshader.fx`, `grass.fx`) wouldn't compile on a modern compiler, so
> their rows come from counting sampler declarations in the source instead.

---

## The effects

What the game's renderer is built from.

Everything below was worked out from the 2005 shader source for Most Wanted — 20
`.fx` effect files and their shared headers. **That source is not included with
MWDX**, so the descriptions here are your substitute for it. If you do have a
copy, reading the original is by far the fastest way to understand a dumped
file: same maths, readable names.

### The one you probably want

**`visualtreatment.fx`** — the full-screen look of the game. This is where Most
Wanted's signature colour grade comes from, and it's the single highest-leverage
file here. It runs after the world is drawn and applies, in order: motion blur
and radial blur, optional depth of field, a colour curve, desaturation, the
pursuit-breaker vignette, brightness, and bloom.

Key parameters in the source: `Desaturation`, `Coeffs0`–`Coeffs3` (the colour
curve — this is the grade), `CombinedBrightness`, `VisualEffectVignette`,
`VisualEffectBrightness`, `g_fBloomScale`, `g_fVignetteScale`.
Techniques: `visualtreatment`, `visualtreatment_branching`,
`downscale4x4alphaluminance`. `vs_1_1` / `ps_3_0`.

Removing or reducing the grade here is what people mean by "getting rid of the
blue filter".

### Cars

| File | What it draws | Shaders | Notable knobs |
|---|---|---|---|
| `car.fx` | Car bodywork — paint, metallic flake, environment reflection, specular. Techniques `car`, `lowlod`, `rvm` (rear-view mirror). | `vs_3_0` / `ps_3_0` | `MetallicScale`, `SpecularPower`, `SpecularHotSpot`, `EnvmapPower`, `DiffuseMin`/`DiffuseRange`, `ShadowColour` |
| `carnormalmap.fx` | Same, for cars with normal maps. Techniques `car_normalmap`, `lowlod`. | `vs_3_0` / `ps_3_0` | as above |
| `glassreflectshader.fx` | Car glass and its reflections. | `vs_1_1` / `ps_2_0` | `SpecularColour`, `SpecularPower`, `Brightness` |

### World

| File | What it draws | Shaders | Notable knobs |
|---|---|---|---|
| `world.fx` | General environment geometry — the bulk of the city. | `vs_1_1` / `ps_2_0` | `Brightness`, `DiffuseColour`, `SkyDiffuseScale`, `MipMapBias`, `TextureOffset` (scrolling textures) |
| `worldnormalmap.fx` | Normal-mapped surfaces, with optional parallax. Second pass adds auxiliary lighting. | `vs_1_1` / `ps_3_0` | `SurfaceSmoothness`, `SpecularPower`, `OffsetBias`, `IsParallexMapped` |
| `worldreflect.fx` | Reflective road surfaces — wet roads and rain. | `vs_1_1` / `ps_2_0` | `SurfaceReflection`, `RainIntensity`, `SurfaceSmoothness` |
| `worldbone.fx` | Skinned / animated world objects (16 blend matrices). Techniques `world`, `RenderLightSkinned`. | `vs_1_1` / `ps_1_1` | `MetallicScale`, `DiffuseMin`/`Range` |
| `standard.fx` | Plain textured geometry, no lighting model. Two samplers — diffuse and opacity. | `vs_1_1` / `ps_2_0` | `DiffuseColour`, `Brightness` |

### Nature and sky

| File | What it draws | Shaders | Notable knobs |
|---|---|---|---|
| `sky.fx` | Sky dome and clouds. Techniques `sky`, `tag_hdr`. | `vs_1_1` / `ps_2_0` | `CloudIntensity`, `SkyFogScale`, `TimeTicker` (cloud drift), `Brightness` |
| `trees.fx` | Trees and foliage, two passes. | `vs_1_1` / `ps_2_0` | `Params` (brightness), `LocalCentre` (sway/billboard pivot) |
| `grass.fx` | Volumetric grass, drawn as five stacked shells. Techniques `Grass`, `GrassTransition`, five passes each. | `vs_2_0` / `ps_3_0` | `GRASSCOLOUR`, `GRASSHEIGHT`, `MAXSHELLS`, `NOISESPACE` |
| `waterseashader.fx` | Sea and water, with an animated wave function. | `vs_1_1` / `ps_2_0` | `SpecularScale`, `vTextureOffset` |

### Effects and post-processing

| File | What it draws | Shaders | Notes |
|---|---|---|---|
| `hdr.fx` | The post chain: `downscale2x2`, `downscale4x4`, `bloom`, `blur`, `finalhdrpass`, plus `yuvmovie` for FMV playback and `screen_passthru` blits. | `vs_1_1` / `ps_2_0` | Ten techniques. Where bloom is actually produced |
| `particles.fx` | Particle effects — smoke, sparks, debris. Techniques `particles`, `noshadow`, `onscreen_distort`. | `vs_1_1` / `ps_2_0` | `MaxParticleSize`, `FocalRange`, `BaseAlphaRef` |
| `shadowvolume.fx` | Stencil shadow volumes and shadow-map meshes. Most passes have `PixelShader = NULL`. | `vs_1_1` / `ps_1_1` | Geometry only — nothing to recolour |
| `shadowscreenfilter.fx` | Debug view of the stencil buffer. | `vs_1_1` / `ps_1_1` | Debug |
| `debugPoly.fx` | Screen-space quad blits in several formats — RGBA, alpha-only, 1D, FP32, multiply, pixel-double. Seven techniques. | `vs_1_1` / `ps_1_1`–`ps_2_0` | Utility blits |

### Shared headers

Not shaders themselves — included by the files above, so their code shows up
inside dumps of other effects.

| File | Contains |
|---|---|
| `global.h` | `WorldViewProj`, `ScreenOffset`, and the `world_position` / `screen_position` helpers used by nearly everything |
| `lightscattering.h` | Atmospheric scattering fog constants (`Fog_Br_Plus_Bm`, `Fog_Const_1`…) |
| `auxiliarylighting.h`, `auxiliarylighting_normalmap.h` | Extra local light sources for world geometry |
| `shadowmap_fx.h`, `shadowmap_fx_def.h` | Shadow-map lookup — this is the extra 2D sampler you'll see in `car.fx` |
| `ZPrePass_fx.h` | Depth pre-pass, position only |

> `visualtreatment - Copy.fx` is a byte-identical duplicate of
> `visualtreatment.fx`. Ignore it.

---

## Recipes

Each of these is one line, added just above the `#if DX9_ATEST` block. Restart
the game to see the change.

**Warm the picture up (or cool it down).** Multiply the colour channels:

```hlsl
oC0.rgb *= float3(1.15, 1.00, 0.85);   // warmer
oC0.rgb *= float3(0.85, 0.95, 1.20);   // cooler
```

**Take the colour out.** Blend toward grey — `0.0` is unchanged, `1.0` is fully
black and white:

```hlsl
oC0.rgb = lerp(oC0.rgb, dot(oC0.rgb, float3(0.2125, 0.7154, 0.0721)).xxx, 0.5);
```

**Push the colour further.** Same idea, past `1.0`:

```hlsl
oC0.rgb = lerp(dot(oC0.rgb, float3(0.2125, 0.7154, 0.0721)).xxx, oC0.rgb, 1.4);
```

**Brighten or darken:**

```hlsl
oC0.rgb *= 1.25;
```

**Lift the contrast** around the mid-point:

```hlsl
oC0.rgb = saturate((oC0.rgb - 0.5) * 1.2 + 0.5);
```

**Find out which shader is which.** The bluntest and most reliable trick — make
one shader bright red, run the game, and see what turns red:

```hlsl
oC0.rgb = float3(1, 0, 0);
```

Do that to one file at a time. It costs a restart per guess but it never lies.

---

## Notes and cautions

- **Edits are read at launch.** Save your file, then restart the game.
- **A shader may be used by more than one thing.** Cars share `car.fx` — you
  can't repaint just the player's car from here.
- **Variants.** A filename ending `_v########` is one specific variant of a
  shader (a particular alpha-test and fog combination). Drop that suffix from the
  filename and your edit applies to *every* variant of that shader, which is
  usually what you want.
- **Same file, both backends.** Shader names are derived from the game's own
  shaders, not from the renderer, so one edited file works on DirectX 11 and
  DirectX 12 alike.
- **`ShaderDiskCache` is ignored** while `ShaderDump` or `ShaderMods` is on, so
  loading is a little slower than normal. That's expected.
- **The source this page describes is the Xbox 360 branch of the 2005 shaders**,
  not the PC build. Its `shadowmap_fx_def.h` uses an `asm { tfetch2D ... }`
  block, which is a 360-only GPU instruction, and its compiled artefacts are
  D3DX9 effect containers holding **Xbox 360 microcode**, stamped by the "Xbox
  360 Shader Compiler" — there is no PC shader bytecode anywhere in it, so
  nothing there could be matched against a dump by hash even if you had it.

  It's the same engine and, for the most part, the same shaders — the maths,
  parameters, samplers and technique names all carry over, which is what makes
  this page useful. But the PC build compiled a related, not identical, source:
  a few of these won't even fit inside PC shader limits as written. So treat this
  page as a map, and your own dump as the territory. Where they disagree, your
  dump is right.

Setup and the dump/mod workflow: [README](../README.md).
