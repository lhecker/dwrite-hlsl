#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <dwrite_3.h>

#include "imgui_dwrite.h"
#include "imgui_internal.h"

namespace {

#define RETURN_FALSE_IF(cond) \
    do {                      \
        if (cond) {           \
            IM_ASSERT(false); \
            return false;     \
        }                     \
    } while (0)

#define RETURN_FALSE_IF_FAILED(hr) \
    RETURN_FALSE_IF(FAILED(hr))

template<typename T>
struct com_ptr {
    ~com_ptr() {
        if (m_ptr) {
            m_ptr->Release();
        }
    }

    T* get() const {
        return m_ptr;
    }

    T* operator->() const {
        return m_ptr;
    }

    T** addressof() {
        return &m_ptr;
    }

private:
    T* m_ptr = nullptr;
};

template<typename T>
struct unique_im {
    unique_im(T* ptr) : m_ptr(ptr) {
    }

    ~unique_im() {
        IM_DELETE(m_ptr);
    }

    T* operator->() {
        return m_ptr;
    }

    T* detach() {
        auto ptr = m_ptr;
        m_ptr = nullptr;
        return ptr;
    }

private:
    T* m_ptr = nullptr;
};

struct TextAnalysisSource final : IDWriteTextAnalysisSource {
    TextAnalysisSource(const wchar_t* locale, const wchar_t* text, const UINT32 textLength) noexcept : m_locale{locale}, m_text{text}, m_text_length{textLength} {
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return 1;
    }

    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return 1;
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(const IID& riid, void** ppvObject) noexcept override {
        if (IsEqualGUID(riid, __uuidof(IDWriteTextAnalysisSource))) {
            *ppvObject = this;
            return S_OK;
        }

        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 textPosition, const WCHAR** textString, UINT32* textLength) noexcept override {
        textPosition = ImMin(textPosition, m_text_length);
        *textString = m_text + textPosition;
        *textLength = m_text_length - textPosition;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 textPosition, const WCHAR** textString, UINT32* textLength) noexcept override {
        textPosition = ImMin(textPosition, m_text_length);
        *textString = m_text;
        *textLength = textPosition;
        return S_OK;
    }

    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() noexcept override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 textPosition, UINT32* textLength, const WCHAR** localeName) noexcept override {
        *textLength = m_text_length - textPosition;
        *localeName = m_locale;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 textPosition, UINT32* textLength, IDWriteNumberSubstitution** numberSubstitution) noexcept override {
        return E_NOTIMPL;
    }

private:
    const wchar_t* m_locale;
    const wchar_t* m_text;
    UINT32 m_text_length;
};

struct dwrite_font_source {
    com_ptr<IDWriteFactory3> factory;
    com_ptr<IDWriteRenderingParams> rendering_params;
    com_ptr<IDWriteFontFace3> font_face;
    com_ptr<IDWriteFontCollection> font_collection;
    com_ptr<IDWriteFontFallback1> font_fallback;
    wchar_t family_name[IM_COUNTOF(ImFontConfig::Name)];
    float em_per_unit;
    float ascent;  // em
    float descent; // em
};

bool font_src_init(ImFontAtlas*, ImFontConfig* src) {
    unique_im data{IM_NEW(dwrite_font_source)};

    RETURN_FALSE_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(data->factory.addressof())));
    RETURN_FALSE_IF_FAILED(data->factory->CreateRenderingParams(data->rendering_params.addressof()));
    RETURN_FALSE_IF_FAILED(data->factory->GetSystemFontCollection(data->font_collection.addressof(), FALSE));

    com_ptr<IDWriteFontFallback> font_fallback;
    RETURN_FALSE_IF_FAILED(data->factory->GetSystemFontFallback(font_fallback.addressof()));
    RETURN_FALSE_IF_FAILED(font_fallback->QueryInterface(data->font_fallback.addressof()));

    const auto name_utf8_len = static_cast<int>(strnlen(src->Name, IM_COUNTOF(src->Name)));
    const auto name_utf16_len = MultiByteToWideChar(CP_UTF8, 0, src->Name, name_utf8_len, &data->family_name[0], IM_COUNTOF(data->family_name) - 1);
    RETURN_FALSE_IF(name_utf16_len <= 0);
    data->family_name[name_utf16_len] = L'\0';

    UINT32 family_index;
    BOOL family_exists;
    RETURN_FALSE_IF_FAILED(data->font_collection->FindFamilyName(&data->family_name[0], &family_index, &family_exists));
    RETURN_FALSE_IF(!family_exists);

    com_ptr<IDWriteFontFamily> font_family;
    com_ptr<IDWriteFont> font;
    com_ptr<IDWriteFontFace> font_face;
    RETURN_FALSE_IF_FAILED(data->font_collection->GetFontFamily(family_index, font_family.addressof()));
    RETURN_FALSE_IF_FAILED(font_family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.addressof()));
    RETURN_FALSE_IF_FAILED(font->CreateFontFace(font_face.addressof()));
    RETURN_FALSE_IF_FAILED(font_face->QueryInterface(data->font_face.addressof()));

    DWRITE_FONT_METRICS metrics;
    data->font_face->GetMetrics(&metrics);

    const auto ascent = static_cast<float>(metrics.ascent);
    const auto descent = static_cast<float>(metrics.descent);
    const auto half_line_gap = static_cast<float>(metrics.lineGap) * 0.5f;
    const auto em_per_unit = 1.0f / static_cast<float>(metrics.designUnitsPerEm);
    data->em_per_unit = em_per_unit;
    data->ascent = (ascent + half_line_gap) * em_per_unit;
    data->descent = (descent + half_line_gap) * em_per_unit;

    src->FontLoaderData = data.detach();
    return true;
}

