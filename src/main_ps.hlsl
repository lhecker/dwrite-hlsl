// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "dwrite.hlsl"

cbuffer ConstBuffer : register(b0)
{
    uint2 splitPos;
    uint2 tileSize;
    float4 background;
    float4 foreground;
    float4 gammaRatios;
    float grayscaleEnhancedContrast;
};

// d2dTexture stores text/glyphs as drawn by Direct2D natively.
// d3dTexture stores the glyphs as a regular alpha texture which we
// still need to blend. Technically d3dTexture could be stored as A8
// instead of B8G8R8A8, but this doesn't matter much for this demo.
Texture2D<float4> d2dTexture : register(t0);
Texture2D<float4> d3dTexture : register(t1);

float4 alphaBlendPremultiplied(float4 bottom, float4 top)
{
    float ia = 1 - top.a;
    return float4(bottom.rgb * ia + top.rgb, bottom.a * ia + top.a);
}

// clang-format off
float4 main(float4 pos: SV_Position): SV_Target
// clang-format on
{
    uint2 upos = uint2(pos.xy);
    bool used3d = upos.y >= splitPos.y;

    if (used3d)
    {
        upos.y -= splitPos.y;
    }

    // We'll wrap d2d/d3dTexture coordinates until it it fills the the entire screen.
    // We add +1 because the right and lower border of each tile will get a grid-line.
    uint2 tilePos = upos % (tileSize + 1);

    // The texture drawn with D2D will be shown as it is.
    // It's already blended with the background and is for comparison only.
    float4 d2dColor = d2dTexture[tilePos];

    // This applies the internal DirectWrite alpha blending algorithm.
    float correctedAlpha = DWrite_GetGrayscaleCorrectedAlpha(gammaRatios, grayscaleEnhancedContrast, false, foreground.rgb / foreground.a, d3dTexture[tilePos].a);
    float4 d3dColor = alphaBlendPremultiplied(background, foreground * correctedAlpha);

    // It's technically not necessary to compute both d2dColor and d3dColor,
    // but it's also not very expensive and I'll probably add a diff mode at some point.
    float4 color = used3d ? d3dColor : d2dColor;

    // This draws the thin grid-line between tiles.
    if (tilePos.x == tileSize.x)
    {
        bool c = (upos.y & 7) < 4;
        color = float4(c, c, c, 1.0f);
    }
    if (tilePos.y == tileSize.y)
    {
        bool c = (upos.x & 7) < 4;
        color = float4(c, c, c, 1.0f);
    }

    // This draws the thick separator between the upper and lower half of the screen.
    if (upos.y >= splitPos.x)
    {
        bool c = (upos.x & 31) < 16;
        color = float4(c, c, c, 1.0f);
    }

    return color;
}
