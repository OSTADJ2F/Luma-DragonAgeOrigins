#ifndef SRC_AREA_STRETCH_HLSL
#define SRC_AREA_STRETCH_HLSL

// Render scale: the top-left area of "source" ("area_scale": its share of the surface) stretched over the whole surface, bilinear,
// clamped to the area so nothing outside it bleeds in. "position": the output's SV_Position.
float4 StretchAreaBilinear(Texture2D<float4> source, float2 position, float2 area_scale)
{
   uint2 size;
   source.GetDimensions(size.x, size.y);
   const int2 last = int2(round(area_scale * size)) - 1;
   const float2 texel = position * area_scale - 0.5;
   const int2 base = int2(floor(texel));
   const float2 weight = texel - base;
   const int2 a = clamp(base, 0, last), b = clamp(base + 1, 0, last);
   const float4 top = lerp(source.Load(int3(a.x, a.y, 0)), source.Load(int3(b.x, a.y, 0)), weight.x);
   const float4 bottom = lerp(source.Load(int3(a.x, b.y, 0)), source.Load(int3(b.x, b.y, 0)), weight.x);
   return lerp(top, bottom, weight.y);
}

#endif // SRC_AREA_STRETCH_HLSL
