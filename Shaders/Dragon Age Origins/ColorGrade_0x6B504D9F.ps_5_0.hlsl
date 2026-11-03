// Dragon Age: Origins - the frame's colour grade, as translated by dgVoodoo (ps_5_0): reads the scene (t0), multiplies it by
// a 3x3 matrix (v6/v7/v8 rows, one per output channel) and looks each result up in a 256x1 curve texture (t1, point
// sampled). Its output is still 0-1: the 8-bit canvas the HUD blends onto.
// HDR: the grade's input already holds values above 1 (the scene buffer is upgraded to float), but the game's curve reaches
// 1 at about 0.8 of its range and is flat after that, so every highlight above it collapses to white. Here the vanilla grade
// is reproduced exactly up to the point where each channel's curve clips; beyond it the curve is continued with the slope it
// had just before (in linear light), then rolled off smoothly into the display peak. That brings back the real detail of
// skies, flames and bright ground instead of faking a highlight boost. The sub-clip look is untouched, except that in HDR
// the 8 bit stepped curve is read as a smoothed continuous one (CURVE_SMOOTHING) to avoid banding on the float scene.
// dgVoodoo rule: every texture fetch is followed by an `and`/`or` pair against b3 (the format masks), dropping it shifts
// colour. The entry point declares all 13 interpolators, as linkage is by REGISTER.

// clang-format off
// ORDER IS LOAD-BEARING: the game-local "Includes/Common.hlsl" must come first (it defines LUMA_GAME_CB_STRUCTS).
#include "Includes/Common.hlsl"
#include "../Includes/Color.hlsl"
#include "../Includes/ColorGradingLUT.hlsl" // SimpleGamutClip
#include "../Includes/Oklab.hlsl"
#include "../Includes/DgVoodoo.hlsl"
#include "Includes/FilmGrain.hlsl"
#include "Includes/UIPreScale.hlsl"
// clang-format on

// b3 is dgVoodoo's D3D9 constant mirror, declared at the original's size (CB3[77]).
cbuffer DgVoodooState : register(b3)
{
   float4 DgvConstants[77] : packoffset(c0);
}

#define DgvMaskT0 DgvConstants[44]
#define DgvFillT0 DgvConstants[45]
#define DgvMaskT1 DgvConstants[46]
#define DgvFillT1 DgvConstants[47]

SamplerState SceneSampler_s : register(s0);
SamplerState CurveSampler_s : register(s1);
Texture2D<float4> SceneTexture : register(t0);
Texture2D<float4> CurveTexture : register(t1); // 256x1, one curve per channel

// The last texel of the curve. The game's curve is 256 texels wide in some areas and 255 in others (Load past the end returns 0,
// which read as a curve that drops to black and switched the highlight recovery off), so it is read from the texture.
static uint CurveLast = 255;

// The curve's value (0-1, encoded) at an integer texel, for one channel.
float CurveValue(uint index, uint channel)
{
   return ApplyDgvMask(CurveTexture.Load(int3(min(index, CurveLast), 0, 0)), DgvMaskT1, DgvFillT1)[channel];
}

// The curve is 8 bit and the game point samples it, so on a float scene it is a 256 step staircase (flat plateaus and skipped
// codes): visible as banding, and in the recovered highlights as hue steps, as the channels that have not clipped yet stay
// stepped while the clipped one is continuous. This reads it as a continuous curve instead: each texel is replaced by the
// average of its two neighbours (so a quantized 140 141 141 142 ramp becomes 140.5 141.5), then interpolated linearly.
// Same math as "Sample1DLUTWithSmoothing" (ColorGradingLUT.hlsl), but with the dgVoodoo format mask applied on every load.
// Texel i is the output for the input byte i, so input x lands on texel x * CurveLast (the game's point sampling gives the same
// texel for every 8 bit input).
#ifndef CURVE_SMOOTHING
#define CURVE_SMOOTHING 1
#endif
float SmoothedCurveValue(float x, uint channel)
{
   const float f = saturate(x) * float(CurveLast);
   const int i = (int)f;
   const int n1 = max(i - 1, 0);
   const int p1 = min(i + 1, (int)CurveLast);
   const int p2 = min(i + 2, (int)CurveLast);
   const float c0 = CurveValue(n1, channel);
   const float c1 = CurveValue(i, channel);
   const float c2 = CurveValue(p1, channel);
   const float c3 = CurveValue(p2, channel);
   // No smoothing on the first and last texels: it would raise the black and lower the white.
   const float intensity = (i <= 0 || i >= (int)CurveLast - 1) ? 0.0 : 1.0;
   const float s1 = lerp(c1, (c0 + c2) * 0.5, intensity);
   const float s2 = lerp(c2, (c1 + c3) * 0.5, intensity);
   return lerp(s1, s2, frac(f));
}

