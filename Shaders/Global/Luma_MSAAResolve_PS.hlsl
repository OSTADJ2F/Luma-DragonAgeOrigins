// HDR-weighted MSAA resolve drawn by Core's "DrawMSAAResolve()" in place of a game's ResolveSubresource of an fp16 MSAA target.
//
// The hardware resolve is a plain average of samples whose values go above 1 before the tonemap or display map: a sample at 16 next
// to one at 0 averages to 8, which still maps to white, so bright silhouettes look un-antialiased. Each sample is weighted by
// 1 / (1 + max3) of its exposed value (AMD's reversible-tonemapper resolve: max3 rather than a weighted sum keeps the hue of edges
// between two colours), which makes bright edges erode instead of expand. A zero exposure (an unbound one reads 0) makes the weight
// 1, a box resolve.
//
// MSAA_RESOLVE_EXPOSURE, what the weight sees:
// 0: the samples as they are (the game made its scene for values around 1, e.g. Bayonetta's gamma encoded one)
// 1: times the x of the 1x1 texture at t1, times MSAA_RESOLVE_EXPOSURE_SCALE (Mirror's Edge's TdToneMapping exposure / 64)
// 2: times the rgb of the second constant of the cbuffer at b0 (Saints Row's copy of its composite's vc4, whose Tint_color is the
//    eye adaptation)
// MSAA_RESOLVE_ALPHA_MIN 1: alpha is the smallest sample's rather than the average. For a depth in alpha (Mirror's Edge's linear view
// depth) the average makes depths that exist on neither side of a silhouette, the nearest sample is a real one.
// MSAA_RESOLVE_FILTER_RADIUS (pixels, a float; undefined = the per pixel box): the colour is filtered over the samples of the neighbouring pixels too, each weighted by a
// Blackman-Harris window of its distance (pixels, through the sample positions) from the pixel's centre, up to this radius
// (MJP's custom resolve). Smoother edges than the per pixel box, slightly softer textures. Alpha stays the pixel's own.

#include "../Includes/Math.hlsl"

#ifndef MSAA_RESOLVE_EXPOSURE
#define MSAA_RESOLVE_EXPOSURE 0
#endif
#ifndef MSAA_RESOLVE_EXPOSURE_SCALE
#define MSAA_RESOLVE_EXPOSURE_SCALE 1.0
#endif
#ifndef MSAA_RESOLVE_ALPHA_MIN
#define MSAA_RESOLVE_ALPHA_MIN 0
#endif

Texture2DMS<float4> sceneMS : register(t0);
#if MSAA_RESOLVE_EXPOSURE == 1
Texture2D<float4> exposureTexture : register(t1);
#elif MSAA_RESOLVE_EXPOSURE == 2
cbuffer MSAAResolveExposure : register(b0)
{
   float4 exposureRGB : packoffset(c1);
}
#endif

float4 main(float4 pos : SV_Position) : SV_Target0
{
   uint width, height, sampleCount;
   sceneMS.GetDimensions(width, height, sampleCount);
#if MSAA_RESOLVE_EXPOSURE == 1
   const float exposure = exposureTexture.Load(int3(0, 0, 0)).x * MSAA_RESOLVE_EXPOSURE_SCALE;
#elif MSAA_RESOLVE_EXPOSURE == 0
   const float exposure = 1.0;
#endif

   float3 weightedColor = 0.0;
   float weightSum = 0.0;
   float alpha = MSAA_RESOLVE_ALPHA_MIN ? FLT_MAX : 0.0;
   uint finiteCount = 0;
   for (uint i = 0; i < sampleCount; i++)
   {
      const float4 color = sceneMS.Load(int2(pos.xy), i);
      // An all-ones exponent is NaN or Inf: one of either would poison the whole pixel, so the sample is dropped
      if (any((asuint(color) & 0x7F800000) == 0x7F800000))
         continue;
#if MSAA_RESOLVE_EXPOSURE == 2
      const float weight = rcp(1.0 + max3(max(color.rgb, 0.0) * exposureRGB.rgb));
#else
      const float weight = rcp(1.0 + max3(max(color.rgb, 0.0)) * exposure);
#endif
      weightedColor += color.rgb * weight;
      weightSum += weight;
      alpha = MSAA_RESOLVE_ALPHA_MIN ? min(alpha, color.a) : (alpha + color.a);
      finiteCount++;
   }
   if (finiteCount == 0)
      return 0.0;
#ifdef MSAA_RESOLVE_FILTER_RADIUS
   // Neighbours whose samples (within half a pixel of their centre) can fall inside the radius
   static const int reach = (int)ceil(MSAA_RESOLVE_FILTER_RADIUS + 0.5) - 1;
   weightedColor = 0.0;
   weightSum = 0.0;
   for (int y = -reach; y <= reach; y++)
   {
      for (int x = -reach; x <= reach; x++)
      {
         const int2 pixel = int2(pos.xy) + int2(x, y);
         if (any(pixel < 0) || any(pixel >= int2(width, height)))
            continue;
         for (uint i = 0; i < sampleCount; i++)
         {
            const float distance = length(float2(x, y) + sceneMS.GetSamplePosition(i)) / MSAA_RESOLVE_FILTER_RADIUS;
            if (distance >= 1.0)
               continue;
            const float4 color = sceneMS.Load(pixel, i);
            if (any((asuint(color.rgb) & 0x7F800000) == 0x7F800000))
               continue;
            // Blackman-Harris, centred: 1 at the pixel's centre, 0 at the radius
            const float window = 0.35875 + 0.48829 * cos(PI * distance) + 0.14128 * cos(2.0 * PI * distance) + 0.01168 * cos(3.0 * PI * distance);
#if MSAA_RESOLVE_EXPOSURE == 2
            const float weight = window * rcp(1.0 + max3(max(color.rgb, 0.0) * exposureRGB.rgb));
#else
            const float weight = window * rcp(1.0 + max3(max(color.rgb, 0.0)) * exposure);
#endif
            weightedColor += color.rgb * weight;
            weightSum += weight;
         }
      }
   }
#endif
   return float4(weightedColor / weightSum, MSAA_RESOLVE_ALPHA_MIN ? alpha : (alpha / finiteCount));
}
