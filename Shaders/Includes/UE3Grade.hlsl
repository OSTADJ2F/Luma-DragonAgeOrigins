#ifndef SRC_UE3_GRADE_HLSL
#define SRC_UE3_GRADE_HLSL

// Unreal Engine 3's UberPostProcessBlend grade, as the D3D9 originals compute it (transcribed from their translated
// disassembly in Mass Effect, Mass Effect 2 and Medal of Honor Airborne). The material rows are passed in: each game
// names them from its own register map.

// The grade's pow() as the original computes it. The tiny floor replaces the compiler's own log2(0) guard.
float3 PowUE3(float3 base, float3 exponent)
{
   return exp2(exponent * log2(max(abs(base), 1e-30)));
}

float PowUE3(float base, float exponent)
{
   return exp2(exponent * log2(max(abs(base), 1e-30)));
}

// Shadows -> highlights scale -> midtones -> desaturation + overlay -> output scale -> inverse display gamma, gamma in and
// out. `clampSDR` false = the extended grade: max(0) instead of each saturate, keeping the real channel ratio.
// `shadowsAndDesat` .xyz = shadows, .w = desaturation; `outputScale` is normally GammaColorScaleAndInverse.xyz and
// `inverseGamma` its .w.
float3 UE3Grade(float3 scene, bool clampSDR, float3 outputScale, float4 shadowsAndDesat, float3 inverseHighLights, float3 midTones, float3 lumaWeights, float3 overlayColor, float inverseGamma)
{
   // Head: shadows -> scale -> midtones.
   float3 c = scene - shadowsAndDesat.xyz;
   c = clampSDR ? saturate(c) : max(0.0, c); // mad_sat in the original
   c = c * inverseHighLights;
   c = PowUE3(c, midTones);

   // Tail: desat + tint + output scale + display encode.
   const float desat = dot(c, lumaWeights);
   c = c * shadowsAndDesat.www + overlayColor;
   c = c + desat;
   c = c * outputScale;
   c = clampSDR ? saturate(c) : max(0.0, c); // mul_sat in the original
   return PowUE3(c, inverseGamma.xxx);
}

#endif // SRC_UE3_GRADE_HLSL
