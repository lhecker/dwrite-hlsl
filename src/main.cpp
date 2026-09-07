// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <d2d1.h>
#include <d3d11_2.h>
#include <dwmapi.h>
#include <dwrite_3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

// wil
#include <wil/com.h>
// imgui
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
#include <imgui.h>
// our stuff
#include <main_ps.h>
#include <main_vs.h>

#include "dwrite.h"
#include "imgui_combowithfilter.h"
#include "imgui_dwrite.h"
#include "util.h"

static u32x2 g_viewportSize;
static bool g_viewportSizeChanged = true;

static UINT g_dpi = USER_DEFAULT_SCREEN_DPI;
static bool g_dpiChanged = true;

enum class BlendMode : u32 {
    DWriteGrayscale,
    DWriteClearType,
    Primitive,
};

struct alignas(16) ConstantBuffer {
    alignas(sizeof(u32x2)) u32x2 splitPos;
    alignas(sizeof(u32x2)) u32x2 tileSize;
    alignas(sizeof(f32x4)) f32x4 background;
    alignas(sizeof(f32x4)) f32x4 foreground;

    alignas(sizeof(f32x4)) float gammaRatios[4];
    alignas(sizeof(f32)) f32 cleartypeEnhancedContrast = 0;
    alignas(sizeof(f32)) f32 grayscaleEnhancedContrast = 0;
    alignas(sizeof(u32)) BlendMode mode = BlendMode::DWriteGrayscale;
};

struct FontAxisSetting {
    DWRITE_FONT_AXIS_RANGE range;
    float value;
    std::string label;
};

struct FontFeatureSetting {
    DWRITE_FONT_FEATURE_TAG tag;
    int value = -1;
    std::string label;
};

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT
ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::wstring u8u16(const std::string_view& input) {
    const auto wideLength = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    THROW_LAST_ERROR_IF(wideLength < 0);

    std::wstring wide(wideLength, L'\0');
    const auto result = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), wide.data(), wideLength);
    THROW_LAST_ERROR_IF(result != wideLength);

    return wide;
}

static std::string u16u8(const std::wstring_view& input) {
    const auto narrowLength = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    THROW_LAST_ERROR_IF(narrowLength <= 0);

    std::string narrow(narrowLength, L'\0');
    const auto result = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), narrow.data(), narrowLength, nullptr, nullptr);
    THROW_LAST_ERROR_IF(result != narrowLength);

    return narrow;
}

static f32x4 premultiply_color(const f32x4& in) noexcept {
    return {in.r * in.a, in.g * in.a, in.b * in.a, in.a};
}

