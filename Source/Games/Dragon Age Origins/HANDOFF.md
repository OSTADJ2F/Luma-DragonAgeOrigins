# Handoff: Dragon Age: Origins Luma mod (updated 2026-11-03)

**Read `AGENTS.md` (same folder) first.** It explains the project, the build, the tools and the rules. This file is the current state and
the work queue. Update it at the end of every session: what changed, what was tested (and how), what is next.

---

## 1. State at a glance

- Branch `dragon-age-origins-sr`, last commit `b5d3d950` (2026-11-02, "Nexus and Discord descriptions for the SDR mode, bloom options and
  Hide UI"). The released feature set is everything committed up to there.
- **Release zip built 2026-11-03 16:10**: `C:\Stuff\Luma-DAO-release\Luma-Dragon_Age_Origins-x32.2026-11-03-1610.zip` (64.4 MB,
  Publishing-Release addon md5 `3d8ba403e713e098e5ddb9a1517340b2`), carrying the **Gamut Expansion slider in the release UI** (see the
  history entry below). `scripts/verify-release-zip.py` passes (runtime set, no executables, addon matches the build, shader identifiers
  current, shipped helper manifest matches the shipped pair). **Not validated in game yet**, and the SR helper pair in it has still never
  been run by a game (T8). The Nexus page text is `Data/Dragon Age Origins/Nexus-Description.txt`.
- **Deployed in the game right now:** the **Development-Release** addon from 2026-11-03 03:25 (md5 `08f4590f40d25dd396bf4c0cb2bbca81`) with the
  uncommitted work of §2, plus the helper pair of T8: `Luma-Upscaler.dll` is now the mod's **shim** (md5 `4b9990ff7819e7b8a8e2499600891bba`)
  and `Luma-Upscaler-Core.dll` is the framework's helper, unchanged (md5 `97cd261a049049edd90de6a6b315b955`). A copy of an older
  Publishing addon sits next to them as `Luma-Dragon Age Origins.addon.release`.
- HDR/SDR: the addon outputs HDR (scRGB) when Windows HDR is on at game start, real 8-bit SDR otherwise (`sdr_swapchain_on_sdr_display`
  in `DllMain`). `enable_hdr` is `static constexpr true`. (The older "Enable HDR" config checkbox described in earlier notes is gone.)

## 2. Uncommitted work

**Nothing is uncommitted as of 2026-11-03**: everything below was committed and pushed together (see §5 for the entries).
The table is kept as the record of what that work was; the next session starts from a clean tree.

The work:

| File | Change | Status |
|---|---|---|
| `Shaders/Dragon Age Origins/Includes/GameCBuffers.hlsl` | 3 new settings appended: `VignetteStrength`, `VignetteRadius`, `ChromaticAberration`; the cancelled Ersh style's `HDRStyle` field removed | built, deployed |
| `Shaders/Dragon Age Origins/ColorGrade_0x6B504D9F.ps_5_0.hlsl` | `ApplyVignette()` (linear light, before grain) in the HDR and SDR paths; chromatic aberration as offset red/blue scene samples (with `ApplyDgvMask`); the Ersh branch, `ErshGrade` and `ErshClippedCurve` removed | built, deployed, **look confirmed by the user 2026-11-02** |
| `Source/Games/Dragon Age Origins/main.cpp` | defaults (all off, radius 0.8), config load, new UI section **"Lens Effects"** after Film Grain; **`GamutExpansion` default changed 1.0 → 0.5**; the Ersh `HDRStyle` default, config read and UI combo removed; **T5: the GUI scale mid hook (`OnGUIScaleRead`, `InstallGUIScaleHook`), its config read in `DllMain` and the "HUD Scale" slider (default Auto)** | built, deployed; HUD scaling and vignette/CA confirmed by the user |
| `Source/Games/Dragon Age Origins/includes/` | **new**: `safetyhook.cpp/.hpp` + `Zydis.c/.h`, copied from Watch Dogs 2's vendored copy (single file amalgamations). Untracked | compiles, see §5 T5 |
| `Source/Games/Dragon Age Origins/Dragon Age Origins.vcxproj` | `safetyhook.cpp` + `Zydis.c` added, with `ForcedIncludeFiles` cleared for both (the project force includes `GameCBuffers.hlsl` into every C/C++ file) | — |
| `Source/Games/Dragon Age Origins/SR Helper Shim/` | **new**: `main.cpp` + `Luma Upscaler Shim.vcxproj` (x64), the FSR loader preload and the forward to `Luma-Upscaler-Core.dll` (T8) | built (0 errors), deployed |
| `Data/Dragon Age Origins/package-release.ps1`, `Release-Packaging.md` | ship the shim as `Luma-Upscaler.dll` and the framework helper as `Luma-Upscaler-Core.dll`; third build step (T8) | — |
| `Source/Games/Dragon Age Origins/ErshTonemap.hlsl` | **deleted** (tracked; the cancelled Ersh HDR style) | built, deployed |
| `Scripts/dao_gallery/` | shader coverage tools (untracked) | working, see §4 |
| `AGENTS.md`, `HANDOFF.md`, `TestScenePlan.md` (this folder) | docs (untracked) | — |
| `Source/External/safetyhook/`, `Source/External/zydis/` | untracked third-party code, **not from this mod's work**: origin unknown. **T5 did not use these**: it vendored Watch Dogs 2's single file copy into `Source/Games/Dragon Age Origins/includes/` instead (these are full CMake trees) | leave alone, ask the user |

Known nits in the uncommitted work:
- Removing `HDRStyle` shifted the `game:<n>` indices of `HighlightColor`, `VignetteStrength`, `VignetteRadius` and `ChromaticAberration`
  down by one: re-count before `luma_set_setting game:<n>`. Saved settings are unaffected (configs load by name).
- The Vignette and Chromatic Aberration are not in `Nexus-Description.txt` / `Discord-Post.txt` yet, nor is the Gamut Expansion default change.
- In the SDR path the vignette makes the grade run its linear branch even with grain and colour controls off (the `else if` now also
  tests `VignetteStrength > 0`): fine, but check that SDR with everything off is still bit-identical to vanilla.
- The HUD scale's tooltip promises that a change lands on the next GUI layout ("change screen, or restart to be sure"): that is unconfirmed
  (T5), reword it to what the game really does once tested. DAFix is restart only.
- `Source/Games/Dragon Age Origins/includes/` (safetyhook + Zydis, ~12 MB of `Zydis.c`) is untracked: decide with the user whether the
  vendored third-party sources get committed before any release build (the other games' copies are uncommitted too).

Cleanup (2026-11-03, uncommitted, built 0 errors): removed the stale `TODO (HDR step 1)` and the wrong 2.87.3 note in `main.cpp`; removed
the never-read `g_frame_index` counter; dropped the dead `CInfinityRolloff.hlsl` include from the colour grade (the rolloff was replaced by
`HighlightShoulder`); deleted the empty template `Includes/Settings.hlsl` and its include in `Includes/Common.hlsl` (with the `AGENTS.md`
Includes list); fixed the Chromatic Aberration cbuffer comment to 0..0.5 (matching the UI slider). `LumaGameData` was left as-is: it is
required by Core's `LUMA_GAME_CB_STRUCTS` guard (`Source/Core/includes/cbuffers.h`), not template scaffolding. Shaders not yet reloaded
in game (the CInfinityRolloff removal is transitively safe — `Math.hlsl` comes from `Common.hlsl`/`Color.hlsl` — but confirm the grade still
loads).

## 3. Work queue (in priority order)

Each task says what "done" means. Do one task at a time, build, and ask the user to test in game where marked **[user]**.

### T1. Finish Vignette / Chromatic Aberration / Gamut Expansion default — **look confirmed by the user 2026-11-02**
1. ~~Build Development-Release~~ built, 0 errors, deployed (both configurations).
2. **[user] confirmed 2026-11-02**: "vignette and chromatic aberration work". The detailed checks (radius 0.3/0.8/1.5, HUD not darkened, CA
   fringes at the edges only, HDR *and* SDR, no shimmer with an upscaler) were not walked through item by item — treat the feature as good, not as
   exhaustively verified in SDR.
3. With MCP: `luma_trace_capture trigger=6B504D9F`, `luma_read_resource` on the grade's render target, settings at 0: no NaN/Inf, and
   compare against a readback from before the change if one exists (`luma_compare`).
4. ~~Decide with the user whether Gamut Expansion 0.5 is the new default~~ **decided 2026-11-03: the slider ships in the release UI with
   0.5 as the default**, so a user can move it back to 0 (vanilla) or up to 1.
5. Add one line each to `Nexus-Description.txt` (Features list) and `Discord-Post.txt`.
6. Ask the user to commit. Suggested message: `Dragon Age Origins: Vignette and Chromatic Aberration options, Gamut Expansion default 0.5`.

Done when: user confirmed the look, texts updated, committed by the user's go-ahead.

### T2. Shader coverage → native HDR per shader (the long-running goal)
Goal (user's words): a record of every shader the game uses, so each one can be given native HDR, **without screenshots**.

Current method (static, replaces the in-game NWScript gallery which was too fragile):
1. Every dev-build session dumps the shaders it sees into `Shaders/Dragon Age Origins/Dump` (accumulates).
2. `python Scripts/dao_gallery/match_dx9.py` names each dumped DX11 shader by matching it to BioWare's DX9 original from
   `shaders.erf` (+ `patch002.erf`). Output `%TEMP%\luma-dao-gallery\dx9_match.json` and `match.txt`.
3. `python Scripts/dao_gallery/hunt_dx9.py` writes `%TEMP%\luma-dao-gallery\hunt.md`: the DX9 shaders not seen yet and where in the game
   to make them draw (areas, effects, models, graphics settings).
4. Last run (2026-11-02 01:33): **76 of 180 DX9 pixel shaders seen, 104 not yet.** Top leads: `terrain3` (far terrain, e.g. look at
   distance from a high point in any exterior), `vfxchunky0` (storm / inferno / stonewall spell effects), `reticle` (selection circles),
   character shaders `ch1*`/`ch2*`, `waterflowing0` (rivers), `fadesky`/`fadeclouds` (the Fade), and LOW/MEDIUM detail permutations
   (lower the graphics settings for a minute).

Next steps:
- **[user]** Play the places from `hunt.md` with the Development build (a few seconds of each is enough; lower the detail settings once).
  Then rerun steps 2 and 3, update the numbers here.
- Then build an **HDR audit list**: for every seen game shader, from its disassembly, does its output end in `_sat` / `saturate` or a
  `min(...,1)` that clips highlights? Is it blended (blend state from a trace)? Is it a light source (emissive, additive)?
  `shader_bank.py report` has a first heuristic for this (`hdr_status`: todo / leave / unclamped / boosted / done). Write the result as a
  table in a new file `Source/Games/Dragon Age Origins/ShaderCatalog.md` (hash, DX9 name, role, HDR status). **Only hashes and DX9
  shader names go in the repo, no disassembly.**
- Then, one shader at a time: replace or patch (see `AGENTS.md` §5) the ones that clip light sources, starting with emissive/additive
  effects. Already done: `Particle_0x5DE6B6F6` (vfx0_addv), `VFXAlpha_0x41DE4D45` (vfx0_atm), `VFXVolume_0x0A2ABD19`
  (vfx0_volumetexture), `Sky_0x1A13F3EF` (sky), water `0x4A96DC95` (clamp patch).

Do NOT revive the NWScript console gallery (`luma_gallery*.nss`, `shader_bank.py capture`) unless the user asks: it was unreliable
(spawned objects were huge, never removed, and `runscript` repeated from the same spot did nothing). Details in `TestScenePlan.md`.

### T3. Light Source Boost check
The boosted sprite/sky shaders shipped (commit `b328c01f`), but a focused in-game check of `LightSourceBoost` (default 0.5) across fire,
torches, lightning and spell effects at 400 and 1000 nits peak was never recorded. **[user]** confirm, then note the result here.

### T4. Performance follow-ups (only if the user reports slowness)
From `PerformanceNotes.md`: the TAA/DLSS/FSR cost was never reproduced (measure uncapped, Development vs Publishing). Options listed
there: AO main pass at half resolution, DoF gather at half resolution, skipping per-draw state readbacks for non-scene draws.

### T5. HUD scale option (DAFix-style engine hook) — **implemented and deployed 2026-11-02, untested in game**
Built as planned, with three changes from the investigation's plan (all better, note them):
- The hook is installed in `DllMain` right after `CoreMain()` (with the anisotropy config read), **not** in `OnInit`: the game can lay its
  GUI out before the first frame, and `OnInit` may be too late for the first layout.
- The setting is a **multiple of the HUD size** (`HUD Scale`, 1 = the game's own, higher = bigger, 0 = Auto), not DAFix's raw divisor: the
  callback divides the engine's own value (`[esp+8]`) by it, so 1 is exactly vanilla at any resolution. **The default is Auto (0)**: the game's own
  size leaves the HUD tiny on a modern display, which is the point of the option, so a fresh install gets the automatic size (1.4x at 1080 lines,
  1.875x at 1440). The reset button goes back to Auto too. `UIScale` in `ReShade.ini` is the key, so an old config with 1 stays at vanilla.
- Auto uses the width/height read from the layout struct on the stack (`[esp+0x28]/[esp+0x2c]`, falling back to `[esp+0x10]/[esp+0x14]`), not
  a resolution of our own: it is the pair the engine itself uses, so no dgVoodoo mismatch.

What is in the code (all uncommitted):
- `includes/safetyhook.cpp/.hpp` + `Zydis.c/.h` (from Watch Dogs 2's vendored copy), added to the vcxproj with `ForcedIncludeFiles` cleared.
- `InstallGUIScaleHook()`: scans the exe for DAFix's signature, requires **exactly one** hit whose first four bytes are `d9 44 24 08`
  (`fld dword ptr [esp+8]`), otherwise logs a warning and leaves the setting off (safe for other exe builds). `create_mid` on that address.
- `OnGUIScaleRead()`: writes the setting (or the auto value) into `[esp+8]`; sanity-checks the engine's value first, and the width/height pair
  it uses for auto. Dev builds log the first 6 layouts (engine value at `[esp+8]`/`[esp+C]`, the resolution found, what it wrote).
- `DllMain`: reads `UIScale` from the config and installs the hook. UI: "HUD Scale" slider (0..3, steps of 0.05, 0 = "Auto") in the UI section,
  greyed out with a note when the hook is not installed.

Verified: the exe's `.text` has exactly one match (RVA `0x111318`), the function it hooks is called from two places (GUI setup,
not per frame as far as static reading shows), the constants in its sub-1024x768 path are the game's own `768.0`, `1024.0` and `1.3333`, and
safetyhook's mid hook stub leaves `ctx.esp` equal to the hook site's `esp` (the two `push esp` and the `+8` fixup in its x86 stub), so writing
`[esp+8]` and the original `fld` both see the same slot.

**To do (needs the user in game, Development build is deployed):**
1. **[user]** With `HUD Scale` at 1: the HUD, menus and the cursor hit targets are exactly as vanilla (this is the "did the hook break anything"
   check). Then 1.5 and 2: everything scales together, the mouse and the inventory/cursor still line up, dialogue wheel and party portraits too.
2. **[user]** At 0 (Auto) at the display's resolution: a comfortable HUD (on 1440 lines the auto value is 1440/768 ≈ 1.88x). Then check the log:
   `ReShade.log` should show `GUI layout <W>x<H>` with the game's real resolution. If it shows `0x0`, the fallback slots were used (still fine),
   and if auto does nothing the width/height slots are wrong: the log lines are the diagnostic.
3. **[user]** In a menu, change the slider: does the HUD follow immediately (per layout) or only after a screen change / restart? Set the tooltip
   and the handoff to whatever it really is (DAFix is restart only).
4. **[user]** In HDR and in SDR (Windows HDR off, restart), and with the mouse over the corners of the screen: the HUD is not offset and the
   selection circles/click regions land on the right pixels.
5. ~~Texts~~ **done 2026-11-02**: `Nexus-Description.txt` and `Discord-Post.txt` now list HUD Scale (with Auto as the default and the
   1080p/1440p numbers), the re-rendered character preview, Vignette and Chromatic Aberration, plus a settings line, two FAQ entries ("the HUD is
   bigger than it used to be", "the HUD looks blurry when it is big") and the summary line. **Not called out**: the Gamut Expansion 0.5 default —
   the user decides whether to keep that as it is before release.

Done when: user confirmed the scaled HUD (layout and click targets correct) in HDR and SDR, texts updated.

### T6. UI resolution: what is soft and what can actually be improved — investigated 2026-11-02
Measured in game (2560x1440, `HUD Scale` = Auto → the hook writes 0.5333, HUD at 1.875x), inventory screen open:
- Every HUD/sprite draw lands on the **2560x1440 canvas at native resolution** (`luma_trace_capture` + `luma_trace_get`): there is no UI
  render target being upscaled, so the composite itself is not the problem.
- **Text is native and sharp.** Glyphs come from a runtime-rasterized 1024x1024 R8G8B8A8 atlas (alpha only, glyphs 11-41 px tall, drawn by
  `0x3DCC5533`, 140 draws an inventory frame) and are 1:1 with the atlas at this scale. Nothing to win here.
- **The inventory character preview is the worst offender and it is fixable.** The 3D paperdoll renders into a **fixed 270x459** R8G8B8A8
  target plus a matching D24S8 depth (cleared every frame, 12 draws, hashes `0xF897D189`, `0x1446A5C1`, `0xFD53DB7C`, `0x54C50C5D`,
  `0x175CE8D4`, `0x198469C4`, `0xED7EF749`, `0xD59F0773`), and that target is then stretched to **~489x831** on screen (measured by
  correlating the readback against the frame: display scale 1.81, and 270x459 is *fixed*, it does not follow the HUD scale). At `HUD Scale` 1
  the panel is drawn at 270x459 and is 1:1, i.e. **the blur is caused by our feature, not by the game**. Rendering it at the display size
  (or 2x for SSAA) would make the model sharp — that is the one big win available.
- **Shipped UI art is magnified 1.875x**: the book/panels/borders/icons come from a BC1 2048x2048 atlas (`0xE9107C4B`) and a BC3 512x512
  atlas (`0xB972AC3B`, item icons). Edges are soft and the smooth gradients (the name-plate banner) show BC1 banding. Only reconstruction
  quality is ours to change (bicubic/Catmull-Rom instead of bilinear, or a light HUD sharpen); the textures themselves cannot improve.
- **12 UI draws are point-magnified** (`0xE9107C4B`, `MIN_MAG_POINT_MIP_LINEAR`, bias -0.5, multiply blend over the 2048 atlas), so those
  elements are nearest-neighbour magnified at any HUD scale above 1. Cheap fix if wanted.
- Single fixed-size UI images (portraits) are not live renders in the trace, so they cannot be supersampled the way the paperdoll can.

Options, cheapest first: (a) linear/`RGSS` magnification for the 12 point draws (~an hour, small win); (b) sharper reconstruction for the
magnified UI shaders (medium, subjective, needs an A/B); (c) supersample the paperdoll: intercept the 270x459 colour+depth creation, scale it
by the HUD scale, and scale the viewport of the 12 draws that render into it (the composite already samples 0..1 UVs) — the real win, but
dgVoodoo owns that surface, so it needs careful testing.

**Dropped: (c) was implemented and then removed again on 2026-11-02, at the user's request.** Kept here because the findings are the reason not to
retry it naively:
- It worked mechanically: `OverrideCreateResource()` (a temporary opt-in Core hook, since reverted) had the game create the 270x459 colour target and
  its R24G8 depth at `round(size * HUD scale)` = 506x861, `ScalePaperdollViewport()` scaled the game's authored 270x459 viewport for the eight
  paperdoll shader hashes only, and a dev-only "Character Preview Supersampling (dev)" toggle existed for A/B. Both configurations built and the log
  confirmed `the character preview target is created at 506x861 (HUD scale 1.875x)`.
- **The bug that sank it**: the composite (`0xB972AC3B`) maps `uv = v5 * cb4[12].zw + cb4[12].xy`, and `cb4[12]` is `0 0 1 1` (identity), so the panel
  samples the whole target — but the *framing* came out wrong in game: with supersampling on the character looked zoomed/cropped, and after toggling
  it off the next target still got the stale 1.875x viewports (only the target was recreated at 270x459, the factor was not reset), which cropped the
  model even more. The reset was fixed, but the feature was dropped before the fix was verified, so the real cause of the "zoomed in" look on the
  supersampled path was never isolated: the likely candidate is the game computing the panel's UV/quad geometry from the surface size dgVoodoo reports
  (270x459) rather than from the texture, in the vertex shader, which a pixel-side analysis would not see.
- Nothing of it remains in the code: `main.cpp`, `Core/includes/game.h` and `Core/core.hpp` are back to no supersample hook, and both release texts no
  longer mention a re-rendered character preview. The findings above (fixed 270x459 target, stretched 1.8x at Auto) still stand as the description of
  what the inventory looks like.
- **Trap to remember**: building Publishing-Release while the game is closed deploys *it* (post-build xcopy), silently replacing the Development addon
  the user tests with: check `md5sum bin_ship/…addon` against `Binaries/Win32-Development-Release/…addon` before saying "deployed".

Crash note (same session): the game crashed once with the feature on, at `DAOrigins.exe+0x4697be` (`mov ecx,[eax]` on the result of a virtual
call — a game null dereference), **not** in the hook (`0x111318`) or the safetyhook stub. `%LOCALAPPDATA%\CrashDumps` has 5 older DAOrigins
dumps (2026-10-31: four in `D3D9.dll+0xc56ee`, one at `exe+0x1278f5`, one ucrtbase abort, one debug-break inside the addon), i.e. the game
was already unstable; watch whether it recurs at high HUD scale only.

### T7. AO halo around character hair — investigated 2026-11-03, **not fixed**

Measured in game (conversation close-up, 2560x1440, Development build) with the MCP tools:

- **The hair is in the depth the AO reads, so this is not a missing-depth problem.** In the captured frame `hair0_pass1` (`0xED7EF749`,
  +alphatest, 4 draws, entries 422-435) writes the hair depth, then 8 blended `hair0` (`0xD59F0773`) draws (443-474) are depth-test-only on
  top of it, then 4 more `hair0` draws (479-492) write depth again. All of that is before the first particle draw (`0x5DE6B6F6`) where
  `RunXeGTAO` runs, and the depth read back at that draw shows the hair silhouette on the visible hair.
- **The AO's kernel is a world-space radius that is never clamped in pixels.** `EFFECT_RADIUS` 0.5 x `RADIUS_MULTIPLIER` 1.457 = **0.73 m**
  (falloff 0.3, so occlusion reaches the full 0.73 m), and `XE_GTAO_ADJUST_SCREENSPACE_RADIUS` is a **no-op** (`Shaders/Includes/XeGTAO.hlsl:98`).
  Pixel radius = 0.73 / (z * 2*tan(30deg)/height): a few dozen px on a wall metres away, **800-2000 px on a head 0.4-1.2 m away**.
- The default technique is **GT-VBAO** with `GTVBAO_THICKNESS = 0.4 * radius` = **0.29 m** of solid slab behind every depth sample, which is
  what thin hair cards are not; `GTVBAO_SLICE_COUNT` is only 3 (`XE_GTAO_QUALITY` 3) and the noise index is frozen at 0, so the two spatial
  denoise passes carry the AO: low frequency, wide.
- **Measured AO field** (8x8 checkerboard A/B patched into `Luma_DAO_AOApply.hlsl`, one frame, so no cross-frame noise): darkening is ~9% at
  the character silhouette, decaying to 3-4% at 100-200 px, and it is on **both sides** (hair and the wall behind). That band is the visible
  "outline around the hair".
- The `AO Debug View` combo is half broken in the GT-VBAO path: view 3 (AO x8) works (it is done in the shared denoiser), views 1, 2 and 4 are
  XeGTAO-only because `GTVBAO.hlsl` has no `DebugViewRT`.

Next (shader-only A/B, hot reload, no rebuild, then the user's eye):
1. Clamp `XE_GTAO_ADJUST_SCREENSPACE_RADIUS` to ~0.05-0.08 of the viewport height (targeted: leaves the world AO alone, fixes close-ups).
2. `GTVBAO_THICKNESS` 0.4 -> ~0.1 (thin occluders: hair, railings).
3. If both are needed, re-measure the band and put the winner behind a dev toggle.

Accident to remember: a **broken hot-reload** of `Luma_DAO_AOApply.hlsl` raises Core's `shader_key_valid` assertion (`core.hpp:2290`) and
**hangs the game** (a modal "Assertion Failed", then no presents). The file was restored byte-for-byte; a shader edit must be syntactically
complete before `luma_reload_shaders`: never write one with a heredoc that drops the closing brace.

### T8. FSR never started on the dll helper - **fixed 2026-11-03 by a mod-owned shim; in-game check pending**

The user reported "FSR is not working". Live reproduction with the MCP tools: `core.sr_user_type` 3 (FSR) logged `started the helper for FSR`,
then `the helper exited`, `SR skipped: DrawUpscaler returned no output`, `active false`; `Luma-Upscaler.log` had
`FSR: amd_fidelityfx_loader_dx12.dll not found next to the game's executable` and `FAIL FSR init or create`, although both AMD dlls sit in
`bin_ship`.

Cause (verified, do not re-investigate): the helper is a dll hosted by `rundll32`, and Core's `FSR::Init` opens the loader by name with
`LOAD_LIBRARY_SEARCH_APPLICATION_DIR`, which is the **host process's** folder - `C:\Windows\System32` - not the game's. It was correct while the
helper was an exe next to the game. DLSS never showed it because NGX finds `nvngx_dlss.dll` its own way. `XeSS.cpp` and both
`FrameGeneration*` headers have the same by-name pattern (XeSS is excluded here).

**A preload inside the framework helper fixed it (FSR 4.1.1 ran in game) and was reverted at the user's request**: that file is framework
code, and his standing rule (`AGENTS.md` §0) is that the mod never edits common code. He also decided **not** to ship the helper as
`Luma-Upscaler.exe` (Nexus forbids executables) and **not** to pin an FSR version (Core's default 4.1.1 stays).

**What ships instead - the mod's own shim** (`Source/Games/Dragon Age Origins/SR Helper Shim/`, x64, `Luma-Upscaler-Shim.dll`): it takes the
name the SR bridge runs (`Luma-Upscaler.dll`), and in its `LumaUpscaler` entry point it (1) loads `amd_fidelityfx_loader_dx12.dll` from its own
folder - the game's - with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`, then (2) loads the framework helper, shipped beside it as
`Luma-Upscaler-Core.dll`, and forwards the call. Core's later by-name open returns the already-loaded loader, because a loaded module is
matched by its name before any search. No framework file changes: the helper is byte-identical, only renamed.

Packaging (`Data/Dragon Age Origins/package-release.ps1` + `Release-Packaging.md`): a third build step, and the pair goes out as
`Luma-Upscaler.dll` (the shim) + `Luma-Upscaler-Core.dll` (the framework helper). Still no `.exe` in the zip.

Verified so far: both x64 projects build with 0 errors; deployed to `bin_ship` (shim md5 `393824ca402a033164a31f53b9ff1d8a`, framework
helper `97cd261a049049edd90de6a6b315b955`); a standalone `rundll32` run with stdin/stdout redirected (the way the bridge starts it) prints
`amd_fidelityfx_loader_dx12.dll preloaded from the game's folder` and then the helper's own protocol reply, i.e. the preload and the forward both
work. **Not yet confirmed in game**: needs a launch, then `core.sr_user_type` 3 and `luma_sr_state` (`draw_count` climbing on FSR, no
`draw_failures`, masks bound), and the user's eye on the picture.

Beyond the preload the shim now carries three mod-owned capabilities (all verified in the standalone `rundll32` harness, which reproduces
the bridge's handle setup):

1. **Version A/B, no rebuild**: `Luma-Upscaler.ini` (game folder, optional, not shipped) `VariantFolder=` makes a folder's copies of
   `nvngx_dlss.dll` / `amd_fidelityfx_loader_dx12.dll` / `amd_fidelityfx_upscaler_dx12.dll` win over the game's own. An FSR container that
   only carries 3.1.x therefore makes Core's own default pick 3.1.5 - the version pin he wanted, as a file, not code. Verified: both FSR
   files resolved `(variant)` and loaded; an unusable `VariantFolder` warns and falls back.
2. **Environment report on every start**, e.g. `SR environment: shim built 2026-11-03 15:47 a344b263858a09d8 | core 1.0.0.0
   a8f0c575ccdefd3c`, the DLSS/FSR file versions (`310.9.16.0`, `4.1.1.2740`, loader `2.3.0.2740`), and one line per missing file that will
   keep an upscaler from starting (`DLSS will not start: nvngx_dlss.dll is not in <folder>`), instead of Core only greying it out. Also warns
   about a stray `Luma-Upscaler.exe`.
3. **Pair check**: `package-release.ps1` writes `Luma-Upscaler-Files.txt` with the FNV-1a 64 of both files; the shim logs `SR pair: matches`
   or refuses with `only one of the pair was replaced` (unless `AllowMismatch=1`, which warns and continues). The C# in the packager and the
   C++ in the shim were checked against each other on the same files (both `55284f347d7c580e`).

The environment report is the one that pays off in support threads; keep its format stable when the shim changes.

Still worth doing: report it upstream (`Filoppi/Luma-Framework`) - the helper should resolve its own directory, or those Core loads should fall
back to the module's folder - and then the shim can be deleted. Left open: FSR is offered in the combo even if the shim or the AMD dlls are
missing (Core only checks that the files exist); picking it then silently drops SR. `Bridge::Init` reports "not supported" when the files are
absent, so not shipping them greys the entry out (nothing to do with the shim).

Also worth keeping: this mod feeds the upscaler the scene at the grade's own size (`render_width = scene_desc.Width`), so FSR/DLSS replace the
game's AA (DLAA-style) at `sr_render_resolution_scale` 1 rather than scaling resolution. `SRUserType` in `ReShade.ini` is still 2 (DLSS).

## 4. Environment details that are easy to lose

- Built addon names: with `-p:SolutionName=Luma` the file is `Luma-Dragon Age Origins.addon`; without it, `-Dragon Age Origins.addon`
  (leading dash) and **no automatic copy** to the game. Both exist in `Binaries\Win32-Development-Release\`; only the deploy name in
  `bin_ship` matters: `Luma-Dragon Age Origins.addon`.
- Dev backups (old addons, original NGX dlls): `C:\Stuff\Luma-DAO-dev-backup`.
- `%TEMP%\luma-dao-gallery\` holds the coverage outputs (`dx9_match.json`, `match.txt`, `hunt.md/json`, `dx9asm\`, `dx11asm\`, `fxo\`,
  `inventory.json`, `bank\bank.sqlite`). All BioWare-derived: never copy into the repo.
- `%TEMP%\luma-mcp\` holds MCP readbacks.
- DA Toolset in `C:\Games\Dragon Age Origins\tools`, SQL Server Express instance `.\SQLEXPRESS` (only for NWScript; not needed for T1-T4).

## 5. History (newest first)

- 2026-11-03 (uncommitted): **Gamut Expansion is a user setting now.** It was `#if DEVELOPMENT`-gated in both the UI and `LoadConfigs()`,
  so the release build always ran its 0.5 default; both guards are gone, so the slider appears in the HDR section of the Luma tab
  (0..1, reset button, tooltip) and a user's value loads from the config. Default stays 0.5. `Nexus-Description.txt` (features line +
  settings list) and `Discord-Post.txt` name it. Both configurations rebuilt 0 errors; the Publishing build is deployed to `bin_ship` and
  packaged as the 16:10 zip. Proof it is in the release: the Publishing addon now contains the `Gamut Expansion` label and tooltip (the
  15:56 build contained neither). Not seen in game yet.
- 2026-11-03 (T8, uncommitted): the shim gained its three mod-owned features - `VariantFolder` DLL swapping for DLSS/FSR version A/B (an
  FSR container with only 3.1.x makes Core's default pick 3.1.5, no code), an environment report on every helper start (file versions,
  missing-file warnings, stray-exe warning), and a pair check against the hashes `package-release.ps1` now writes into
  `Luma-Upscaler-Files.txt` (refuses loudly when only one file was replaced). All exercised in the standalone rundll32 harness; the in-game
  FSR check from the previous entry is still pending.
- 2026-11-03 (T8, uncommitted): **FSR runs again through a mod-owned shim.** The helper is a dll under rundll32, so Core's by-name
  `amd_fidelityfx_loader_dx12.dll` load searches System32 and FSR never started. A framework-side preload fixed it and was reverted (boundary
  rule); instead `SR Helper Shim` takes the name the bridge runs, preloads the loader from the game's folder and forwards to the framework
  helper shipped as `Luma-Upscaler-Core.dll`. No version pin: Core's default 4.1.1 stays. Standalone rundll32 test passes; in-game check
  still pending. Packaging updated (third build, two helper files, still no exe).
- 2026-11-03 (T8, nothing left in the tree): FSR cannot start on the dll helper - Core's by-name load of `amd_fidelityfx_loader_dx12.dll`
  with `LOAD_LIBRARY_SEARCH_APPLICATION_DIR` searches rundll32's System32. A helper-side preload fixed it (FSR 4.1.1 ran in game) and was
  reverted at the user's request, because that file is framework code: the framework stays byte-identical and the mod ships without
  working FSR. The diagnosis, the upstream fix and the remaining packaging decision is in T8; the lesson is in `AGENTS.md` §8. No FSR version pin: Core's default (4.1.1 here) stays, per
  the user's 2026-11-03 call.
- 2026-11-03 (investigation, no code; T7): the AO's wide outline around character hair was measured in game. The hair is in the depth
  buffer the AO reads; the cause is the world-space radius (0.73 m) never clamped in screen space plus GT-VBAO's 0.29 m thickness
  slab on thin hair geometry, at 3 slices with the noise index frozen. Shader-only A/B plan in section 3, T7. The game hung on a
  broken hot-reload assertion (`shader_key_valid`); the shader file is restored.
- 2026-11-03 (uncommitted): code cleanup, no behaviour change — stale header TODO and dgVoodoo 2.87.3 note removed, dead `g_frame_index`
  removed, dead `CInfinityRolloff.hlsl` include removed, empty template `Settings.hlsl` deleted, Chromatic Aberration cbuffer comment
  corrected to 0..0.5. Development-Release built with 0 errors (no deploy).
- 2026-11-02 (uncommitted): the **AO defaults are now the author's own tuning** — `g_ao_strength` 0.25 → **0.75** and `g_ao_darkness` 0.15 → **0.40**
  (his last `ReShade.ini` values, `XeGTAOStrength=0.75` / `XeGTAODarkness=0.4`), with both reset buttons following them. These are dev-only settings
  (read from the config and exposed in "Advanced (for Enthusiasts)" only under `#if DEVELOPMENT`), so this is what the **release** ships: AO is
  noticeably stronger and darker out of the box than before. Worth a line in the release notes.

- 2026-11-02 (uncommitted): the **character-preview supersample was removed at the user's request** — the inventory paperdoll renders at the game's
  own 270x459 again, magnified by the HUD scale like the rest of the interface. `main.cpp` lost `OverrideCreateResource()`, `ScalePaperdollViewport()`,
  `OnInitSwapchain()`, the shader-hash list and the dev toggle; the temporary `Game::OverrideCreateResource()` hook was reverted out of Core
  (`core.hpp` + `includes/game.h` are back to upstream); both release texts lost their "re-rendered character preview" lines. Both configurations built
  with 0 errors and the Development build is deployed (md5 checked). Why it was dropped, and what a future attempt should watch out for, is in §3 T6.
- 2026-11-02 (uncommitted): **Vignette and Chromatic Aberration confirmed working by the user** ("vignette and chromatic aberration work"); the
  release texts were updated for them, for HUD Scale (Auto is now the **default**: 1.4x at 1080p, 1.9x at 1440p) and for the character-preview
  supersample, with two FAQ entries about the bigger and softer HUD. Remaining on T1: the optional MCP NaN check and the Gamut Expansion 0.5
  default decision, then the commit.
- 2026-11-02 (investigation, no code; T6): the UI's resolution was measured in game through the MCP tools. Text is native and sharp; the
  inventory paperdoll is a fixed 270x459 3D render stretched 1.8x (caused by our HUD scale, and fixable); shipped UI art is magnified 1.875x
  from BC1/BC3 atlases (not fixable); 12 UI draws are point-magnified. Also analysed a crash dump: the game's null dereference at
  `exe+0x4697be`, unrelated to the hook, with five older dumps from 2026-10-31.
- 2026-11-02 (uncommitted, T5): the **HUD scale option** was implemented (DAFix-style mid hook on the engine's own GUI scale). safetyhook +
  Zydis vendored into `Source/Games/Dragon Age Origins/includes/` from Watch Dogs 2's copy, vcxproj updated, hook installed in `DllMain` after
  `CoreMain`, "HUD Scale" slider in the UI section. **Both** configurations built with 0 errors and the Development one redeployed after the
  Publishing build overwrote it (19:36 build backed up to `C:\Stuff\Luma-DAO-dev-backup`). **Not tested in game**: see §3 T5 for what to check.
- 2026-11-02 (investigation only, no code): DAFix-style HUD scale studied (Lyall/DAFix on Codeberg, MIT) and checked against our GOG exe —
  pattern matches uniquely at RVA 0x111318, the engine's own GUI-scale load. Feasible as a safetyhook mid-hook inside our addon; queued as T5.
  Nothing built or deployed.
- 2026-11-02 (uncommitted, after the Vignette/CA work): the Ersh HDR style was cancelled and its code removed — `ErshTonemap.hlsl`
  deleted (tracked), the grade's Ersh branch and the `HDRStyle` setting (cbuffer, config, UI) removed. Both configurations rebuilt
  (0 errors), the Development build redeployed to `bin_ship`. Not tested in game (the grade's Ostad path is byte-for-byte what it was).
- 2026-11-02 (uncommitted): Vignette, Chromatic Aberration, Gamut Expansion default 0.5. Coverage moved to static DX9 matching
  (`match_dx9.py`, `hunt_dx9.py`). Diagnosed a full-black flicker as the game skipping its world render (not a mod bug; went away).
- `b5d3d950` Nexus/Discord texts for SDR mode, bloom options, Hide UI.
- `2c61c8f9` Real SDR output, SDR gating of HDR-only settings, Bloom Off / Game / Luma.
- `b328c01f` Highlight shoulder, Highlight Color, Hide UI, Light Source Boost shaders (particles, VFX alpha/volume, sky), release defaults.
- `23212263` UI brightness slider, bokeh DoF, water fix, bloom and tone changes, XeSS removed.
- `6325992e` Film grain, RCAS, colour grading, DoF and HDR fixes.
- Earlier: anisotropic filtering, FSR version option, SR bridge as a dll, dgVoodoo packaging, Luma bloom, AO, motion vectors, SMAA.

## 6. End-of-session checklist (for whoever works next)

- [ ] Build is 0 errors in the configuration you changed (and Publishing-Release if you touched `#if DEVELOPMENT` code).
- [ ] Every claim of "works" is backed by the user's confirmation or an MCP measurement; everything else is marked "untested in game".
- [ ] `HANDOFF.md` §1-§3 updated (state, uncommitted files, queue).
- [ ] Nothing committed or pushed without the user's explicit go-ahead.