// The curve's own top, not 1: some areas have a curve whose last texel is 254/255 (a 255 texel curve), so a fixed "reaches 1"
// test never saw it clip and switched the recovery off. The input past the curve's end is always clamped to this top.
float CurveClipLevel(uint channel)
{
   return CurveValue(CurveLast, channel) - 0.5 / 255.0;
}

// First texel where one channel's curve reaches its top (the last one if it never does). The curve never decreases, so bisect.
uint CurveClipTexel(uint channel)
{
   const float clipLevel = CurveClipLevel(channel);
   uint lo = 0, hi = CurveLast;
   [unroll] for (uint i = 0; i < 8; i++)
   {
      const uint mid = (lo + hi) >> 1;
      if (CurveValue(mid, channel) >= clipLevel)
         hi = mid;
      else
         lo = mid + 1;
   }
   return lo;
}

// Linear light multiplier (>= 1) of one channel's curve continued past its clip point, 1 if the input has not reached it.
// `x` is the curve input (the matrix result). Above the clip point it is 1 plus the slope the curve had just before the
// clip, in linear light.
float ExtendedChannel(float x, uint channel)
{
   // Cheap early out (the curve never decreases): the texel this input lands on is not clipped, so nothing to recover.
   if (CurveValue((uint)(saturate(x) * float(CurveLast)), channel) < CurveClipLevel(channel))
      return 1.0;
   const uint lo = CurveClipTexel(channel);
   const float xClip = float(lo) / float(CurveLast);
   // The curve's slope (encoded value per unit of input) over its last stretch before the clip.
   const uint a = (lo >= 20) ? lo - 20 : 0;
   const uint b = (lo >= 4) ? lo - 4 : lo;
   float slope = 1.0;
   if (b > a)
      slope = (CurveValue(b, channel) - CurveValue(a, channel)) * float(CurveLast) / float(b - a);
   slope = max(slope, 0.1);
#if VANILLA_ENCODING_TYPE == 1
   const float linearSlope = slope * 2.2; // gamma 2.2 decode's slope at 1.0
#else
   const float linearSlope = slope * (2.4 / 1.055); // sRGB decode's slope at 1.0
#endif
   return 1.0 + linearSlope * max(x - xClip, 0.0);
}

