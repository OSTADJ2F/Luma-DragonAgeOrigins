#include "../Includes/Common.hlsl"

Texture2D<float4> sourceTexture : register(t0);

// Custom Luma shader to encode the fp16 scRGB proxy (linear BT.709, 1 = sRGB_WhiteLevelNits) into the real R10G10B10A2 swapchain.
// "LumaData.CustomData1": 0 = HDR10 (BT.2020 PQ), 1 = SDR (sRGB gamma, clipped like DWM does for scRGB).
float4 main(float4 pos : SV_Position) : SV_Target0
{
   float4 color = sourceTexture.Load((int3)pos.xyz);

   if (LumaData.CustomData1 == 0)
   {
      color.rgb = Linear_to_PQ(max(BT709_To_BT2020(color.rgb), 0.0) * (sRGB_WhiteLevelNits / HDR10_MaxWhiteNits), GCT_NONE);
   }
   else
   {
      color.rgb = linear_to_sRGB_gamma(saturate(color.rgb), GCT_NONE);
   }
   return color;
}
