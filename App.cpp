#include "App.h"
#include "Math.h"

// CubeObject: derived from RenderableObject
class RotatingObject : public RenderableObject {
public:
    RotatingObject(
        const std::string& modelName,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader,
        float3 pos,
        float3 scale)
        :RenderableObject(matPreset, inputLayout, graphicsShader)
    {
        transformPos_ = Math::mat4::Translation({ pos.x, pos.y, pos.z });
        transformScale_ = Math::mat4::Scaling(scale.x, scale.y, scale.z);
        modelName_ = modelName;
    }

    void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
    {
        RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);
        if (!modelName_.empty())
        {
            mesh_ = renderer->GetMeshManager()->Load(modelName_, renderer, uploadCmdList, uploadKeepAlive, { true, false, 0 });
        }
        else
        {
            std::vector<VertexPNTUV> cubeVerts;
            std::vector<uint32_t> cubeIndices;
            BuildCubeCW(cubeVerts, cubeIndices);

            GetMesh()->CreateGPU_PNTUV(renderer->GetDevice(), uploadCmdList, uploadKeepAlive, cubeVerts, cubeIndices.data(), (UINT)cubeIndices.size(), true);
        }
    }

    void Tick(float deltaTime) override {
        rotationY_ += angularSpeed_ * deltaTime;
        if (rotationY_ > XM_2PI) {
            rotationY_ -= XM_2PI;
        }

        SetModelMatrix(transformScale_ * Math::mat4::RotationY(rotationY_) * transformPos_);
    }

    float GetRotationY() const { return rotationY_; }
    void SetRotationY(float angle) { rotationY_ = angle; }

    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl) override
    {
        matData_->StageGBufferBindings(renderer, graphicsCtx_, 0, 0);
    }

    void UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj) override
    {
        UpdateUniform("world", modelMatrix_.xm());
        UpdateUniform("view", view.xm());
        UpdateUniform("proj", proj.xm());

        ApplyMaterialParamsToCB();
    }

    bool IsSimpleRender() const { return true; }

private:
    Math::mat4 transformPos_;
    Math::mat4 transformScale_;
    float rotationY_ = 0.0f;
    float angularSpeed_ = 0.0f;// 10.0f * Math::DEG2RAD;
    std::string modelName_;
};

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

    const LONG defWidth = 1600;
	const LONG defHeight = 900;

    RECT rect = { 0, 0, defWidth, defHeight };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);

    RECT screenRect;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &screenRect, 0);

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

    renderer_.GetMaterialDataManager()->RegisterPreset("brick", { L"textures/brick_albedo.png",  L"textures/brick_mr.png",  L"textures/brick_normal_rg.png",  /*RG*/true, /*TBN*/true });
    renderer_.GetMaterialDataManager()->RegisterPreset("bronze", { L"textures/bronze_albedo.png", L"textures/bronze_mr.png", L"textures/bronze_normal_rg.png", /*RG*/true, /*TBN*/true });

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

    auto box = std::make_unique<RotatingObject>("models/box.obj", "brick", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(0.0f, 0, -2.0f), float3(1, 1, 1));
    box->MaterialParamsRef().texFlags.w = 2;
    scene_.AddObject(std::move(box));
    scene_.AddObject(std::make_unique<RotatingObject>("models/teapot.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(-1.0f, 0, -1.0f), float3(1, 1, 1)));
    scene_.AddObject(std::make_unique<RotatingObject>("models/sphere.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(-3.0f, 0, -1.0f), float3(1, 1, 1)));
    scene_.AddObject(std::make_unique<RotatingObject>("models/corgi.obj", "brick", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(3.0f, 0, -1.0f), float3(1, 1, 1)));

    scene_.AddObject(std::make_unique<DebugGrid>(100.0f));

    scene_.AddObject(std::make_unique<GpuInstancedModels>("models/teapot.obj", 100, "bronze", "PosNormTanUV", L"shaders/gbuffer_inst.hlsl", L"shaders/instance_anim.hlsl"));

    renderer_.InitFence();

    InitScene();

    scene_.CameraRef().SetPosition({ 0.f, 1.f, -10.f });

    MSG msg = {};
    double lastTime = GetTimeSeconds();
    while (isRunning_) {
        input_.NewFrame();

        // Не блокируемся, просто обрабатываем все сообщения, если есть
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

        double now = GetTimeSeconds();
        float deltaTime = static_cast<float>(now - lastTime);
        lastTime = now;
        
		deltaTime = Math::Clamp(deltaTime, 1e-6f, 0.1f);

		renderer_.Tick(deltaTime);
        scene_.Tick(deltaTime);
        scene_.Render(&renderer_);
    }

    scene_.Clear();

    TaskSystem::Get().Stop();
}