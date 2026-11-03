# Dragon Age: Origins — Core independence plan (analysis 2026-11-02)

Goal: the DAO mod adds nothing to shared code (`Source/Core`, `Shaders/Includes`, `Shaders/Global`). Every
change it needs lives in `Source/Games/Dragon Age Origins` or `Shaders/Dragon Age Origins`, so merging this
branch onto the fork's `main` (or cherry-picking Core fixes) never conflicts.

Baselines (fetched 2026-11-02):

- `upstream` = `https://github.com/Filoppi/Luma-Framework`, `main` @ `6fce3405` (2026-11-01).
- `origin`   = `https://github.com/OSTADJ2F/Luma-Framework`, `main` @ `f32ebefe` (2026-10-30) — the "other fork"
  this branch sits on. `HEAD` = `b5d3d950`.
- Merge base with upstream: `dc471fa5` (2026-10-22). **`upstream/main..HEAD` touches 120 files / +48,379 / -17,832**
  in `Source/Core` alone. So "identical to Filoppi's Luma" is unachievable for any mod on this fork. The useful
  invariant is **"no diff against origin/main outside the DAO folders"**.

## 1. What the mod needs that Filoppi's original does not have at all

Absent from `upstream/main` (verified with `git cat-file` / `git grep`):

| Capability | Where (fork) | Needed by DAO for |
|---|---|---|
| Super-resolution bridge (DLSS / FSR / XeSS in a 64-bit helper) | `Source/Core/sr_bridge/*`, `includes/super_resolution.h` (partial) | DLSS, FSR |
| Luma TAA | `Source/Core/taa/*` | Luma TAA upscaler |
| Scene motion vectors | `Source/Core/includes/scene_motion_vectors.h` (whole file) | any upscaler, jitter, reactive masks |
| scRGB swapchain proxy (HDR10 PQ output) | `Source/Core/includes/swapchain_proxy.hpp`, `ENABLE_SWAPCHAIN_PROXY` | HDR output on HDR displays |
| `Game::OverrideResolveTextureRegion()` | `includes/game.h` | SMAA on top of the game's 8x MSAA |
| `force_separate_ui_paper_white` | `core.hpp` | (not used by DAO — UI paper white is set explicitly) |

Present upstream and unchanged in their use here: resource/format upgrades (incl. indirect + chained),
`UI_DRAW_TYPE`, `GAMUT_MAPPING_TYPE`, `use_os_reference_white_level`, `POST_PROCESS_SPACE_TYPE`,
`VANILLA_ENCODING_TYPE`, `GAMMA_CORRECTION_TYPE`, `original_draw_dispatch_func`,
`ENABLE_POST_DRAW_DISPATCH_CALLBACK`, `ENABLE_DRAW_DISPATCH_DATA_CACHE`, `LUMA_PATCH_BYTECODE_SYNC`,
`samplers_upgrade_mode` / `force_upgrade_linear_samplers`.

