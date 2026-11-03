# Dragon Age: Origins - vertex shader camera census (offline, dgVoodoo 2.87.3 dump)

Source: the 61 vertex shaders in `Shaders/Dragon Age Origins/Dump` (disassembled with fxc /dumpbin), tracing which `cb4`
rows (dgVoodoo's vc4, c<N> = cb4[N + 20]) feed SV_Position. Only a first pass: the dump may not hold every vertex shader the
game uses, and it must be redone if the dgVoodoo version changes.

| Position fed by cb4 rows | c registers | Shaders | Notes |
|---|---|---|---|
| 238-241 | c218-c221 | 6 (69F2500C, 77E88745, 82C3436C, 8B63BF45, C11FF647, EBD1FF40) | `CB4[276], dynamicIndexed` (skinned: bones indexed), matches the view projection in `DAOCamera.hlsl` |
| 221-224 | c201-c204 | 4 (184F3939, 21E45F9B, 3902A573, 6EC2ECFF) | indexed too, a second skinned/instanced kind |
| 20-23 | c0-c3 | 11 (080AA5EF, 0A45D6AF, 26D72BC6, 318D7C41, 370310C5, 3FC613D1, ...) | immediate indexed: static world view projection |
| 140-143 (+157,158) | c120-c123 | 2 (52DAFB11, B2A21672) | indexed |
| 37-40, 25-28, 32-35, 29-33 | | 2 + 2 + 1 + 1 | single-shader kinds, probably effects/particles |
| none (cb0[35..38]) | | 22 | do not use cb4: position from cb0 (a different path, check whether these are UI, shadow or the scene) |
| mixed 2-matrix | c0-c3 + c6-c11 etc. | ~12 | world view + projection split, or extra rows for lighting |

All 16 `CB4[276], dynamicIndexed` shaders write `mov o0, r.xyzw`, so the position math is split across temporaries.

## Next steps
1. Confirm in game with `luma_trace_capture`/`luma_read_cbuffer` which of these draw the opaque scene, characters and particles,
   and read the real values of cb4 rows 238-241 to confirm they are the view projection.
2. Write `MotionVectorPatches.h` (copy Metal Gear Rising's) with these camera kinds, in priority order, and the tie-break
   rows (bone / world matrix).
3. Investigate the 22 cb0 shaders: if they draw the scene, the patch layout needs a second object slot.

## In game confirmation (luma_read_cbuffer, frame 3000, dgVoodoo 2.87.x build in use)
- cb4 rows 238-241 (c218-c221) hold the **frame view projection**: stable for the frame, changes with the camera, y scale
  1.7254 = 1/tan(30 degrees) (so the 60 degree vertical FOV in `DAOCamera.hlsl` is right), z/w columns are a standard D3D projection.
  It is present in every scene draw regardless of the vertex shader, so it is the `frame_camera` (VIEW_PROJECTION, row 238).
- cb4 rows 20-23 (c0-c3), read by VS 0x318D7C41 (93+ scene draws): a **per-object world view projection** (row 23 is the
  translation). `CameraType::WORLD_VIEW_PROJECTION`, row 20.
- cb4 rows 221-224 (c201-c204), read by the skinned/instanced VS 0x21E45F9B and 0x6EC2ECFF: also a full per-object WVP
  (translation row 224 = 38.9 w). `WORLD_VIEW_PROJECTION`, row 221. Rows 201-204 of this and the other shader are not a camera.
- Frame structure (one frame, 5062 trace entries): ~93 opaque draws with PS 38A5D9CD + ~85 with D24D2C23 into the 2560x1440 scene
  target (MSAA, upgraded fp16 behind the R8G8B8A8 handle), depth-only/tested 752783AE (35), shadow maps (49E804A7, 1024x1024, 79 draws),
  ~489 particle draws (5DE6B6F6, depth test only), then the grade 6B504D9F, then HUD (31AE03D5, B972AC3B, 3DCC5533), then dgVoodoo's present 880A17D3.
- Still open: the 22 `cb0[35..38]` shaders. 0x0ACB3A21 draws 35 times with PS 752783AE, depth "Test Only" into the scene target
  (decals/water/overlays?), so they ARE scene draws and need a second object slot or the camera-only fallback.