// The recovered highlights mapped into the display: identity up to paper white, then an extended Reinhard on the part above it,
// sized by the display's headroom (peak / paper white - 1): slope 1 at paper white, and the content white point ("HighlightWhite",
// in paper whites) lands exactly on the peak. So it fits any peak / paper white pair by itself.
// This replaced a smoothstep lift onto the peak over a range above paper white, which put everything above the range flat on the
// peak: measured on a fire and sunbeam scene (13% of the screen recovered, 1 to 23 paper whites, median 2.1, p90 4.8, p99 10.9),
// ~60% of the recovered pixels sat flat on the peak at the default range and ~17% at 4x it, whatever the display, as the content's
// range doesn't depend on the display. With the white point at the content's p99 (11), 1-2% do, on any display.
// "HighlightPunch" sharpens the knee: the shoulder is t / (1 + a t^n)^(1/n), with a = 1 - wn^-n so the white point still lands on
// the peak, slope 1 at paper white for any n (no crease on gradients), monotonic. n = 1 is a plain Reinhard (gentlest), a larger n
// stays near identity longer and then bends harder (brighter highlights, more of the brightest flat on the peak). Measured on the
// same scene at a 2.0 peak ratio, white 11: n 1 / 2 / 3 put 2 / 14 / 29% of the recovered pixels on the peak at 76 / 82 / 84% of
// it on average; at a 4.9 ratio, 1.2 / 1.5 / 2%. A lift of y + p y^2 (1 - y) on the plain shoulder was tried first: at most ~5%
// brighter, too little to see.
// The white point is never taken below the headroom (wn >= 1): on a display with more headroom than the content, the content is not
// stretched to fill it.
float HighlightShoulder(float brightness, float peakRatio)
{
   if (brightness <= 1.0)
      return brightness;
   const float headroom = max(peakRatio - 1.0, FLT_MIN);
   const float t = (brightness - 1.0) / headroom;
   const float wn = max((max(LumaSettings.GameSettings.HighlightWhite, 1.0) - 1.0) / headroom, 1.0);
   const float n = 1.0 + 2.0 * saturate(LumaSettings.GameSettings.HighlightPunch);
   const float a = 1.0 - pow(wn, -n);
   const float y = min(t / pow(1.0 + a * pow(t, n), 1.0 / n), 1.0);
   return 1.0 + headroom * y;
}

// The graded colour in linear light (1 = paper white) with the clipped highlights recovered and rolled off into the
// display peak, blended from the vanilla grade by `strength` (0 = vanilla).
// The recovery is ONE gain for the whole colour (what the furthest-extended channel would have reached), applied to
// the vanilla colour: extending each channel on its own makes the channel whose curve clips last run ahead of the others
// and shifts the hue (fire and bright whites went red).
float3 RecoverHighlights(float3 coords, float3 vanillaGamma, float strength)
{
   const float peakRatio = PeakWhiteNits / GamePaperWhiteNits;
   const float3 vanillaLinear = game_gamma_to_linear(max(vanillaGamma, 0.0), false);
   if (peakRatio <= 1.0 || strength <= 0.0)
      return vanillaLinear;
   const float3 extendedChannels = float3(ExtendedChannel(coords.x, 0), ExtendedChannel(coords.y, 1), ExtendedChannel(coords.z, 2));
   if (all(extendedChannels <= 1.0)) // nothing clipped
      return vanillaLinear;
   // The largest extension of any channel (1 for a channel that has not clipped yet). A max of continuous functions is
   // continuous: averaging over the channels that happen to be clipped jumps whenever another channel clips, which drew
   // thin contour lines (outlines) through the clouds.
   const float gain = max(max3(extendedChannels), 1.0);
   float3 extended = vanillaLinear * gain;
   // The single gain keeps the vanilla colour's hue, but where all three channels clipped that colour is plain white, so fire and
   // sunlit windows came out white though the scene holds their colour. The hue is taken from the scene's own colour, graded as the
   // game grades it: the curve input scaled down (all channels alike, so its hue is kept) until no channel is past its clip point,
   // then through the curves. The gain's result keeps its Oklab lightness and borrows that colour's saturation (chroma per lightness,
   // which a uniform scale doesn't change in Oklab) and hue, by "HighlightColor".
   // Not each channel continued on its own curve (vanilla * extendedChannels): that hue comes from the slopes of the curves at their
   // clip points, not from the scene, and turned the light shafts of a teal graded scene lavender (2% of the screen).
   // Hue-preserving, on the largest channel: the content white point lands on the set peak (see "HighlightShoulder").
   const float brightest = max3(extended);
   const float rolledOff = HighlightShoulder(brightest, peakRatio);
   extended *= rolledOff / max(brightest, FLT_MIN);
   // The colour goes on after the rolloff, at the rolled off colour's luminance: before it, a more saturated colour has a larger
   // top channel for the same brightness, so the rolloff (on that channel) took it further down and the highlights went ~20% dimmer.
   // At the same luminance, the colour then only goes as far as the room under the peak allows: scaling a colour whose top channel passed
   // the peak back down instead dimmed the frame by ~24% (most recovered highlights sit at the peak, where no colour fits at all).
   // The top channel of a blend is at most the blend of the top channels, so blending by that room keeps it under the peak.
   const float hueRestore = saturate(LumaSettings.GameSettings.HighlightColor);
   if (hueRestore > 0.0)
   {
      const float3 clipPoints = float3(CurveClipTexel(0), CurveClipTexel(1), CurveClipTexel(2)) / float(CurveLast);
      const float3 sceneCoords = max(coords, 0.0);
      const float unclipScale = saturate(min3(clipPoints / max(sceneCoords, FLT_MIN)));
      const float3 unclipped = sceneCoords * unclipScale;
      const float3 sceneGraded = game_gamma_to_linear(float3(SmoothedCurveValue(unclipped.x, 0), SmoothedCurveValue(unclipped.y, 1), SmoothedCurveValue(unclipped.z, 2)), false);
      const float luminance = GetLuminance(extended, CS_BT709);
      float3 lab = Oklab::linear_srgb_to_oklab(extended);
      const float3 sceneLab = Oklab::linear_srgb_to_oklab(sceneGraded);
      lab.yz = lerp(lab.yz, sceneLab.yz * (lab.x / max(sceneLab.x, FLT_MIN)), hueRestore);
      float3 colored = max(Oklab::oklab_to_linear_srgb(lab), 0.0);
      colored *= luminance / max(GetLuminance(colored, CS_BT709), FLT_MIN);
      const float room = saturate((peakRatio - max3(extended)) / max(max3(colored) - max3(extended), FLT_MIN));
      extended = lerp(extended, colored, room);
   }
   // The brightest recovered highlights roll off towards white, as a film or a camera sensor does when it saturates: the largest channel (the
   // one that sets the peak) stays, the others rise to meet it, from paper white up to the peak. Hue kept all the way up (0) turns fire and
   // the sun into neon.
   const float desaturation_start = max(LumaSettings.GameSettings.HighlightDesatStart, 0.1);
   const float desaturation_end = max(max(peakRatio, 1.5) * LumaSettings.GameSettings.HighlightDesatEnd, desaturation_start + 0.05);
   const float desaturation = saturate(LumaSettings.GameSettings.HighlightDesaturation) * smoothstep(desaturation_start, desaturation_end, rolledOff);
   extended = lerp(extended, rolledOff.xxx, desaturation);
   return lerp(vanillaLinear, extended, strength);
}

