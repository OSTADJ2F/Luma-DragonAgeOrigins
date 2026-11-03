# Luma for Dragon Age: Origins

HDR output, recovered highlights and super resolution for **Dragon Age: Origins** (BioWare, 2009 — Eclipse engine,
32-bit, Direct3D 9 running through dgVoodoo2).

This repository is the Luma framework (vendored unmodified from Hlib Omelchenko's fork, see
`README-Luma-Framework.md`) plus this one mod. The mod lives entirely in `Source/Games/Dragon Age Origins`,
`Shaders/Dragon Age Origins` and `Data/Dragon Age Origins`; nothing in the framework is edited, so it can be updated
without touching the mod (see `NOTICE` and `Source/Games/Dragon Age Origins/PORT-STATUS.md`).

## What it does

- **HDR output**: the scene is upgraded to fp16 and the game's own colour-grade curve is continued past the point
  where it clips, so skies, fire and the sun keep their detail up to the display's peak. Paper white, peak white,
  highlight rolloff and highlight colour are adjustable; the UI has its own brightness.
- **Light source boost**: the cores of fire, torches, sparks, magic and the sun are lifted at the shader that draws
  them, which is what gives the recovery real highlights to work with.
- **Super resolution**: DLSS, FSR, and Luma TAA (Luma TAA needs the motion vectors; **MSAA must be off** in the
  game's options), with RCAS sharpening.
- **Ambient occlusion**: XeGTAO, or GT-VBAO as an alternative.
- **Bloom** (off / game / Luma pyramid), bokeh depth of field for cutscenes, film grain, vignette, chromatic
  aberration, colour grading (exposure/contrast/saturation/temperature), gamut expansion towards BT.2020.
- **HUD scale** (the game's own GUI scale), 16x anisotropic filtering, hide UI, hide gameplay UI.
- Pre-rendered Bink videos get a light AutoHDR.

Not in this build: SMAA on the MSAA resolve, HDR10 PQ output (HDR goes through the scRGB swapchain), per-level
anisotropic filtering, the 1000 nit peak cap, and the FSR version choice.

## Requirements

- Dragon Age: Origins (GOG or Steam), 32-bit executable.
- **dgVoodoo2 2.87.3** (`d3d9.dll` in `bin_ship`). The shader hashes are tied to the wrapper version; changing it
  breaks every shader replacement.
- **ReShade 6.8+ with add-on support** (`dxgi.dll`).
- Windows 10/11, an HDR display for HDR output. On an SDR display the mod runs in SDR (decided at game start).
- DLSS/FSR need `Luma-Upscaler.exe` next to the game's exe (it ships in the release zip).

## Build

The framework is compiled into the add-on by the game's project (it includes `Source/Core/unity_build.cpp`):

```
MSBuild "Source/Games/Dragon Age Origins/Dragon Age Origins.vcxproj" -p:Configuration=Development-Release ^
  -p:Platform=Win32 "-p:SolutionDir=<repo>\\" -p:SolutionName=Luma -m

MSBuild "Source/Tools/SR Bridge Helper/SR Bridge Helper.vcxproj" -p:Configuration=Publishing-Release ^
  -p:Platform=x64 "-p:SolutionDir=<repo>\\" -m
```

`-p:SolutionDir` is mandatory in a development build: it bakes the shader path into the add-on. The mod's own notes
(behaviour, shader hashes, port record, the repository boundary rule) are in `Source/Games/Dragon Age Origins/`
(`AGENTS.md`, `HANDOFF.md`, `PORT-STATUS.md`).

## License and credits

Luma is MIT with two addenda: any reuse must name the authors or the project, and commercial use needs their
permission. See `LICENSE.md` and `NOTICE`.