static f32x4 srgb_to_linear(f32x4 in) noexcept {
    for (int i = 0; i < 3; i++) {
        auto& c = in[i];
        if (c <= 0.04045f) {
            c /= 12.92f;
        } else {
            c = std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
    }
    return in;
}

const D2D1_COLOR_F& as_d2d_color(const f32x4& color) noexcept {
    return *reinterpret_cast<const D2D1_COLOR_F*>(&color);
}

static std::vector<std::string> get_system_font_names(IDWriteFontCollection* fontCollection, const wchar_t* localeName) {
    const auto count = fontCollection->GetFontFamilyCount();

    std::vector<std::wstring> wideFontNames;
    wideFontNames.reserve(count);

    for (UINT32 i = 0; i < count; ++i) {
        wil::com_ptr<IDWriteFontFamily> fontFamily;
        THROW_IF_FAILED(fontCollection->GetFontFamily(i, fontFamily.addressof()));

        wil::com_ptr<IDWriteLocalizedStrings> localizedFamilyNames;
        THROW_IF_FAILED(fontFamily->GetFamilyNames(localizedFamilyNames.addressof()));

        UINT32 index;
        BOOL exists = FALSE;

        THROW_IF_FAILED(localizedFamilyNames->FindLocaleName(localeName, &index, &exists));
        if (!exists) {
            THROW_IF_FAILED(localizedFamilyNames->FindLocaleName(L"en-US", &index, &exists));
        }
        if (!exists) {
            index = 0;
        }

        UINT32 length;
        THROW_IF_FAILED(localizedFamilyNames->GetStringLength(index, &length));

        std::wstring buffer(length, L'\0');
        THROW_IF_FAILED(localizedFamilyNames->GetString(index, buffer.data(), length + 1));

        wideFontNames.emplace_back(std::move(buffer));
    }

    std::sort(wideFontNames.begin(), wideFontNames.end(), [&, locale = ImDWriteGetUserDefaultLocaleName()](const auto& a, const auto& b) -> bool {
        return CompareStringEx(locale, 0, a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });

    std::vector<std::string> fontNames;
    fontNames.reserve(count);

    for (const auto& fontName : wideFontNames) {
        fontNames.emplace_back(u16u8(fontName));
    }

    return fontNames;
}

static int get_font_name_index_from_collection(const std::vector<std::string>& fontNames, const std::string_view& needle) {
    int idx = 0;
    for (const auto& fontName : fontNames) {
        if (fontName == needle) {
            return idx;
        }
        idx++;
    }
    return 0;
}

static std::string format_open_type_tag_fallback(UINT32 tag) {
    static constexpr auto isAlnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };

    const auto chars = reinterpret_cast<const char*>(&tag);
    for (int i = 0; i < 4; ++i) {
        if (!isAlnum(chars[i])) {
            return "Unknown";
        }
    }
    return std::string(chars, 4);
}

static std::string format_open_type_axis(DWRITE_FONT_AXIS_TAG tag) {
    const char* str;

    // clang-format off
    switch (tag)
    {
    // https://learn.microsoft.com/en-us/typography/opentype/spec/dvaraxisreg
    case DWRITE_MAKE_FONT_AXIS_TAG('i', 't', 'a', 'l'): str = "Italic"; break;
    case DWRITE_MAKE_FONT_AXIS_TAG('o', 'p', 's', 'z'): str = "Optical Size"; break;
    case DWRITE_MAKE_FONT_AXIS_TAG('s', 'l', 'n', 't'): str = "Slant"; break;
    case DWRITE_MAKE_FONT_AXIS_TAG('w', 'd', 't', 'h'): str = "Width"; break;
    case DWRITE_MAKE_FONT_AXIS_TAG('w', 'g', 'h', 't'): str = "Weight"; break;
    default: return format_open_type_tag_fallback(tag);
    }
    // clang-format on

    return str;
}

static std::string format_open_type_feature(DWRITE_FONT_FEATURE_TAG tag) {
    static constexpr auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
    const char* str;
    char lo, hi;

    // clang-format off
    switch (tag)
    {
    // https://learn.microsoft.com/en-us/typography/opentype/spec/featurelist
    case DWRITE_MAKE_FONT_FEATURE_TAG('a', 'a', 'l', 't'): str = "Access All Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('a', 'b', 'v', 'f'): str = "Above-base Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('a', 'b', 'v', 'm'): str = "Above-base Mark Positioning"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('a', 'b', 'v', 's'): str = "Above-base Substitutions"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('a', 'f', 'r', 'c'): str = "Alternative Fractions"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('a', 'k', 'h', 'n'): str = "Akhand"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('a', 'p', 'k', 'n'): str = "Kerning for Alternate Proportional Widths"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('b', 'l', 'w', 'f'): str = "Below-base Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('b', 'l', 'w', 'm'): str = "Below-base Mark Positioning"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('b', 'l', 'w', 's'): str = "Below-base Substitutions"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'a', 'l', 't'): str = "Contextual Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'a', 's', 'e'): str = "Case-sensitive Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'c', 'm', 'p'): str = "Glyph Composition / Decomposition"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'f', 'a', 'r'): str = "Conjunct Form After Ro"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'h', 'w', 's'): str = "Contextual Half-width Spacing"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'j', 'c', 't'): str = "Conjunct Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'l', 'i', 'g'): str = "Contextual Ligatures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'p', 'c', 't'): str = "Centered CJK Punctuation"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'p', 's', 'p'): str = "Capital Spacing"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 's', 'w', 'h'): str = "Contextual Swash"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'u', 'r', 's'): str = "Cursive Positioning"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', '2', 'p', 'c'): str = "Petite Capitals From Capitals"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', '2', 's', 'c'): str = "Small Capitals From Capitals"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('d', 'i', 's', 't'): str = "Distances"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('d', 'l', 'i', 'g'): str = "Discretionary Ligatures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('d', 'n', 'o', 'm'): str = "Denominators"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('d', 't', 'l', 's'): str = "Dotless Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('e', 'x', 'p', 't'): str = "Expert Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('f', 'a', 'l', 't'): str = "Final Glyph on Line Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('f', 'i', 'n', '2'): str = "Terminal Forms #2"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('f', 'i', 'n', '3'): str = "Terminal Forms #3"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('f', 'i', 'n', 'a'): str = "Terminal Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('f', 'l', 'a', 'c'): str = "Flattened Accent Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('f', 'r', 'a', 'c'): str = "Fractions"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('f', 'w', 'i', 'd'): str = "Full Widths"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'a', 'l', 'f'): str = "Half Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'a', 'l', 'n'): str = "Halant Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'a', 'l', 't'): str = "Alternate Half Widths"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'i', 's', 't'): str = "Historical Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'k', 'n', 'a'): str = "Horizontal Kana Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'l', 'i', 'g'): str = "Historical Ligatures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'n', 'g', 'l'): str = "Hangul"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'o', 'j', 'o'): str = "Hojo Kanji Forms (JIS X 0212-1990 Kanji Forms)"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('h', 'w', 'i', 'd'): str = "Half Widths"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('i', 'n', 'i', 't'): str = "Initial Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('i', 's', 'o', 'l'): str = "Isolated Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('i', 't', 'a', 'l'): str = "Italics"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('j', 'a', 'l', 't'): str = "Justification Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('j', 'p', '7', '8'): str = "JIS78 Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('j', 'p', '8', '3'): str = "JIS83 Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('j', 'p', '9', '0'): str = "JIS90 Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('j', 'p', '0', '4'): str = "JIS2004 Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('k', 'e', 'r', 'n'): str = "Kerning"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('l', 'f', 'b', 'd'): str = "Left Bounds"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('l', 'i', 'g', 'a'): str = "Standard Ligatures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('l', 'j', 'm', 'o'): str = "Leading Jamo Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('l', 'n', 'u', 'm'): str = "Lining Figures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('l', 'o', 'c', 'l'): str = "Localized Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('l', 't', 'r', 'a'): str = "Left-to-right Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('l', 't', 'r', 'm'): str = "Left-to-right Mirrored Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('m', 'a', 'r', 'k'): str = "Mark Positioning"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('m', 'e', 'd', '2'): str = "Medial Forms #2"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('m', 'e', 'd', 'i'): str = "Medial Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('m', 'g', 'r', 'k'): str = "Mathematical Greek"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('m', 'k', 'm', 'k'): str = "Mark to Mark Positioning"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('m', 's', 'e', 't'): str = "Mark Positioning via Substitution"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('n', 'a', 'l', 't'): str = "Alternate Annotation Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('n', 'l', 'c', 'k'): str = "NLC Kanji Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('n', 'u', 'k', 't'): str = "Nukta Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('n', 'u', 'm', 'r'): str = "Numerators"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('o', 'n', 'u', 'm'): str = "Oldstyle Figures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('o', 'p', 'b', 'd'): str = "Optical Bounds"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('o', 'r', 'd', 'n'): str = "Ordinals"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('o', 'r', 'n', 'm'): str = "Ornaments"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 'a', 'l', 't'): str = "Proportional Alternate Widths"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 'c', 'a', 'p'): str = "Petite Capitals"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 'k', 'n', 'a'): str = "Proportional Kana"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 'n', 'u', 'm'): str = "Proportional Figures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 'r', 'e', 'f'): str = "Pre-base Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 'r', 'e', 's'): str = "Pre-base Substitutions"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 's', 't', 'f'): str = "Post-base Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 's', 't', 's'): str = "Post-base Substitutions"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('p', 'w', 'i', 'd'): str = "Proportional Widths"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('q', 'w', 'i', 'd'): str = "Quarter Widths"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 'a', 'n', 'd'): str = "Randomize"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 'c', 'l', 't'): str = "Required Contextual Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 'k', 'r', 'f'): str = "Rakar Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 'l', 'i', 'g'): str = "Required Ligatures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 'p', 'h', 'f'): str = "Reph Form"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 't', 'b', 'd'): str = "Right Bounds"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 't', 'l', 'a'): str = "Right-to-left Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 't', 'l', 'm'): str = "Right-to-left Mirrored Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 'u', 'b', 'y'): str = "Ruby Notation Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('r', 'v', 'r', 'n'): str = "Required Variation Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 'a', 'l', 't'): str = "Stylistic Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 'i', 'n', 'f'): str = "Scientific Inferiors"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 'i', 'z', 'e'): str = "Optical size"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 'm', 'c', 'p'): str = "Small Capitals"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 'm', 'p', 'l'): str = "Simplified Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 's', 't', 'y'): str = "Math Script-style Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 't', 'c', 'h'): str = "Stretching Glyph Decomposition"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 'u', 'b', 's'): str = "Subscript"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 'u', 'p', 's'): str = "Superscript"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 'w', 's', 'h'): str = "Swash"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('t', 'i', 't', 'l'): str = "Titling"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('t', 'j', 'm', 'o'): str = "Trailing Jamo Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('t', 'n', 'a', 'm'): str = "Traditional Name Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('t', 'n', 'u', 'm'): str = "Tabular Figures"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('t', 'r', 'a', 'd'): str = "Traditional Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('t', 'w', 'i', 'd'): str = "Third Widths"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('u', 'n', 'i', 'c'): str = "Unicase"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'a', 'l', 't'): str = "Alternate Vertical Metrics"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'a', 'p', 'k'): str = "Kerning for Alternate Proportional Vertical Metrics"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'a', 't', 'u'): str = "Vattu Variants"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'c', 'h', 'w'): str = "Vertical Contextual Half-width Spacing"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'e', 'r', 't'): str = "Vertical Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'h', 'a', 'l'): str = "Alternate Vertical Half Metrics"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'j', 'm', 'o'): str = "Vowel Jamo Forms"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'k', 'n', 'a'): str = "Vertical Kana Alternates"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'k', 'r', 'n'): str = "Vertical Kerning"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'p', 'a', 'l'): str = "Proportional Alternate Vertical Metrics"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'r', 't', '2'): str = "Vertical Alternates and Rotation"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('v', 'r', 't', 'r'): str = "Vertical Alternates for Rotation"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('z', 'e', 'r', 'o'): str = "Slashed Zero"; break;
    default: goto try_ss_cv;
    }
    // clang-format on

    return str;