void font_src_destroy(ImFontAtlas*, ImFontConfig* src) {
    IM_DELETE(static_cast<dwrite_font_source*>(src->FontLoaderData));
    src->FontLoaderData = nullptr;
}

bool font_src_contains_glyph(ImFontAtlas*, ImFontConfig* src, ImWchar codepoint) {
    const auto data = static_cast<dwrite_font_source*>(src->FontLoaderData);
    const UINT32 codepoint32 = codepoint;
    UINT16 glyph_index;
    return SUCCEEDED(data->font_face->GetGlyphIndicesW(&codepoint32, 1, &glyph_index)) && glyph_index != 0;
}

bool font_baked_init(ImFontAtlas*, ImFontConfig* src, ImFontBaked* baked, void*) {
    if (!src->MergeMode) {
        const auto data = static_cast<dwrite_font_source*>(src->FontLoaderData);
        baked->Ascent = roundf(baked->Size * data->ascent / (data->ascent + data->descent));
        baked->Descent = baked->Ascent - baked->Size;
    }
    return true;
}

UINT16 map_codepoint_with_fallback(dwrite_font_source* data, ImWchar codepoint, IDWriteFontFace3** font_face) {
    const UINT32 codepoint32 = codepoint;
    UINT16 glyph_index;
    if (FAILED(data->font_face->GetGlyphIndicesW(&codepoint32, 1, &glyph_index))) {
        return 0;
    }
    if (glyph_index != 0) {
        data->font_face->AddRef();
        *font_face = data->font_face.get();
        return glyph_index;
    }

#ifdef IMGUI_USE_WCHAR32
    wchar_t text[2];
    UINT32 text_length;
    if (codepoint > 0xFFFF) {
        text[0] = static_cast<wchar_t>(0xD800 + ((codepoint - 0x10000) >> 10));
        text[1] = static_cast<wchar_t>(0xDC00 + ((codepoint - 0x10000) & 0x3FF));
        text_length = 2;
    } else {
        text[0] = static_cast<wchar_t>(codepoint);
        text_length = 1;
    }
#else
    const auto text = reinterpret_cast<const wchar_t*>(&codepoint);
    constexpr UINT32 text_length = 1;
#endif

    TextAnalysisSource analysis_source{ImDWriteGetUserDefaultLocaleName(), text, text_length};
    UINT32 mapped_length;
    float scale;
    IDWriteFontFace5* font_face5;
    if (FAILED(data->font_fallback->MapCharacters(
            /* analysisSource     */ &analysis_source,
            /* textPosition       */ 0,
            /* textLength         */ text_length,
            /* baseFontCollection */ data->font_collection.get(),
            /* baseFamilyName     */ &data->family_name[0],
            /* fontAxisValues     */ nullptr,
            /* fontAxisValueCount */ 0,
            /* mappedLength       */ &mapped_length,
            /* scale              */ &scale,
            /* mappedFontFace     */ &font_face5
        ))) {
        return 0;
    }

    // NOTE: MapCharacters() can return S_OK and yet mappedFontFace can be null.
    // This indicates that no font contains the requested codepoint.
    if (!font_face5 || FAILED(font_face5->GetGlyphIndicesW(&codepoint32, 1, &glyph_index))) {
        glyph_index = 0;
    }

    *font_face = font_face5;
    return glyph_index;
}

