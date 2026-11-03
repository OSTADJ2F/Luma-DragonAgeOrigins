// Luma's pyramid bloom for Dragon Age: Origins, in place of the game's (a bright pass and two 7 tap blurs at half resolution, composited at
// full resolution before the grade). The scene it reads is the gamma encoded one the game's own bloom read, so the threshold is a gamma
// space brightness; "DrawBloom" blurs first and calls this on the sum, the composite pass of the game then adds the result.

// DrawBloom binds only b11; main.cpp binds live LumaSettings at b13 before calling it, for the threshold and the intensity.
#include "Includes/Common.hlsl"

// The glow's colour: saturation around its luminance, then a warm / cool tint (like the grade's temperature)
float3 DAO_BloomColor(float3 color)
{
   color = max(lerp(GetLuminance(color).xxx, color, LumaSettings.GameSettings.BloomSaturation), 0.0);
   const float warmth = LumaSettings.GameSettings.BloomWarmth * 0.15;
   return color * float3(1.0 + warmth, 1.0, 1.0 - warmth);
}

float3 DAO_BloomThreshold(float3 color)
{
   // Negative and non finite values blur into a whole kernel
   color = (isfinite(color.r + color.g + color.b)) ? max(color, 0.0) : 0.0;


   // A quadratic soft knee on the brightest channel, then linear above the threshold (the shared one, with the settings' threshold)
   const float threshold = max(LumaSettings.GameSettings.BloomThreshold, 1e-3);
   const float knee = threshold * 0.5;
   const float brightness = max(max(color.r, color.g), max(color.b, 1e-6));
   const float soft = clamp(brightness - (threshold - knee), 0.0, knee * 2.0);
   const float contribution = max((0.25 / knee) * soft * soft, brightness - threshold);
   color = DAO_BloomColor(color * (contribution / brightness));
   return color * LumaSettings.GameSettings.BloomIntensity;
}

#define LUMA_BLOOM_THRESHOLD_FUNCTION(color) DAO_BloomThreshold(color)
#define LUMA_BLOOM_SCALE 1.0

#include "../Includes/Bloom.hlsl"
