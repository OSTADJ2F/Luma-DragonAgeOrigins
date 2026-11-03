#ifndef LUMA_PSYCHOV31_HLSL
#define LUMA_PSYCHOV31_HLSL

// PsychoV31, RenoDX's tonemapper (Carlos Lopez and Musa Haji, MIT), with the RenoDX shader files it needs in "RenoDX/".
// They are copied as is from clshortfuse/renodx 811a37a2: "psychov_customtest31.hlsl" is the "customtest31.hlsli" shared
// by its Onimusha, Pragmata, AC Black Flag and 007 First Light mods, minus its include of the game's "common.hlsli".
// Luma's mods call "PsychoVDisplayMap()" below. Include this after the game's "Common.hlsl", as it reads "LumaSettings".

// Luma's "Math.hlsl" defines these as macros, which would break RenoDX's constants of the same names. fxc has no "push_macro",
// so they are defined again below, as in "Math.hlsl" (keep them in sync).
#undef FLT_MIN
#undef FLT_MAX
#undef FLT_EPSILON
#undef FLT10_MAX
#undef FLT11_MAX
#undef FLT16_MAX
#undef PI

#include "RenoDX/color/macleod_boynton.hlsl"
#include "RenoDX/tonemap/psychov/test30.hlsl"

namespace renodx
{
namespace tonemap
{
// From RenoDX's "tonemap/neutwo.hlsl" (Carlos Lopez, MIT): the only part of it PsychoV31 uses, the rest needs all of RenoDX's
// color library
float Neutwo(float x, float peak)
{
   // also written as x * rhypot(x, peak)
   float p = peak;

   float numerator = p * x;
   float denominator_squared = mad(x, x, p * p);
   return numerator * rsqrt(denominator_squared);
}
} // namespace tonemap
} // namespace renodx

// fxc, unlike RenoDX's dxc: X3577 drops "isnan()" checks it proves redundant (they are, there), and X4000 flags the early
// returns of "custom_psycho31_ShoulderMeanA2Weight()" as an uninitialized result. Both come at code generation, so they stay
// off for the whole shader, as in "Common.hlsl".
#pragma warning(disable : 3577)
#pragma warning(disable : 4000)
#include "RenoDX/psychov_customtest31.hlsl"

#define FLT_MIN     asfloat(0x00800000)
#define FLT_MAX     asfloat(0x7F7FFFFF)
#define FLT_EPSILON 1.192092896e-07
#define FLT10_MAX   64512.f
#define FLT11_MAX   65024.f
#define FLT16_MAX   65504.f
#define PI          3.141592653589793238462643383279502884197

// The display peak relative to paper white. It can't go below paper white, and a 0 paper white (unset) would divide the frame into
// NaNs.
float PsychoVPeakRatio()
{
   const float paperWhite = max(GamePaperWhiteNits, 1.0);
   return max(PeakWhiteNits, paperWhite) / paperWhite;
}

// Maps a finished BT.709 grade (linear, 1 = paper white) to the display peak, with the gamut compression of RenoDX's mods
// (BT.2020 in HDR, BT.709 otherwise), which keeps the channels within the peak and slightly desaturates the most saturated colors.
// PsychoV's own grading only takes the user's contrast (its cone response power), saturation (its purity) and highlights
// desaturation (its "dechroma"), all no-ops at their defaults.
// HDR displays only: at an SDR peak (= paper white) it already compresses paper white (1 -> 0.68), so SDR displays take the
// game's vanilla grade instead (Mass Effect 2 doesn't gate it yet).
float3 PsychoVDisplayMap(float3 color, float contrast = 1.0, float saturation = 1.0, float highlightsDesaturation = 0.0)
{
   const int targetGamut = (LumaSettings.DisplayMode == 1) ? renodx::tonemap::psychov::CUSTOM_PSYCHO31_TARGET_GAMUT_BT2020
                                                           : renodx::tonemap::psychov::CUSTOM_PSYCHO31_TARGET_GAMUT_BT709;
   return renodx::tonemap::psychov::custom_psychotm_test31(color,
                                                           PsychoVPeakRatio(),                                        // peak_value
                                                           1.0,                                                       // exposure
                                                           1.0,                                                       // highlights
                                                           1.0,                                                       // shadows
                                                           contrast,                                                  // contrast
                                                           0.0,                                                       // flare
                                                           1.0,                                                       // highlight_contrast
                                                           1.0,                                                       // shadow_contrast
                                                           saturation,                                                // purity_scale
                                                           1.0,                                                       // highlight_saturation
                                                           highlightsDesaturation,                                    // dechroma
                                                           0.18,                                                      // current_adaptive_state_bt709
                                                           0.18,                                                      // current_background_state_bt709
                                                           0.0,                                                       // sdr_eotf_emulation
                                                           1.0,                                                       // gamut_compression
                                                           targetGamut,                                               // gamut_compression_mode
                                                           1.5,                                                       // compression
                                                           1.0,                                                       // mean_a2_shadow_source_weight
                                                           0.5,                                                       // mean_a2_midgray_source_weight
                                                           0.0,                                                       // mean_a2_highlight_source_weight
                                                           renodx::tonemap::psychov::PSYCHO30_SOURCE_BOUNDARY_BT709); // source_boundary
}

#endif // LUMA_PSYCHOV31_HLSL
