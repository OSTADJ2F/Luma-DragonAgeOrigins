#ifndef LUMA_GAME_CB_STRUCTS
#define LUMA_GAME_CB_STRUCTS

#ifdef __cplusplus
// This include is needed to allow reading shader types from c++.
#include "../../../Source/Core/includes/shader_types.h" // __cplusplus only: the shader compiler never sees this include
#endif

// Mirrors c++ name spaces.
namespace CB
{
	// Define the game specific cbuffer settings here. They don't need 4 bytes alignment.
	struct LumaGameSettings
	{
		float HighlightExpansion; // 0-1: how far the clipped highlights are pushed towards the display peak
		float GamutExpansion;     // 0-1: how far the colours are stretched from BT.709 towards BT.2020
		float VideoAutoHDREnable; // Video_0x39642745: 0/1. 1 = light PumboAutoHDR on the Bink movie pass (HDR only); 0 = flat SDR at paper white.
		float VideoAutoHDRBoost;  // 0..1. Highlight expansion strength; peak = lerp(sRGB white, 250 nits, boost). 0 = off.
		float BloomType;          // 0 = off (the game's bloom passes are skipped), 1 = the game's bloom, 2 = Luma's pyramid bloom in place of it (3549D37F bright pass, 6184D504 blurs, 769096CC composite)
		float BloomIntensity;     // Scales the bloom before the game's own composite (1 = as extracted)
		float BloomThreshold;     // The scene's brightness (gamma encoded, 1 = SDR white) where the bloom starts, with a soft knee
		float FilmGrainStrength;  // 0..1. Luma's film grain in the color grade (the game has none of its own). 0 = off
		float Exposure;           // Stops (EV), 0 = unchanged
		float Contrast;           // Around mid grey, 1 = unchanged
		float Saturation;         // 1 = unchanged, 0 = greyscale
		float Temperature;        // -1 (cooler) .. 1 (warmer), 0 = unchanged
		float BokehDoFEnable;     // 0/1. The cutscene depth of field (DoFComposite_0xB507EFCE) gathers a bokeh disc from the sharp scene instead of the game's blur
		float HighlightDesaturation; // 0..1: how far the recovered highlights roll off towards white as they near the peak (0 = hue kept all the way up)
		float BloomSpread;           // Scales the width of the bloom's blurs (1 = the original), 0.5 tight .. 2 wide
		float BloomSaturation;       // 1 = the scene's colours, 0 = a white glow
		float BloomWarmth;           // -1 (cooler) .. 1 (warmer) tint of the glow, 0 = none
		// Tuning constants (dev sliders: the defaults are the values the shaders were written with)
		float ShadowLiftRange;       // The encoded brightness where the shadow lift has faded out (default 0.3)
		float HighlightDesatStart;   // Where the highlight desaturation starts, in paper whites (default 1)
		float HighlightDesatEnd;     // Where it is complete, as a fraction of the display peak in paper whites (default 1)
		float BokehTaps;             // Samples of the bokeh gather (default 96)
		float BokehHighlightBoost;   // How much brighter taps weigh more, for disc shaped highlights (default 4)
		float HighlightWhite;        // The recovered brightness (in paper whites) that lands on the display peak (default 11: the p99 of a fire and sunbeam scene)
		float HighlightPunch;        // 0..1: lifts the middle of the highlight shoulder towards the peak (0 = plain extended Reinhard)
		float LightSourceBoost;      // 0..: how much brighter the cores of the additive light sprites (fire, torches, sparks, magic) are made (0 = as the game drew them)
		float ShadowLift;            // 0..1: how far the darks go back from the display's gamma 2.2 decode towards the game's own (sRGB) one, against black crush (0 = off)
		float HighlightColor;        // 0..1: how much of their own hue and saturation the recovered highlights get back where the game's curve clipped them to white (0 = white, as before)
		float VignetteStrength;      // 0..1: how far the corners are darkened in the color grade (0 = off)
		float VignetteRadius;        // 0.3..1.5: the distance from the centre (in half screen heights) where the darkening starts
		float ChromaticAberration;   // 0..1: red and blue fringes that grow towards the screen edges, in the color grade (0 = off)
	};
	
	// The motion vector fill's constants (Luma_DAO_MotionVectorFill.hlsl, b0), written by "DrawUpscaler"
	struct MotionVectorFillConstants
	{
		row_major float4x4 reprojection; // Current clip space to the previous frame's (column vectors)
		float2 jitter_ndc;               // This frame's projection jitter: in the depth, not in the motion vectors
		float reactive_scale;
		float reactive_threshold;
		float reactive_enabled; // 0/1
		float3 padding;
	};

	// Define the game specific cbuffer (instance/pass) data here
	struct LumaGameData
	{
		float Dummy; // hlsl doesn't support empty structs
	};
}

#endif // LUMA_GAME_CB_STRUCTS
