# Dragon Age: Origins Luma mod: project description for coding agents

Read this file first, then `HANDOFF.md` (same folder) for the current state and the task list. This file is stable reference; `HANDOFF.md`
changes every session.

Everything here was checked against the code on 2026-11-02 (branch `dragon-age-origins-sr`, last commit `b5d3d950`).

## 0. Repository boundary (read first)

This repository is the Luma framework (vendored unmodified from Hlib Omelchenko's fork at `23019de8`) plus this one
mod. **Everything the mod owns is in `Source/Games/Dragon Age Origins`, `Shaders/Dragon Age Origins` and
`Data/Dragon Age Origins`.** Nothing else may be edited: `Source/Core`, `Source/Tools`, `Source/External`,
`Shaders/Includes`, `Shaders/Global`, the `Luma.*.props` files and `Scripts/` are the framework, and a change there
makes every future framework update a conflict. `Scripts/check-mod-boundary.sh` enforces this as a `commit-msg` hook
and rejects any commit that touches those paths unless its message starts with `Framework:` (used when vendoring an
upstream update). See `PORT-STATUS.md` for what was removed to get here (the axed features are gone for good).

Older notes below (`HANDOFF.md`, `TestScenePlan.md`, the paths in the build section) still describe the previous
repo, `C:\Stuff\Luma`, which had every other Luma game and the framework's own history. The build commands are the
same, with `C:\Stuff\Luma-framework-clean` as the repo root.


---

## 1. What the project is

A **Luma** mod for **Dragon Age: Origins** (DAO, BioWare 2009, Eclipse engine, **32-bit**, **Direct3D 9**). Luma is a C++ framework that runs
as a **ReShade add-on** (`.addon` = a DLL) and replaces or adds rendering passes in Direct3D 11 games.

DAO is D3D9, so it runs through **dgVoodoo2** (`d3d9.dll` in the game folder), which translates every D3D9 call and shader to D3D11. Luma
then hooks the D3D11 side. Consequences you must remember:

- Every game shader Luma sees is a **dgVoodoo translation** (`ps_5_0`), not BioWare's original. Shader hashes depend on the dgVoodoo
  version. The pinned version is **2.87.6** (`dgVoodoo2_87_6.zip`, the x86 `D3D9.dll`, shipped with `Data/Dragon Age Origins/dgVoodoo.conf`). Changing dgVoodoo changes the hashes: do not.
- Game shaders read only constant buffers **cb3** and **cb4**. D3D9 constant `cN` is `cb4[N + 8]` in pixel shaders. Vertex shader `cN` is
  `cb4[N + 20]`.
- **The dgVoodoo mask rule:** after every texture sample, dgVoodoo emits an `and`/`or` pair against `cb3[44]`/`cb3[45]` (format masks). A
  replacement shader must keep it (the helper is `ApplyDgvMask(...)` from `Shaders/Includes/DgVoodoo.hlsl`). Dropping it shifts colours.
- A replacement shader's entry point must declare **the same input registers** as the original (linkage is by register, not by name).
- Shaders that are `ps_4_0`, or that read cb0-cb2, are dgVoodoo's own blits, Luma's, or ReShade's: not the game's.

What the mod adds (all shipped in the current release unless marked):

| Feature | Where |
|---|---|
| HDR output (scRGB swapchain) with recovered highlights, Paper White / Peak White | `ColorGrade_0x6B504D9F.ps_5_0.hlsl`, Core display composition |
| Real SDR output when Windows HDR is off (decided at start, restart to change) | `sdr_swapchain_on_sdr_display` in `main.cpp` DllMain, `Source/Core/core.hpp` |
| Light Source Boost (brighter cores of fire/magic sprites, sky sun) | `Particle_0x5DE6B6F6`, `VFXAlpha_0x41DE4D45`, `VFXVolume_0x0A2ABD19`, `Sky_0x1A13F3EF` |
| Bloom: Off / Game / Luma pyramid bloom | `OnDrawOrDispatch` bloom block, `Luma_Bloom_impl.hlsl` |
| Ambient occlusion (XeGTAO or GT-VBAO, the game has none) | `RunXeGTAO()`, `Luma_DAO_XeGTAO.hlsl`, `Luma_DAO_AOApply.hlsl`, `Luma_DAO_DepthLinearize.hlsl` |
| Upscalers / AA: Luma TAA, DLSS, FSR (DLSS/FSR run in a 64-bit helper, "SR bridge") | `DrawUpscaler()`, `FillMotionVectors()`, `MotionVectorPatches.h`, Core `scene_motion_vectors.h` |
| SMAA on top of the game's 8x MSAA, HDR-aware MSAA resolve | `OverrideResolveTextureRegion()`, `ResolveAndSMAA()`, `Luma_SMAA_impl.hlsl`, `Luma_DAO_ResolveMSAA.hlsl` |
| RCAS sharpening after the upscaler | `DrawUpscaler()` |
| Cutscene bokeh depth of field | `DoFComposite_0xB507EFCE.ps_5_0.hlsl`, `BindDoFScene()` |
| Film grain, Exposure/Contrast/Saturation/Temperature, Shadow Lift, Gamut Expansion | colour grade shader |
| Video (Bink movies) clamp + light AutoHDR | `Video_0x39642745.ps_5_0.hlsl` |
| UI brightness slider separate from the scene (UI Paper White) | `UI_DRAW_TYPE 2`, `Includes/UIPreScale.hlsl` |
| Hide UI (session only), anisotropic filtering (restart) | `OnDrawOrDispatch` top, DllMain |
| HUD scale (engine's own GUI scale, replaced by a mid hook; **Auto by default**) | `OnGUIScaleRead()`, `InstallGUIScaleHook()`, `includes/safetyhook.hpp`; confirmed working by the user |
| Vignette, Chromatic Aberration | implemented, confirmed working by the user (see `HANDOFF.md`) |

Author / credits: the user publishes as "Ostad Gaming" (Ko-fi https://ko-fi.com/ostadgaming). Community: "HDR Den" Discord.

---

## 2. The frame, in draw order (pixel shader hashes)

Use these hashes to find things in a trace (`luma_trace_capture`) and in `main.cpp`. Names in brackets are the BioWare DX9 shader names
(from `Scripts/dao_gallery/match_dx9.py`).

1. Shadow maps: `0x49E804A7` (pssm), 1024x1024.
2. Opaque scene: `0x38A5D9CD` (static_depth / skin_depth), `0xD24D2C23`, and many more mesh shaders, into the 2560x1440 (swapchain size)
   scene target. Its handle says R8G8B8A8 but Luma upgrades it to **R16G16B16A16_FLOAT** behind the game's back. The first draw of
   `38A5D9CD`/`D24D2C23` **opens the scene** for motion vectors.
3. Depth-tested overlays `0x752783AE` (decals/water-like, vertex shaders read cb0 not cb4).
4. Post-opaque copy pass `0x9617B1F5` (distortion).
5. **First particle draw `0x5DE6B6F6`** (vfx0_addv, additive sprites): AO runs right before it (`RunXeGTAO`).
6. Particles / effects: `0x5DE6B6F6` (~500 draws), `0x41DE4D45` (vfx0_atm alpha sprites; also used by the console text), `0x0A2ABD19`.
7. Sky `0x1A13F3EF`, water `0x4A96DC95` (bytecode-patched clamp, not file-replaced).
8. Bloom: bright pass `0x3549D37F` (illuminationmaskmult), blur `0x6184D504` x2, composite `0x769096CC`.
9. Cutscene DoF composite `0xB507EFCE`.
10. **Colour grade `0x6B504D9F`** (hslmatrix): 3x3 matrix + 256x1 curve LUT. This is "the tonemap". Before it runs, the motion vector fill
    and the upscaler run, and the upscaler's output is bound in place of the scene. SMAA runs on the next MSAA resolve after it.
11. HUD / Scaleform: `0x31AE03D5`, `0xB972AC3B`, `0x3DCC5533`, `0x285B7BA2`, `0xE9107C4B`, gui `0xA3DF8467`. They draw onto the same
    8-bit canvas as the grade, in gamma space. Hide UI skips draws into that canvas after the grade.
12. dgVoodoo's present copy `0x880A17D3` (ps_4_0), then Luma's display composition (encodes to scRGB HDR or SDR).

Movies: `0x39642745` (binknoalpha) writes straight to the canvas, no grade.

---

## 3. Repository map (only what this mod touches)

Repo root: `C:\Stuff\Luma`. Paths below are relative to it.

### C++ (rebuild + game restart needed after any change)
- `Source/Games/Dragon Age Origins/main.cpp` (~2460 lines) — the whole game mod. Key parts by line (approximate):
  - 1-110: `#define` feature switches (`ENABLE_SMAA`, `ENABLE_BLOOM`, `ENABLE_LUMA_TAA`, `ENABLE_SR_BRIDGE_XESS 0`, ...) and global knobs.
  - 107-227: `GameTime` (dev only: pause / slow the game's clock; Pause/Break toggles it).
  - 229-336: `DragonAgeOriginsGameDeviceData` (per-device state).
  - 346-450: `OnInit()` — custom shader registrations, Core defines, paper white defaults, **every game setting default**.
  - 495-640: MSAA resolve + SMAA.
  - 640-1040: scene open, jitter, reactive masks, motion vector fill, `DrawUpscaler()`.
  - 1043-1115: `MotionVectorGame` traits for Core's `SceneMotionVectors`.
  - 1144-1405: AO (`RunXeGTAO`).
  - 1408-1658: **`OnDrawOrDispatch()`** — the per-draw hook: Hide UI, AO trigger, bloom, DoF, motion vectors, grade/upscaler.
  - 1660-1720: `BindDoFScene()`.
  - 1721-1834: `OnPresent()` (per-frame resets).
  - 1835-1911: `LoadConfigs()` — reads settings from `ReShade.ini` (`reshade::get_config_value`).
  - 1912-2338: `DrawImGuiSettings()` — the settings UI (sections: HDR Color, Color Grading, Bloom, Film Grain, Lens Effects, Depth of
    Field, Videos, UI, Anti-Aliasing, Textures, Ambient Occlusion, Advanced, Tuning (dev), Debug).
  - 2339-2390: About/credits. 2392-end: `DllMain` (swapchain, texture upgrade, sampler setup; anisotropy read after `CoreMain`).
  - Top of the file (after the anisotropy globals): the **HUD scale** hook — `PlausibleGameResolution()`, `OnGUIScaleRead()` (the mid hook
    callback) and `InstallGUIScaleHook()` (the exe pattern scan). Installed from `DllMain` after `CoreMain`, see §8.
- `Source/Games/Dragon Age Origins/includes/` — `safetyhook.cpp/.hpp` + `Zydis.c/.h`: safetyhook's single file amalgamation and the Zydis
  instruction decoder it uses, copied from Watch Dogs 2's vendored copy (same approach as Granblue / Blue Reflection). Third party, not ours.
- `Source/Games/Dragon Age Origins/MotionVectorPatches.h` — which cb4 rows hold the cameras (see `MotionVectorCensus.md`).
- `Source/Core/core.hpp` (~21000 lines) and `Source/Core/includes/*` — **shared by every Luma game**. Change only when unavoidable, keep
  new behaviour behind a flag that defaults to off, and say so in the commit message ("Core: ..."). The DAO mod currently changes **nothing** in
  Core (a temporary `Game::OverrideCreateResource()` hook was added for the character-preview supersample and removed again when that feature was
  dropped, so Core is back to upstream).

### Shaders (hot reload, no rebuild)
- `Shaders/Dragon Age Origins/<Name>_0x<HASH>.ps_5_0.hlsl` — file replacements of game shaders, matched by hash in the file name.
- `Shaders/Dragon Age Origins/Luma_*.hlsl` — Luma's own passes (registered in `OnInit` or by Core).
- `Shaders/Dragon Age Origins/Includes/GameCBuffers.hlsl` — **`CB::LumaGameSettings`, the settings struct shared by C++ and HLSL.**
- `Shaders/Dragon Age Origins/Includes/` — `Common.hlsl`, `Settings.hlsl`, `DAOCamera.hlsl` (near 0.15, far 4000, 60° vertical FOV),
  `FilmGrain.hlsl`, `UIPreScale.hlsl`.
- `Shaders/Includes/` — generic Luma includes (Color, Oklab, Reinhard, DgVoodoo, SMAA, MotionVectorFill, ...). Shared with other games.
- `Shaders/Dragon Age Origins/Dump/` — shaders dumped by dev builds (531 files). `*.cso` is git-ignored.

### Data, scripts, docs
- `Data/Dragon Age Origins/Nexus-Description.txt`, `Discord-Post.txt` — release texts (BBCode). Update them when a user-visible feature changes.
- `Data/Dragon Age Origins/Release-Packaging.md` — **rules for the release zip. Follow exactly.**
- `Scripts/package.ps1` — builds the release zip. `Scripts/luma_mcp.py` — the Luma MCP server (live inspection, see §6).
- `Scripts/dao_gallery/` (untracked) — shader-coverage tools: `erf.py`/`gff4.py` (read game archives), `match_dx9.py` (names dumped
  shaders by matching the game's DX9 originals), `hunt_dx9.py` (where to go in game to see the shaders not seen yet), `shader_bank.py`,
  `gamekeys.py`, `gen_steps.py`, `inventory.py`, `*.nss` (NWScript experiments, abandoned).
- Docs in this folder: `HANDOFF.md` (current state), `PerformanceNotes.md` (measurements, how to measure), `MotionVectorCensus.md`,
  `TestScenePlan.md` (the in-game NWScript gallery: **obsolete**, kept for history).

---

## 4. How to add or change a setting (the most common task)

Follow every step; missing one gives a setting that silently does nothing or is not saved.

1. `Shaders/Dragon Age Origins/Includes/GameCBuffers.hlsl`: add `float MySetting;` **at the end** of `CB::LumaGameSettings`, with a comment
   giving its range and what 0 means. Never insert in the middle: the field index is the `game:<n>` index used by `luma_set_setting`.
2. `main.cpp` `OnInit()`: set the default on both structs, in the same style as its neighbours:
   `default_luma_global_game_settings.MySetting = cb_luma_global_settings.GameSettings.MySetting = 0.f; // Off`
3. `main.cpp` `LoadConfigs()`: `reshade::get_config_value(runtime, NAME, "MySetting", cb_luma_global_settings.GameSettings.MySetting);`
   (wrap in `#if DEVELOPMENT` if the setting is dev-only).
4. `main.cpp` `DrawImGuiSettings()`: slider/checkbox in the right section. On change: `reshade::set_config_value(...)` and
   `device_data.cb_luma_global_settings_dirty = true;`. Add a tooltip (`ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)`) and a
   `DrawResetButton(...)`. Copy an existing block (e.g. "Film Grain") rather than writing from scratch.
5. The shader reads it as `LumaSettings.GameSettings.MySetting`.
6. Rebuild (struct changed), deploy, restart the game.
7. If users see it: add a line to `Nexus-Description.txt` and `Discord-Post.txt`.

HDR-only settings must do nothing in SDR (`LumaSettings.DisplayMode > 0` gate in the shader) and their UI sits inside the `if (enable_hdr)`
blocks, greyed out when `cb_luma_global_settings.DisplayMode == DisplayModeType::SDR`.

## 5. How to replace or patch a game shader

1. Get the hash from a trace (`luma_trace_capture` → `luma_trace_list`), or from `Shaders/Dragon Age Origins/Dump/0x<HASH>.ps_5_0.cso`.
2. Disassemble: `"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe" /dumpbin /Fc out.asm 0x<HASH>.ps_5_0.cso`
   (run from **PowerShell or cmd**, Git Bash mangles `/Fc`). Or `luma_dump_shader`.
3. Write `Shaders/Dragon Age Origins/<Role>_0x<HASH>.ps_5_0.hlsl` that reproduces the original exactly (same input registers, same
   cb3/cb4 reads, `ApplyDgvMask` after every sample), then change only what HDR needs. Start the file with a comment block: what the
   shader is, which DX9 shader it came from, and what was changed for HDR (see the existing files for the style).
4. `luma_reload_shaders`, then check the output with `luma_trace_capture` + `luma_read_resource` (stats: min/max/NaN).
5. **Blended shaders:** prefer a bytecode patch (`PatchShaderBytecodeSync` + `DXBC::ClampOutput0`, see the water `0x4A96DC95` in
   `main.cpp` near line 476) over a file replacement. A file-replaced scene shader **loses its motion vector patch** and ghosts with
   DLSS/FSR/TAA.

---

## 6. Build, deploy, test

### Machine facts
- OS Windows 11. Repo `C:\Stuff\Luma`. Game `C:\Games\Dragon Age Origins` (GOG), exe `bin_ship\DAOrigins.exe`, addon goes in `bin_ship`.
- MSBuild: `C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe` (VS 2026). Platform is always **Win32**.
- Env var `LUMA_DRAGON_AGE_ORIGINS_BIN_PATH` is set: a build **with** `-p:SolutionName=Luma` copies the addon into `bin_ship` by itself
  (post-build xcopy). That overwrites whatever is deployed: back it up first (`C:\Stuff\Luma-DAO-dev-backup`).
- Python 3 with numpy (for `Scripts/luma_mcp.py` and the gallery scripts).
- If you run in **WSL2**, the repo is `/mnt/c/Stuff/Luma`; run MSBuild, fxc and `package.ps1` through `powershell.exe -Command "..."` or
  `cmd.exe /c "..."` because they are Windows programs. Keep Windows paths (backslashes) inside those commands.

### Build (Development, for testing)
```powershell
& "C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" "C:\Stuff\Luma\Source\Games\Dragon Age Origins\Dragon Age Origins.vcxproj" -p:Configuration=Development-Release -p:Platform=Win32 "-p:SolutionDir=C:\Stuff\Luma\\" -p:SolutionName=Luma -m -v:minimal
```
- `-p:SolutionDir` is **mandatory**: it bakes the repo path in, so the dev build loads shaders from `C:\Stuff\Luma\Shaders`. Without it
  the game shows "display composition shaders failed to be found" asserts.
- Output: `C:\Stuff\Luma\Binaries\Win32-Development-Release\Luma-Dragon Age Origins.addon`.
- Configurations: `Development-Release` (dev tools, MCP pipe, dev sliders), `Development-Debug` (slow, only for debugging),
  `Test-Release`, `Publishing-Release` (the release; no MCP pipe; dev-only settings are fixed at their defaults and not read from config).
- A build succeeds only with **0 errors**. Read the last 30 lines of the output. Warnings that existed before are fine.

### Deploy
- The game **must be closed** (the DLL is locked while it runs). Check with `tasklist /FI "IMAGENAME eq DAOrigins.exe"`.
- Copy the addon to `C:\Games\Dragon Age Origins\bin_ship\Luma-Dragon Age Origins.addon` (automatic with `-p:SolutionName=Luma`).
- **If `bin_ship\Luma\` exists, a dev build ignores the repo `Shaders` folder.** It must not exist for dev testing. Ask the user to delete it.
- Never overwrite the user's `dgVoodoo.conf`, `ReShade.ini`, `ReShadePreset.ini`, `d3d9.dll`, `dxgi.dll` in `bin_ship`.

### Live testing (needs the user)
- Only the user can start the game and put it in a scene. Ask them, and tell them what to look at.
- The Luma MCP server (`Scripts/luma_mcp.py`, declared in the repo's `.mcp.json`; for Hermes add it under `mcp_servers` in
  `~/.hermes/config.yaml` with `command: python`, `args: ["C:/Stuff/Luma/Scripts/luma_mcp.py"]`, working directory the repo root) only
  answers with a **Development** build and a game that is **presenting** (in front, not minimised, not paused in a loading screen).
  `luma_status` answers even when not presenting (`presenting=false`).
- Tools: `luma_status` → `luma_trace_capture` (`trigger=<hash>` for a pass that does not draw every frame, `frames=N`) → `luma_trace_list`
  → `luma_trace_get` / `luma_read_resource` / `luma_read_cbuffer` by entry index. `luma_reload_shaders` (hot reload; `unload=true`
  runs the vanilla shaders), `luma_set_setting game:<index> <value>` (live, not saved), `luma_ab` (A/B a dev value with noise floor),
  `luma_perf` (frame times; turn vsync / frame caps off first), `luma_compare` (diff two readbacks, `metrics=["flip"]`), `luma_log`.
  Readbacks land in `%TEMP%\luma-mcp` (`.bin`, `.json`, `.png`).
- Reading the grade output: values are **gamma encoded and pre-scaled**: nits = `g^2.2 × UIPaperWhite`. Not linear.

### Release (only when the user asks)
1. Build `Publishing-Release` (same command, other configuration).
2. From PowerShell: `C:\Stuff\Luma\Scripts\package.ps1 -Project "Dragon Age Origins" -Config Publishing-Release -Platform Win32 -OutDir C:\Stuff\Luma-DAO-release -NoExe`
3. Check the zip against `Data/Dragon Age Origins/Release-Packaging.md` (no `.exe`, no `Dump`, no `.cso`, no ReShade.ini).

---

## 7. Rules (do not break these)

1. **Never commit, push, or create a PR unless the user asks.** Branch is `dragon-age-origins-sr`; `main` is upstream's. Commit messages
   start with `Dragon Age Origins: ` (or `Core: ` for core changes) and describe the user-visible change.
2. **No BioWare data in the repo.** Extracted archives, DX9 shader disassembly, `script.ldf`, inventory/bank/hunt outputs stay in
   `%TEMP%\luma-dao-gallery` or another folder outside the repo.
3. **Do not change dgVoodoo** (version or `dgVoodoo.conf`): all hashes depend on it.
4. **Core (`Source/Core`, `Shaders/Includes`) is shared with other games.** New behaviour there must be opt-in, default off.
5. **No config reads in `DllMain` before `CoreMain()`**: it breaks the ReShade add-on registration ("invalid module handle"). Boot-time
   reads go after `CoreMain()` (see the anisotropy read at the end of `DllMain`) or in `OnInit`.
6. Append new cbuffer fields at the end (§4).
7. Keep the vanilla look reproducible: every HDR change must reduce to the vanilla image in SDR mode (`DisplayMode == 0`) unless the
   setting is explicitly an SDR feature too (bloom, AO, grain, AA, vignette).
8. Don't delete things in the game folder yourself. Ask the user.
9. Match the existing code style: 3-space indent in C++ (tabs in `GameCBuffers.hlsl`), full-sentence comments explaining **why**,
   British spelling ("colour") in comments and UI text, settings named in Title Case in the UI.
10. Line endings: the repo uses CRLF on checkout; keep whatever the file has. Git warns "LF will be replaced by CRLF": harmless.
11. Never claim something works in game unless the user confirmed it or you measured it through the MCP tools. Write "untested in game"
    otherwise, in `HANDOFF.md` too.

---

## 8. Hard-won lessons (check these first when something looks wrong)

- **fp16 doesn't clamp.** The game was made for 8-bit targets that clamp to 0..1 for free. On the upgraded fp16 targets, blended
  materials can write -100..65504 (water `0x4A96DC95`, the DoF composite's alpha of 211). Symptom: garbage colours, wild reflections
  at some angles. Fix with a clamp in a bytecode patch (blended) or in the replacement (non-blended).
- **File-replaced shaders skip the motion vector patch** → ghosting with DLSS/FSR/TAA. Use a bytecode patch for scene draws.
- **DLSS auto exposure must stay off** (`.auto_exposure = sr_type != DLSS` in `DrawUpscaler`): transformer presets J-M flicker the sky.
- **The frame camera read flips ~180° for a frame or two** (VS `0x318D7C41`'s buffer); Core logs "implausible camera reprojection".
  `reuse_last_reprojection` bridges it.
- **Full-black flicker** was the game **skipping the world render** on some frames (geometry hashes `0x38A5D9CD`, `0x729B99A4`,
  `0x61C7CFF5` at 0 draws), not a shader bug. Diagnose with `luma_trace_capture frames=200`, check geometry draw counts per frame, then
  repeat with `luma_reload_shaders unload=true` before blaming shader edits.
- **UI prescale:** the HUD blends in gamma space on the same canvas as the scene, and the display composition scales the whole canvas by
  UI/Game paper white. So the grade and the video divide by that ratio (`ApplyUIPaperWhitePreScale`), using the composition's own decode
  (sRGB + gamma 2.2 correction), not `game_gamma_*`. SMAA's threshold is scaled to match (`SMAA_THRESHOLD_SCALE`).
- **A plain Reinhard tone-map cannot reach the display peak from the game's ~1.3x paper white range** (measured ≈220 nits vs 1009 at a
  1009-nit peak when Ersh's RenoDX port was tried): highlight recovery has to continue the game's own curve instead.
- **Bokeh DoF cost** is linear in taps (~0.026 ms/tap at 1440p); default 24 taps with a mip chain. See `PerformanceNotes.md`.
- **SR bridge:** DLSS/FSR run in a 64-bit helper (game is 32-bit), so each frame copies ~6 textures across processes. Low GPU usage
  with DLSS is the fence wait, not a free GPU.
- **dgVoodoo creates DX11 shaders lazily.** A DX9 shader the game never drew this session is not in the dump. Coverage is built up by
  playing different areas/effects/graphics settings (`hunt_dx9.py` lists where).
- **Python via shell heredocs:** `\n` inside strings can become a real newline. Write `chr(92) + 'n'` when generating C++ string literals.
- **The project force includes `GameCBuffers.hlsl` into every C/C++ file** (`ForcedIncludeFiles` in the vcxproj). Any vendored C or C++ file
  added to it (safetyhook, Zydis) must clear that property or it fails on the HLSL header.
- **A safetyhook mid hook's `ctx.esp` is the `esp` at the hook site**, so reading and writing the original stack frame from the callback works
  and the relocated instructions in the trampoline see the same frame (verified from its x86 stub: two `push esp`, then `ctx.esp += 8`). This is
  why the HUD scale hook can rewrite `[esp+8]` without touching the engine's registers. It is also the least portable thing in the repo: the
  whole feature dies politely if the exe's pattern does not match exactly once.
- **Patch code in `DllMain` after `CoreMain()`, not in `OnInit()`**, when the game may read the patched value before the first frame: `OnInit`
  runs at device/first present time, which is too late for a GUI that is laid out during start up (the HUD scale hook is installed there).
- `Scripts/dao_gallery/*.py` import each other: run them as `python Scripts/dao_gallery/<tool>.py`, not with `python -I`.

## 9. Game modding facts (for the shader coverage work)

- Game data: `C:\Games\Dragon Age Origins\packages\core\data\*.erf` (ERF V2.0, read with `Scripts/dao_gallery/erf.py`). Shaders ship as DX9
  bytecode in `shaders.erf` (`*.psh.fxo`, 178 distinct PS; `sm2_*` are unused fallbacks). `packages\core\patch\patch002.erf` replaces 42
  post-processing shaders: match against it.
- Effect → shader chain is pure data: VFX row (`vfx_base.gda`) → model → `.mao` (materialobjects.erf) → `.mat.xml` (materials.erf,
  Semantic → Pass `PixelShader`).
- Engine script API: `script.ldf` inside `packages\core\data\scripts.erf` (all functions with signatures, ~480 constants).
- DA Toolset 1.0.1008 + SQL Server Express are installed (only needed to compile NWScript). The NWScript gallery approach was dropped
  as too fragile; prefer the static DX9 matching (`match_dx9.py` → `hunt_dx9.py`).
- Game launched with `-enabledeveloperconsole`; console key is backtick; the ReShade overlay must be closed for the console to work.

## 10. Glossary

- **Ostad style**: the default HDR grade (reproduces vanilla exactly up to each curve's clip point, then continues the curve and rolls
  highlights off to the display peak). Named after the author.
- **Paper white**: the brightness of "SDR white" in nits. Scene default 102, UI default 203.
- **Canvas**: dgVoodoo's 8-bit backbuffer surface that the grade, HUD and movies draw on (upgraded by Luma).
- **SR**: super resolution (DLSS/FSR/Luma TAA). **SR bridge**: the 64-bit helper (`Luma-Upscaler.dll`) that runs DLSS/FSR.
- **Hash**: the 32-bit shader hash Luma uses (`0x6B504D9F`), in file names and traces.
- **MCP**: the Luma live-inspection tool server (`Scripts/luma_mcp.py`).
