# MWDX

**Run Need for Speed: Most Wanted (2005) on a modern graphics API.**

MWDX is a drop-in renderer for Most Wanted. It installs as a replacement
`d3d9.dll`, reads one setting, and runs the game through **Direct3D 11** or
**Direct3D 12** instead of the legacy Direct3D 9 path — with the original
renderer still available whenever you want it.

The game itself is never modified. No patched executables, no altered assets,
no installer. Copy two files in; delete two files to remove it.

---

## Why

Most Wanted targets Direct3D 9, an API from 2002. It still runs on Windows 11,
but through a compatibility path that modern drivers merely maintain rather than
optimise — which is where the stutter, the alt-tab problems and the fullscreen
oddities on current hardware come from.

MWDX runs the same game on an API your GPU driver actually cares about. The
result is smoother frame delivery, sane borderless-fullscreen behaviour, and a
renderer that behaves predictably on hardware built two decades after the game.

MWDX is a renderer, not an enhancement mod. It reproduces what the original
renderer did — same geometry, same materials, same look — with a modern API
underneath. It does not change art, effects or gameplay.

---

## Requirements

- Need for Speed: Most Wanted (2005), **PC retail v1.3** (32-bit)
- Windows 10 or Windows 11
- A GPU with Direct3D 11 or Direct3D 12 support, and current drivers

---

## Install

1. Copy `d3d9.dll` and `MW4Real.ini` into the folder containing `speed.exe`.
2. Set `Backend` in `MW4Real.ini`.
3. Play.

To uninstall, delete those two files. Nothing else is touched.

> **Online play:** MWDX replaces a system library inside the game process. Don't
> use it with services that police process integrity.

---

## Choosing a renderer

```ini
[Renderer]
; 1 = DirectX 9   the original renderer, passed through untouched
; 2 = DirectX 11
; 3 = DirectX 12
Backend=2
```

| Backend | Use it when |
|---|---|
| **1 — DirectX 9** | You want the game exactly as it shipped, or you're establishing a baseline to compare against. |
| **2 — DirectX 11** | The broadly recommended choice. Excellent compatibility across a wide range of hardware. |
| **3 — DirectX 12** | Newest hardware and drivers, and the lowest CPU overhead of the three. |

If your machine can't provide the renderer you asked for, MWDX falls back to one
it can and records why in the log rather than failing to launch.

---

## Settings

Everything lives in `MW4Real.ini`, next to the game executable. Settings are read
at launch — change a value and restart.

### `[Renderer]`

| Key | Default | What it does |
|---|---|---|
| `Backend` | `2` | Renderer to use: `1` DX9, `2` DX11, `3` DX12. |
| `BorderlessFullscreen` | `1` | Serve fullscreen as a borderless window. Alt-tabs instantly. Recommended. |
| `LowLatency` | `1` | Keep the queued-frame count short to reduce input lag. |
| `HalfPixelFix` | `1` | Corrects a sub-pixel sampling difference between Direct3D 9 and later APIs. Leave on. |
| `LegacyColorDefault` | `1` | Restores a Direct3D 9 default some of the game's 2D art relies on. Leave on. |
| `ShaderDiskCache` | `1` | Reuse prepared shaders between launches for faster load times. |
| `UploadRingMB` | `0` | Staging memory per frame, in MB. `0` uses the tuned default. |

### Shader mods

MWDX can load your own shader replacements, so you can change how the game
looks without touching game files.

| Key | Default | What it does |
|---|---|---|
| `ShaderDump` | `0` | Write the shaders MWDX generates into `MWDX\Shaders\Dumped`. Turn this on once to get files to work from. |
| `ShaderMods` | `0` | Load your edited shaders from `MWDX\Shaders`. |

The workflow:

1. Set `ShaderDump=1` and play the section of the game you want to change. MWDX
   writes the shaders it used into `MWDX\Shaders\Dumped`, each one named after
   the shader it came from.
2. Copy a shader out of `Dumped` into `MWDX\Shaders`, and edit it.
3. Set `ShaderMods=1` and `ShaderDump=0`, and restart.

A filename ending in `_v########` targets one variant of a shader; drop that
suffix to apply your edit to every variant. Keep the entry point named `main`
and leave the declared inputs, outputs and register assignments alone —
everything between them is yours.

Shader mods are keyed to the game's own shaders, not to a backend, so a mod you
write once works on both DirectX 11 and DirectX 12.

If an edited shader fails to compile, MWDX logs the error and falls back to the
original for that shader — a mistake in a mod never takes the game down.

### Diagnostics

For troubleshooting and bug reports. Leave them off for normal play; several
cost real performance.

| Key | Default | What it does |
|---|---|---|
| `VerboseLog` | `0` | Full diagnostic stream to `dx9to11.log`. |
| `DebugLayer` | `0` | Graphics API validation layer. Slower, but names problems precisely. |
| `GpuValidation` | `0` | Deeper GPU-side validation. **Much** slower. |
| `PerfLog` | `0` | Periodic frame counters in the log. |
| `DumpFrame`, `CensusLimit` | `0`, `6000` | Per-draw capture for a few frames. Produces large logs. |

---

## Logs

MWDX writes two files next to `speed.exe`:

- **`MW4Real.log`** — short. Which renderer was requested, which is in use, and
  why if those differ. Check this first.
- **`dx9to11.log`** — the detailed stream. Warnings and errors always land here;
  `VerboseLog=1` adds full detail.

When reporting a problem, attach both, say which `Backend` you used, and what
you were doing.

---

## Troubleshooting

**The game started on the original renderer instead of the one I picked.**
`MW4Real.log` records when MWDX couldn't provide the requested renderer and why.
Usually outdated GPU drivers.

**The game won't start.**
Set `Backend=1` and confirm it runs. If it does, capture a log with
`VerboseLog=1` and report it. If it doesn't, remove `d3d9.dll` and check the
game runs on its own — the problem is elsewhere.

**A dialog appeared naming a log file.**
That means MWDX caught the failure and wrote a full record rather than vanishing
silently. Send the file it names.

**Performance is lower than expected.**
Check `DebugLayer` and `GpuValidation` are both `0` — they're diagnostic tools
and they're slow. Then check `dx9to11.log` for a line suggesting a larger
`UploadRingMB`.

**Something looks subtly shifted.**
Try `HalfPixelFix=0` and compare. Either way, that comparison is useful in a
report.

**My shader mod isn't being used.**
Check `ShaderMods=1`, that the file is in `MWDX\Shaders` and not `Dumped`, and
that the filename still matches the shader it came from. `dx9to11.log` names
every override it loads and every one that failed to compile.

---

## Licence and credits

Need for Speed: Most Wanted is © Electronic Arts. MWDX is an independent project,
not affiliated with, endorsed by, or supported by Electronic Arts. It contains no
game assets and requires a legitimate copy of the game.
