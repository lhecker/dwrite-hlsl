# dwrite-hlsl

This project demonstrates how to blend text in a HLSL shader and have it look like native DirectWrite.

## License

This project is an extract of [Windows Terminal](https://github.com/microsoft/terminal). See [LICENSE](./LICENSE).

## Implemented features

* Correctly blending pre-rendered glyphs from a texture atlas with RGBA background and foreground colors in a shader and have it look identical to native DirectWrite/Direct2D.
* Support for grayscale-antialiased Glyphs

## Missing features

* ClearType<br>
  I'll add support for this at a later point in time.
* Emojis<br>
  Emojis are luckily very simple to draw and I'll add support later:
  * Draw them with grayscale antialiasing (and not ClearType).
  * _Don't_ use linear gamma space (meaning: `DWrite_GetRenderParams`). Instead call `SetTextRenderingParams(nullptr)` before using the rendering target, to reset it to the regular, gamma corrected parameters.
  * Do a simple premultiplied alpha blend with the Emoji's RGBA values on your background color in your shader.
* This demo doesn't actually use a glyph atlas and only shows the "blending" part of the algorithm.

## How to use this

* [dwrite.hlsl](./src/dwrite.hlsl) contains all of the shader functions relevant for grayscale-antialiased alpha blending. `DWrite_GetGrayScaleCorrectedAlpha` is the entrypoint function that you need to call in your shader.
* [dwrite.cpp](./src/dwrite.cpp) contains support functions which are required to fill out the parameters for `DWrite_GetGrayScaleCorrectedAlpha`.
* Draw your glyphs with DirectWrite into your texture atlas however you like them. Basically there are 3 ways to do this:
  * [`ID2D1RenderTarget::DrawTextLayout`](https://docs.microsoft.com/en-us/windows/win32/api/d2d1/nf-d2d1-id2d1rendertarget-drawtextlayout)<br>
    Direct2D provides various different kinds of render targets. It handles font fallback, provides you with metrics, etc. and is simple to use. However it's not particularly configurable, not the most performant solution and uses extra GPU/CPU memory for Direct2D's internal glyph atlas. This demo application uses this approach.
  * [Implement your own custom `IDWriteTextRenderer`](https://docs.microsoft.com/en-us/windows/win32/directwrite/how-to-implement-a-custom-text-renderer)<br>
    Personally I'm not sure about the value of this approach. I feel like it's the worst of both worlds, as it still ties you into the Direct2D ecosystem and allows you to only marginally improve performance, while simultaneously being very difficult to implement. I'd not suggest anyone to use this.
  * [`IDWriteGlyphRunAnalysis::CreateAlphaTexture`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwriteglyphrunanalysis-createalphatexture)<br>
    This is the most performant and also most complex approach, but it's what every serious DirectWrite application uses, including libraries like skia. It straight up yields rasterized glyphs and allows you to do your own anti-aliasing. In order to call this function you need to segment and layout your input text yourself, by calling, as far as I know, _at a minimum_ in that order:
    * [`IDWriteFontFallback::MapCharacters`](https://docs.microsoft.com/en-us/windows/win32/directwrite/idwritefontfallback-mapcharacters)
    * [`IDWriteTextAnalyzer::AnalyzeBidi`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-analyzebidi)
    * [`IDWriteTextAnalyzer::AnalyzeScript`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-analyzescript)
    * [`IDWriteTextAnalyzer1::GetTextComplexity`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite_1/nf-dwrite_1-idwritetextanalyzer1-gettextcomplexity) (optionally; allows you to skip `GetGlyphs`)
    * [`IDWriteTextAnalyzer::GetGlyphs`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-getglyphs)
    * [`IDWriteTextAnalyzer::GetGlyphPlacements`](https://docs.microsoft.com/en-us/windows/win32/api/dwrite/nf-dwrite-idwritetextanalyzer-getglyphplacements)

    Grayscale antialiasing using `IDWriteGlyphRunAnalysis::CreateAlphaTexture` requires you to render the glyph at 4 times the actual pixel size in both directions (oversampling). Afterwards you can compute the alpha values of the glyph as a percentage of how many of the 16 oversampled pixels are present for each actual pixel. As we need to create the glyph texture in linear gamma space, the implementation can use a linear gradient for the alpha value. The complexity of calling all the text segmentation/layouting functions however isn't that great. But it's doable in a few hundred lines of code and I plan to add a custom glyph atlas implementation based on that in the future. In the meantime I'm using an approach based on `ID2D1RenderTarget::DrawTextLayout` myself.
