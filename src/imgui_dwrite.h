#pragma once

struct ImFontAtlas;
struct ImFontConfig;
struct ImFontLoader;

const wchar_t* ImDWriteGetUserDefaultLocaleName();
ImFontLoader* ImDWriteCreateFontLoader();
void ImDWriteAddFontFamily(ImFontAtlas* atlas, const char* family);
