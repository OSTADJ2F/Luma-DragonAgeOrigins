// Dragon Age: Origins - the additive sprite shader (ps_5_0, as translated by dgVoodoo): texture (t0) * vertex colour (v2) * material
// colour (cb4[8]), drawn with "SRC_ALPHA, ONE" blending over the lit scene without writing depth. It is the glow of fire, torches,
// sparks and magic. The game made it for an 8 bit target: the colour was never meant to go past 1, so a flame's core sat at ~1.0
// like a white wall, and nothing downstream could tell a light source from a bright surface.
// HDR: the cores of these sprites are lifted here, at the source of the light, with the hue kept: a sprite pixel at full brightness
// is multiplied up to (1 + LightSourceBoost) times, fading out below ~0.4, so dim glow and soft edges stay as they were. The colour
// grade then carries it up to the display peak (see "HighlightRange" there). The alpha is left as the game computed it.
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
   float4 color = ApplyDgvMask(t0.Sample(s0_s, v5.xy), DgvMaskT0, DgvFillT0);
   color *= v2;
   color *= cb4[8];
   const float brightest = max(max(color.x, color.y), color.z);
   o0.w = min(color.w, brightest);
   o0.xyz = color.xyz;
   [branch] if (LumaSettings.DisplayMode > 0)
      o0.xyz *= 1.0 + max(LumaSettings.GameSettings.LightSourceBoost, 0.0) * smoothstep(0.4, 1.0, brightest);
}
