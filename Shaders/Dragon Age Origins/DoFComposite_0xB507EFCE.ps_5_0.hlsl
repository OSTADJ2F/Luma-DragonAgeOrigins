// Dragon Age: Origins - the depth of field composite (cutscenes), as translated by dgVoodoo (ps_5_0): decodes the packed scene depth (t3)
// into a blur amount, takes the larger of it and the blurred scene's own (t2.w), and draws the blurred scene (t2) over the sharp one with
// that amount as its alpha ("SRC_ALPHA, INV_SRC_ALPHA" blend onto the scene).
// HDR: the blur amount was clamped to 0-1 for free by the 8 bit target, the fp16 one blends with the raw alpha: an amount of 211 (a far
// pixel with a strong scale) blended the scene with a 211x weight, which drew wild positive and negative colours at some camera angles.
// The amount (and the debug mask) is saturated here instead. The blurred colour stays unclamped (HDR).
// Bokeh: instead of drawing the game's blurred (half resolution) scene, the sharp scene (t4, a copy made by the game mod at this draw) is gathered
// over a disc: its radius follows the pixel's blur amount, every tap is weighted by its own amount so the sharp pixels around don't bleed into
// the blur (and a blurred foreground still spreads over a sharp background), and bright taps weigh more, so highlights turn into discs. The
// same amount (alpha) still blends it over the sharp scene. Falls back on the game's blur when t4 isn't bound or the option is off.
// dgVoodoo rule: every texture fetch is followed by an `and`/`or` pair against b3 (the format masks), dropping it shifts
// colour. The entry point declares all 13 interpolators, as linkage is by REGISTER.

// clang-format off
// ORDER IS LOAD-BEARING: the game-local "Includes/Common.hlsl" must come first (it defines LUMA_GAME_CB_STRUCTS).
#include "Includes/Common.hlsl"
#include "../Includes/DgVoodoo.hlsl"
// clang-format on

// b3 is dgVoodoo's D3D9 constant mirror, declared at the original's size (CB3[77]). b4 is the game's (CB4[236]).
cbuffer DgVoodooState : register(b3)
{
   float4 DgvConstants[77] : packoffset(c0);
}
cbuffer GameState : register(b4)
{
   float4 cb4[236] : packoffset(c0);
}

#define DgvMaskT2 DgvConstants[48]
#define DgvFillT2 DgvConstants[49]
#define DgvMaskT3 DgvConstants[50]
#define DgvFillT3 DgvConstants[51]

SamplerState s2_s : register(s2);
SamplerState s3_s : register(s3);
SamplerState s4_s : register(s4); // Bilinear clamp, bound by the game mod at this draw (point if it isn't)
Texture2D<float4> t2 : register(t2); // The blurred scene (rgb) and its blur amount (a)
Texture2D<float4> t3 : register(t3); // The packed scene depth
Texture2D<float4> t4 : register(t4); // The sharp scene (HDR), bound by the game mod at this draw

// The disc's radius at blur amount 1 and bokeh radius 1, in rows of the image (0.0125 = 18 pixels at 1440p)
#define BOKEH_MAX_RADIUS_PER_HEIGHT 0.0125
// A pixel takes only the samples its blur needs: this many per pixel of radius (a small blur is covered by a few), up to the Bokeh Taps setting. The
// cost of the pass is nearly all in the samples.
#define BOKEH_TAPS_PER_PIXEL 2.0

// The blur amount the game's composite would use at a texel (same maths as the original, "v6" is constant over the quad)
float BlurAmountAt(float2 uv, float2 depthScale)
{
   const float4 packedDepth = ApplyDgvMask(t3.SampleLevel(s3_s, uv, 0), DgvMaskT3, DgvFillT3);
   const float depth = dot(packedDepth, float4(1.0 / 65536.0, 1.0 / 256.0, 1.0, -1.0));
   const float depthAmount = saturate((depth * cb4[8].x - depthScale.x) * depthScale.y);
   const float blurredAmount = ApplyDgvMask(t2.SampleLevel(s4_s, uv, 0), DgvMaskT2, DgvFillT2).w;
   return saturate(max(blurredAmount, depthAmount) * cb4[9].x);
}

