// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

// Exclude stuff from <Windows.h> we don't need.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <d2d1.h>
#include <dwrite_1.h>

#include "util.h"

// See .cpp file for documentation.
void DWrite_GetRenderParams(IDWriteFactory1* factory, float* gamma, float* grayscaleEnhancedContrast, IDWriteRenderingParams1** linearParams);
f32x4 DWrite_GetGammaRatios(f32 gamma) noexcept;
bool DWrite_IsThinFontFamily(const wchar_t* canonicalFamilyName) noexcept;
bool DWrite_IsThinFontFamily(IDWriteFontCollection* fontCollection, const wchar_t* familyName);
