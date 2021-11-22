// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

// Exclude stuff from <Windows.h> we don't need.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <Windows.h>
#include <dwmapi.h>
#include <d2d1.h>
#include <d3d11_2.h>
#include <dwrite_1.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

// wil
#include <wil/com.h>
// imgui
#include <imgui.h>
#include <backends/imgui_impl_dx11.h>
#include <backends/imgui_impl_win32.h>
// our stuff
#include <main_vs.h>
#include <main_ps.h>

#include "dwrite.h"

static u32x2 g_viewportSize;
static bool g_viewportSizeChanged = true;

static UINT g_dpi = USER_DEFAULT_SCREEN_DPI;
static bool g_dpiChanged = true;

struct alignas(16) ConstantBuffer
{
    alignas(sizeof(u32x2)) u32x2 splitPos;
    alignas(sizeof(u32x2)) u32x2 tileSize;
    alignas(sizeof(f32x4)) f32x4 background;
    alignas(sizeof(f32x4)) f32x4 foreground;

    alignas(sizeof(f32x4)) f32x4 gammaRatios;
    alignas(sizeof(f32)) f32 grayscaleEnhancedContrast = 0;
};

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT
ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static std::wstring u8u16(const std::string_view& input)
{
    const auto wideLength = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0);
    THROW_LAST_ERROR_IF(wideLength <= 0);

    std::wstring wide(wideLength, L'\0');
    const auto result = MultiByteToWideChar(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), wide.data(), wideLength);
    THROW_LAST_ERROR_IF(result != wideLength);

    return wide;
}

static std::string u16u8(const std::wstring_view& input)
{
    const auto narrowLength = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    THROW_LAST_ERROR_IF(narrowLength <= 0);

    std::string narrow(narrowLength, L'\0');
    const auto result = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), narrow.data(), narrowLength, nullptr, nullptr);
    THROW_LAST_ERROR_IF(result != narrowLength);

    return narrow;
}

static f32x4 premultiplyColor(const f32x4& in) noexcept
{
    return { in.r * in.a, in.g * in.a, in.b * in.a, in.a };
}

