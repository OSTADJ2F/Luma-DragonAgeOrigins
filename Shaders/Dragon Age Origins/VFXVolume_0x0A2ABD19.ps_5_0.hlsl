// Dragon Age: Origins - the alpha blended effect sprite shader with a volume texture (vfx0_volumetexture, ps_5_0, as translated by dgVoodoo):
// the same as VFXAlpha_0x41DE4D45, except the colour comes from a 3D texture (t32, sampled at v5.xyz) and the result is alpha tested
// against cb3[8].z (the 8 bit alpha reference, in 1/255 steps). Smoke and haze volumes around characters and spells.
// HDR: the bright cores are lifted at the source with the hue kept, with the same curve and "LightSourceBoost" as the other effect sprites.
// The alpha and the alpha test are left as the game computed them (the test runs on the unboosted alpha, which the boost does not touch).
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

#define DgvMaskT32 DgvConstants[44]
#define DgvFillT32 DgvConstants[45]
#define DgvAlphaRef DgvConstants[8].z

SamplerState s0_s : register(s0);
Texture3D<float4> t32 : register(t32);

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD8,
    centroid float4 v2 : COLOR0,
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
   const float fade = max(v7.x, cb4[9].x);
   float4 color = ApplyDgvMask(t32.Sample(s0_s, v5.xyz), DgvMaskT32, DgvFillT32);
   color *= v2;
   const float3 fog = cb4[8].xyz;
   o0.xyz = fade * (color.xyz * cb4[10].xyz - fog) + fog;
   o0.w = color.w * cb4[10].w;

   // Alpha test, as in the original: discard when the 8 bit alpha is not above the reference
   if (asuint(DgvAlphaRef) >= uint(o0.w * 255.0 + 0.0001))
      discard;

   [branch] if (LumaSettings.DisplayMode > 0)
   {
      const float brightest = max(max(o0.x, o0.y), o0.z);
      o0.xyz *= 1.0 + max(LumaSettings.GameSettings.LightSourceBoost, 0.0) * smoothstep(0.4, 1.0, brightest);
   }
}
