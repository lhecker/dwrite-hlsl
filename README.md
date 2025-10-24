# dwrite-hlsl

This project demonstrates how to blend text in a HLSL shader and have it look like native DirectWrite.
It implements both, ClearType (`D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE`) and grayscale (`D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE`) anti-aliasing.

## License

This project is an extract of [Windows Terminal](https://github.com/microsoft/terminal). See [LICENSE](./LICENSE).

## Missing features

* Emojis<br>
  Emojis are luckily very simple to draw and I'll add support later:
  * Draw them with grayscale antialiasing (and not ClearType).
  * _Don't_ use linear gamma space (meaning: `DWrite_GetRenderParams`). Instead call `SetTextRenderingParams(nullptr)` before using the rendering target, to reset it to the regular, gamma corrected parameters.
  * Do a simple premultiplied alpha blend with the Emoji's RGBA values on your background color in your shader.
* This demo doesn't actually use a glyph atlas and only shows the "blending" part of the algorithm.

## How to use this

* [dwrite.hlsl](./src/dwrite.hlsl) contains all of the shader functions relevant for grayscale-antialiased alpha blending. `DWrite_GetGrayScaleCorrectedAlpha` is the entrypoint function that you need to call in your shader.
* [dwrite.cpp](./src/dwrite.cpp) contains support functions which are required to fill out the parameters for `DWrite_GetGrayScaleCorrectedAlpha`.
* Draw your glyphs with DirectWrite into your texture atlas however you like them. These are the approaches I'm aware about:
  * [`ID2D1RenderTarget::DrawTextLayout`](https://docs.microsoft.com/en-us/windows/win32/api/d2d1/nf-d2d1-id2d1rendertarget-drawtextlayout)<br>
    Direct2D provides various different kinds of render targets. It handles font fallback, provides you with metrics, etc. and is simple to use. However it's not particularly configurable, not the most performant solution and uses extra GPU/CPU memory for Direct2D's internal glyph atlas. This demo application uses this approach.
  * [Implement your own custom `IDWriteTextRenderer`](https://docs.microsoft.com/en-us/windows/win32/directwrite/how-to-implement-a-custom-text-renderer)<br>
    Personally I'm not sure about the value of this approach. I feel like it's the worst of both worlds, as it still ties you into the Direct2D ecosystem and allows you to only marginally improve performance, while simultaneously being very difficult to implement. I'd not suggest anyone to use this.
  * [`ID2D1RenderTarget::DrawGlyphRun`](https://learn.microsoft.com/en-us/windows/win32/api/d2d1/nf-d2d1-id2d1rendertarget-drawglyphrun)<br>
    This is Direct2D's low level API, which ties better into DirectWrite. This API is preferable if you're implementing a glyph atlas, because it allows you to cache individual glyphs that you got from `IDWriteTextAnalyzer`. Here's what you need to call when using DirectWrite:
    * [`IDWriteFontFallback::MapCharacters`](https://docs.microsoft.com/en-us/windows/win32/directwrite/idwritefontfallback-mapcharacters)
    * [`IDWriteTextAnalyzer::AnalyzeBidi`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-analyzebidi)
    * [`IDWriteTextAnalyzer::AnalyzeScript`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-analyzescript)
    * [`IDWriteTextAnalyzer1::GetTextComplexity`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite_1/nf-dwrite_1-idwritetextanalyzer1-gettextcomplexity) (optionally; allows you to skip `GetGlyphs`)
    * [`IDWriteTextAnalyzer::GetGlyphs`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-getglyphs)
    * [`IDWriteTextAnalyzer::GetGlyphPlacements`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-getglyphplacements)
    
    You can call [`ID2D1DeviceContext::GetGlyphRunWorldBounds`](https://learn.microsoft.com/en-us/windows/win32/api/d2d1_1/nf-d2d1_1-id2d1devicecontext-getglyphrunworldbounds) before calling `DrawGlyphRun` to get pixel-precise boundaries for the given glyph. Unfortunately, `GetGlyphRunWorldBounds` only works reliably for regular OpenType glyphs and not for SVG layers in COLRv0 emojis or for COLRv1 in general.
  * [`IDWriteGlyphRunAnalysis::CreateAlphaTexture`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwriteglyphrunanalysis-createalphatexture)<br>
    This is the lowest level approach that is technically the best. It's what every serious DirectWrite application uses, including libraries like skia. It straight up yields rasterized glyphs and allows you to do your own anti-aliasing. Don't be fooled by `DWRITE_TEXTURE_ALIASED_1x1`: It yields grayscale textures (allegedly). It effectively replaces the `DrawGlyphRun` call in the previous point, but if you just replace it 1:1 you might notice a reduction in performance. This is because Direct2D internally uses a pool of upload heaps to efficiently send batches of glyphs up to the GPU memory. Preferably, you'd do something similar.
