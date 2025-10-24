// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

// Exclude stuff from <Windows.h> we don't need.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <dwrite_1.h>

// The `gamma` and `grayscaleEnhancedContrast` values are required for DWrite_GetGrayscaleCorrectedAlpha()
// in shader.hlsl and can be passed in your constant buffer, for instance.
//
// The returned `linearParams` object should be passed to various DirectWrite/D2D
// methods, like ID2D1RenderTarget::SetTextRenderingParams, because dwrite.hlsl
// expects the glyphs to be in gamma = 1.0. This is because DirectWrite's alpha
// blending is gamma corrected with the help of the text foreground color.
//
// !!! NOTE !!!
// This also means that you should NEVER create your glyph atlas with DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
// or similar. Instead, use DXGI_FORMAT_B8G8R8A8_UNORM. Otherwise, you'd apply gamma correction twice.
// (Once during texture load and then via dwrite.hlsl.)
//
// Under Windows applications aren't expected to refresh the rendering params after startup,
// allowing you to cache these values for the lifetime of your application.
void DWrite_GetRenderParams(IDWriteFactory1* factory, float* gamma, float* cleartypeEnhancedContrast, float* grayscaleEnhancedContrast, IDWriteRenderingParams1** linearParams);

// This function produces 4 magic constants for DWrite_ApplyAlphaCorrection() in dwrite.hlsl
// and are required as an argument for DWrite_GetGrayscaleCorrectedAlpha().
// gamma should be set to the return value of DWrite_GetRenderParams() or (pseudo-code):
//   IDWriteRenderingParams* defaultParams;
//   dwriteFactory->CreateRenderingParams(&defaultParams);
//   gamma = defaultParams->GetGamma();
//
// gamma is chosen using the gamma value you pick in the "Adjust ClearType text" application.
// If not configured by the user, the Windows default values are the 1.8 gamma ratios.
//
// NOTE: This function should be used if your shader uses linear gamma color values,
// such as when you're using DXGI_FORMAT_B8G8R8A8_UNORM_SRGB as your render target.
// (= You pass linear colors to DWrite_GrayscaleBlend, etc., and expect it to return them as well.)
// Otherwise see DWrite_GetGammaRatiosForEncodedTarget.
void DWrite_GetGammaRatiosForLinearTarget(float gamma, float (&out)[4]) noexcept;

// NOTE: This function should be used when your shader uses sRGB colors,
// such as when you're using DXGI_FORMAT_B8G8R8A8_UNORM as your render target.
// For all other cases, use DWrite_GetGammaRatiosForLinearTarget.
void DWrite_GetGammaRatiosForEncodedTarget(float gamma, float (&out)[4]) noexcept;

// DWrite_IsThinFontFamily returns true if the specified family name is in our hard-coded list of "thin fonts".
// These are fonts that require special rendering because their strokes are too thin.
//
// The history of these fonts is interesting. The glyph outlines were originally created by digitizing the typeballs of
// IBM Selectric typewriters. Digitizing the metal typeballs yielded very precise outlines. However, the strokes are
// consistently too thin in comparison with the corresponding typewritten characters because the thickness of the
// typewriter ribbon was not accounted for. This didn't matter in the earliest versions of Windows because the screen
// resolution was not that high and you could not have a stroke thinner than one pixel. However, with the introduction
// of anti-aliasing the thin strokes manifested in text that was too light. By this time, it was too late to change
// the fonts so instead a special case was added to render these fonts differently.
//
// ---
//
// The canonical family name is a font's family English name, when
// * There's a corresponding font face name with the same language ID
// * If multiple such pairs exist, en-us is preferred
// * Otherwise (if en-us is not a translation) it's the lowest LCID
//
// However my (lhecker) understanding is that none of the thinFontFamilyNames come without an en-us translation.
// As such you can simply get the en-us name of the font from a IDWriteFontCollection for instance.
// See the overloaded alternative version of isThinFontFamily.
bool DWrite_IsThinFontFamily(const wchar_t* canonicalFamilyName) noexcept;

// The actual DWrite_IsThinFontFamily() expects you to pass a "canonical" family name,
// which technically isn't that trivial to determine. This function might help you with that.
// Just give it the font collection you use and any family name from that collection.
// (For instance from IDWriteFactory::GetSystemFontCollection.)
bool DWrite_IsThinFontFamily(IDWriteFontCollection* fontCollection, const wchar_t* familyName);
