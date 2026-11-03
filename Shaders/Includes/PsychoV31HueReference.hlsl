#ifndef SRC_PSYCHOV31_HUE_REFERENCE_HLSL
#define SRC_PSYCHOV31_HUE_REFERENCE_HLSL

// Kept apart from "PsychoV31.hlsl" so its other consumers don't pull in the MacLeod-Boynton model (its static initializers alone
// perturb fxc's code generation). Include this after the game's "Common.hlsl".
#include "MacLeodBoynton.hlsl"
#include "PsychoV31.hlsl"
#include "Reinhard.hlsl"

// "PsychoVDisplayMap()", then the hue of "hueReferenceBT2020" by "hueEmulation" (0..1). PsychoV31 compresses per cone and leans
// saturated highlights by itself, so the reference replaces its hue after the map instead of adding a lean before it, as RenoDX's
// test17 "hue_emulation" does: MacLeod-Boynton puts the reference's hue direction on the mapped color's purity and T = L + M
// anchor. Where the BT.2020 boundary is farther along the new hue (deep blue), that same purity can put a channel past the peak,
// so the color is desaturated toward its relative luminance there, in BT.2020 channels as PsychoV31 targets them in HDR.
// "color" is linear BT.709, 1 = paper white.
float3 PsychoVDisplayMapWithHueEmulation(float3 color, float3 hueReferenceBT2020, float contrast, float saturation, float highlightsDesaturation,
                                         float hueEmulation = 1.0)
{
   const float3 mapped = PsychoVDisplayMap(color, contrast, saturation, highlightsDesaturation);
   [branch] if (hueEmulation <= 0.0)
   {
      return mapped;
   }
   const float3 hueEmulatedBT2020 = MacLeodBoynton::HueOnlyBT2020(BT709_To_BT2020(mapped), hueReferenceBT2020);
   const float3 containedBT2020 = CorrectOutOfRangeColor(hueEmulatedBT2020, false, true, 1.0, PsychoVPeakRatio(), 0.0, CS_BT2020);
   return lerp(mapped, BT2020_To_BT709(containedBT2020), hueEmulation);
}

// The hue emulation for grades whose vanilla clipped per channel (RenoDX's BL1 port): the reference is a per channel
// ReinhardPiecewise(x, 5, 1.5) of the color in BT.2020, linear below 1.5 and rolling toward 5 above, so it bends a saturated
// highlight's hue the way the clip did, without its whitening (and never reaches an achromatic white, which has no
// MacLeod-Boynton direction). Colors with no channel above 1.5 skip the MacLeod-Boynton solve (some 300 instructions): there the
// reference is the color itself (BT.2020 channels never exceed the BT.709 max) and the emulation would only undo PsychoV31's own
// sub-degree hue drift.
float3 PsychoVDisplayMapWithHueReference(float3 color, float contrast, float saturation, float highlightsDesaturation, float hueEmulation = 1.0)
{
   static const float hueReferenceShoulder = 1.5;
   [branch] if (max3(color) <= hueReferenceShoulder)
   {
      return PsychoVDisplayMap(color, contrast, saturation, highlightsDesaturation);
   }
   return PsychoVDisplayMapWithHueEmulation(color, Reinhard::ReinhardPiecewise(BT709_To_BT2020(color), 5.0, hueReferenceShoulder),
                                            contrast, saturation, highlightsDesaturation, hueEmulation);
}

#endif // SRC_PSYCHOV31_HUE_REFERENCE_HLSL
