# Dragon Age: Origins - performance notes

What was measured, what it cost, and what was done about it. Add to it when something else is profiled.

## How to measure

Needs a Development build running (the MCP pipe) and the scene to measure standing still.

- `luma_perf` reports the median and p95 of the CPU and GPU frame time over 120 frames. A **vsync or frame limiter hides small costs**: turn them off,
  or measure where the game runs below the cap. Under a cap (e.g. 144 fps = 6.9 ms) CPU and GPU times read the same and an extra millisecond is absorbed.
- `luma_set_setting` with `game:<index>` changes a setting live (not saved). The index is the field's position in `LumaGameSettings`
  (`Shaders/Dragon Age Origins/Includes/GameCBuffers.hlsl`), e.g. `game:12` is `BokehDoFEnable` and `game:20` is `BokehTaps` at the time of writing:
  re-count the fields before using an index.
- The upscaler can't be switched from the tools: pick it in the Luma tab, then call `luma_perf` again.
- Shader edits hot reload with `luma_reload_shaders` (no restart); anything in `main.cpp` or the settings struct needs a rebuild and a restart.

## Cutscene bokeh depth of field (`DoFComposite_0xB507EFCE.ps_5_0.hlsl`)

Reported as "nukes performance" in a depth of field heavy scene. Measured at 2560x1440, no upscaler, 120 frame medians:

| Setting | Frame time | Cost of the bokeh |
|---|---|---|
| Bokeh off | 6.57 ms | |
| 96 taps (the original default) | 9.04 ms | 2.47 ms (about 152 fps down to 110) |
| 32 taps | 7.60 ms | 1.03 ms |
| 16 taps | 7.04 ms | 0.47 ms |

**Cause:** the cost is almost exactly linear in the tap count (about 0.026 ms per tap), so the gather itself is the whole bill. The scene copy at the
draw, the setup and the blend are negligible. Per tap the shader did three texture fetches (the sharp scene, the packed depth and the game's blurred
amount), three `pow` for the linear light average, and a `cos` / `sin`, for every blurred pixel, which in a DoF heavy shot is most of the frame.

**Changes** (all in the DoF shader and the scene copy in `main.cpp`):

1. **Adaptive taps:** a pixel takes `ceil(radius * BOKEH_TAPS_PER_PIXEL)` samples (min 12), capped by the Bokeh Taps setting, so a small blur is
   not paid for like a large one. Measured: 2.47 ms down to 1.98 ms at the 96 tap cap (modest: most pixels of the scene are at a large radius).
2. **Cheaper samples:** the average space is a plain square (`c * |c|`, then `sqrt`) instead of `pow(c, 2.2)` per channel per tap, and the sample
   direction is rotated by the golden angle each iteration instead of recomputed with `cos` / `sin`.
3. **Mip chain on the scene copy:** the copy made at the composite (`BindDoFScene`) has a full mip chain (`GenerateMips` after every copy, trilinear
   sampler) and each tap reads the level whose texels match the gap between taps (`tap_lod = log2(radius * 1.8 / sqrt(taps))`). Few taps then look as
   smooth as many, the usual way to cut a gather's cost. Costs a `GenerateMips` of the full size fp16 copy per DoF frame (expected well under 0.2 ms).
4. **Defaults:** `BokehTaps` 96 down to 32 (`BOKEH_TAPS_PER_PIXEL` is 2).

**Result with all four** (same scene, 120 frame medians; the off baseline moves a little between sessions, 6.6 to 6.7 ms):

| Setting | Frame time | Cost of the bokeh |
|---|---|---|
| Bokeh off | 6.73 ms | |
| 32 taps with mips (the current default) | 7.30 ms | **0.57 ms** (from 2.47 ms: 77% less) |
| 24 taps with mips (the default now) | 7.17 to 7.22 ms | about 0.44 ms |
| 12 taps with mips | 6.94 ms | 0.21 ms (the floor: the scene copy, `GenerateMips` and the full size pass) |

The look should be checked against the 96 tap original: with the mips and 32 taps the discs of very bright highlights may be less crisp; raise Bokeh
Taps (Tuning (dev)) if so.

