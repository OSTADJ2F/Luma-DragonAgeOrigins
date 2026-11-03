// XeGTAO for Dragon Age: Origins (Eclipse engine, DX9 -> dgVoodoo2 -> DX11). Source: https://github.com/GameTechDev/XeGTAO
//
// The game has no ambient occlusion at all, so this one is added, not replaced. It runs on the scene depth right after the
// opaque geometry (see "main.cpp"), and its result is multiplied into the scene colour by "Luma_DAO_AOApply.hlsl".
// - Depth input = "Luma_DAO_DepthLinearize.hlsl": full resolution R32F linear view space depth, in meters.
// - There is no normal buffer: the normals are generated from depth (XE_GTAO_GENERATE_NORMALS).
// - The NDC to view space mapping comes from the 60 degree vertical field of view and the depth buffer's aspect ratio
//   (the knobs buffer holds the tan of the half field of view for x and y).
// - No TAA and no motion vectors: the noise index is frozen at 0, and the denoise runs twice, so all the stability is spatial.
// - The knobs live in b9, which the other XeGTAO ports use too (core owns b11 for the bloom).

cbuffer LumaGTAO : register(b9)
{
   float FinalValuePowerRT;    // The main darkness dial (higher is darker)
   float DepthScaleRT;         // View z divisor (1 = meters, which is what the game's units are)
   float RadiusOverrideRT;     // > 0 overrides EFFECT_RADIUS (meters)
   float DebugViewRT;          // DEVELOPMENT: 0 = off, 1 = depth gradient, 2 = normals, 3 = AO x8, 4 = edges
   float2 ViewportPixelSizeRT; // 1 / the depth buffer's resolution
   float2 TanHalfFovRT;        // tan(fov / 2) for x and y: (tan_y * aspect, tan_y)
}

#ifndef XE_GTAO_GENERATE_NORMALS
#define XE_GTAO_GENERATE_NORMALS 1
#endif

#ifndef EFFECT_RADIUS
#define EFFECT_RADIUS 0.5 // Meters before RADIUS_MULTIPLIER; RadiusOverrideRT > 0 wins. To be tuned in game.
#endif

#ifndef EFFECT_FALLOFF_RANGE
#define EFFECT_FALLOFF_RANGE 0.3 // Intel's default is 0.615 (soft), the HBAO style games here use 0.005 (crisp contact)
#endif

#define VIEWPORT_PIXEL_SIZE ViewportPixelSizeRT

// viewPos.xy = (uv * MUL + ADD) * viewZ, with y flipped (uv.y goes down, view space y goes up).
#define NDC_TO_VIEW_MUL (float2(2.0, -2.0) * TanHalfFovRT)
#define NDC_TO_VIEW_ADD (float2(-1.0, 1.0) * TanHalfFovRT)

// The depth is already linear view space depth in meters.
float XeGTAO_ScreenSpaceToViewSpaceDepth(const float screenDepth)
{
   return max(0.0, screenDepth) / max(1e-3, DepthScaleRT);
}

#define NoiseIndexRT 0

// The AO target is an R8_UNORM visibility texture.
#define XE_GTAO_FINAL_OUTPUT_TYPE unorm float
#define XE_GTAO_ENCODE_FINAL(v) (v)

// GT-VBAO (Mirko Salm, "../Includes/GTVBAO.hlsl") is an alternative main pass on the same inputs: "gtvbao_main_pass_cs" in place of
// "main_pass_cs", with the same prefilter and denoise. Including it also brings in all of XeGTAO's entry points.
// Visibility bitmask: it models a thickness behind every depth sample, so it handles thin occluders (railings, foliage, props)
// that horizon based GTAO over darkens. A quality level adds slices; the steps per side stay low as the rest is spatially denoised.
#ifndef GTVBAO_SLICE_COUNT
#define GTVBAO_SLICE_COUNT max(2.0, (float)XE_GTAO_QUALITY)
#endif
#ifndef GTVBAO_STEP_COUNT
#define GTVBAO_STEP_COUNT 8.0
#endif
#ifndef GTVBAO_THICKNESS
#define GTVBAO_THICKNESS(effectRadius) (0.4 * (effectRadius)) // View space meters assumed behind every depth sample. To be tuned in game.
#endif

#include "../Includes/GTVBAO.hlsl"
