#ifndef LUMA_DAO_FILM_GRAIN_HLSL
#define LUMA_DAO_FILM_GRAIN_HLSL

// Luma film grain: luminance dependent (film density), applied on linear light where 1 = paper white.
// Bartleson's graininess model (the same one RenoDX's film grain uses): the denser (brighter) the film, the more grain.
// Grain is multiplicative, so black stays black and an HDR highlight keeps its hue.

float FilmGrainHash(float2 p)
{
   float3 p3 = frac(float3(p.xyx) * 0.1031);
   p3 += dot(p3, p3.yzx + 33.33);
   return frac((p3.x + p3.y) * p3.z);
}

float FilmGrainGraininess(float density)
{
   if (density <= 0.0)
      return 0.0;
   const float bof_d_over_c = 0.880 - (0.736 * density) - (0.003 * pow(density, 7.6));
   return pow(10.0, bof_d_over_c);
}

// `strength` is the user's 0-1 setting, `frame` the frame counter (a new pattern every frame)
float3 ApplyLumaFilmGrain(float3 linearColor, float2 pixelPos, uint frame, float strength)
{
   if (strength <= 0.0)
      return linearColor;
   const float2 seed = float2(float(frame % 4096u) * 1.6180339, float(frame % 2048u) * 2.7182818);
   const float random_factor = FilmGrainHash(pixelPos + seed) * 2.0 - 1.0;
   const float3 signs = sign(linearColor);
   const float3 magnitude = abs(linearColor);
   const float luminance = dot(magnitude, float3(0.2126, 0.7152, 0.0722));
   const float graininess = FilmGrainGraininess(min(luminance, 4.0) * 3.0); // Film density 0-3 for 0-1, capped so HDR highlights don't flicker wildly
   const float boost = 1.667;
   return signs * magnitude * (1.0 + random_factor * graininess * strength * 0.03 * boost);
}

#endif // LUMA_DAO_FILM_GRAIN_HLSL
