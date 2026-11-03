// Dragon Age: Origins - the procedural sky (sky, ps_5_0, as translated by dgVoodoo): an atmospheric scattering model (Rayleigh/Mie style
// terms from the sky parameters in cb4[8..12], the sun direction in cb4[15], the fog/horizon colours in cb4[16..17]) plus the celestial body
// (sun or moon billboard, t0) added on top, ending in a saturate (`mad_sat`), so the sun disc and the glow around it were clamped to 1.0.
// This is a port of the DX9 shader (dx9asm/sky), kept instruction for instruction, apart from the output:
// - the saturate is gone, so the glow around the sun can pass 1 (negative values are still clamped);
// - HDR: the celestial body is lifted like the other light sources, with the hue kept: a pixel at full brightness is multiplied up to
//   (1 + LightSourceBoost) times, fading out below ~0.4.
// o1 is the second render target the game writes a constant to (kept as is).
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

// DX9 constant registers, as they land in b4 (cN -> cb4[N + 8])
#define SkyParams0 cb4[8]
#define SkyParams1 cb4[9]
#define SkyParams2 cb4[10]
#define SkyParams3 cb4[11]
#define SkyParams4 cb4[12]
#define SunDirection cb4[15]
#define FogParams0 cb4[16]
#define FogParams1 cb4[17]
#define OpacityMultiplier cb4[18]

SamplerState s0_s : register(s0);
Texture2D<float4> t0 : register(t0);

void main(
    float4 v0 : SV_POSITION0,
    float4 v1 : TEXCOORD8,
    float4 v2 : COLOR0,
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
    out float4 o0 : SV_TARGET0,
    out float4 o1 : SV_TARGET1)
{
   // Angle around the vertical axis (an arctangent approximation), only its cosine is used
   const float invXY = DgVoodooRcp(dot(v6.xy, v6.xy));
   const float zz = v6.z * v6.z;
   const float tan2 = invXY * zz;
   const float belowZero = 1.0 - zz * invXY;
   const float t = saturate(tan2) * DgVoodooRcp(max(tan2, 1.0));
   const float t2 = t * t;
   float poly = t2 * 0.0208350997 - 0.0851330012;
   poly = t2 * poly + 0.180141002;
   poly = t2 * poly - 0.330299497;
   poly = t2 * poly + 0.999866009;
   const float atanT = t * poly;
   const float atanFlip = atanT * -2.0 + 1.57079637;
   float angle = atanFlip * ((belowZero >= 0.0) ? 0.0 : 1.0) + atanT;
   angle = frac(angle * 0.318309873 + 0.5) * 6.28318548 - 3.14159274;
   const float cosAngle = cos(angle);

   // Angle to the sun (an arccosine approximation) and the scattering along it
   const float3 dir = normalize(v6.xyz);
   const float mu = dot(-SunDirection.xyz, dir);
   const float absMu = abs(mu);
   float acosPoly = absMu * -0.0187292993 + 0.0742610022;
   acosPoly = acosPoly * absMu - 0.212114394;
   acosPoly = acosPoly * absMu + 1.57072878;
   acosPoly *= sqrt(max(1.0 - absMu, 0.0));
   const float theta = (acosPoly * -2.0 + 3.14159274) * ((mu >= 0.0) ? 0.0 : 1.0) + acosPoly;
   const float theta2 = theta * theta;
   float sunGlow = 1.0 - theta2 * 25.0 * DgVoodooRcp(theta2 * 25.0 + 1.0);
   sunGlow = (sunGlow + cosAngle * 0.0900000036 + 0.389999986) * 170000.0;
   sunGlow *= sunGlow;
   const float3 extinction = 1.0 - exp2(-sunGlow * SkyParams1.xyz * SkyParams1.xyz * 1.44269502);

   const float muSat = saturate(mu);
   const float phaseDenominator = pow(abs(SkyParams0.x - SkyParams0.y * muSat), 1.5);
   float3 scatter = DgVoodooRcp(phaseDenominator) * SkyParams2.xyz;
   scatter = SkyParams3.xyz * (muSat * muSat * 0.5 + 2.0) + scatter;
   scatter *= float3(DgVoodooRcp(SkyParams1.x), DgVoodooRcp(SkyParams1.y), DgVoodooRcp(SkyParams1.z));
   scatter *= float3(SkyParams1.w, SkyParams2.w, SkyParams3.w);

   // Distance fog towards the horizon, by the height of the view direction
   const float3 fogged = FogParams1.x * (scatter * extinction - FogParams0.xyz) + FogParams0.xyz - scatter * extinction;
   const float3 inscatter = scatter * extinction;
   const float heightPow3 = dir.z * dir.z * dir.z;
   const float fogAmount = saturate(exp2(-heightPow3 * DgVoodooRcp(FogParams1.z * 0.0636619925) * 1.44269502));
   const float3 fogBlended = fogAmount * fogged + inscatter;
   float3 color = ((FogParams1.z * -0.636619925 + 0.00100000005) >= 0.0) ? inscatter : fogBlended;

   // The celestial body (sun or moon), projected from v7
   const float2 bodyUV = v7.xy * DgVoodooRcp(v7.w);
   float4 body = ApplyDgvMask(t0.Sample(s0_s, bodyUV), DgvMaskT0, DgvFillT0);
   body.xyz *= body.w;
   float3 bodyColor = body.xyz * SkyParams4.x;
   [branch] if (LumaSettings.DisplayMode > 0)
   {
      const float brightest = max(max(bodyColor.x, bodyColor.y), bodyColor.z);
      bodyColor *= 1.0 + max(LumaSettings.GameSettings.LightSourceBoost, 0.0) * smoothstep(0.4, 1.0, brightest);
      o0.xyz = max(bodyColor + color, 0.0);
   }
   else
   {
      o0.xyz = saturate(bodyColor + color);
   }
   o0.w = OpacityMultiplier.x;
   o1 = float4(0.344639987, 0.99609375, 0.996084154, 1.0);
}