// Gamut expansion on the (gamma space) graded colour. Each colour's chroma (around its own luminance) is stretched by how
// much further the BT.2020 edge is than the BT.709 one along its own hue/luminance ray, so at `amount` 1 a colour on the
// BT.709 edge lands on the BT.2020 edge and nothing goes past it. Neutrals stay put. 0 = vanilla.
// The game's colours are mostly well inside BT.709, so only the already saturated ones end up outside it (the rest just
// gain a little chroma). The result goes negative in BT.709 for those, which scRGB can represent.
float3 ExpandGamut(float3 gammaColor, float amount)
{
   const float3 linearColor = max(game_gamma_to_linear(gammaColor, true), 0.0);
   const float y = GetLuminance(linearColor, CS_BT709);
   // Fade out near black, where the chroma is noise and the push would only produce invalid (negative luminance) pixels.
   amount *= smoothstep(0.0, 0.02, y);
   float3 bt2020 = BT709_To_BT2020(linearColor);
   // Distance to each gamut's edge along the ray from the grey of the same luminance (the edge is where a channel hits 0).
   const float room709 = y - min3(linearColor);  // chroma left in BT.709 terms: 0 for a grey
   const float room2020 = y - min3(bt2020);      // same, measured in BT.2020 (never larger than room709)
   // How much further the BT.2020 edge is along this ray. Its real maximum is about 1.9 (a colour on the BT.709 edge), but
   // for a nearly neutral colour both rooms are ~0 and can round to 0 or below, which made this ratio explode into a huge
   // chroma push on a white pixel: bound it.
   const float headroom = min(room709 / max(room2020, FLT_MIN), 2.0);
   float scale = pow(max(headroom, 1.0), amount); // 100%: the BT.709 edge of this hue stretches to the BT.2020 one
   // Stop 3% of the luminance short of the BT.2020 edge: landing exactly on it lets the rounding of the later passes (16 bit
   // float, the gamma conversions) tip colours a hair outside, which analyzers read as AP0 or invalid.
   if (room2020 > FLT_MIN)
      scale = max(1.0, min(scale, 0.97 * y / room2020));
   bt2020 = y + (bt2020 - y) * scale;
   float3 expanded = BT2020_To_BT709(bt2020);
   // The pushed colours can raise one channel (e.g. the red of a bright orange) past the display peak, so scale the colour
   // back down (hue kept) until its largest channel fits.
   const float peakRatio = max(PeakWhiteNits / GamePaperWhiteNits, 1.0);
   expanded *= min(1.0, peakRatio / max(max3(expanded), FLT_MIN));
   // Hard guarantee that nothing leaves BT.2020 (the push stops at its edge, but rounding and near-black noise can still
   // leave a channel slightly below 0 in BT.2020, which analyzers read as AP0 or invalid): map such colours onto the
   // nearest point of the BT.2020 edge.
   const float3 expandedBT2020 = BT709_To_BT2020(expanded);
   if (any(expandedBT2020 < 0.0))
      expanded = BT2020_To_BT709(SimpleGamutClip(expandedBT2020, true));
   return linear_to_game_gamma(expanded, true);
}

