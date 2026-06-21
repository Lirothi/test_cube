#include "app/App.h"
#include "core/math/Math.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <mimalloc.h>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

#include "app/levels/DemoLevel.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/core/RenderStats.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

static void MiOut(const char* msg, void* /*arg*/) { OutputDebugStringA(msg); }

namespace
{
    constexpr const wchar_t* kLoadingScreenPath = L"data/loading_screen.jpg";

    class ScopedComInitialization
    {
    public:
        ScopedComInitialization()
            : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
            , uninitialize_(SUCCEEDED(hr_))
        {
        }

        ~ScopedComInitialization()
        {
            if (uninitialize_)
            {
                CoUninitialize();
            }
        }

        bool IsValid() const
        {
            return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
        }

    private:
        HRESULT hr_;
        bool uninitialize_;
    };

    HBITMAP LoadBitmapFromWicFile(const wchar_t* path, BITMAP& outInfo)
    {
        outInfo = {};

        ScopedComInitialization com;
        if (!com.IsValid())
        {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr))
        {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        hr = factory->CreateDecoderFromFilename(
            path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf());
        if (FAILED(hr))
        {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        hr = decoder->GetFrame(0, frame.GetAddressOf());
        if (FAILED(hr))
        {
            return nullptr;
        }

        UINT width = 0;
        UINT height = 0;
        hr = frame->GetSize(&width, &height);
        if (FAILED(hr) || width == 0 || height == 0 || width > static_cast<UINT>(INT_MAX) || height > static_cast<UINT>(INT_MAX) ||
            width > UINT_MAX / 4u || height > UINT_MAX / (width * 4u))
        {
            return nullptr;
        }

        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(hr))
        {
            return nullptr;
        }

        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (FAILED(hr))
        {
            return nullptr;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
        bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        void* pixels = nullptr;
        HDC screenDc = GetDC(nullptr);
        if (!screenDc)
        {
            return nullptr;
        }

        HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
        ReleaseDC(nullptr, screenDc);
        if (!bitmap || !pixels)
        {
            if (bitmap)
            {
                DeleteObject(bitmap);
            }
            return nullptr;
        }

        const UINT stride = width * 4u;
        const UINT byteCount = stride * height;
        hr = converter->CopyPixels(nullptr, stride, byteCount, static_cast<BYTE*>(pixels));
        if (FAILED(hr))
        {
            DeleteObject(bitmap);
            return nullptr;
        }

        GetObjectW(bitmap, sizeof(outInfo), &outInfo);
        return bitmap;
    }
}

App::~App()
{
    ReleaseLoadingScreen();
}

LRESULT CALLBACK App::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    // When the window is first created the app pointer is not set yet, but that's fine
    if (message == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    if (app && app->systems_) {
        auto& renderer = app->systems_->renderer;
        renderer.HandleImGuiWndProc(hWnd, message, wParam, lParam);

        const bool mouseMessage = message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
        const bool keyMessage = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        const bool consumedByImGui =
            (mouseMessage && renderer.ImGuiWantsMouse()) ||
            (keyMessage && renderer.ImGuiWantsKeyboard());

        if (!consumedByImGui) {
            app->systems_->input.OnWndProc(hWnd, message, wParam, lParam);
        }
    }

    switch (message) {
    case WM_SIZE:
    {
        UINT width = LOWORD(lParam);
        UINT height = HIWORD(lParam);
        if (app && app->systems_ && app->systems_->renderer.GetDevice() && wParam != SIZE_MINIMIZED) {
            app->systems_->renderer.OnResize(width, height);
        }
        break;
    }
    case WM_ERASEBKGND:
        if (app && app->loadingScreenVisible_)
        {
            app->PaintLoadingScreen(reinterpret_cast<HDC>(wParam));
            return 1;
        }
        break;
    case WM_PAINT:
        if (app && app->loadingScreenVisible_)
        {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hWnd, &ps);
            app->PaintLoadingScreen(dc);
            EndPaint(hWnd, &ps);
            return 0;
        }
        break;
    case WM_DESTROY:
        if (app)
        {
            app->SetRunnig(false);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

void App::InitWindow(HINSTANCE hInstance, int nCmdShow) {
    assert(systems_);

    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = App::WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"D3D12WindowClass";

    RegisterClassEx(&wc);

    RECT screenRect;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);

    float scale = 1.0f, aspect = 9.0f/16.0f;
    LONG defWidth = (LONG)(2560 * scale);
	LONG defHeight = (LONG)(1440 * scale);

    defWidth = std::min(defWidth, (screenRect.right - screenRect.left) - 8);
    defHeight = LONG(defWidth * aspect);

    RECT rect = { 0, 0, defWidth, defHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    int windowWidth = rect.right - rect.left;
    int windowHeight = rect.bottom - rect.top;
    int posX = (screenRect.right - windowWidth) / 2;
    int posY = (screenRect.bottom - windowHeight) / 2;

    hWnd_ = CreateWindow(
        wc.lpszClassName,
        L"D3D12 Multi-Mesh Renderer",
        WS_OVERLAPPEDWINDOW,
        posX, posY,
        windowWidth,
        windowHeight,
        nullptr, nullptr,
        hInstance, this
    );
    SetWindowLongPtr(hWnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    LoadLoadingScreen();
    loadingScreenVisible_ = true;
    ShowWindow(hWnd_, nCmdShow);
    UpdateWindow(hWnd_);

    auto& renderer = systems_->renderer;
    auto& input = systems_->input;
    renderer.InitD3D12(hWnd_, defWidth, defHeight);
    input.Initialize(hWnd_);
#if PROF_GPU_ENABLED
    Profiler::Get().InitGpu(renderer.GetDevice(), renderer.GetCommandQueue());
#endif
}

void App::InitScene()
{
    assert(systems_);

    auto& renderer = systems_->renderer;
    auto& scene = systems_->scene;
    auto& input = systems_->input;
    auto& levelManager = systems_->levelManager;

    if (!input.LoadActions(L"input/bindings.json"))
    {
        assert(false && "No bindings.json found!");
    }

    const bool presetsLoaded = renderer.GetMaterialDataManager()->LoadPresetsFromJsonFile(L"data/materials.json");
    assert(presetsLoaded && "No data/materials.json found!");
    (void)presetsLoaded;

    UploadBatch uploadBatch;
    const bool batchBegun = uploadBatch.Begin(&renderer);
    assert(batchBegun && "Failed to begin upload batch");
    (void)batchBegun;

    renderer.InitTextSystem(uploadBatch.CommandList(), uploadBatch.KeepAlive(), L"fonts");
    if (auto* debugDraw = renderer.GetDebugDrawSystem())
    {
        debugDraw->Initialize(&renderer, uploadBatch.CommandList(), uploadBatch.KeepAlive());
    }

    LevelLoadContext loadCtx{ uploadBatch, renderer, scene };

    if (!levelManager.HasLevel(DemoLevel::kName))
    {
        levelManager.RegisterLevel<DemoLevel>();
    }

    const bool levelLoaded = levelManager.LoadLevel(DemoLevel::kName, loadCtx);
    assert(levelLoaded && "Failed to load initial level");

    uploadBatch.SubmitAndWait(&renderer);
    HideLoadingScreen();
}

void App::LoadLoadingScreen()
{
    ReleaseLoadingScreen();

    loadingBitmap_ = LoadBitmapFromWicFile(kLoadingScreenPath, loadingBitmapInfo_);
}

void App::ReleaseLoadingScreen()
{
    if (loadingBitmap_)
    {
        DeleteObject(loadingBitmap_);
        loadingBitmap_ = nullptr;
    }
    loadingBitmapInfo_ = {};
}

void App::HideLoadingScreen()
{
    loadingScreenVisible_ = false;
    ReleaseLoadingScreen();
}

void App::PaintLoadingScreen(HDC dc) const
{
    if (!dc || !hWnd_)
    {
        return;
    }

    RECT client{};
    GetClientRect(hWnd_, &client);
    const int clientW = std::max<LONG>(client.right - client.left, 1);
    const int clientH = std::max<LONG>(client.bottom - client.top, 1);

    HBRUSH blackBrush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(dc, &client, blackBrush);

    if (loadingBitmap_ && loadingBitmapInfo_.bmWidth > 0 && loadingBitmapInfo_.bmHeight > 0)
    {
        HDC imageDc = CreateCompatibleDC(dc);
        HGDIOBJ oldBitmap = SelectObject(imageDc, loadingBitmap_);

        const double scaleX = static_cast<double>(clientW) / static_cast<double>(loadingBitmapInfo_.bmWidth);
        const double scaleY = static_cast<double>(clientH) / static_cast<double>(loadingBitmapInfo_.bmHeight);
        const double scale = std::max(scaleX, scaleY);
        const int drawW = std::max(1, static_cast<int>(static_cast<double>(loadingBitmapInfo_.bmWidth) * scale + 0.5));
        const int drawH = std::max(1, static_cast<int>(static_cast<double>(loadingBitmapInfo_.bmHeight) * scale + 0.5));
        const int drawX = (clientW - drawW) / 2;
        const int drawY = (clientH - drawH) / 2;

        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchBlt(dc, drawX, drawY, drawW, drawH,
            imageDc, 0, 0, loadingBitmapInfo_.bmWidth, loadingBitmapInfo_.bmHeight, SRCCOPY);

        SelectObject(imageDc, oldBitmap);
        DeleteDC(imageDc);
    }

    const int minDim = std::max(1, std::min(clientW, clientH));
    const int margin = std::max(24, minDim / 24);
    const int fontHeight = std::max(32, minDim / 18);
    HFONT font = CreateFontW(
        -fontHeight, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    const int oldBkMode = SetBkMode(dc, TRANSPARENT);
    const COLORREF oldColor = SetTextColor(dc, RGB(0, 0, 0));

    RECT textRect{
        client.left + margin,
        client.top + margin,
        client.right - margin,
        client.bottom - margin
    };
    RECT shadowRect = textRect;
    OffsetRect(&shadowRect, 3, 3);
    DrawTextW(dc, L"Loading...", -1, &shadowRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, RGB(245, 248, 252));
    DrawTextW(dc, L"Loading...", -1, &textRect, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(dc, oldColor);
    SetBkMode(dc, oldBkMode);
    if (oldFont)
    {
        SelectObject(dc, oldFont);
    }
    if (font)
    {
        DeleteObject(font);
    }
}

void App::Run(HINSTANCE hInstance, int nCmdShow) {
    systems_ = std::make_unique<Systems::AppSystems>();
    Systems::Init(systems_.get());

    {
        auto& renderer = systems_->renderer;
        auto& scene = systems_->scene;
        auto& input = systems_->input;
        auto& levelManager = systems_->levelManager;

        InitWindow(hInstance, nCmdShow);
        TaskSystem::Get().Start(static_cast<unsigned int>(std::thread::hardware_concurrency() * 0.75f));
        //TaskSystem::Get().Start(8);
        Profiler::Get().SetThreadName("MainThread");

        InitScene();

        MSG msg = {};
        double lastTime = GetTimeSeconds();
        while (isRunning_) {
            Profiler::Get().BeginFrame(renderer.GetTotalFrameNumber());
            render::g_renderStats.NextFrame(); // snapshot last frame's draw/primitive counts
            TaskSystem::Get().WaitForTrackedAsyncTasks();
            renderer.BeginFrame();
#if PROF_GPU_ENABLED
            Profiler::Get().CollectGpuResults();
#endif

            {
                CPU_SCOPE(ProfilerScopes::kWholeCycle);
                input.NewFrame();

                {
                    CPU_SCOPE(ProfilerScopes::kWinMessages);
                    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                        TranslateMessage(&msg);
                        DispatchMessage(&msg);
                        if (msg.message == WM_QUIT) {
                            break; // Break out of the loop and stop rendering
                        }
                    }
                    if (msg.message == WM_QUIT) {
                        break;
                    }
                }

                renderer.BeginImGuiFrame();

                Profiler::Get().Tick();

                double now = GetTimeSeconds();
                float deltaTime = static_cast<float>(now - lastTime);
                lastTime = now;

                deltaTime = Math::Clamp(deltaTime, 1e-6f, 0.1f);

                renderer.Tick(deltaTime);
                appController_.Tick(input, renderer, scene, deltaTime);
                scene.Tick(deltaTime);

                levelManager.Tick(deltaTime);

                if (auto pendingLevel = levelManager.ConsumePendingLevelRequest())
                {
                    UploadBatch levelBatch;
                    if (levelBatch.Begin(&renderer))
                    {
                        LevelLoadContext levelCtx{ levelBatch, renderer, scene };

                        if (levelManager.LoadLevel(*pendingLevel, levelCtx))
                        {
                            levelBatch.SubmitAndWait(&renderer);
                        }
                    }
                }

                appController_.WaitForHudBuild();
                scene.Render(&renderer);
            }

            Profiler::Get().EndFrame();
        }

        TaskSystem::Get().Stop();

        scene.Clear();
        renderer.Shutdown();
    }

    Systems::Shutdown();
    systems_.reset();

    mi_register_output(MiOut, nullptr);
    mi_collect(true);
    mi_option_set(mi_option_show_stats, 1);
    //mi_stats_print(nullptr);
}