try_ss_cv:
    // clang-format off
    switch (tag & DWRITE_MAKE_FONT_FEATURE_TAG(0xff, 0xff, 0, 0))
    {
    case DWRITE_MAKE_FONT_FEATURE_TAG('s', 's', 0, 0): str = "Stylistic Set"; break;
    case DWRITE_MAKE_FONT_FEATURE_TAG('c', 'v', 0, 0): str = "Character Variant"; break;
    default: goto fallback;
    }
    // clang-format on

    hi = (tag >> 16) & 0xFF;
    lo = (tag >> 24) & 0xFF;
    if (!isDigit(hi) || !isDigit(lo)) {
        goto fallback;
    }

    char buffer[32];
    sprintf_s(buffer, "%s%c%c", str, hi, lo);
    return buffer;

fallback:
    return format_open_type_tag_fallback(tag);
}

static void get_font_axes_and_features(IDWriteFactory* factory, IDWriteFontCollection2* fontCollection, const wchar_t* familyName, const wchar_t* localeName, std::vector<FontAxisSetting>& axes, std::vector<FontFeatureSetting>& features) {
    axes.clear();
    features.clear();

    // Get the font set for the given family, as well as the regular/default variant out of it.
    wil::com_ptr<IDWriteFontSet1> fontSet;
    wil::com_ptr<IDWriteFontFace5> regularFont;
    {
        UINT32 familyIndex = 0;
        BOOL familyExists = FALSE;
        THROW_IF_FAILED(fontCollection->FindFamilyName(familyName, &familyIndex, &familyExists));
        if (!familyExists) {
            return;
        }

        wil::com_ptr<IDWriteFontFamily2> family;
        THROW_IF_FAILED(fontCollection->GetFontFamily(familyIndex, family.addressof()));
        THROW_IF_FAILED(family->GetFontSet(fontSet.addressof()));

        wil::com_ptr<IDWriteFont> font;
        THROW_IF_FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.addressof()));

        wil::com_ptr<IDWriteFontFace> fontFace;
        THROW_IF_FAILED(font->CreateFontFace(fontFace.addressof()));
        fontFace.query_to(regularFont.addressof());
    }

    // Extract a list of font axes (weight, slant, etc.).
    {
        // Get the union of ranges across all fonts in the family.
        std::vector<DWRITE_FONT_AXIS_RANGE> ranges;
        {
            UINT32 count = 0;
            const auto hr = fontSet->GetFontAxisRanges(nullptr, 0, &count);
            THROW_HR_IF(hr, FAILED(hr) && hr != E_NOT_SUFFICIENT_BUFFER);

            ranges.resize(count);
            THROW_IF_FAILED(fontSet->GetFontAxisRanges(ranges.data(), count, &count));
            ranges.resize(count);
        }

        // Use the regular variant's axis values as the defaults.
        std::vector<DWRITE_FONT_AXIS_VALUE> values;
        {
            const auto count = regularFont->GetFontAxisValueCount();
            values.resize(count);
            THROW_IF_FAILED(regularFont->GetFontAxisValues(values.data(), count));
        }

        axes.reserve(ranges.size());

        // Join ranges and defaults.
        UINT index = 0;
        for (const auto& r : ranges) {
            // Ignore axes that cannot be changed (typically wdth).
            if (r.minValue >= r.maxValue) {
                index++;
                continue;
            }

            auto value = r.minValue;

            // Dumb O(n^2), but it's fine. There's typically only 4 axes.
            for (const auto& v : values) {
                if (v.axisTag == r.axisTag) {
                    value = v.value;
                    break;
                }
            }

            axes.push_back({r, value, format_open_type_axis(r.axisTag)});
            index++;
        }
    }

    // Extract a list of font features (ligatures on/off, etc.).
    {
        wil::com_ptr<IDWriteTextAnalyzer> analyzer;
        THROW_IF_FAILED(factory->CreateTextAnalyzer(analyzer.addressof()));
        const auto analyzer2 = analyzer.query<IDWriteTextAnalyzer2>();

        static constexpr DWRITE_SCRIPT_ANALYSIS defaultScript{};
        UINT32 featureCount = 0;
        const auto countResult = analyzer2->GetTypographicFeatures(regularFont.get(), defaultScript, localeName, 0, &featureCount, nullptr);
        THROW_HR_IF(countResult, FAILED(countResult) && countResult != E_NOT_SUFFICIENT_BUFFER);

        std::vector<DWRITE_FONT_FEATURE_TAG> tags(featureCount);
        THROW_IF_FAILED(analyzer2->GetTypographicFeatures(regularFont.get(), defaultScript, localeName, featureCount, &featureCount, tags.data()));
        features.reserve(featureCount);
        for (const auto tag : tags) {
            features.push_back({tag, -1, format_open_type_feature(tag)});
        }
    }
}

