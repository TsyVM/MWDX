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

1. Copy `d3d9.dll` and `MWDX.ini` into the folder containing `speed.exe`.
2. Set `Backend` in `MWDX.ini`.
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
Backend=3
```

| Backend | State | Use it when |
|---|---|---|
| **1 — DirectX 9** | Complete | You want the game exactly as it shipped, or you're establishing a baseline to compare against. |
| **2 — DirectX 11** | One known defect — see below | Your GPU or driver can't give you DirectX 12, and you can do without the HUD gauges. |
| **3 — DirectX 12** | Complete | **The recommended choice.** Newest hardware and drivers, the lowest CPU overhead of the three, and the whole game renders correctly. |

If your machine can't provide the renderer you asked for, MWDX falls back to one
it can and records why in the log rather than failing to launch.

### Known issue — DirectX 11 HUD gauges

`Backend=2` runs the entire game and is fully playable: world, cars, traffic,
effects, menus, races, pursuits and the free-roam map all render and behave
correctly. One thing does not:

- the **speedometer needle** and the **live speed digits** don't draw
- the **minimap contents** — roads, blips, the player arrow — don't draw

The dial frames, the glass overlay and the minimap border are all still there;
it's what goes inside them that's missing. Nothing else is affected, and it
costs you no performance and no progress — you just can't read your speed or
the minimap off the HUD.

This is a bug in MWDX's DirectX 11 path, not in your setup, and no setting
works around it. It does not occur on `Backend=1` or `Backend=3`. If the HUD
matters to you, use `Backend=3`.

---

## Settings

Everything lives in `MWDX.ini`, next to the game executable. Settings are read
at launch — change a value and restart.

### `[Renderer]`

| Key | Default | What it does |
|---|---|---|
| `Backend` | `3` | Renderer to use: `1` DX9, `2` DX11, `3` DX12. |
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
| `ShaderDump` | `0` | Write the shaders MWDX generates into `MWDX\Shaders\Dumped`, so you have files to work from. |
| `ShaderMods` | `0` | Load your edited shaders from `MWDX\Shaders`. |

**Set them both to `1` and leave them there.** They do different jobs and are
meant to run together:

```ini
ShaderDump=1
ShaderMods=1
```

That gives you two folders next to the game:

```
MWDX\Shaders\           <- your edits go here
MWDX\Shaders\Dumped\    <- MWDX writes the originals here, untouched
```

Then:

1. **Play.** Every shader the game uses gets written into `Dumped`, named after
   the shader it came from. Drive the part of the game you want to change —
   only the shaders actually used show up, so this is also how you find the
   one you're after.
2. **Copy** a file out of `Dumped` into `MWDX\Shaders` one level up, and edit
   your copy.
3. **Restart.** MWDX now uses your version of that shader and the stock version
   of everything else.

Dumping only ever writes the *original* shader, and only into `Dumped`, so it
can't overwrite your work — you always have a clean reference to go back to.
Leave both keys on and steps 1–3 repeat as often as you like. Edits are read at
launch, so restart the game each time you change a file.

While either key is on, `ShaderDiskCache` is ignored for the run, so a cached
copy of the original can never mask a shader you've edited. Loading is a little
slower as a result; turn both off again when you're done and the cache comes
back.

A filename ending in `_v########` targets one variant of a shader; drop that
suffix to apply your edit to every variant. Keep the entry point named `main`
and leave the declared inputs, outputs and register assignments alone —
everything between them is yours.

**[Index/Shaders.md](Index/Shaders.md) is the field guide** — how to tell which
shader you've dumped, what each one of the game's effects does, where in the file
it's safe to edit, and a set of copy-paste recipes. Start there.

Shader mods are keyed to the game's own shaders, not to a backend, so a mod you
write once works on both DirectX 11 and DirectX 12.

If an edited shader fails to compile, MWDX logs the error and falls back to the
original for that shader — a mistake in a mod never takes the game down.

### Diagnostics

For troubleshooting and bug reports. Leave them off for normal play; several
cost real performance.

| Key | Default | What it does |
|---|---|---|
| `VerboseLog` | `0` | Full diagnostic stream to `MWDX-render.log`. |
| `DebugLayer` | `0` | Graphics API validation layer. Slower, but names problems precisely. |
| `GpuValidation` | `0` | Deeper GPU-side validation. **Much** slower. |
| `PerfLog` | `0` | Periodic frame counters in the log. |
| `DumpFrame`, `CensusLimit` | `0`, `6000` | Per-draw capture for a few frames. Produces large logs. |

---

## Logs

MWDX writes two files next to `speed.exe`:

- **`MWDX.log`** — short. Which renderer was requested, which is in use, and
  why if those differ. Check this first.
- **`MWDX-render.log`** — the detailed stream. Warnings and errors always land here;
  `VerboseLog=1` adds full detail.

When reporting a problem, attach both, say which `Backend` you used, and what
you were doing.

---

## Troubleshooting

**The game started on the original renderer instead of the one I picked.**
`MWDX.log` records when MWDX couldn't provide the requested renderer and why.
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
and they're slow. Then check `MWDX-render.log` for a line suggesting a larger
`UploadRingMB`.

**Something looks subtly shifted.**
Try `HalfPixelFix=0` and compare. Either way, that comparison is useful in a
report.

**My shader mod isn't being used.**
Check `ShaderMods=1`, that you restarted the game after saving, that the file is
in `MWDX\Shaders` and not still down in `Dumped`, and that the filename still
matches the shader it came from. `MWDX-render.log` names every override it loads
and every one that failed to compile.

---

## Licence and credits

Need for Speed: Most Wanted is © Electronic Arts. MWDX is an independent project,
not affiliated with, endorsed by, or supported by Electronic Arts. It contains no
game assets and requires a legitimate copy of the game.
