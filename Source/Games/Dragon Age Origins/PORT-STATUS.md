# Port status: DAO mod on a pristine Luma clone (2026-11-02)

Working copy: `C:\Stuff\Luma-framework-clean` — a full clone of `github.com/Filoppi/Luma-Framework`
(`main` @ `6fce3405`, 2026-11-01) with submodules initialised. The old repos are untouched
(`C:\Stuff\Luma`, `C:\Stuff\Luma-DAO-dev-backup`).

Goal: the mod lives entirely in `Source/Games/Dragon Age Origins`, `Shaders/Dragon Age Origins` and
`Data/Dragon Age Origins`; common code (`Source/Core`, `Shaders/Includes`, `Shaders/Global`) is never edited,
so merging the framework back in can't conflict.

## 1. What was added

Copied from the working repo, adapted: `Source/Games/Dragon Age Origins` (main.cpp, MotionVectorPatches.h,
vcxproj, docs, vendored safetyhook + Zydis), `Shaders/Dragon Age Origins` (no `Dump/`, no `.cso`),
`Data/Dragon Age Origins`.

## 2. The eleven decisions, applied

| # | Decision | State in the clone |
|---|---|---|
| 1 | Peak white cap: revert, Core untouched | **done** — `max_scene_peak_white` never set; the peak is whatever the display reports |
| 2 | UI paper white 203, Core's own default | **done** — `default_paper_white`/`default_ui_paper_white` not overridden; the scene's 102 nits is applied game-side by `ApplyScenePaperWhiteDefault()` (from `LoadConfigs()` and the new `OnDisplayModeChanged()` override) whenever the value still sits at Core's default and the mode is HDR |
| 3 | SDR swapchain, game-local | **done** — `DllMain` probes `Display::IsHDRSupportedAndEnabled()` on the primary display before `CoreMain` and picks `SwapchainUpgradeType::None` when HDR is off; Core's own rule then puts the frame in the SDR display mode. Loss accepted: the display settings section stays visible, and a game on a secondary HDR display takes the SDR path |
| 4 | XeSS, game-local | **done** — the `ENABLE_SR_BRIDGE_XESS` define and Core guards are gone; the package simply doesn't ship `libxess.dll`, so the (still listed) XeSS entry fails if picked |
| 5 | FSR version option | **dropped** — no `SR::fsr_prefer_3`, no `SettingsData::prefer_fsr3`, no combo, no `FSRPrefer3` config key |
| 6 | Version line | **dropped** — `Globals::SHOW_VERSION` not set; the About page prints the internal version |
| 7 | Anisotropic filtering, fixed 16x | **done** — the AF combo, its globals and its config key are gone; Core's sampler upgrade stays at its own fixed 16x (`enable_samplers_upgrade`, `samplers_upgrade_mode`, `force_upgrade_linear_samplers`) |
| 8 | Motion vectors: copy | **done** — `scene_motion_vectors.h`, `patched_draws.h`, `motion_vector_patch.h`, `dxbc.h` (3,674 lines) vendored into `includes/`, `MotionVectorPatches.h` includes the local copies, `dxbc.h`'s two Core includes repointed |
| 9 | SMAA: copy | **done** — `Shaders/Dragon Age Origins/Includes/{SMAA.hlsl,SMAA_Passes.hlsl}` vendored, `SMAA_THRESHOLD_SCALE` applied in the local copy, `Luma_SMAA_impl.hlsl` includes the local `SMAA_Passes.hlsl` |
| 10 | TAA look, game-local | **blocked** — pristine Luma has no Luma TAA at all (see §3). When the framework's TAA is vendored, its `Luma_TAA_CS.hlsl` needs a two-line generic `#ifndef TAA_LOCK_MIN_LUMINANCE` default so the value can come from `shader_defines_data` |
| 11 | xess/reflex edits: revert | **moot** — pristine Core never had them |

## 3. It does not build yet: the framework is missing six subsystems

`MSBuild … Dragon Age Origins.vcxproj` fails on **~60 errors, all of them missing framework APIs**, none of
them in game code:

