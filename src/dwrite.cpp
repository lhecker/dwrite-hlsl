// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "dwrite.h"

#include <wil/com.h>

#pragma warning(disable : 26429) // Symbol '...' is never tested for nullness, it can be marked as not_null (f.23).

template<typename T>
constexpr T clamp(T v, T min, T max) noexcept
{
    return std::max(min, std::min(max, v));
}

void DWrite_GetRenderParams(IDWriteFactory1* factory, float* gamma, float* cleartypeEnhancedContrast, float* grayscaleEnhancedContrast, IDWriteRenderingParams1** linearParams)
{
    // If you're concerned with crash resilience don't use reinterpret_cast
    // and use .query<IDWriteRenderingParams1>() or ->QueryInterface() instead.
    wil::com_ptr<IDWriteRenderingParams1> defaultParams;
    THROW_IF_FAILED(factory->CreateRenderingParams(reinterpret_cast<IDWriteRenderingParams**>(defaultParams.addressof())));

    *gamma = defaultParams->GetGamma();
    *cleartypeEnhancedContrast = defaultParams->GetEnhancedContrast();
    *grayscaleEnhancedContrast = defaultParams->GetGrayscaleEnhancedContrast();

    THROW_IF_FAILED(factory->CreateCustomRenderingParams(1.0f, 0.0f, 0.0f, defaultParams->GetClearTypeLevel(), defaultParams->GetPixelGeometry(), defaultParams->GetRenderingMode(), linearParams));
}

// The following tables are taken from directly from DirectWrite and were not modified.
//
// The ratios are divided by 4, in order to avoid overflow in pixel shaders.
// They're derived programmatically with the following intent:
// > [The derive code] was used to figure out how best to simulate gamma correction for text given that in a pixel
// > shader we cannot implement true blending functions... to do so would require reading back from the backbuffer.
// > [The chosen solution] uses the foreground color of the brush to determine how best to alter the
// > alpha channel in order to simulate a conversion of the render target to 1.0 space for blending.
static constexpr float sc_gammaIncorrectTargetRatios[13][4]{
    { 0.0000f / 4.f, -0.0000f / 4.f, 0.0000f / 4.f, -0.0000f / 4.f }, // gamma = 1.0
    { 0.0166f / 4.f, -0.0807f / 4.f, 0.2227f / 4.f, -0.0751f / 4.f }, // gamma = 1.1
    { 0.0350f / 4.f, -0.1760f / 4.f, 0.4325f / 4.f, -0.1370f / 4.f }, // gamma = 1.2
    { 0.0543f / 4.f, -0.2821f / 4.f, 0.6302f / 4.f, -0.1876f / 4.f }, // gamma = 1.3
    { 0.0739f / 4.f, -0.3963f / 4.f, 0.8167f / 4.f, -0.2287f / 4.f }, // gamma = 1.4
    { 0.0933f / 4.f, -0.5161f / 4.f, 0.9926f / 4.f, -0.2616f / 4.f }, // gamma = 1.5
    { 0.1121f / 4.f, -0.6395f / 4.f, 1.1588f / 4.f, -0.2877f / 4.f }, // gamma = 1.6
    { 0.1300f / 4.f, -0.7649f / 4.f, 1.3159f / 4.f, -0.3080f / 4.f }, // gamma = 1.7
    { 0.1469f / 4.f, -0.8911f / 4.f, 1.4644f / 4.f, -0.3234f / 4.f }, // gamma = 1.8
    { 0.1627f / 4.f, -1.0170f / 4.f, 1.6051f / 4.f, -0.3347f / 4.f }, // gamma = 1.9
    { 0.1773f / 4.f, -1.1420f / 4.f, 1.7385f / 4.f, -0.3426f / 4.f }, // gamma = 2.0
    { 0.1908f / 4.f, -1.2652f / 4.f, 1.8650f / 4.f, -0.3476f / 4.f }, // gamma = 2.1
    { 0.2031f / 4.f, -1.3864f / 4.f, 1.9851f / 4.f, -0.3501f / 4.f }, // gamma = 2.2
};
static constexpr float sc_gammaCorrectTargetRatios[13][4]{
    { 0.0256f / 4.f, -0.1020f / 4.f, -1.5787f / 4.f, 0.8339f / 4.f }, // gamma = 1.0
    { 0.0105f / 4.f, -0.0428f / 4.f, -1.4340f / 4.f, 0.7358f / 4.f }, // gamma = 1.1
    { -0.0009f / 4.f, 0.0038f / 4.f, -1.2934f / 4.f, 0.6450f / 4.f }, // gamma = 1.2
    { -0.0092f / 4.f, 0.0392f / 4.f, -1.1567f / 4.f, 0.5610f / 4.f }, // gamma = 1.3
    { -0.0147f / 4.f, 0.0644f / 4.f, -1.0238f / 4.f, 0.4834f / 4.f }, // gamma = 1.4
    { -0.0178f / 4.f, 0.0804f / 4.f, -0.8946f / 4.f, 0.4115f / 4.f }, // gamma = 1.5
    { -0.0189f / 4.f, 0.0881f / 4.f, -0.7689f / 4.f, 0.3451f / 4.f }, // gamma = 1.6
    { -0.0182f / 4.f, 0.0882f / 4.f, -0.6467f / 4.f, 0.2838f / 4.f }, // gamma = 1.7
    { -0.0160f / 4.f, 0.0816f / 4.f, -0.5278f / 4.f, 0.2271f / 4.f }, // gamma = 1.8
    { -0.0124f / 4.f, 0.0687f / 4.f, -0.4122f / 4.f, 0.1749f / 4.f }, // gamma = 1.9
    { -0.0078f / 4.f, 0.0503f / 4.f, -0.2998f / 4.f, 0.1267f / 4.f }, // gamma = 2.0
    { -0.0022f / 4.f, 0.0270f / 4.f, -0.1904f / 4.f, 0.0823f / 4.f }, // gamma = 2.1
    { 0.0042f / 4.f, -0.0009f / 4.f, -0.0840f / 4.f, 0.0414f / 4.f }, // gamma = 2.2
};