bool font_baked_load_glyph(ImFontAtlas* atlas, ImFontConfig* src, ImFontBaked* baked, void*, ImWchar codepoint, ImFontGlyph* out_glyph, float* out_advance_x) {
    const auto data = static_cast<dwrite_font_source*>(src->FontLoaderData);

    com_ptr<IDWriteFontFace3> font_face;
    const auto glyph_index = map_codepoint_with_fallback(data, codepoint, font_face.addressof());
    if (glyph_index == 0) {
        return false;
    }

    DWRITE_GLYPH_METRICS glyph_metrics;
    RETURN_FALSE_IF_FAILED(font_face->GetDesignGlyphMetrics(&glyph_index, 1, &glyph_metrics, FALSE));

    const auto font_size = baked->Size / (data->ascent + data->descent);
    const auto advance_x = roundf(static_cast<float>(glyph_metrics.advanceWidth) * data->em_per_unit * font_size);

    if (out_advance_x) {
        *out_advance_x = advance_x;
        return true;
    }

    static constexpr DWRITE_GLYPH_OFFSET zero_offset{};
    const DWRITE_GLYPH_RUN glyph_run{
        .fontFace = font_face.get(),
        .fontEmSize = font_size,
        .glyphCount = 1,
        .glyphIndices = &glyph_index,
        .glyphAdvances = &advance_x,
        .glyphOffsets = &zero_offset,
        .isSideways = FALSE,
        .bidiLevel = 0,
    };

    DWRITE_RENDERING_MODE1 renderingMode;
    DWRITE_GRID_FIT_MODE gridFitMode;
    RETURN_FALSE_IF_FAILED(font_face->GetRecommendedRenderingMode(
        /* fontEmSize       */ glyph_run.fontEmSize,
        /* dpiX             */ 96.0f,
        /* dpiY             */ 96.0f,
        /* transform        */ nullptr,
        /* isSideways       */ FALSE,
        /* outlineThreshold */ DWRITE_OUTLINE_THRESHOLD_ANTIALIASED,
        /* measuringMode    */ DWRITE_MEASURING_MODE_NATURAL,
        /* renderingParams  */ data->rendering_params.get(),
        /* renderingMode    */ &renderingMode,
        /* gridFitMode      */ &gridFitMode
    ));

    com_ptr<IDWriteGlyphRunAnalysis> analysis;
    RETURN_FALSE_IF_FAILED(data->factory->CreateGlyphRunAnalysis(
        &glyph_run,
        nullptr,
        renderingMode,
        DWRITE_MEASURING_MODE_NATURAL,
        gridFitMode,
        DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
        0,
        baked->Ascent,
        analysis.addressof()
    ));

    RECT bounds;
    RETURN_FALSE_IF_FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds));

    out_glyph->Codepoint = codepoint;
    out_glyph->AdvanceX = advance_x;

    if (IsRectEmpty(&bounds)) {
        return true;
    }

    const auto width = bounds.right - bounds.left;
    const auto height = bounds.bottom - bounds.top;

    ImVector<ImU8> pixels;
    pixels.resize(width * height);
    RETURN_FALSE_IF_FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, pixels.Data, pixels.Size));

    const auto pack_id = ImFontAtlasPackAddRect(atlas, width, height);
    RETURN_FALSE_IF(pack_id == ImFontAtlasRectId_Invalid);

    const auto offset_x = src->GlyphOffset.x;
    const auto offset_y = src->GlyphOffset.y;
    out_glyph->X0 = static_cast<float>(bounds.left) + offset_x;
    out_glyph->Y0 = static_cast<float>(bounds.top) + offset_y;
    out_glyph->X1 = static_cast<float>(bounds.right) + offset_x;
    out_glyph->Y1 = static_cast<float>(bounds.bottom) + offset_y;
    out_glyph->Visible = true;
    out_glyph->PackId = pack_id;

    const auto rect = ImFontAtlasPackGetRect(atlas, pack_id);
    ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out_glyph, rect, pixels.Data, ImTextureFormat_Alpha8, width);
    return true;
}

} // namespace

const wchar_t* ImDWriteGetUserDefaultLocaleName() {
    static wchar_t localeName[LOCALE_NAME_MAX_LENGTH];

    if (localeName[0] == L'\0') {
        if (!GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH)) {
            static constexpr wchar_t fallbackLocale[]{L"en-US"};
            memcpy(&localeName[0], &fallbackLocale[0], sizeof(fallbackLocale));
        }
    }

    return &localeName[0];
}

ImFontLoader* ImDWriteCreateFontLoader() {
    static auto loader = [] {
        ImFontLoader loader;
        loader.Name = "DirectWrite";
        loader.FontSrcInit = font_src_init;
        loader.FontSrcDestroy = font_src_destroy;
        loader.FontSrcContainsGlyph = font_src_contains_glyph;
        loader.FontBakedInit = font_baked_init;
        loader.FontBakedDestroy = nullptr;
        loader.FontBakedLoadGlyph = font_baked_load_glyph;
        return loader;
    }();
    return &loader;
}

void ImDWriteAddFontFamily(ImFontAtlas* atlas, const char* family) {
    ImFontConfig config;
    strcpy_s(config.Name, family);
    config.FontLoader = ImDWriteCreateFontLoader();
    atlas->AddFont(&config);
}
