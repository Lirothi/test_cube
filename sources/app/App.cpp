#include "app/App.h"
#include "core/math/Math.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include <algorithm>
#include <cassert>
#include <mimalloc.h>
#include <vector>

#include "app/levels/DemoLevel.h"
#include "rendering/core/UploadBatch.h"

static void MiOut(const char* msg, void* /*arg*/) { OutputDebugStringA(msg); }

LRESULT CALLBACK App::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    // When the window is first created the app pointer is not set yet, but that's fine
    if (message == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    if (app && app->systems_) {
        app->systems_->input.OnWndProc(hWnd, message, wParam, lParam);
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

    HWND hWnd = CreateWindow(
        wc.lpszClassName,
        L"D3D12 Multi-Mesh Renderer",
        WS_OVERLAPPEDWINDOW,
        posX, posY,
        windowWidth,
        windowHeight,
        nullptr, nullptr,
        hInstance, this
    );
    SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    ShowWindow(hWnd, nCmdShow);

    auto& renderer = systems_->renderer;
    auto& input = systems_->input;
    renderer.InitD3D12(hWnd, defWidth, defHeight);
    input.Initialize(hWnd);
#if PROF_GPU_ENABLED
    Profiler::Get().InitGpu(renderer.GetDevice(), renderer.GetCommandQueue());
#endif
}

void App::InitScene()
{
    assert(systems_);

    auto& renderer = systems_->renderer;
    auto& input = systems_->input;
    auto& levelManager = systems_->levelManager;

    if (!input.LoadActions(L"input/bindings.json"))
    {
        assert(false && "No bindings.json found!");
    }

    renderer.GetMaterialDataManager()->RegisterPreset("brick", { L"textures/brick_albedo.dds",  L"textures/brick_mr.dds",  L"textures/brick_normal.dds",  /*RG*/false, /*TBN*/true });
    renderer.GetMaterialDataManager()->RegisterPreset("bronze", { L"textures/bronze_albedo.dds", L"textures/bronze_mr.dds", L"textures/bronze_normal.dds", /*RG*/false, /*TBN*/true });
    renderer.GetMaterialDataManager()->RegisterPreset("damaged_plaster", { L"textures/damaged_plaster_albedo.dds", L"textures/damaged_plaster_mr.dds", L"textures/damaged_plaster_normal.dds", /*RG*/false, /*TBN*/true });
    renderer.GetMaterialDataManager()->RegisterPreset("sandstone_cracks", { L"textures/sandstone_cracks_albedo.dds", L"textures/sandstone_cracks_mr.dds", L"textures/sandstone_cracks_normal.dds", /*RG*/false, /*TBN*/true });

    UploadBatch uploadBatch;
    const bool batchBegun = uploadBatch.Begin(&renderer);
    assert(batchBegun && "Failed to begin upload batch");
    (void)batchBegun;

    renderer.InitTextSystem(uploadBatch.CommandList(), uploadBatch.KeepAlive(), L"fonts");
    if (auto* debugDraw = renderer.GetDebugDrawSystem())
    {
        debugDraw->Initialize(&renderer, uploadBatch.CommandList(), uploadBatch.KeepAlive());
    }

    LevelLoadContext loadCtx{ uploadBatch };

    if (!levelManager.HasLevel(DemoLevel::kName))
    {
        levelManager.RegisterLevel<DemoLevel>();
    }

    const bool levelLoaded = levelManager.LoadLevel(DemoLevel::kName, loadCtx);
    assert(levelLoaded && "Failed to load initial level");

    uploadBatch.SubmitAndWait(&renderer);
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

                Profiler::Get().Tick();

                double now = GetTimeSeconds();
                float deltaTime = static_cast<float>(now - lastTime);
                lastTime = now;

                deltaTime = Math::Clamp(deltaTime, 1e-6f, 0.1f);

                renderer.Tick(deltaTime);
                appController_.Tick(input, renderer, scene);
                scene.Tick(deltaTime);

                levelManager.Tick(deltaTime);

                if (auto pendingLevel = levelManager.ConsumePendingLevelRequest())
                {
                    UploadBatch levelBatch;
                    if (levelBatch.Begin(&renderer))
                    {
                        LevelLoadContext levelCtx{ levelBatch };

                        if (levelManager.LoadLevel(*pendingLevel, levelCtx))
                        {
                            levelBatch.SubmitAndWait(&renderer);
                        }
                    }
                }

                appController_.BuildHud(renderer, scene, input);
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
