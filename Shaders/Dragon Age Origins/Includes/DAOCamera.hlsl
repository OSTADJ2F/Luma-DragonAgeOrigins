// The game's camera projection, shared by the AO shaders.
// Derived from the view projection in the vertex shader constants (cb4 rows 238 to 241) of a frame capture:
// the z and w columns differ by n/(f-n) * viewZ, and the translation terms give f*n/(f-n) = 0.15, so near = 0.15, far = 4000
// (4000 is also stored in the pixel shader constants). The vertical field of view is 60 degrees (1 / tan = 1.7304).
// Standard (not reversed) D3D depth: d = f / (f - n) * (1 - n / z). View space is in meters.
// To be verified against something of a known size in the game, see the AO radius.

#ifndef LUMA_DAO_CAMERA_HLSL
#define LUMA_DAO_CAMERA_HLSL

#define DAO_NEAR 0.15
#define DAO_FAR 4000.0
#define DAO_TAN_HALF_FOV_Y 0.577350269 // tan(30 degrees)

float DAO_LinearizeDepth(float d)
{
   return (DAO_FAR * DAO_NEAR) / (DAO_FAR - d * (DAO_FAR - DAO_NEAR));
}

#endif // LUMA_DAO_CAMERA_HLSL
