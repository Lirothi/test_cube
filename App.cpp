#include "App.h"
#include "Profiler.h"

LRESULT CALLBACK App::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = reinterpret_cast<App*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    // Если окно только создается — app еще нет, но нам это не страшно
    if (message == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProc(hWnd, message, wParam, lParam);
    }

    if (app) {
        app->input_.OnWndProc(hWnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_SIZE:
    {
        UINT width = LOWORD(lParam);
        UINT height = HIWORD(lParam);
        if (app && app->renderer_.GetDevice() && wParam != SIZE_MINIMIZED) {
            app->renderer_.OnResize(width, height);
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
    LONG defWidth = (LONG)(1600 * scale);
	LONG defHeight = (LONG)(900 * scale);

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
    renderer_.InitD3D12(hWnd);
    input_.Initialize(hWnd);
#if PROF_GPU_ENABLED
    Profiler::Get().InitGpu(renderer_.GetDevice(), renderer_.GetCommandQueue());
#endif
}

void App::InitScene()
{
    std::vector<ComPtr<ID3D12Resource>> pendingUploads;
    scene_.SetInput(&input_);
	scene_.SetActions(&actions_);
    if (!actions_.LoadFromJsonFile(L"bindings.json"))
    {
        assert(false && "No bindings.json found!");
    }

    renderer_.GetMaterialDataManager()->RegisterPreset("brick", { L"textures/brick_albedo.dds",  L"textures/brick_mr.dds",  L"textures/brick_normal.dds",  /*RG*/false, /*TBN*/true });
    renderer_.GetMaterialDataManager()->RegisterPreset("bronze", { L"textures/bronze_albedo.dds", L"textures/bronze_mr.dds", L"textures/bronze_normal.dds", /*RG*/false, /*TBN*/true });
    renderer_.GetMaterialDataManager()->RegisterPreset("damaged_plaster", { L"textures/damaged_plaster_albedo.dds", L"textures/damaged_plaster_mr.dds", L"textures/damaged_plaster_normal.dds", /*RG*/false, /*TBN*/true });
    renderer_.GetMaterialDataManager()->RegisterPreset("sandstone_cracks", { L"textures/sandstone_cracks_albedo.dds", L"textures/sandstone_cracks_mr.dds", L"textures/sandstone_cracks_normal.dds", /*RG*/false, /*TBN*/true });

    // Заранее создаем upload command list
    ComPtr<ID3D12CommandAllocator> uploadAlloc;
    ComPtr<ID3D12GraphicsCommandList> uploadCmdList;
    renderer_.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&uploadAlloc));
    renderer_.GetDevice()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAlloc.Get(), nullptr, IID_PPV_ARGS(&uploadCmdList));

    renderer_.InitTextSystem(uploadCmdList.Get(), &pendingUploads, L"fonts");
    scene_.InitAll(&renderer_, uploadCmdList.Get(), &pendingUploads);

    uploadCmdList->Close();
    ID3D12CommandList* cmdLists[] = { uploadCmdList.Get() };
    renderer_.GetCommandQueue()->ExecuteCommandLists(1, cmdLists);
    // Ждем, пока upload завершится (фенс ивент)
    renderer_.WaitForPreviousFrame();
}

void App::Run(HINSTANCE hInstance, int nCmdShow) {
    InitWindow(hInstance, nCmdShow);
    TaskSystem::Get().Start(static_cast<unsigned int>(std::thread::hardware_concurrency() * 0.75f));

    InitScene();

    MSG msg = {};
    double lastTime = GetTimeSeconds();
    while (isRunning_) {
        Profiler::Get().BeginFrame(renderer_.GetTotalFrameNumber());
        renderer_.BeginFrame();
#if PROF_GPU_ENABLED
        Profiler::Get().CollectGpuResults();
#endif

        {
            CPU_SCOPE("Whole Cycle");
            input_.NewFrame();

            {
                CPU_SCOPE("Win Messages");
                while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                    if (msg.message == WM_QUIT) {
                        break; // Прерываем цикл, не рендерим больше!
                    }
                }
                if (msg.message == WM_QUIT) {
                    break;
                }
            }

            double now = GetTimeSeconds();
            float deltaTime = static_cast<float>(now - lastTime);
            lastTime = now;

            deltaTime = Math::Clamp(deltaTime, 1e-6f, 0.1f);

            renderer_.Tick(deltaTime);
            scene_.Tick(deltaTime);
            scene_.Render(&renderer_);
        }

        Profiler::Get().EndFrame();
    }

    scene_.Clear();

    TaskSystem::Get().Stop();
}