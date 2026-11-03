// Dragon Age: Origins - the alpha blended effect sprite shader (vfx0_atm, ps_5_0, as translated by dgVoodoo): texture (t0) * vertex colour (v2)
// scaled by the material colour (cb4[10]) and mixed with the fog/ambient colour (cb4[8]) by a fade factor (v7.x / cb4[9].x), drawn with
// "SRC_ALPHA, INV_SRC_ALPHA" blending over the lit scene without writing depth. It is the smoke, glow and magic haze around characters
// and spells. Like the additive sprite shader (Particle_0x5DE6B6F6), it was made for an 8 bit target and its bright cores sit at ~1.0.
// HDR: the bright cores are lifted at the source with the hue kept, with the same curve and "LightSourceBoost" as the additive sprites:
// a pixel at full brightness is multiplied up to (1 + LightSourceBoost) times, fading out below ~0.4, so dim haze and soft edges stay as
// they were. The alpha is left as the game computed it, so the blend weight does not change.
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

#define DgvMaskT0 DgvConstants[44]
#define DgvFillT0 DgvConstants[45]

SamplerState s0_s : register(s0);
Texture2D<float4> t0 : register(t0);

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
   float4 color = ApplyDgvMask(t0.Sample(s0_s, v5.xy), DgvMaskT0, DgvFillT0);
   color *= v2;
   const float3 fog = cb4[8].xyz;
   o0.xyz = fade * (color.xyz * cb4[10].xyz - fog) + fog;
   o0.w = color.w * cb4[10].w;
   [branch] if (LumaSettings.DisplayMode > 0)
   {
      const float brightest = max(max(o0.x, o0.y), o0.z);
      o0.xyz *= 1.0 + max(LumaSettings.GameSettings.LightSourceBoost, 0.0) * smoothstep(0.4, 1.0, brightest);
   }
}
