// The upscalers' depth and the camera motion of the texels no patched mesh draw wrote (Core's "SceneMotionVectors": the sky and
// background layers, particles), from the scene's hardware depth (D24) and the frame camera's reprojection (this frame's
// view projection against last frame's, built on the CPU). With "reactive_enabled", FSR's and Luma TAA's masks from what the alpha
// blended draws wrote, as Bayonetta's and Metal Gear Rising's.

#include "../Includes/MotionVectorFill.hlsl"
#include "Includes/GameCBuffers.hlsl"

cbuffer MotionVectorFill : register(b0)
{
   CB::MotionVectorFillConstants fill;
};

Texture2D<float> depth : register(t0);       // R24_UNORM_X8_TYPELESS view of the scene depth
Texture2D<float2> mask_input : register(t1); // x reactive, y transparency & composition (both max blended)
RWTexture2D<float2> motion_vectors : register(u0);
RWTexture2D<float> device_depth : register(u1);
RWTexture2D<float> reactive : register(u2);
RWTexture2D<float> transparency : register(u3);
RWTexture2D<float> previous_additive : register(u4); // The additive draws' mask of the previous frame, replaced here

[numthreads(8, 8, 1)] void main(uint3 id : SV_DispatchThreadID) {
   uint2 size;
   motion_vectors.GetDimensions(size.x, size.y);
   if (any(id.xy >= size))
      return;
   if (fill.reactive_enabled != 0.0)
   {
      // The additive draws write y = 0, so x > y is theirs (sparks, glows, fire), scaled and capped at 0.9 without a threshold, else they
      // smear, and kept from the previous frame (a flickering one that went out is still lit in the upscaler's history); the alpha
      // blended ones (x = y = a: smoke, haze) only over the threshold, as a continuous mask over them shakes the whole sprite
      const float2 mask = mask_input.Load(int3(id.xy, 0));
      const float current_additive = (mask.x > mask.y ? mask.x : 0.0);
      const float additive = min(max(current_additive, previous_additive[id.xy]) * fill.reactive_scale, 0.9);
      previous_additive[id.xy] = current_additive;
      const float alpha_blended = (mask.y < fill.reactive_threshold ? 0.0 : 0.9);
      reactive[id.xy] = max(additive, alpha_blended);
      transparency[id.xy] = mask.y;
   }
   // saturate() also drops a NaN
   const float z = saturate(depth.Load(int3(id.xy, 0)));
   device_depth[id.xy] = z;
   if (motion_vectors[id.xy].x < MOTION_VECTOR_MARKER)
      return;
   WriteCameraMotion(motion_vectors, id.xy, size, fill.reprojection, fill.jitter_ndc, z);
}