// The gather happens in a space close to linear light with the UI paper white pre scale taken out (so the highlight weights don't move with the UI
// slider): a plain square, consistently both ways (only an average space, and a pow per sample and channel was a large part of the cost)
float3 ToBlurSpace(float3 gammaColor, float preScale)
{
   return gammaColor * abs(gammaColor) / preScale;
}
float3 FromBlurSpace(float3 linearColor, float preScale)
{
   return sign(linearColor) * sqrt(abs(linearColor * preScale));
}

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
   uint sceneWidth, sceneHeight;
   t4.GetDimensions(sceneWidth, sceneHeight); // 0 when nothing is bound
   const bool bokeh = LumaSettings.GameSettings.BokehDoFEnable > 0.5 && sceneWidth > 0 && cb4[10].x - 1.0 != 0.0;

   const float4 packedDepth = ApplyDgvMask(t3.Sample(s3_s, v5.xy), DgvMaskT3, DgvFillT3);
   const float depth = dot(packedDepth, float4(1.0 / 65536.0, 1.0 / 256.0, 1.0, -1.0));
   const float depthAmount = saturate((depth * cb4[8].x - v6.x) * v6.y);

   const float4 blurred = ApplyDgvMask(t2.Sample(s2_s, v5.xy), DgvMaskT2, DgvFillT2);
   const float amount = max(blurred.w, depthAmount) * cb4[9].x;

   if (bokeh)
   {
      const float2 size = float2(sceneWidth, sceneHeight);
      const int2 pixel = int2(v5.xy * size);
      const float preScale = max(GetUIPaperWhitePreScale(GamePaperWhiteNits, UIPaperWhiteNits, true), 0.001);
      const float maxRadius = BOKEH_MAX_RADIUS_PER_HEIGHT * size.y;
      const float radius = amount * maxRadius;
      const float3 center = t4.Load(int3(pixel, 0)).rgb;
      if (radius < 0.75)
      {
         // Too small to see: the sharp scene itself (the blend keeps it)
         o0 = float4(center, saturate(amount));
         return;
      }
      // Per pixel rotation (interleaved gradient noise) so the 64 taps' pattern doesn't show as rings
      const float rotation = 6.2831853 * frac(52.9829189 * frac(dot(float2(pixel), float2(0.06711056, 0.00583715))));
      float3 sum = 0.0;
      float weightSum = 0.0;
      const float max_taps = clamp(LumaSettings.GameSettings.BokehTaps, 8.0, 256.0);
      const int taps = int(clamp(ceil(radius * BOKEH_TAPS_PER_PIXEL), min(12.0, max_taps), max_taps));
      // Vogel disc: equal area spacing, the golden angle between samples (the direction is rotated by it each time, not recomputed)
      float2 direction;
      sincos(rotation, direction.y, direction.x);
      const float2 golden = float2(-0.737368878, 0.675490294); // cos and sin of 2.3999632
      // The gap between samples on the disc is about radius * 1.8 / sqrt(taps) pixels
      const float tap_lod = log2(max(radius * 1.8 * rsqrt(float(taps)), 1.0));
      [loop] for (int i = 0; i < taps; i++)
      {
         const float r = sqrt((i + 0.5) / taps);
         const float2 offset = direction * (r * radius);
         direction = float2(direction.x * golden.x - direction.y * golden.y, direction.x * golden.y + direction.y * golden.x);
         // Sub pixel tap position (rounding it to whole pixels jagged the edges of the blur)
         const float2 tapUV = clamp((float2(pixel) + 0.5 + offset) / size, 0.5 / size, 1.0 - 0.5 / size);
         const float tapAmount = BlurAmountAt(tapUV, v6.xy);
         const float tapDistance = r * radius;
         // The tap's own disc has to reach this pixel (a soft one pixel edge)
         float weight = saturate(tapAmount * maxRadius - tapDistance + 1.0);
         // The scene's mip chain smooths what few samples can't: each sample reads the level whose texels are about as wide as the gap between samples
         const float3 tapColor = ToBlurSpace(t4.SampleLevel(s4_s, tapUV, tap_lod).rgb, preScale);
         // Bright taps weigh more (disc shaped highlights), capped against fireflies
         weight *= 1.0 + min(LumaSettings.GameSettings.BokehHighlightBoost * max(dot(tapColor, float3(0.2126, 0.7152, 0.0722)) - 1.0, 0.0), 15.0);
         sum += tapColor * weight;
         weightSum += weight;
      }
      const float3 blurredColor = weightSum > 0.0001 ? FromBlurSpace(sum / weightSum, preScale) : center;
      o0 = float4(blurredColor, saturate(amount));
   }
   else if (cb4[10].x - 1.0 == 0.0)
   {
      // The debug view of the amount
      o0 = float4(saturate(amount).xxx, 1.0);
   }
   else
   {
      // The blurred scene over the sharp one, by the amount (the blend's alpha must stay 0-1)
      o0 = float4(blurred.xyz, saturate(amount));
   }
}