// The grading sliders, in linear light (1 = paper white). Identity at their defaults.
bool ColorControlsActive()
{
   return LumaSettings.GameSettings.Exposure != 0.0 || LumaSettings.GameSettings.Contrast != 1.0 || LumaSettings.GameSettings.Saturation != 1.0 || LumaSettings.GameSettings.Temperature != 0.0;
}
float3 ApplyColorControls(float3 linearColor)
{
   linearColor *= exp2(LumaSettings.GameSettings.Exposure);
   // A warm tint raises red and lowers blue (and the opposite for a cool one), green stays
   const float temperature = LumaSettings.GameSettings.Temperature * 0.15;
   linearColor *= float3(1.0 + temperature, 1.0, 1.0 - temperature);
   if (LumaSettings.GameSettings.Contrast != 1.0)
   {
      // Around mid grey (18%), in a power curve: brightness ratios against it get stretched or squeezed
      static const float pivot = 0.18;
      linearColor = pivot * pow(max(linearColor, 0.0) / pivot, LumaSettings.GameSettings.Contrast);
   }
   const float y = GetLuminance(linearColor, CS_BT709);
   linearColor = max(lerp(y.xxx, linearColor, LumaSettings.GameSettings.Saturation), 0.0);
   return linearColor;
}

// Darkens the corners (linear light). uv is the 0..1 screen position, aspect is width / height.
float3 ApplyVignette(float3 linearColor, float2 uv, float aspect)
{
   const float strength = saturate(LumaSettings.GameSettings.VignetteStrength);
   if (strength <= 0.0)
      return linearColor;
   // Distance from the centre in half screen heights: 1 at the middle of the top and bottom edges
   const float r = length((uv - 0.5) * float2(aspect, 1.0)) * 2.0;
   const float radius = clamp(LumaSettings.GameSettings.VignetteRadius, 0.3, 1.5);
   const float falloff = smoothstep(radius, radius + 1.2, r);
   return linearColor * (1.0 - 0.85 * strength * falloff);
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
   uint curveWidth, curveHeight;
   CurveTexture.GetDimensions(curveWidth, curveHeight);
   CurveLast = max(curveWidth, 1u) - 1u;

   uint sceneWidth, sceneHeight;
   SceneTexture.GetDimensions(sceneWidth, sceneHeight);
   const float sceneAspect = float(sceneWidth) / max(float(sceneHeight), 1.0);

   float4 scene = ApplyDgvMask(SceneTexture.Sample(SceneSampler_s, v5.xy), DgvMaskT0, DgvFillT0);
   const float chromaticAberration = saturate(LumaSettings.GameSettings.ChromaticAberration);
   if (chromaticAberration > 0.0)
   {
      // Red and blue are sampled at opposite radial offsets (growing with the distance from the centre), green stays: fringes at the edges
      const float2 offset = (v5.xy - 0.5) * (chromaticAberration * 0.02);
      scene.x = ApplyDgvMask(SceneTexture.Sample(SceneSampler_s, v5.xy + offset), DgvMaskT0, DgvFillT0).x;
      scene.z = ApplyDgvMask(SceneTexture.Sample(SceneSampler_s, v5.xy - offset), DgvMaskT0, DgvFillT0).z;
   }
   const float4 sceneRGB1 = float4(scene.xyz, 1.0);

   // One matrix row per output channel, then that channel's curve (point sampled at the row's result, v = 0.5).
   const float3 coords = float3(dot(sceneRGB1, v6), dot(sceneRGB1, v7), dot(sceneRGB1, v8));
   float4 curveR = ApplyDgvMask(CurveTexture.Sample(CurveSampler_s, float2(coords.x, 0.5)), DgvMaskT1, DgvFillT1);
   float4 curveG = ApplyDgvMask(CurveTexture.Sample(CurveSampler_s, float2(coords.y, 0.5)), DgvMaskT1, DgvFillT1);
   float4 curveB = ApplyDgvMask(CurveTexture.Sample(CurveSampler_s, float2(coords.z, 0.5)), DgvMaskT1, DgvFillT1);

   float3 graded = float3(curveR.x, curveG.y, curveB.z);

   const float grainStrength = saturate(LumaSettings.GameSettings.FilmGrainStrength);
   if (LumaSettings.DisplayMode > 0)
   {
#if CURVE_SMOOTHING
      graded = float3(SmoothedCurveValue(coords.x, 0), SmoothedCurveValue(coords.y, 1), SmoothedCurveValue(coords.z, 2));
#endif
      float3 gradedLinear;
      // Ostad: the vanilla look, with the clipped highlights recovered
      {
         gradedLinear = max(RecoverHighlights(coords, graded, saturate(LumaSettings.GameSettings.HighlightExpansion)), 0.0);
         if (ColorControlsActive())
         {
            gradedLinear = ApplyColorControls(gradedLinear);
            // Exposure can push a colour past the display peak: scale it back down (hue kept)
            const float peakRatio = max(PeakWhiteNits / GamePaperWhiteNits, 1.0);
            gradedLinear *= min(1.0, peakRatio / max(max3(gradedLinear), FLT_MIN));
         }
      }
      gradedLinear = ApplyVignette(gradedLinear, v5.xy, sceneAspect);
      gradedLinear = ApplyLumaFilmGrain(gradedLinear, v0.xy, LumaSettings.FrameIndex, grainStrength);
      graded = linear_to_game_gamma(gradedLinear, false);
      graded = ExpandGamut(graded, saturate(LumaSettings.GameSettings.GamutExpansion));
      graded = ApplyShadowLift(graded, LumaSettings.GameSettings.ShadowLift);
   }
   else if (grainStrength > 0.0 || ColorControlsActive() || LumaSettings.GameSettings.VignetteStrength > 0.0)
   {
      float3 gradedLinear = game_gamma_to_linear(max(graded, 0.0), false);
      if (ColorControlsActive())
         gradedLinear = ApplyColorControls(gradedLinear);
      gradedLinear = ApplyVignette(gradedLinear, v5.xy, sceneAspect);
      gradedLinear = ApplyLumaFilmGrain(gradedLinear, v0.xy, LumaSettings.FrameIndex, grainStrength);
      graded = linear_to_game_gamma(saturate(gradedLinear), false);
   }

   // UI_DRAW_TYPE 2: the HUD blends onto this canvas in gamma space and the display composition scales the whole canvas by UI / Game paper
   // white, so the scene is divided by that here to land back at Game Paper White (see "UIPreScale.hlsl"). Done last, after the grain and
   // the grade controls, which must not see it. A no-op in SDR.
   graded = ApplyUIPaperWhitePreScale(graded);

   o0 = float4(graded, 1.0);
}
