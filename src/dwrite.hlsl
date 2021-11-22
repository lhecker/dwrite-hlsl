// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

float DWrite_ApplyLightOnDarkContrastAdjustment(float grayscaleEnhancedContrast, float3 color)
{
    float lightness = 0.30f * color.r + 0.59f * color.g + 0.11f * color.b;
    float multiplier = saturate(4.0f * (0.75f - lightness));
    return grayscaleEnhancedContrast * multiplier;
}

float DWrite_CalcColorIntensity(float3 color)
{
    return (color.r + color.g + color.g + color.b) / 4.0f;
}

float DWrite_EnhanceContrast(float alpha, float k)
{
    return alpha * (k + 1.0f) / (alpha * k + 1.0f);
}

float DWrite_ApplyAlphaCorrection(float a, float f, float4 g)
{
    return a + a * (1 - a) * ((g.x * f + g.y) * a + (g.z * f + g.w));
}

// Call this function to get the same gamma corrected alpha blending effect
// as DirectWrite's native algorithm for grayscale antialiased glyphs.
// The returned value needs to be multiplied with your RGBA foreground color
// which can then be composited as usual on top of your background color.
//
// gammaRatios:
//   Magic voodoo constants produced by DWrite_GetGammaRatios() in dwrite.cpp.
//   The default value for this are the 1.8 gamma ratios, which equates to:
//     0.148054421f, -0.894594550f, 1.47590804f, -0.324668258f
// grayscaleEnhancedContrast:
//   An additional contrast boost, making the font ligher/darker.
//   The default value for this is 1.0f.
//   This value should be set to the return value of DWrite_GetRenderParams() or (pseudo-code):
//     IDWriteRenderingParams1* defaultParams;
//     dwriteFactory->CreateRenderingParams(&defaultParams);
//     gamma = defaultParams->GetGrayscaleEnhancedContrast();
// isThinFont:
//   This constant is true for certain fonts that are simply too thin for AA.
//   Unlike the previous two values, this value isn't a constant and can change per font family.
//   If you only use modern fonts (like Roboto) you can safely assume that it's false.
//   If you draw your glyph atlas with any DirectWrite method except IDWriteGlyphRunAnalysis::CreateAlphaTexture
//   then you must set this to false as well, as not even tricks like setting the
//   gamma to 1.0 disables the internal thin-font contrast-boost inside DirectWrite.
//   Applying the contrast-boost twice would then look incorrectly.
// textColor:
//   The text's foreground color in straight (!) alpha.
// glyphAlpha:
//   The alpha value of the current glyph pixel in your texture atlas.
float DWrite_GetGrayscaleCorrectedAlpha(float4 gammaRatios, float grayscaleEnhancedContrast, bool isThinFont, float3 textColor, float glyphAlpha)
{
    float contrastBoost = isThinFont ? 0.5f : 0.0f;
    float enhancedContrast = contrastBoost + DWrite_ApplyLightOnDarkContrastAdjustment(grayscaleEnhancedContrast, textColor);
    float intensity = DWrite_CalcColorIntensity(textColor);
    float contrasted = DWrite_EnhanceContrast(glyphAlpha, enhancedContrast);
    return DWrite_ApplyAlphaCorrection(contrasted, intensity, gammaRatios);
}