static void DWrite_GetGammaRatios(float gamma, float (&out)[4], const float (&table)[13][4]) noexcept
{
    static constexpr auto norm13 = static_cast<float>(static_cast<double>(0x10000) / (255 * 255) * 4);
    static constexpr auto norm24 = static_cast<float>(static_cast<double>(0x100) / (255) * 4);

#pragma warning(suppress : 26451) // Arithmetic overflow: Using operator '+' on a 4 byte value and then casting the result to a 8 byte value.
    const auto index = clamp<ptrdiff_t>(static_cast<ptrdiff_t>(gamma * 10.0f + 0.5f), 10, 22) - 10;
#pragma warning(suppress : 26446) // Prefer to use gsl::at() instead of unchecked subscript operator (bounds.4).
#pragma warning(suppress : 26482) // Only index into arrays using constant expressions (bounds.2).
    const auto& ratios = table[index];

    out[0] = norm13 * ratios[0];
    out[1] = norm24 * ratios[1];
    out[2] = norm13 * ratios[2];
    out[3] = norm24 * ratios[3];
}

void DWrite_GetGammaRatiosForLinearTarget(float gamma, float (&out)[4]) noexcept
{
    DWrite_GetGammaRatios(gamma, out, sc_gammaCorrectTargetRatios);
}

void DWrite_GetGammaRatiosForEncodedTarget(float gamma, float (&out)[4]) noexcept
{
    DWrite_GetGammaRatios(gamma, out, sc_gammaIncorrectTargetRatios);
}

// This belongs to isThinFontFamily().
// Keep this in alphabetical order, or the loop will break.
// Keep thinFontFamilyNamesMaxWithNull updated.
static constexpr const wchar_t* thinFontFamilyNames[]{
    L"Courier New",
    L"Fixed Miriam Transparent",
    L"Miriam Fixed",
    L"Rod",
    L"Rod Transparent",
    L"Simplified Arabic Fixed"
};
static constexpr size_t thinFontFamilyNamesMaxLengthWithNull = 25;

bool DWrite_IsThinFontFamily(const wchar_t* canonicalFamilyName) noexcept
{
    int n = 0;

    for (const auto familyName : thinFontFamilyNames)
    {
        n = wcscmp(canonicalFamilyName, familyName);
        if (n <= 0)
        {
            break;
        }
    }

    return n == 0;
}

bool DWrite_IsThinFontFamily(IDWriteFontCollection* fontCollection, const wchar_t* familyName)
{
    UINT32 index;
    BOOL exists;
    if (FAILED(fontCollection->FindFamilyName(familyName, &index, &exists)) || !exists)
    {
        return false;
    }

    wil::com_ptr<IDWriteFontFamily> fontFamily;
    THROW_IF_FAILED(fontCollection->GetFontFamily(index, fontFamily.addressof()));

    wil::com_ptr<IDWriteLocalizedStrings> localizedFamilyNames;
    THROW_IF_FAILED(fontFamily->GetFamilyNames(localizedFamilyNames.addressof()));

    THROW_IF_FAILED(localizedFamilyNames->FindLocaleName(L"en-US", &index, &exists));
    if (!exists)
    {
        return false;
    }

    UINT32 length;
    THROW_IF_FAILED(localizedFamilyNames->GetStringLength(index, &length));

    if (length >= thinFontFamilyNamesMaxLengthWithNull)
    {
        return false;
    }

    wchar_t enUsFamilyName[thinFontFamilyNamesMaxLengthWithNull];
    THROW_IF_FAILED(localizedFamilyNames->GetString(index, &enUsFamilyName[0], thinFontFamilyNamesMaxLengthWithNull));

    return DWrite_IsThinFontFamily(&enUsFamilyName[0]);
}
