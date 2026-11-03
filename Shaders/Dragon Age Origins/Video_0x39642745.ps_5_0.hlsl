// Dragon Age: Origins - the Bink movie pass, as translated by dgVoodoo (ps_5_0): three planes (Y t0, U t1, V t2, sampled at the
// same uv) turned into RGB with a fixed BT.601 video range matrix, straight onto the canvas. Movies never go through the grade.
// Restores the vanilla clamp (the 8 bit canvas gave it for free, the fp16 one clips nothing), then a light PumboAutoHDR.
// dgVoodoo rule: every texture fetch is followed by an `and`/`or` pair against b3 (the format masks), dropping it shifts
// colour. The entry point declares all 13 interpolators, as linkage is by REGISTER (only TEXCOORD0, the movie uv, is read).

// clang-format off
// ORDER IS LOAD-BEARING: the game-local "Includes/Common.hlsl" must come first (it defines LUMA_GAME_CB_STRUCTS).
#include "Includes/Common.hlsl"
#include "../Includes/Color.hlsl"
#include "../Includes/DgVoodoo.hlsl"
#include "../Includes/ColorGradingLUT.hlsl"
#include "Includes/UIPreScale.hlsl"
// clang-format on

// Light AutoHDR on movies. The peak is kept low: Bink is low bitrate and pushing it amplifies the block artifacts.
// HDR only: its peak comes from the boost (165 nits at 0.5), not from the display, so in SDR it would push the movies' highlights 2x past
// white for the SDR output to clip.
#ifndef ENABLE_VIDEO_AUTO_HDR
#define ENABLE_VIDEO_AUTO_HDR 1
#endif
#ifndef VIDEO_AUTO_HDR_PEAK_NITS
#define VIDEO_AUTO_HDR_PEAK_NITS 250.0
#endif

// b3 is dgVoodoo's D3D9 constant mirror, declared at the original's size (CB3[77]). b4 is the game's (CB4[236]).
cbuffer DgVoodooState : register(b3)
{
   float4 DgvConstants[77] : packoffset(c0);
}
cbuffer GameState : register(b4)
{
   float4 cb4[236] : packoffset(c0);
}

#define DgvMaskT0 DgvConstants[44]
#define DgvFillT0 DgvConstants[45]
#define DgvMaskT1 DgvConstants[46]
#define DgvFillT1 DgvConstants[47]
#define DgvMaskT2 DgvConstants[48]
#define DgvFillT2 DgvConstants[49]

SamplerState s0_s : register(s0);
SamplerState s1_s : register(s1);
SamplerState s2_s : register(s2);
Texture2D<float4> t0 : register(t0); // Y plane
Texture2D<float4> t1 : register(t1); // U plane
Texture2D<float4> t2 : register(t2); // V plane

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD8,
    float4 v2 : COLOR0,
    float4 v3 : COLOR1,
    float4 v4 : TEXCOORD9,
    float4 v5 : TEXCOORD0,
    float4 v6 : TEXCOORD1,
    float4 v7 : TEXCOORD2,
    float4 v8 : TEXCOORD3,
    float4 v9 : TEXCOORD4,
    float4 v10 : TEXCOORD5,
    float4 v11 : TEXCOORD6,
    float4 v12 : TEXCOORD7,
    out float4 o0 : SV_TARGET0)
{
   // YUV plane fetch + dgVoodoo mask (verbatim).
   float4 yuv;
   yuv.x = ApplyDgvMask(t0.Sample(s0_s, v5.xy), DgvMaskT0, DgvFillT0).x;
   yuv.y = ApplyDgvMask(t1.Sample(s1_s, v5.xy), DgvMaskT1, DgvFillT1).x;
   yuv.z = ApplyDgvMask(t2.Sample(s2_s, v5.xy), DgvMaskT2, DgvFillT2).x;
   yuv.w = 1.0;

   // YUV -> RGB (verbatim dp4 rows).
   float3 rgb;
   rgb.r = dot(float4(1.164124, 1.595795, 0.0, -0.870655), yuv);
   rgb.g = dot(float4(1.164124, -0.813477, -0.391449, 0.529705), yuv);
   rgb.b = dot(float4(1.164124, 0.0, 2.017822, -1.081669), yuv);

   // The vanilla clamp.
   rgb = saturate(rgb);

#if ENABLE_VIDEO_AUTO_HDR
   if (LumaSettings.DisplayMode > 0 && LumaSettings.GameSettings.VideoAutoHDREnable > 0.5)
   {
      // The game's own encoding (gamma 2.2 or sRGB, "VANILLA_ENCODING_TYPE"), the same the grade encodes with and the display
      // composition decodes with, so the movie lands at gameplay brightness.
      float3 lin = game_gamma_to_linear(rgb, false);
      // boost 0 = peak at sRGB white (80 nits): PumboAutoHDR no-ops (off); 1 = the full VIDEO_AUTO_HDR_PEAK_NITS.
      const float peakNits = lerp(sRGB_WhiteLevelNits, VIDEO_AUTO_HDR_PEAK_NITS, saturate(LumaSettings.GameSettings.VideoAutoHDRBoost));
      lin = PumboAutoHDR(lin, peakNits, LumaSettings.GamePaperWhiteNits);
      rgb = linear_to_game_gamma(lin, false);
   }
#endif

   // UI_DRAW_TYPE 2: the display composition scales the whole canvas by UI / Game paper white, the movie is scene content (see the grade)
   // The movies are decoded like the scene: the same black crush, the same lift (HDR only, where the correction type applies)
   if (LumaSettings.DisplayMode > 0)
      rgb = ApplyShadowLift(rgb, LumaSettings.GameSettings.ShadowLift);
   rgb = ApplyUIPaperWhitePreScale(rgb);

   o0 = float4(rgb, cb4[8].x); // vanilla alpha (the constant)
}