static constexpr f32 defaultGamma = 1.8f;
static constexpr f32 defaultCleartypeEnhancedContrast = 0.5f;
static constexpr f32 defaultGrayscaleEnhancedContrast = 1.0f;

static void save_dwrite_settings(float gamma, float cleartypeEnhancedContrast, float grayscaleEnhancedContrast) {
    const DWORD gammaLevel = static_cast<DWORD>(std::lrintf(gamma * 1000.0f));
    const DWORD enhancedContrastLevel = static_cast<DWORD>(std::lrintf(cleartypeEnhancedContrast * 100.0f));
    const DWORD grayscaleEnhancedContrastLevel = static_cast<DWORD>(std::lrintf(grayscaleEnhancedContrast * 100.0f));
    wchar_t keyPath[72];

    // DirectWrite unfortunately does not feature a "default setting" that
    // applies to all displays. When you call `CreateRenderingParams` to get
    // the defaults, it actually fetches them for the primary display device.
    // Brilliant, isn't it? Who would want default settings.
    //
    // Anyway, we're getting the primary display device here,
    // and construct the DirectWrite registry path.
    {
        DISPLAY_DEVICEW dd{
            .cb = sizeof(DISPLAY_DEVICEW),
        };

        for (DWORD index = 0;; ++index) {
            if (!EnumDisplayDevicesW(nullptr, index, &dd, 0)) {
                THROW_HR(E_FAIL);
            }

            if (WI_IsFlagSet(dd.StateFlags, DISPLAY_DEVICE_PRIMARY_DEVICE)) {
                auto name = &dd.DeviceName[0];
                if (memcmp(name, LR"(\\.\)", 8) == 0) {
                    name += 4;
                }
                _snwprintf_s(keyPath, _TRUNCATE, LR"(Software\Microsoft\Avalon.Graphics\%s)", name);
                break;
            }
        }
    }

    // Default values --> Delete all keys.
    if (gammaLevel == 1800 && enhancedContrastLevel == 50 && grayscaleEnhancedContrastLevel == 100) {
        HKEY rawKey = nullptr;
        auto status = RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_SET_VALUE, &rawKey);
        if (status == ERROR_FILE_NOT_FOUND) {
            return;
        }
        THROW_IF_WIN32_ERROR(status);
        const wil::unique_hkey key{rawKey};

        for (const auto valueName : {L"GammaLevel", L"EnhancedContrastLevel", L"GrayscaleEnhancedContrastLevel"}) {
            status = RegDeleteValueW(key.get(), valueName);
            THROW_HR_IF(status, FAILED(status) && status != ERROR_FILE_NOT_FOUND);
        }
    } else {
        HKEY rawKey = nullptr;
        THROW_IF_WIN32_ERROR(RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &rawKey, nullptr));
        const wil::unique_hkey key{rawKey};

        THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), L"GammaLevel", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&gammaLevel), sizeof(gammaLevel)));
        THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), L"EnhancedContrastLevel", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enhancedContrastLevel), sizeof(enhancedContrastLevel)));
        THROW_IF_WIN32_ERROR(RegSetValueExW(key.get(), L"GrayscaleEnhancedContrastLevel", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&grayscaleEnhancedContrastLevel), sizeof(grayscaleEnhancedContrastLevel)));
    }

    // Inform the system that the display settings have changed.
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &result);
}

static void create_d2d_render_target_texture(ID3D11Device* device, ID2D1Factory* d2dFactory, DXGI_FORMAT format, UINT width, UINT height, UINT dpi, ID2D1RenderTarget** renderTarget, ID3D11ShaderResourceView** textureView) {
    wil::com_ptr<ID3D11Texture2D> texture;
    {
        const D3D11_TEXTURE2D_DESC desc{
            .Width = width,
            .Height = height,
            .MipLevels = 1,
            .ArraySize = 1,
            .Format = format,
            .SampleDesc = {.Count = 1},
            .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        };
        THROW_IF_FAILED(device->CreateTexture2D(&desc, nullptr, texture.addressof()));
    }

    wil::com_ptr<ID3D11ShaderResourceView> textureViewPtr;
    THROW_IF_FAILED(device->CreateShaderResourceView(texture.get(), nullptr, textureViewPtr.addressof()));

    wil::com_ptr<ID2D1RenderTarget> renderTargetPtr;
    {
        const D2D1_RENDER_TARGET_PROPERTIES props{
            .type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
            .pixelFormat = {format, D2D1_ALPHA_MODE_PREMULTIPLIED},
            .dpiX = static_cast<f32>(dpi),
            .dpiY = static_cast<f32>(dpi),
        };
        THROW_IF_FAILED(d2dFactory->CreateDxgiSurfaceRenderTarget(texture.query<IDXGISurface>().get(), &props, renderTargetPtr.addressof()));
    }

    renderTargetPtr->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    *renderTarget = renderTargetPtr.detach();
    *textureView = textureViewPtr.detach();
}

static void update_dark_mode_for_window(HWND hwnd) {
    static const auto ShouldAppsUseDarkMode = []() {
        const auto uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return uxtheme ? reinterpret_cast<bool(WINAPI*)()>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(132))) : nullptr;
    }();
    static constexpr auto IsHighContrastOn = []() {
        bool highContrast = false;
        HIGHCONTRAST hc{sizeof(hc)};
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0)) {
            highContrast = (HCF_HIGHCONTRASTON & hc.dwFlags) != 0;
        }
        return highContrast;
    };

    if (ShouldAppsUseDarkMode) {
        const BOOL useDarkMode = ShouldAppsUseDarkMode() && !IsHighContrastOn();
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(BOOL));
    }
}