**What the rest of the cost is** (experiments with the gather's per tap blur amount, 32 taps, same scene):

| Per tap amount | Frame time | Saved |
|---|---|---|
| Both fetches (the game's blurred `t2.w` and the packed depth, the current code) | 7.30 ms | |
| Only the blurred `t2.w` | 7.12 ms | 0.18 ms |
| Only the packed depth | 7.08 ms | 0.22 ms |
| None (the pixel's own amount for every tap) | 6.90 ms | 0.40 ms |

So most of what is left is the two fetches per tap that give each tap its own blur amount (what stops sharp things from bleeding into a blur): about
0.2 ms each, in latency, not arithmetic. Dropping either loses a case (the near blur from `t2.w`, or the far blur from the depth) and dropping both
loses the edge handling, so none of these was kept. The experiment is not in the shader any more.

**Further options if it is still too much:**

- Fewer taps: 24 is 0.13 ms cheaper than 32 (the Bokeh Taps setting, and its default in `main.cpp`).
- One fetch per tap, from a per pixel amount texture made first by a cheap pass (the two fetches and the depth decode done once per pixel, not per tap): needs
  the composite's `v6` depth scale in that pass (they come from the original vertex shader), so the pass would be a second draw of the composite
  with a different output. Expected gain is a fraction of the 0.2 ms of the second fetch, minus the pass.
- Gather at half resolution (a quarter of the pixels), with the same `v6` caveat.
- Skip pixels whose neighbourhood is entirely sharp (a coarse tile test).

**Further options if it is still too much:**

- Gather at half resolution (a quarter of the pixels): needs a half size target and the composite reading it, and the composite's `v6` depth scale
  inputs in that extra pass (they come from the original vertex shader).
- Read only one of the two blur sources per tap (the game's blurred `t2.w`, or the depth term) when one clearly dominates.
- Skip pixels whose neighbourhood is entirely sharp (a coarse tile test).

## Ambient occlusion (XeGTAO / GT-VBAO)

Reported as costly in an AO heavy scene, with the GPU well under full use. Measured at 2560x1440, no upscaler, 120 frame medians (the same spot differs by
about 0.3 to 0.5 ms between runs, so only differences larger than that mean something):

| `XE_GTAO_QUALITY` (slices) | Frame time |
|---|---|
| 3 (13 slices, the default) | 7.94 to 8.54 ms |
| 1 (7 slices) | 7.51 ms |
| 0 (4 slices) | 7.55 ms (a different run, against 8.54 ms at quality 3) |

What each part costs (shader experiments through hot reload, same spot):

- **Slices:** about 0.07 to 0.11 ms each, so the main pass is the cost and it scales with them.
- **Steps per slice** (3 down to 2): **no change** (7.99 against 7.94 ms): the depth fetches are not where the time goes, the per slice arithmetic is
  (the trigonometry and the normal projection each slice).
- **Capping the screen space radius** at 64 pixels (`XE_GTAO_ADJUST_SCREENSPACE_RADIUS`, to bound the reach of close geometry): **no change**
  (8.01 against 8.01 ms). Not kept.
- **The depth linearize pass** stubbed out: **no change** (7.93 against 7.94 ms): it is free. The prefilter, the two denoisers and the apply were not stubbed
  (the denoise and prefilter macros are redefined inside `XeGTAO.hlsl`, so a wrapper can't stub them: an attempt measured nothing).
- The CPU side (`RunXeGTAO`) has no readbacks, waits or per frame allocations (the scratch targets are made once per size).

So the options are: fewer slices (a lower quality define: the default is set in `OnInit`, the AO strength and darkness are low so the difference is small
by eye, but it was not judged by eye here), or computing the main pass at half resolution (a quarter of the pixels) with a depth aware upsample in the
apply pass (needs C++: a half size target and dispatch, a restart to test).

## Temporal anti-aliasing (DLSS, FSR, Luma TAA)

Reported as a noticeable frame rate loss with the GPU far from fully used. **Not reproduced yet:** in the scene tested, with DLSS running (the upscaler's
draw count was increasing) the frame time was 6.89 ms against 6.95 ms with no upscaler, and both were pinned at 144 fps by a cap, so any cost was hidden.
Re-measure uncapped, or in the heavy scene where the loss was seen, for none, Luma TAA, DLSS and FSR, in the Development and the Publishing build.

Where cost is expected from reading the code (not measured):

- **DLSS and FSR** run in a 64 bit helper process (the game is 32 bit): each frame copies about six full size textures into shared textures, signals a
  fence and calls `Flush()`, sends a pipe message, and makes the game's GPU wait on the helper's fence, then copies the output back
  (`SRBridge.cpp`, `SRBridgeD3D11.h`). The GPU idles during the wait, so utilisation can read low without the card being the limit. Luma TAA is in
  process and has no such wait.
- **All three** add per draw CPU work in `OnDrawOrDispatch` / `SceneMotionVectors::DrawWithMotionVectors` for every scene draw: render target, depth
  and blend state read back, a viewport re-set for the jitter, a constant buffer copy and a lookup of last frame's matching object. In a game that is
  already CPU bound in dgVoodoo's translation this comes straight off the frame time.
- The **Development build** adds trace annotations, counters and per frame collision counting that the Publishing build does not.

If it is the per draw path, the candidates are: skip the state readbacks for draws that cannot be scene geometry, and cache the render target and viewport state.

## Other costs worth knowing

- The scene copy for the DoF composite is one full size fp16 copy per DoF frame (`BindDoFScene`), plus the mip chain above. Both exist only while the
  bokeh is on and a cutscene uses the DoF composite.
- The game is a 32 bit process (about 2.1 GB of 4 GB of address space in use in the captures): extra full size targets cost address space as well as time.