Conclusion: the fork is the real base. Everything in §1 belongs to the fork (i.e. not this mod's doing), so it
is not merge risk unless it is edited.

## 2. The mod's own edits to shared files (the actual conflict surface)

Every hunk below was added by this mod's commits (`git log --author=Amir` on shared paths), totalling ~300
lines. "Our commits" column = commits that would disappear with the item.

| File | +/− | What it is | Our commits | Disposition |
|---|---|---|---|---|
| `Source/Core/core.hpp` | +94/−17 | 7 items, see §3 | `2c61c8f9`, `b328c01f`, `23212263`, `6325992e`, `1f6de5a9`, `78d8e870`, `9a3f87b6` | split, see §3 |
| `Source/Core/includes/globals.h` | +14 | `max_scene_peak_white`, `default_ui_paper_white`, `GetDefaultUIPaperWhite()`, `SHOW_VERSION` | same | remove (except one if kept) |
| `Source/Core/includes/scene_motion_vectors.h` | +187/−9 | frame camera from any scene draw, row-vector matrices, static meshes by world matrix, MVs on the colourless depth prepass, several target draws, implausible-reprojection guard | 6 commits | **upstream to the fork** (mechanism, not policy) |
| `Source/Core/includes/super_resolution.h` | +7 | `SR::fsr_prefer_3`, `prefer_fsr3` in `SettingsData` | `1f6de5a9` | drop, or upstream |
| `Source/Core/fsr/FSR.cpp` | +20/−2 | pick newest 3.x when `prefer_fsr3` | `1f6de5a9` | drop, or upstream |
| `Source/Core/sr_bridge/SRBridge.cpp` | +23/−7 | protocol string, library lookup in the helper's folder | `4e89795a`, `466d525c`, `1f6de5a9` | upstream (fork-level) |
| `Source/Core/sr_bridge/SRBridgeProtocol.h` | +3/−1 | protocol field | same | upstream (fork-level) |
| `Source/Core/xess/XeSS.cpp` | +1/−1 | `LOAD_LIBRARY_SEARCH_USER_DIRS` | `466d525c` | **revert** — DAO disables XeSS |
| `Source/Core/includes/reflex.h` | +21/−2 | dev-only foreign frame-id logging | `b78e7151` | **revert** — DAO sets `ENABLE_REFLEX 0` |
| `Shaders/Includes/SMAA.hlsl` | +7/−2 | `SMAA_THRESHOLD_SCALE` (default 1.0) | `23212263` | keep as generic `#ifndef`, upstream it |
| `Shaders/Global/Luma_TAA_CS.hlsl` | +7/−0 | `TAA_LOCK_MIN_LUMINANCE 0.45` under `#ifdef GAME_DRAGON_AGE_ORIGINS` | `ad134faf` | make it a plain `#ifndef` default; value moves to the game's shader defines |

## 3. `core.hpp` item by item

| Item | Commit | Can it be game-side? | How |
|---|---|---|---|
| `max_scene_peak_white` clamps (3 sites) + slider max + `AlwaysClamp` | `b328c01f` | **yes** | game clamps the *applied* value: `cb_luma_global_settings.ScenePeakWhite = min(..., 1000.f)` in `LoadConfigs()` and again in `OnPresent()` (or `UpdateGlobalCB`), plus writing the clamped value back to the config. Cost: Core's slider still shows up to 10000; values above 1000 are ignored (document it). |
| `GetDefaultUIPaperWhite()` at 4 sites | `b328c01f` | **yes** | game sets `UIPaperWhite = 203` in `OnInit`/`LoadConfigs` when the config has no value, and persists it. Cost: Core's undo button resets to the scene default until the next boot write. |
| `force_sdr_display_mode` + `sdr_swapchain_on_sdr_display` | `b328c01f`, `2c61c8f9` | **yes** | do the HDR-on-at-start check in `DllMain` *before* `CoreMain()` (the mod already sets `swapchain_upgrade_type` there): `Display::IsHDRSupportedAndEnabled(nullptr, supported, enabled, nullptr, MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY))`; if HDR is off, `swapchain_upgrade_type = SwapchainUpgradeType::None` (real 8-bit SDR swapchain). Core's existing rule already forces `DisplayMode = SDR` when HDR is not engaged. Cost: the Display Mode / Peak / Paper White section stays visible on SDR displays (cosmetic). |
| `ENABLE_SR_BRIDGE_XESS` | `23212263` | mostly | Core keeps XeSS in the list; ship no `libxess.dll` (packaging already controls what goes in the zip). The entry appears and fails only if picked. Alternative: keep one 2-line Core guard. |
| "FSR Version" combo | `1f6de5a9` | **no** | the setting lives in Core's `SettingsData`, built by Core every frame. Either drop the option (FSR 4 default) or upstream it as a generic feature. |
| `Globals::SHOW_VERSION` | `6325992e` | **no** | drop: the About page prints the internal version (harmless). |
| `sampler_max_anisotropy` + dev sampler log | `78d8e870`, `9a3f87b6` | **no** | replace the 0–16 slider with an on/off checkbox: Core's `samplers_upgrade_mode` + `force_upgrade_linear_samplers` already upgrade to the hardcoded 16x. Cost: no per-level AF choice. |

## 4. Plan

**Phase 1 — remove policy from Core (no feature loss, all game-side).** Order matters only for build/test
convenience; each step is self-contained.

1. Peak cap → `LoadConfigs()` clamp + `OnPresent()` re-clamp + config write-back; delete `max_scene_peak_white`
   from `globals.h` and its 3 sites + slider max in `core.hpp`.
2. UI paper white → `OnInit` default 203 + config write; delete `default_ui_paper_white` /
   `GetDefaultUIPaperWhite()` and its 4 sites.
3. SDR swapchain → `DllMain` HDR check (see §3); delete both flags and the swapchain-creation block.
4. AF → checkbox in place of the slider; delete `sampler_max_anisotropy`, its constexpr replacement and the
   `#if DEVELOPMENT` sampler log.
5. Revert `xess/XeSS.cpp` and `includes/reflex.h` (unused by this game).
6. Drop `SHOW_VERSION` and the About-page guard.
7. Decide on "FSR Version": drop the combo + `SR::fsr_prefer_3` plumbing, or upstream it.

**Phase 2 — move mechanism to the fork's Core (PR, not local patch).** These cannot be game-local without
reimplementing a framework feature:

- The six `scene_motion_vectors.h` commits (each is already generic and default-off: "another game keeps its
  behaviour"). Submit as one PR/branch on the fork; nothing in the DAO folder changes.
- `Shaders/Includes/SMAA.hlsl` + `Shaders/Global/Luma_TAA_CS.hlsl`: keep only the `#ifndef` default (3 lines
  each, no behaviour change for other games) and move the DAO values into the game's shader defines
  (`SMAA_THRESHOLD_SCALE` is already set in `Luma_SMAA_impl.hlsl`; `TAA_LOCK_MIN_LUMINANCE` moves to
  `shader_defines_data` in `OnInit`).
- SR bridge (protocol/lib lookup, FSR version) and the XeSS/reflex logging: fork-level, not needed by DAO;
  upstream them or leave to the fork owner.

**Phase 3 — lock it in.**

- Add the invariant check to the session routine:
  `git diff --stat origin/main -- Source/Core Shaders/Includes Shaders/Global` → must print nothing.
  (Replace `origin/main` with whatever the merge target is.)
- Rebuild `Development-Release` **and** `Publishing-Release` (0 errors), redeploy and verify with md5.
- In game: HDR on and off (restart), peak clamps to 1000, UI at 203, AF still on, DLSS/FSR/TAA still engage,
  SMAA threshold unchanged at equal paper whites, motion vector ghosting unchanged.

## 5. Risks / caveats

- The peak lock becomes a clamp on the applied value, not a limit on the UI: a user can still type/see 1000+
  until the next boot. Acceptable, but the release notes should say so.
- The SDR decision at `DllMain` runs before the game's window exists: monitor selection falls back to the
  primary display. A multi-monitor user whose game runs on a secondary HDR display could get the SDR path.
  Mitigation if it matters: also re-check in `OnInitSwapchain` (cannot change the format there, but can log)
  and document "restart with the game on the HDR display".
- Dropping the FSR version option removes a shipped setting (it is in `Nexus-Description.txt`? check before
  release); likewise the AF slider becomes a checkbox — both are user-visible changes to document.
- `scene_motion_vectors.h` stays a Core diff until Phase 2 lands; until then DLSS/FSR/TAA depend on it, so it
  is the one item that blocks "zero diff" if the fork owner does not take it.
