// Multiplies the XeGTAO visibility into the scene colour (the blend state does the multiplication: dest * source).
// The scene colour is the game's lighting before the tonemap, so the AO darkens everything that was lit, which is the only
// choice with no separate ambient term. "Strength" blends it in: 0 = no AO, 1 = the full XeGTAO visibility.

Texture2D<float> AOTex : register(t0);

cbuffer LumaAOApply : register(b9)
{
   float Strength;
   float3 Padding;
}

float4 main(float4 pos : SV_Position) : SV_Target0
{
   const float visibility = AOTex.Load(int3(int2(pos.xy), 0));
   const float f = lerp(1.0, saturate(visibility), saturate(Strength));
   return float4(f, f, f, 1.0); // Alpha is kept by the blend state
}
