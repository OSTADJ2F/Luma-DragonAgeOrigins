// Turns the game's D24 scene depth (single sample, or sample 0 of the MSAA one) into a full resolution R32F linear view space
// depth in meters, which is what the XeGTAO depth prefilter reads (see "Luma_DAO_XeGTAO.hlsl").
// Pixels with no geometry (depth 1, or 0 if the depth was cleared) are sent to the far plane, where the AO is negligible.

#include "Includes/DAOCamera.hlsl"

#ifndef DAO_DEPTH_MS
#define DAO_DEPTH_MS 0
#endif

#if DAO_DEPTH_MS
Texture2DMS<float> DepthTex : register(t0); // R24_UNORM_X8_TYPELESS view of the depth
#else
Texture2D<float> DepthTex : register(t0);
#endif

RWTexture2D<float> LinearDepth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
   uint width, height;
#if DAO_DEPTH_MS
   uint samples;
   DepthTex.GetDimensions(width, height, samples);
#else
   DepthTex.GetDimensions(width, height);
#endif
   if (tid.x >= width || tid.y >= height)
      return;

#if DAO_DEPTH_MS
   const float depth = DepthTex.Load(int2(tid.xy), 0);
#else
   const float depth = DepthTex.Load(int3(tid.xy, 0));
#endif

   LinearDepth[tid.xy] = (depth <= 0.0 || depth >= 1.0) ? DAO_FAR : min(DAO_LinearizeDepth(depth), DAO_FAR);
}