static std::vector<std::string> getSystemFontNames(IDWriteFontCollection* fontCollection, const wchar_t* localeName)
{
    const auto count = fontCollection->GetFontFamilyCount();

    std::vector<std::wstring> wideFontNames;
    wideFontNames.reserve(count);

    for (UINT32 i = 0; i < count; ++i)
    {
        wil::com_ptr<IDWriteFontFamily> fontFamily;
        THROW_IF_FAILED(fontCollection->GetFontFamily(i, fontFamily.addressof()));

        wil::com_ptr<IDWriteLocalizedStrings> localizedFamilyNames;
        THROW_IF_FAILED(fontFamily->GetFamilyNames(localizedFamilyNames.addressof()));

        UINT32 index;
        BOOL exists = FALSE;

        THROW_IF_FAILED(localizedFamilyNames->FindLocaleName(localeName, &index, &exists));
        if (!exists)
        {
            THROW_IF_FAILED(localizedFamilyNames->FindLocaleName(L"en-US", &index, &exists));
        }
        if (!exists)
        {
            index = 0;
        }

        UINT32 length;
        THROW_IF_FAILED(localizedFamilyNames->GetStringLength(index, &length));

        std::wstring buffer(length, L'\0');
        THROW_IF_FAILED(localizedFamilyNames->GetString(index, buffer.data(), length + 1));

        wideFontNames.emplace_back(std::move(buffer));
    }

    std::sort(wideFontNames.begin(), wideFontNames.end(), [&](const auto& a, const auto& b) -> bool {
        return CompareStringEx(&localeName[0], 0, a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });

    std::vector<std::string> fontNames;
    fontNames.reserve(count);

    for (const auto& fontName : wideFontNames)
    {
        fontNames.emplace_back(u16u8(fontName));
    }

    return fontNames;
}

static const std::string* getFontNameRefFromCollection(const std::vector<std::string>& fontNames, const std::string_view& needle)
{
    for (const auto& fontName : fontNames)
    {
        if (fontName == needle)
        {
            return &fontName;
        }
    }
    return &fontNames[0];
}

static void updateDarkModeForWindow(HWND hwnd)
{
    static const auto ShouldAppsUseDarkMode = []() {
        const auto uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return uxtheme ? reinterpret_cast<bool(WINAPI*)()>(GetProcAddress(uxtheme, MAKEINTRESOURCEA(132))) : nullptr;
    }();
    static constexpr auto IsHighContrastOn = []() {
        bool highContrast = false;
        HIGHCONTRAST hc{ sizeof(hc) };
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0))
        {
            highContrast = (HCF_HIGHCONTRASTON & hc.dwFlags) != 0;
        }
        return highContrast;
    };

    if (ShouldAppsUseDarkMode)
    {
        const BOOL useDarkMode = ShouldAppsUseDarkMode() && !IsHighContrastOn();
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(BOOL));
    }
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
try
{
    if (const auto result = ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam))
    {
        return result;
    }

    switch (message)
    {
    case WM_SETTINGCHANGE:
        if (wParam == 0 && wcscmp(reinterpret_cast<const wchar_t*>(lParam), L"ImmersiveColorSet") == 0)
        {
            updateDarkModeForWindow(hWnd);
        }
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            g_viewportSize.x = LOWORD(lParam);
            g_viewportSize.y = HIWORD(lParam);
            g_viewportSizeChanged = true;
        }
        return 0;
    case WM_DPICHANGED:
    {
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

static void winMainImpl(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    wchar_t localeName[LOCALE_NAME_MAX_LENGTH];
    if (!GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH))
    {
        static constexpr wchar_t fallbackLocale[]{ L"en-US" };
        memcpy(&localeName[0], &fallbackLocale[0], sizeof(fallbackLocale));
    }

    // Win32 setup
    wil::unique_hwnd hwnd;
    {
        WNDCLASSEXW wcex;
        wcex.cbSize = sizeof(WNDCLASSEX);
        wcex.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wcex.lpfnWndProc = WndProc;
        wcex.cbClsExtra = 0;
        wcex.cbWndExtra = 0;
        wcex.hInstance = hInstance;
        wcex.hIcon = nullptr;
        wcex.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wcex.hbrBackground = nullptr;
        wcex.lpszMenuName = nullptr;
        wcex.lpszClassName = L"dwrite-hlsl";
        wcex.hIconSm = nullptr;
        RegisterClassExW(&wcex);

        hwnd.reset(THROW_LAST_ERROR_IF_NULL(CreateWindowExW(0L, L"dwrite-hlsl", L"dwrite-hlsl", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, 0, CW_USEDEFAULT, 0, nullptr, nullptr, hInstance, nullptr)));
        updateDarkModeForWindow(hwnd.get());

        g_dpi = GetDpiForWindow(hwnd.get());
    }

    // D2D / DirectWrite setup
    wil::com_ptr<ID2D1Factory> d2dFactory;
    wil::com_ptr<IDWriteFactory1> dwriteFactory;
    wil::com_ptr<IDWriteFontCollection> fontCollection;
    wil::com_ptr<IDWriteRenderingParams1> linearParams;
    f32 gamma = 1.8f;
    f32 grayscaleEnhancedContrast = 0.5f;
    {
        THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(d2dFactory), d2dFactory.put_void()));
        THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(dwriteFactory), dwriteFactory.put_unknown()));
        THROW_IF_FAILED(dwriteFactory->GetSystemFontCollection(fontCollection.addressof()));
        DWrite_GetRenderParams(dwriteFactory.get(), &gamma, &grayscaleEnhancedContrast, linearParams.addressof());
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
            /* pAdapter */ nullptr,
            /* DriverType */ D3D_DRIVER_TYPE_HARDWARE,
            /* Software */ nullptr,
            /* Flags */ D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG,
            /* pFeatureLevels */ &featureLevel,
            /* FeatureLevels */ 1,
            /* SDKVersion */ D3D11_SDK_VERSION,
            /* ppDevice */ device.put(),
            /* pFeatureLevel */ nullptr,
            /* ppImmediateContext */ deviceContext.put()));

        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_NONE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

        wil::com_ptr<IDXGIFactory2> dxgiFactory;
        THROW_IF_FAILED(CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.addressof())));
        THROW_IF_FAILED(dxgiFactory->CreateSwapChainForHwnd(device.get(), hwnd.get(), &desc, nullptr, nullptr, swapChain.put()));

        frameLatencyWaitableObject.reset(swapChain.query<IDXGISwapChain2>()->GetFrameLatencyWaitableObject());
    }
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(ConstantBuffer);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
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
    ImGui_ImplWin32_Init(hwnd.get());
    ImGui_ImplDX11_Init(device.get(), deviceContext.get());

    const auto cleanup = wil::scope_exit([]() {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    });

    // settings via ImGui
    const auto fontNames = getSystemFontNames(fontCollection.get(), &localeName[0]);
    auto selectedFontName = getFontNameRefFromCollection(fontNames, "Consolas");
    int fontSize = 12;
    f32x4 background{ 0.0f, 0.0f, 0.0f, 1.0f };
    f32x4 foreground{ 1.0f, 1.0f, 1.0f, 1.0f };
    char textBuffer[1024]{};
    bool textChanged = true;

    // DirectWrite results
    wil::com_ptr<ID3D11RenderTargetView> renderTargetView;
    wil::com_ptr<ID3D11ShaderResourceView> d2dTextureView;
    wil::com_ptr<ID3D11ShaderResourceView> d3dTextureView;
    u32x2 tileSize;
    bool constantBufferInvalidated = true;

    for (;;)
    {
        WaitForSingleObjectEx(frameLatencyWaitableObject.get(), 100, true);

        {
            bool done = false;
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                if (msg.message == WM_QUIT)
                {
                    done = true;
                }
            }
            if (done)
            {
                break;
            }
        }

        if (g_dpiChanged)
        {
            const auto scale = static_cast<f32>(g_dpi) / static_cast<f32>(USER_DEFAULT_SCREEN_DPI);
            auto& io = ImGui::GetIO();
            auto& style = ImGui::GetStyle();

            io.Fonts->Clear();
            if (g_dpi <= USER_DEFAULT_SCREEN_DPI)
            {
                io.Fonts->AddFontDefault();
            }
            else
            {
                // At fractional, higher display scales (like 150%, 250%),
                // fonts like Consolas look a lot better than ProggyClean.
                ImFontConfig config{};
                config.SizePixels = std::floor(scale * 13.0f);
                config.GlyphOffset.y = 1.0f;
                config.OversampleH = 1;
                config.PixelSnapH = true;
                io.Fonts->AddFontFromFileTTF(R"(C:\Windows\Fonts\consola.ttf)", config.SizePixels, &config);
            }
            io.Fonts->Build();

            style = ImGuiStyle{};
            style.ScaleAllSizes(scale);

            ImGui_ImplDX11_InvalidateDeviceObjects();

            textChanged = true;
            g_dpiChanged = false;
        }

        ImGui_ImplWin32_NewFrame();
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowSize(ImVec2{ ImGui::GetFontSize() * 24.0f, 0 }, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings"))
        {
            {
                ImGui::TextWrapped("^ The upper half shows text drawn with Direct2D for reference.");
                ImGui::TextWrapped("v The lower half shows text drawn with our custom HLSL shader.");
            }
            ImGui::Separator();
            {
                textChanged |= ImGui::InputTextWithHint("##text", "text", &textBuffer[0], std::size(textBuffer));
            }
            ImGui::Separator();
            {
                if (ImGui::BeginCombo("##font", selectedFontName->c_str()))
                {
                    for (const auto& fontName : fontNames)
                    {
                        ImGui::PushID(fontName.data());
                        if (ImGui::Selectable(fontName.data(), &fontName == selectedFontName))
                        {
                            selectedFontName = &fontName;
                            textChanged = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }

                textChanged |= ImGui::SliderInt("pt", &fontSize, 1, 100);
            }
            ImGui::Separator();
            {
                textChanged |= ImGui::ColorPicker4("Background", background);
                ImGui::Spacing();
                textChanged |= ImGui::ColorPicker4("Foreground", foreground);
            }
        }
        ImGui::End();

        ImGui::Render();

        if (textChanged)
        {
            const auto scale = static_cast<f32>(g_dpi) / static_cast<f32>(USER_DEFAULT_SCREEN_DPI);
            const auto fontName = u8u16(*selectedFontName);
            const auto fontSizeInDIP = static_cast<float>(fontSize) * USER_DEFAULT_SCREEN_DPI / 72.0f;

            size_t textLength = strnlen_s(&textBuffer[0], std::size(textBuffer));
            std::wstring wideTextBuffer;
            const wchar_t* wideText;

            if (textLength)
            {
                wideTextBuffer = u8u16({ &textBuffer[0], textLength });
                wideText = wideTextBuffer.data();
            }
            else
            {
                static constexpr std::wstring_view defaultText{ L"DirectWrite" };
                wideText = defaultText.data();
                textLength = defaultText.size();
            }

            wil::com_ptr<IDWriteTextFormat> textFormat;
            THROW_IF_FAILED(dwriteFactory->CreateTextFormat(fontName.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, fontSizeInDIP, &localeName[0], textFormat.addressof()));

            wil::com_ptr<IDWriteTextLayout> textLayout;
            THROW_IF_FAILED(dwriteFactory->CreateTextLayout(wideText, static_cast<UINT32>(textLength), textFormat.get(), INFINITY, INFINITY, textLayout.addressof()));

            {
                DWRITE_TEXT_METRICS metrics;
                THROW_IF_FAILED(textLayout->GetMetrics(&metrics));

                tileSize.x = static_cast<u32>(std::ceil(metrics.widthIncludingTrailingWhitespace * scale));
                tileSize.y = static_cast<u32>(std::ceil(metrics.height * scale));
            }

            wil::com_ptr<ID3D11Texture2D> d2dTexture;
            wil::com_ptr<ID3D11Texture2D> d3dTexture;
            {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = tileSize.x;
                desc.Height = tileSize.y;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                THROW_IF_FAILED(device->CreateTexture2D(&desc, nullptr, d2dTexture.put()));
                THROW_IF_FAILED(device->CreateTexture2D(&desc, nullptr, d3dTexture.put()));
                THROW_IF_FAILED(device->CreateShaderResourceView(d2dTexture.get(), nullptr, d2dTextureView.put()));
                THROW_IF_FAILED(device->CreateShaderResourceView(d3dTexture.get(), nullptr, d3dTextureView.put()));
            }

            wil::com_ptr<ID2D1RenderTarget> d2dTextureRenderTarget;
            wil::com_ptr<ID2D1RenderTarget> d3dTextureRenderTarget;
            {
                D2D1_RENDER_TARGET_PROPERTIES props{};
                props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
                props.pixelFormat = { DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED };
                props.dpiX = static_cast<f32>(g_dpi);
                props.dpiY = static_cast<f32>(g_dpi);
                THROW_IF_FAILED(d2dFactory->CreateDxgiSurfaceRenderTarget(d2dTexture.query<IDXGISurface>().get(), &props, d2dTextureRenderTarget.put()));
                THROW_IF_FAILED(d2dFactory->CreateDxgiSurfaceRenderTarget(d3dTexture.query<IDXGISurface>().get(), &props, d3dTextureRenderTarget.put()));

                d2dTextureRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                d3dTextureRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                d3dTextureRenderTarget->SetTextRenderingParams(linearParams.get());
            }

            {
                D2D1_COLOR_F backgroundColor{ background.r, background.g, background.b, background.a };
                D2D1_COLOR_F foregroundColor{ foreground.r, foreground.g, foreground.b, foreground.a };

                wil::com_ptr<ID2D1SolidColorBrush> foregroundBrush;
                THROW_IF_FAILED(d2dTextureRenderTarget->CreateSolidColorBrush(&foregroundColor, nullptr, foregroundBrush.addressof()));

                d2dTextureRenderTarget->BeginDraw();
                d2dTextureRenderTarget->Clear(&backgroundColor);
                d2dTextureRenderTarget->DrawTextLayout({}, textLayout.get(), foregroundBrush.get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                THROW_IF_FAILED(d2dTextureRenderTarget->EndDraw());
            }

            {
                static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
                wil::com_ptr<ID2D1SolidColorBrush> brush;
                THROW_IF_FAILED(d3dTextureRenderTarget->CreateSolidColorBrush(&color, nullptr, brush.addressof()));

                d3dTextureRenderTarget->BeginDraw();
                d3dTextureRenderTarget->Clear();
                d3dTextureRenderTarget->DrawTextLayout({}, textLayout.get(), brush.get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
                THROW_IF_FAILED(d3dTextureRenderTarget->EndDraw());
            }

            constantBufferInvalidated = true;
            textChanged = false;
        }

        if (g_viewportSizeChanged)
        {
            // ResizeBuffer() docs:
            //   Before you call ResizeBuffers, ensure that the application releases all references [...].
            //   You can use ID3D11DeviceContext::ClearState to ensure that all [internal] references are released.
            renderTargetView.reset();
            deviceContext->ClearState();
            deviceContext->Flush();
            swapChain->ResizeBuffers(0, g_viewportSize.x, g_viewportSize.y, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);

            wil::com_ptr<ID3D11Texture2D> buffer;
            THROW_IF_FAILED(swapChain->GetBuffer(0, __uuidof(buffer), buffer.put_void()));
            THROW_IF_FAILED(device->CreateRenderTargetView(buffer.get(), nullptr, renderTargetView.put()));
            deviceContext->OMSetRenderTargets(1, renderTargetView.addressof(), nullptr);

            D3D11_VIEWPORT viewport{};
            viewport.Width = static_cast<f32>(g_viewportSize.x);
            viewport.Height = static_cast<f32>(g_viewportSize.y);
            deviceContext->RSSetViewports(1, &viewport);

            constantBufferInvalidated = true;
            g_viewportSizeChanged = false;
        }

        if (constantBufferInvalidated)
        {
            ConstantBuffer data;
            data.splitPos.x = g_viewportSize.y / 2;
            data.splitPos.y = data.splitPos.x + 4;
            data.tileSize = tileSize;
            data.background = premultiplyColor(background);
            data.foreground = premultiplyColor(foreground);
            data.gammaRatios = DWrite_GetGammaRatios(gamma);
            data.grayscaleEnhancedContrast = grayscaleEnhancedContrast;
            deviceContext->UpdateSubresource(constantBuffer.get(), 0, nullptr, &data, 0, 0);
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
            std::array resourceViews{ d2dTextureView.get(), d3dTextureView.get() };
            deviceContext->PSSetShaderResources(0, static_cast<UINT>(resourceViews.size()), resourceViews.data());

            deviceContext->OMSetRenderTargets(1, renderTargetView.addressof(), nullptr);
            deviceContext->Draw(3, 0);
        }

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        swapChain->Present(1, 0);
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nShowCmd)
{
    try
    {
        winMainImpl(hInstance, hPrevInstance, lpCmdLine, nShowCmd);
    }
    catch (const wil::ResultException& e)
    {
        std::array<wchar_t, 2048> message;
        wil::GetFailureLogString(message.data(), message.size(), e.GetFailureInfo());
        MessageBoxW(nullptr, &message[0], L"Exception", MB_ICONERROR | MB_OK);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
    }

    return 0;
}