| Missing | Symbols in the errors |
|---|---|
| Super-resolution bridge (DLSS / FSR / XeSS host) | `SRBridge::*`, `CreateSharableTexture`, `HasShaders`, `LockSRForDraw`, `LatchSRFrame`, `GetSRJitter`, `DeviceData::sr_type`, `sr_output_color`, `sr_suppressed`, `force_reset_sr`, `release_sr_resources`, `dlss_render_preset`, `sr_game_tooltip` |
| Luma TAA | `LumaTAA::*` |
| MSAA resolve override | `Game::OverrideResolveTextureRegion()` (doesn't exist upstream) |
| scRGB swapchain proxy | `ENABLE_SWAPCHAIN_PROXY` / `swapchain_proxy.hpp` (absent; only needed for the HDR10 PQ output path) |
| Draw/perf helpers | `ReleaseBloom`, `ReleaseKarisAverage`, `GetViewTextureSize`, `SetTextureMipLodBias`, `Perf::*`, `Mcp::Annotate` |
| Patch provider API | `Patch::PatchContext::GetShaderData()` |

These are all part of the fork the mod was written on (261 commits by Hlib Omelchenko, ~120 files /
48k lines in `Source/Core`), not of the mod itself.

## 4. Two ways forward

**A. Vendor the framework snapshot unmodified, add only the DAO folder (recommended).**
Take the fork's `Source/Core`, `Shaders/Includes`, `Shaders/Global` at a fixed commit, drop them into the
clone as-is, then keep only the DAO folders on top. The mod then builds with **zero DAO edits in common
code** — which is the property we actually want — and the DAO never conflicts when the framework is
updated. Cost: the clean repo carries the fork's framework, so it is a "DAO-only fork", not a
"pristine-Luma fork".

**B. Port the mod down to pristine Luma.**
Drop DLSS/FSR (and the SR bridge), Luma TAA, the MSAA-resolve override and the swapchain proxy, keeping the
HDR grade, AO, bloom, DoF, SMAA-on-resolve is also lost. The mod then builds against upstream Luma with the
game-local replacements already made, but loses most of its feature set.

Either way, item 10 remains a two-line generic default in `Luma_TAA_CS.hlsl` (or the DAO's TAA lock stays
Core's default, which dims sub-pixel highlights).

## 5. Vendoring audit: what else can move into the mod's folders?

Asked "can we vendor all the rest": of the 13 framework pieces the mod needs, **6 are now vendored**, 7 are
framework-bound.

| Piece | Vendored? | Why |
|---|---|---|
| `Shaders/Includes/GTVBAO.hlsl` | **yes** — `Shaders/Dragon Age Origins/Includes/` | only the DAO AO shader includes it |
| `Shaders/Includes/MotionVectorFill.hlsl` | **yes**, same folder | only the DAO MV fill shader includes it |
| `Scripts/dgVoodoo/dgVoodoo.conf` (2.87.6) | **yes** — `Data/Dragon Age Origins/dgVoodoo.conf` | packaging input |
| `draw.hpp`'s `GetViewTextureSize` (12 lines), `ReleaseBloom` (8), `ReleaseKarisAverage` (5) | **yes** — reimplemented as `DaoViewTextureSize` / `DaoReleaseBloom` / `DaoReleaseKarisAverage` in `main.cpp` | they only reach Core globals (`draw_bloom_mips`, `device_data.managed_resources`), which the game TU is compiled next to. `DrawBloom`, `DrawKarisAverage`, `DrawRCAS`, `DrawSMAA` are upstream and stay |
| `SetTextureMipLodBias` | **no** | its live path needs the fork's `RebindUpgradedSamplers` and `CommandListData::applied_texture_mip_lod_bias_offset`; upstream only applies `texture_mip_lod_bias_offset` when a sampler is created, so a copy would be a no-op |
| `includes/swapchain_proxy.hpp` | **no** | `core.hpp` calls it 16× (swapchain creation, `GetBuffer` interception, resize, the encode at present) |
| `taa/LumaTAA.hpp` + `Shaders/Global/Luma_TAA_CS.hlsl` | **no** | `core.hpp` builds the TAA impl in `sr_implementations` and ticks/dispatches it (16 call sites + `DeviceData` fields) |
| `sr_bridge/*` + `Source/Tools/SR Bridge Helper/*` | **no** | `DeviceData::sr_type/sr_output_color/sr_suppressed/force_reset_sr/release_sr_resources` and Core's init/lock/feed calls are the interface |
| `Game::OverrideResolveTextureRegion()` | **no** | a Core virtual: only exists if Core calls it (this is the one worth a generic upstream PR, ~4 lines) |
| `Shaders/Global/Luma_SwapchainEncode.hlsl` | **no** | payload of the proxy: dead without Core's encode dispatch |

A Core native shader *can* be re-pointed game-side (`native_shaders_definitions.insert_or_assign` in `OnInit`,
before the shaders load), but that doesn't help here: the C++ that dispatches the TAA and encode passes is
Core's, so owning the file alone buys nothing.

The rule the audit produces: **you can vendor code the mod includes; you cannot vendor code Core runs.** The
7 remaining pieces are all called by `core.hpp` itself, so "vendoring" them would mean copying their call
sites too — i.e. forking `core.hpp`.

## 6. Axed on request (2026-11-02)

Four of the framework dependencies were declared unnecessary and removed from the mod, so they no longer
block the port:

| Axed | What was deleted | What the mod loses |
|---|---|---|
| `SetTextureMipLodBias` | the per-frame call in `OnPresent` | with DLSS/FSR/TAA at a lower internal resolution, textures keep mips for the internal res: slightly softer output, nothing else |
| `OverrideResolveTextureRegion` | the override + `ResolveHDR` + `ResolveAndSMAA` (~11 KB of `main.cpp`), the `DAO MSAA Resolve PS` registration, the `smaa_*` fields, the SMAA setting/config/UI, and the 4 shader files (`Luma_SMAA_impl.hlsl`, `Luma_DAO_ResolveMSAA.hlsl`, `Includes/SMAA.hlsl`, `Includes/SMAA_Passes.hlsl`) | SMAA is gone, and the game's own MSAA resolve is used again: a plain average of fp16 samples, so a very bright sample can alias against a dark background (sun/fire edges). SMAA's UI section became "Sharpening" (RCAS only). The upscaler's "MSAA must be off" gate survives — it is the grade-path check at `main.cpp:876`, not the removed resolve tracker |
| swapchain proxy (`swapchain_proxy.hpp` + `Luma_SwapchainEncode.hlsl`) | nothing in the mod (Core-owned) | HDR still works through Core's plain fp16 scRGB swapchain and DWM; the explicit HDR10 PQ / 10-bit encode is not used, so near-peak gradients can band a little more |
| `Perf` + `Mcp::Annotate` | the two annotation calls, the dev-only `Perf` rail in the vendored `scene_motion_vectors.h` (and its timer) | dev MCP only: no `mv` / `mv_reject` labels on trace entries, no shader-create timings |

Build errors after the axe: **78 → 64**, and every remaining one is the SR bridge, Luma TAA, or
`Patch::PatchContext::GetShaderData()` (motion vectors) — i.e. exactly the three things still kept.

## 7. Option A executed (2026-11-02): the clone is the repo now

`C:\Stuff\Luma-framework-clean` is now the working repo and **builds both configurations with 0 errors**.

What changed since §1:

- **Framework vendored, unmodified**: `Source/Core`, `Source/Tools`, `Source/External`,
  `Shaders/Includes`, `Shaders/Global` copied from Hlib's tree at commit `23019de8` (his last commit before
  the DAO work started), so none of this mod's former edits are in it. `Source/Games/Luma.*.props` and
  `Luma.Properties.xml` come from the same tree.
- **Other games removed**: 126 folders deleted — `Source/Games` is now `Dragon Age Origins` plus the three
  props/xml files; `Shaders` is `Dragon Age Origins`, `Includes`, `Global`, `Decompiler`, `_Template`.
  Repo size 1.1 GB (460 MB of it `Source/External` binaries, which can be trimmed later).
- **The vendored copies were reverted** where the framework now provides them: the four motion-vector headers
  (`scene_motion_vectors.h`, `patched_draws.h`, `motion_vector_patch.h`, `dxbc.h`), `GTVBAO.hlsl`,
  `MotionVectorFill.hlsl`, and the three `Dao*` helper functions are gone; `MotionVectorPatches.h` and the two
  DAO shaders include Core's again. Keeping them made `DXBC`/`Patch` symbols redefined.
- **Build**: `Development-Release` and `Publishing-Release` both 0 errors.
  Development addon md5 `85a5839b250df10d9e1109d9fd0e2b7e`, deployed to `bin_ship` (a Publishing build
  overwrites `bin_ship` through the post-build copy, as usual — the dev one was re-copied after).

## 8. Committed and guarded

- `c7947d6d` **Framework: vendor Luma-Framework at 23019de8 (Hlib Omelchenko's fork), Dragon Age Origins only** —
  the framework and the removal of the other 126 game folders.
- `c6b54265` **Dragon Age Origins: HDR mod, game-local (no framework edits)** — the mod itself.
- `d560a6e1` **Framework: add the mod boundary check** — `Scripts/check-mod-boundary.sh`, installed as
  `.git/hooks/commit-msg`: a commit touching `Source/Core`, `Source/Tools`, `Source/External`, `Shaders/Includes`,
  `Shaders/Global`, the `Luma.*.props/xml` files or `Scripts/` is rejected unless its message starts with
  `Framework:`. Verified both ways (blocked without the prefix, allowed with it).

## 9. Open points


- **TAA lock luminance**: Core's shader defines are single-character (`SHADER_DEFINES_MAX_VALUE_LENGTH == 2`),
  so `TAA_LOCK_MIN_LUMINANCE` (0.45 vs the default 1.0) cannot be passed from the game. Either accept the
  default — TAA then dims sub-pixel highlights (candles, glints) it should lock — or shadow Core's TAA
  pipelines with a game-local copy of `Luma_TAA_CS.hlsl` (8 `native_shaders_definitions` entries to re-point
  with `insert_or_assign`). Not done.
- Nothing is committed yet: the framework replacement and the DAO folders are uncommitted changes in the
  clone.
- No in-game test of this build, and no `git` guard yet to keep future sessions out of `Source/Core`.

## 10. First launch: two framework dependencies found

In game (2026-11-03) the upscalers did not run: `luma_status` reported `mod_active:false`, the log said
`SR skipped: no input (scene bound true, motion vector target false)`, `scene open 0`, and DLSS/FSR were greyed out.

1. **Motion vectors need our six Core commits.** The mod declares `Game::frame_camera_in_every_draw` and
   `Game::reuse_last_reprojection`, both read by `SceneMotionVectors::BeginFrame()` in the fork's extra 187 lines.
   The vendored framework at `23019de8` does not have them, so the scene never opened and every upscaler (Luma TAA
   included) skipped. Fixed by **vendoring `scene_motion_vectors.h` into `Source/Games/Dragon Age Origins/includes/`**
   (the fork's version with those features, its two sibling includes repointed at Core), and pointing
   `MotionVectorPatches.h` at it. Core is untouched.
2. **The SR helper is an exe, not a dll.** The fork's later commit made it a rundll32 DLL; the vendored framework
   looks for `Luma-Upscaler.exe` (`kHelperName`), and `SRBridge::IsSupported()` greys DLSS/FSR out while that file is
   missing — the shipped file was `Luma-Upscaler.dll`. Fixed by restoring `Source/Tools` from `23019de8` and
   building the x64 helper (it now ships as `Luma-Upscaler.exe`; the stale dll is `Luma-Upscaler.dll.stale` in
   `bin_ship`). Release packaging must learn that the helper is an exe: `Release-Packaging.md` currently forbids
   `.exe` in the zip.

Still required in game's options: **Anti-Aliasing (MSAA) off** — the upscalers need the single sample scene and
depth, and the MSAA resolve override was axed.