static void draw_overlay_label(const char* id, const char* text, const ImVec2& position) {
    static constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoInputs;

    ImGui::SetNextWindowPos(position, ImGuiCond_Always, {1, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {8, 4});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, {0, 0, 0, 1});
    ImGui::PushStyleColor(ImGuiCol_Border, {1, 1, 1, 1});
    ImGui::PushStyleColor(ImGuiCol_Text, {1, 1, 1, 1});
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 2.0f);

    if (ImGui::Begin(id, nullptr, flags)) {
        ImGui::TextUnformatted(text);
    }
    ImGui::End();

    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
}

static LRESULT CALLBACK wnd_proc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    try {
    if (const auto result = ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam)) {
        return result;
    }

    switch (message) {
    case WM_SETTINGCHANGE:
        if (!wParam && lParam && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0) {
            update_dark_mode_for_window(hWnd);
        }
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_viewportSize.x = LOWORD(lParam);
            g_viewportSize.y = HIWORD(lParam);
            g_viewportSizeChanged = true;
        }
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wParam);
        g_dpiChanged = true;
        const auto prcNewWindow = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hWnd, nullptr, prcNewWindow->left, prcNewWindow->top, prcNewWindow->right - prcNewWindow->left, prcNewWindow->bottom - prcNewWindow->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
}
CATCH_RETURN()

