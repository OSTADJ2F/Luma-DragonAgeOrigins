#ifndef SRC_DAO_UI_PRESCALE_HLSL
#define SRC_DAO_UI_PRESCALE_HLSL

// Include after "Includes/Common.hlsl" and "../Includes/ColorGradingLUT.hlsl".

// UI_DRAW_TYPE 2: the display composition scales the whole canvas by UI / Game paper white after decoding it with the game's encoding and the
// gamma correction (with the correction only inside 0-1 if "GAMMA_CORRECTION_RANGE_TYPE" is 1). The scene (the grade, the videos) is divided by
// that here, through the very same decode and its exact inverse: the generic "game_gamma_to_linear" is the plain vanilla encoding (sRGB here),
// which isn't what the composition decodes with (gamma 2.2 inside 0-1), and the mismatch showed as a contrast change on the UI Paper White slider.
// Takes and returns the game's gamma encoded colour (negatives allowed). A no-op when the two paper whites match (and in SDR).
float3 ApplyUIPaperWhitePreScale(float3 gammaColor)
{
   const float preScale = GetUIPaperWhitePreScale(GamePaperWhiteNits, UIPaperWhiteNits, true);
   if (preScale == 1.0)
      return gammaColor;
#if GAMMA_CORRECTION_RANGE_TYPE == 1
   const float3 linearColor = ColorGradingLUTTransferFunctionOutCorrected(gammaColor, VANILLA_ENCODING_TYPE, GAMMA_CORRECTION_TYPE) * preScale;
   return ColorGradingLUTTransferFunctionInCorrected(linearColor, GAMMA_CORRECTION_TYPE, VANILLA_ENCODING_TYPE);
#else
   const float3 linearColor = ColorGradingLUTTransferFunctionOut(gammaColor, GAMMA_CORRECTION_TYPE, true) * preScale;
   return ColorGradingLUTTransferFunctionIn(linearColor, GAMMA_CORRECTION_TYPE, true);
#endif
}

// Black crush: the game's output is encoded sRGB, and the display composition decodes it as gamma 2.2 ("GAMMA_CORRECTION_TYPE" 1), which is darker near
// black (at an encoded 0.1 about 40% less light, several times less below 0.03). This moves the darks back towards the game's own decode by "amount",
// fading out by an encoded 0.3, so the mids and the highlights stay as they are. The composition decodes with the correction type's curve inside 0-1,
// so the lifted value is picked in linear light between that decode and the game's, then encoded for it again. Only meaningful for the two single
// curve correction types, a no-op otherwise (and when the game's encoding and the correction type are the same one). Takes and returns the game's
// gamma encoded colour (negatives are left alone).
float3 ApplyShadowLift(float3 gammaColor, float amount)
{
#if GAMMA_CORRECTION_TYPE <= 1 && VANILLA_ENCODING_TYPE != GAMMA_CORRECTION_TYPE
   if (amount <= 0.0)
      return gammaColor;
   const float3 encoded = max(gammaColor, 0.0);
   const float3 displayLinear = ColorGradingLUTTransferFunctionOut(encoded, GAMMA_CORRECTION_TYPE, false);
   const float3 gameLinear = ColorGradingLUTTransferFunctionOut(encoded, VANILLA_ENCODING_TYPE, false);
   const float3 fade = 1.0 - smoothstep(0.0, max(LumaSettings.GameSettings.ShadowLiftRange, 0.01), encoded);
   const float3 lifted = ColorGradingLUTTransferFunctionIn(lerp(displayLinear, gameLinear, saturate(amount) * fade), GAMMA_CORRECTION_TYPE, false);
   return lerp(gammaColor, lifted, (float3)(gammaColor > 0.0));
#else
   return gammaColor;
#endif
}

#endif // SRC_DAO_UI_PRESCALE_HLSL