static void winMainImpl(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Win32 setup
    wil::unique_hwnd hwnd;
    {
        const WNDCLASSEXW wcex{
            .cbSize = sizeof(WNDCLASSEX),
            .style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC,
            .lpfnWndProc = wnd_proc,
            .cbClsExtra = 0,
            .cbWndExtra = 0,
            .hInstance = hInstance,
            .hIcon = nullptr,
            .hCursor = LoadCursor(nullptr, IDC_ARROW),
            .hbrBackground = nullptr,
            .lpszMenuName = nullptr,
            .lpszClassName = L"dwrite-hlsl",
            .hIconSm = nullptr,
        };
        RegisterClassExW(&wcex);

        hwnd.reset(THROW_LAST_ERROR_IF_NULL(CreateWindowExW(
            /* dwExStyle    */ 0,
            /* lpClassName  */ L"dwrite-hlsl",
            /* lpWindowName */ L"dwrite-hlsl",
            /* dwStyle      */ WS_OVERLAPPEDWINDOW,
            /* X            */ CW_USEDEFAULT,
            /* Y            */ CW_USEDEFAULT,
            /* nWidth       */ CW_USEDEFAULT,
            /* nHeight      */ CW_USEDEFAULT,
            /* hWndParent   */ nullptr,
            /* hMenu        */ nullptr,
            /* hInstance    */ hInstance,
            /* lpParam      */ nullptr
        )));

        update_dark_mode_for_window(hwnd.get());
        g_dpi = GetDpiForWindow(hwnd.get());
    }

    // D2D / DirectWrite setup
    wil::com_ptr<ID2D1Factory> d2dFactory;
    wil::com_ptr<IDWriteFactory1> dwriteFactory;
    wil::com_ptr<IDWriteFontCollection2> fontCollection;
    wil::com_ptr<IDWriteRenderingParams1> systemParams;
    wil::com_ptr<IDWriteRenderingParams1> linearParams;
    f32 systemGamma = defaultGamma;
    f32 systemCleartypeEnhancedContrast = defaultCleartypeEnhancedContrast;
    f32 systemGrayscaleEnhancedContrast = defaultGrayscaleEnhancedContrast;
    {
        THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(d2dFactory), d2dFactory.put_void()));
        THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(dwriteFactory), dwriteFactory.put_unknown()));

        DWrite_GetRenderParams(dwriteFactory.get(), &systemGamma, &systemCleartypeEnhancedContrast, &systemGrayscaleEnhancedContrast, linearParams.addressof());

        wil::com_ptr<IDWriteFontCollection> sfc;
        THROW_IF_FAILED(dwriteFactory->GetSystemFontCollection(sfc.addressof()));
        sfc.query_to(fontCollection.addressof());

        wil::com_ptr<IDWriteRenderingParams> rp;
        THROW_IF_FAILED(dwriteFactory->CreateRenderingParams(rp.addressof()));
        rp.query_to(systemParams.addressof());
    }

    // D3D setup
    wil::com_ptr<ID3D11Buffer> constantBuffer;
    wil::com_ptr<ID3D11Device> device;
    wil::com_ptr<ID3D11DeviceContext> deviceContext;
    wil::com_ptr<ID3D11PixelShader> pixelShader;
    wil::com_ptr<ID3D11VertexShader> vertexShader;
    wil::com_ptr<IDXGISwapChain1> swapChain;
    wil::unique_handle frameLatencyWaitableObject;
    {
        static constexpr D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        THROW_IF_FAILED(D3D11CreateDevice(
            /* pAdapter           */ nullptr,
            /* DriverType         */ D3D_DRIVER_TYPE_HARDWARE,
            /* Software           */ nullptr,
            /* Flags              */ D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            /* pFeatureLevels     */ &featureLevel,
            /* FeatureLevels      */ 1,
            /* SDKVersion         */ D3D11_SDK_VERSION,
            /* ppDevice           */ device.put(),
            /* pFeatureLevel      */ nullptr,
            /* ppImmediateContext */ deviceContext.put()
        ));

        static constexpr DXGI_SWAP_CHAIN_DESC1 desc{
            .Format = DXGI_FORMAT_B8G8R8A8_UNORM,
            .SampleDesc = {.Count = 1},
            .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
            .BufferCount = 2,
            .Scaling = DXGI_SCALING_NONE,
            .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
            .AlphaMode = DXGI_ALPHA_MODE_IGNORE,
            .Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT,
        };

        wil::com_ptr<IDXGIFactory2> dxgiFactory;
        THROW_IF_FAILED(CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.addressof())));
        THROW_IF_FAILED(dxgiFactory->CreateSwapChainForHwnd(device.get(), hwnd.get(), &desc, nullptr, nullptr, swapChain.put()));

        frameLatencyWaitableObject.reset(swapChain.query<IDXGISwapChain2>()->GetFrameLatencyWaitableObject());
    }
    {
        static constexpr D3D11_BUFFER_DESC desc{
            .ByteWidth = sizeof(ConstantBuffer),
            .Usage = D3D11_USAGE_DEFAULT,
            .BindFlags = D3D11_BIND_CONSTANT_BUFFER,
        };
        THROW_IF_FAILED(device->CreateBuffer(&desc, nullptr, constantBuffer.put()));
    }
    {
        THROW_IF_FAILED(device->CreateVertexShader(&main_vs[0], sizeof(main_vs), nullptr, vertexShader.put()));
        THROW_IF_FAILED(device->CreatePixelShader(&main_ps[0], sizeof(main_ps), nullptr, pixelShader.put()));
    }

    ShowWindow(hwnd.get(), nShowCmd);
    UpdateWindow(hwnd.get());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().Fonts->SetFontLoader(ImDWriteCreateFontLoader());
    ImDWriteAddFontFamily(ImGui::GetIO().Fonts, "Segoe UI");
    ImGui_ImplWin32_Init(hwnd.get());
    ImGui_ImplDX11_Init(device.get(), deviceContext.get());

    const auto cleanup = wil::scope_exit([]() {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    });

    struct UIState {
        char textBuffer[1024]{};
        bool textChanged = true;
        std::vector<std::string> fontNames;
        int fontNameIndex = 0;
        float fontSize = 12;
        std::vector<FontAxisSetting> fontAxes;
        std::vector<FontFeatureSetting> fontFeatures;
        BlendMode renderingMode = BlendMode::DWriteGrayscale;
        bool renderingWithSrgb = false;
        bool dwriteUseSystemSettings = true;
        bool dwriteDirty = false;
        f32 dwriteGamma = defaultGamma;
        f32 dwriteCleartypeEnhancedContrast = defaultCleartypeEnhancedContrast;
        f32 dwriteGrayscaleEnhancedContrast = defaultGrayscaleEnhancedContrast;
        f32x4 background{0.0f, 0.0f, 0.0f, 1.0f};
        f32x4 foreground{1.0f, 1.0f, 1.0f, 1.0f};
    } state;
    state.fontNames = get_system_font_names(fontCollection.get(), ImDWriteGetUserDefaultLocaleName());
    state.fontNameIndex = get_font_name_index_from_collection(state.fontNames, "Segoe UI");
    state.dwriteGamma = systemGamma;
    state.dwriteCleartypeEnhancedContrast = systemCleartypeEnhancedContrast;
    state.dwriteGrayscaleEnhancedContrast = systemGrayscaleEnhancedContrast;
    {
        const auto fontNameUTF16 = u8u16(state.fontNames[state.fontNameIndex]);
        get_font_axes_and_features(dwriteFactory.get(), fontCollection.get(), fontNameUTF16.c_str(), ImDWriteGetUserDefaultLocaleName(), state.fontAxes, state.fontFeatures);
    }

    // DirectWrite results
    wil::com_ptr<ID3D11RenderTargetView> renderTargetView;
    wil::com_ptr<ID3D11ShaderResourceView> d2dTextureView;
    wil::com_ptr<ID3D11ShaderResourceView> d3dTextureView;
    u32x2 tileSize;
    bool constantBufferInvalidated = true;

    for (;;) {
        WaitForSingleObjectEx(frameLatencyWaitableObject.get(), 10000, true);

        {
            bool done = false;
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT) {
                    done = true;
                }
            }
            if (done) {
                break;
            }
        }

        if (g_dpiChanged) {
            const auto scale = static_cast<f32>(g_dpi) / static_cast<f32>(USER_DEFAULT_SCREEN_DPI);
            auto& style = ImGui::GetStyle();

            style.FontSizeBase = 20.0f;
            style.FontScaleDpi = scale;

            ImGui_ImplDX11_InvalidateDeviceObjects();

            state.textChanged = true;
            g_dpiChanged = false;
        }

        ImGui_ImplWin32_NewFrame();
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2{ImGui::GetFontSize() * 30.0f, 0}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings")) {
            {
                state.textChanged |= ImGui::InputTextWithHint("Text", "DirectWrite", &state.textBuffer[0], std::size(state.textBuffer));
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
                if (ImGui::ComboWithFilter("Font Family", &state.fontNameIndex, state.fontNames, 20)) {
                    state.fontNameIndex = std::clamp(state.fontNameIndex, 0, static_cast<int>(state.fontNames.size()) - 1);
                    const auto fontNameUTF16 = u8u16(state.fontNames[state.fontNameIndex]);
                    get_font_axes_and_features(dwriteFactory.get(), fontCollection.get(), fontNameUTF16.c_str(), ImDWriteGetUserDefaultLocaleName(), state.fontAxes, state.fontFeatures);
                    state.textChanged = true;
                }
                state.textChanged |= ImGui::SliderFloat("Font Size (pt)", &state.fontSize, 1, 100, "%.1f");

                if (!state.fontAxes.empty() && ImGui::CollapsingHeader("Font Axes", ImGuiTreeNodeFlags_DefaultOpen)) {
                    for (auto& axis : state.fontAxes) {
                        ImGui::PushID(static_cast<int>(axis.range.axisTag));
                        state.textChanged |= ImGui::SliderFloat(axis.label.c_str(), &axis.value, axis.range.minValue, axis.range.maxValue, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                        ImGui::PopID();
                    }
                }

                if (!state.fontFeatures.empty() && ImGui::CollapsingHeader("OpenType Features (Default Script)")) {
                    static constexpr const char* featureValues[]{"Default", "Off", "On"};
                    for (auto& feature : state.fontFeatures) {
                        auto value = feature.value + 1;
                        ImGui::PushID(feature.tag);
                        if (ImGui::Combo(feature.label.c_str(), &value, featureValues, std::size(featureValues))) {
                            feature.value = value - 1;
                            state.textChanged = true;
                        }
                        ImGui::PopID();
                    }
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
                static constexpr const char* modes[] = {
                    "DWrite Grayscale",
                    "DWrite ClearType",
                    "Primitive Copy",
                    "DWrite Grayscale (sRGB)",
                    "DWrite ClearType (sRGB)",
                    "Primitive Copy (sRGB)",
                };

                const auto count = IM_COUNTOF(modes);
                auto current = static_cast<int>(state.renderingMode) + 3 * state.renderingWithSrgb;
                current = std::min(current, count - 1);

                if (ImGui::Combo("Rendering Mode", &current, modes, count)) {
                    state.renderingMode = static_cast<BlendMode>(current % 3);
                    state.renderingWithSrgb = current >= 3;
                    state.textChanged = true;
                    g_viewportSizeChanged = true; // force recreation of render targets
                }
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
                if (ImGui::Checkbox("Use system DWrite settings", &state.dwriteUseSystemSettings)) {
                    constantBufferInvalidated = true;
                }

                ImGui::BeginDisabled(state.dwriteUseSystemSettings);
                bool dwriteSettingsChanged = false;
                ImGui::PushItemWidth(ImGui::GetFontSize() * 9.0f);
                dwriteSettingsChanged |= ImGui::SliderFloat("Gamma", &state.dwriteGamma, 1.0f, 2.2f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
                dwriteSettingsChanged |= ImGui::SliderFloat("ClearType Contrast", &state.dwriteCleartypeEnhancedContrast, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                dwriteSettingsChanged |= ImGui::SliderFloat("Grayscale Contrast", &state.dwriteGrayscaleEnhancedContrast, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::PopItemWidth();
                ImGui::EndDisabled();

                if (dwriteSettingsChanged) {
                    state.dwriteDirty = true;
                    constantBufferInvalidated = true;
                }

                ImGui::BeginDisabled(state.dwriteUseSystemSettings);
                if (ImGui::Button("Restore Defaults")) {
                    state.dwriteGamma = defaultGamma;
                    state.dwriteCleartypeEnhancedContrast = defaultCleartypeEnhancedContrast;
                    state.dwriteGrayscaleEnhancedContrast = defaultGrayscaleEnhancedContrast;
                    state.dwriteDirty = true;
                    constantBufferInvalidated = true;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!state.dwriteDirty);
                if (ImGui::Button("Save to Registry")) {
                    save_dwrite_settings(state.dwriteGamma, state.dwriteCleartypeEnhancedContrast, state.dwriteGrayscaleEnhancedContrast);
                    DWrite_GetRenderParams(dwriteFactory.get(), &systemGamma, &systemCleartypeEnhancedContrast, &systemGrayscaleEnhancedContrast, linearParams.put());
                    THROW_IF_FAILED(dwriteFactory->CreateRenderingParams(reinterpret_cast<IDWriteRenderingParams**>(systemParams.put())));
                    state.dwriteDirty = false;
                    state.textChanged = true;
                    constantBufferInvalidated = true;
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
            }
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            {
                state.textChanged |= ImGui::ColorPicker4("Background", state.background);
                ImGui::Spacing();
                state.textChanged |= ImGui::ColorPicker4("Foreground", state.foreground);
            }
        }
        ImGui::End();

        {
            const auto viewport = ImGui::GetMainViewport();
            const auto padding = ImGui::GetFontSize();
            const auto right = viewport->Pos.x + viewport->Size.x - padding;

            draw_overlay_label("##direct2d", "Direct2D", {right, viewport->Pos.y + padding});
            draw_overlay_label("##hlsl", "HLSL shader", {right, viewport->Pos.y + viewport->Size.y * 0.5f + padding});
        }

        ImGui::Render();

        if (state.textChanged) {
            state.fontNameIndex = std::clamp(state.fontNameIndex, 0, static_cast<int>(state.fontNames.size()) - 1);
            state.fontSize = std::clamp(state.fontSize, 0.1f, 1000.0f);

            const auto scale = static_cast<f32>(g_dpi) / static_cast<f32>(USER_DEFAULT_SCREEN_DPI);
            const auto fontNameUTF16 = u8u16(state.fontNames[state.fontNameIndex]);
            const auto fontSizeInDIP = state.fontSize * USER_DEFAULT_SCREEN_DPI / 72.0f;

            size_t textLength = strnlen_s(&state.textBuffer[0], std::size(state.textBuffer));
            auto wideText = u8u16({&state.textBuffer[0], textLength});
            if (wideText.empty()) {
                wideText = L"DirectWrite";
            }

            wil::com_ptr<IDWriteTextFormat> textFormat;
            THROW_IF_FAILED(dwriteFactory->CreateTextFormat(fontNameUTF16.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSizeInDIP, ImDWriteGetUserDefaultLocaleName(), textFormat.addressof()));

            wil::com_ptr<IDWriteTextLayout> textLayout;
            THROW_IF_FAILED(dwriteFactory->CreateTextLayout(wideText.c_str(), static_cast<UINT32>(wideText.size()), textFormat.get(), INFINITY, INFINITY, textLayout.addressof()));

            const DWRITE_TEXT_RANGE textRange{0, static_cast<UINT32>(wideText.size())};
            if (!state.fontAxes.empty()) {
                std::vector<DWRITE_FONT_AXIS_VALUE> values;
                values.reserve(state.fontAxes.size());
                for (const auto& axis : state.fontAxes) {
                    values.push_back({axis.range.axisTag, axis.value});
                }

                const auto textLayout4 = textLayout.query<IDWriteTextLayout4>();
                THROW_IF_FAILED(textLayout4->SetFontAxisValues(values.data(), static_cast<UINT32>(values.size()), textRange));
            }

            wil::com_ptr<IDWriteTypography> typography;
            for (const auto& feature : state.fontFeatures) {
                if (feature.value >= 0) {
                    if (!typography) {
                        THROW_IF_FAILED(dwriteFactory->CreateTypography(typography.addressof()));
                    }
                    const DWRITE_FONT_FEATURE fontFeature{feature.tag, static_cast<UINT32>(feature.value)};
                    THROW_IF_FAILED(typography->AddFontFeature(fontFeature));
                }
            }
            if (typography) {
                THROW_IF_FAILED(textLayout->SetTypography(typography.get(), textRange));
            }

            {
                DWRITE_TEXT_METRICS metrics;
                THROW_IF_FAILED(textLayout->GetMetrics(&metrics));

                tileSize.x = static_cast<u32>(std::ceil(metrics.widthIncludingTrailingWhitespace * scale));
                tileSize.y = static_cast<u32>(std::ceil(metrics.height * scale));
            }

            wil::com_ptr<ID2D1RenderTarget> d2dTextureRenderTarget;
            wil::com_ptr<ID2D1RenderTarget> d3dTextureRenderTarget;
            create_d2d_render_target_texture(device.get(), d2dFactory.get(), state.renderingWithSrgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM, tileSize.x, tileSize.y, g_dpi, d2dTextureRenderTarget.addressof(), d2dTextureView.put());
            create_d2d_render_target_texture(device.get(), d2dFactory.get(), DXGI_FORMAT_B8G8R8A8_UNORM, tileSize.x, tileSize.y, g_dpi, d3dTextureRenderTarget.addressof(), d3dTextureView.put());

            if (state.renderingMode == BlendMode::DWriteClearType) {
                d2dTextureRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
                d3dTextureRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            }

            d2dTextureRenderTarget->SetTextRenderingParams(systemParams.get());
            d3dTextureRenderTarget->SetTextRenderingParams(linearParams.get());

            {
                const auto b = state.renderingWithSrgb ? srgb_to_linear(state.background) : state.background;
                const auto f = state.renderingWithSrgb ? srgb_to_linear(state.foreground) : state.foreground;
                wil::com_ptr<ID2D1SolidColorBrush> foregroundBrush;
                THROW_IF_FAILED(d2dTextureRenderTarget->CreateSolidColorBrush(&as_d2d_color(f), nullptr, foregroundBrush.addressof()));

                d2dTextureRenderTarget->BeginDraw();
                d2dTextureRenderTarget->Clear(&as_d2d_color(b));
                d2dTextureRenderTarget->DrawTextLayout({}, textLayout.get(), foregroundBrush.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                THROW_IF_FAILED(d2dTextureRenderTarget->EndDraw());
            }

            {
                static constexpr D2D1_COLOR_F color{1, 1, 1, 1};
                wil::com_ptr<ID2D1SolidColorBrush> brush;
                THROW_IF_FAILED(d3dTextureRenderTarget->CreateSolidColorBrush(&color, nullptr, brush.addressof()));

                d3dTextureRenderTarget->BeginDraw();
                d3dTextureRenderTarget->Clear();
                d3dTextureRenderTarget->DrawTextLayout({}, textLayout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
                THROW_IF_FAILED(d3dTextureRenderTarget->EndDraw());
            }

            constantBufferInvalidated = true;
            state.textChanged = false;
        }

        if (g_viewportSizeChanged) {
            // ResizeBuffer() docs:
            //   Before you call ResizeBuffers, ensure that the application releases all references [...].
            //   You can use ID3D11DeviceContext::ClearState to ensure that all [internal] references are released.
            renderTargetView.reset();
            deviceContext->ClearState();
            deviceContext->Flush();
            swapChain->ResizeBuffers(0, g_viewportSize.x, g_viewportSize.y, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);

            wil::com_ptr<ID3D11Texture2D> buffer;
            THROW_IF_FAILED(swapChain->GetBuffer(0, __uuidof(buffer), buffer.put_void()));

            const D3D11_RENDER_TARGET_VIEW_DESC desc{
                .Format = state.renderingWithSrgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM,
                .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D,
            };
            THROW_IF_FAILED(device->CreateRenderTargetView(buffer.get(), &desc, renderTargetView.put()));
            deviceContext->OMSetRenderTargets(1, renderTargetView.addressof(), nullptr);

            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<f32>(g_viewportSize.x);
            viewport.Height = static_cast<f32>(g_viewportSize.y);
            deviceContext->RSSetViewports(1, &viewport);

            constantBufferInvalidated = true;
            g_viewportSizeChanged = false;
        }

        if (constantBufferInvalidated) {
            ConstantBuffer data;
            data.splitPos.x = g_viewportSize.y / 2;
            data.splitPos.y = data.splitPos.x + 4;
            data.tileSize = tileSize;
            data.background = premultiply_color(state.background);
            data.foreground = premultiply_color(state.foreground);
            const auto effectiveGamma = state.dwriteUseSystemSettings ? systemGamma : state.dwriteGamma;
            data.cleartypeEnhancedContrast = state.dwriteUseSystemSettings ? systemCleartypeEnhancedContrast : state.dwriteCleartypeEnhancedContrast;
            data.grayscaleEnhancedContrast = state.dwriteUseSystemSettings ? systemGrayscaleEnhancedContrast : state.dwriteGrayscaleEnhancedContrast;
            data.mode = state.renderingMode;

            if (state.renderingWithSrgb) {
                data.background = srgb_to_linear(data.background);
                data.foreground = srgb_to_linear(data.foreground);
                DWrite_GetGammaRatiosForLinearTarget(effectiveGamma, data.gammaRatios);
            } else {
                DWrite_GetGammaRatiosForEncodedTarget(effectiveGamma, data.gammaRatios);
            }

            deviceContext->UpdateSubresource(constantBuffer.get(), 0, nullptr, &data, 0, 0);
            constantBufferInvalidated = false;
        }

        {
            // Our vertex shader uses a trick from Bill Bilodeau published in "Vertex Shader Tricks"
            // at GDC14 to draw a fullscreen triangle without vertex/index buffers.
            deviceContext->IASetInputLayout(nullptr);
            deviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
            deviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
            deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            deviceContext->VSSetShader(vertexShader.get(), nullptr, 0);

            deviceContext->PSSetShader(pixelShader.get(), nullptr, 0);
            deviceContext->PSSetConstantBuffers(0, 1, constantBuffer.addressof());
            std::array resourceViews{d2dTextureView.get(), d3dTextureView.get()};
            deviceContext->PSSetShaderResources(0, static_cast<UINT>(resourceViews.size()), resourceViews.data());

            deviceContext->OMSetRenderTargets(1, renderTargetView.addressof(), nullptr);
            deviceContext->Draw(3, 0);
        }

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        swapChain->Present(1, 0);
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd) {
    try {
        winMainImpl(hInstance, hPrevInstance, lpCmdLine, nShowCmd);
    } catch (const wil::ResultException& e) {
        std::array<wchar_t, 2048> message;
        wil::GetFailureLogString(message.data(), message.size(), e.GetFailureInfo());
        MessageBoxW(nullptr, &message[0], L"Exception", MB_ICONERROR | MB_OK);
    } catch (...) {
        LOG_CAUGHT_EXCEPTION();
    }

    return 0;
}
