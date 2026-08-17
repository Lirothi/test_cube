#include "app/App.h"
#include "core/math/Math.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <mimalloc.h>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>
#include "vfx/WindState.h" // --shot-count phase series steps the frozen wind clock (g_windStep)

// Boot-level override; see App.h. Set by main.cpp from "--level=<path>".
std::string g_bootLevelPath;
bool  g_camOverride = false;
float g_camPos[3] = { 0.0f, 0.0f, 0.0f };
float g_camRot[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
// One-shot screenshot; see App.h. Set by main.cpp from "--shot=<path>" / "--shot-delay=<sec>".
std::string g_shotPath;
double g_shotDelaySec = 7.0;
// Phase series (see App.h): "--shot-count=<n> --shot-step=<sec> [--shot-interval=<sec>]".
int    g_shotCount = 1;
double g_shotStepSec = 0.0;
double g_shotIntervalSec = 1.0;
// Temporary VSM perf harness; see App.h.
std::string g_profDumpPath;
uint32_t g_traceFrames = 0;
// Boot upscaler mode; see App.h. Set by main.cpp from "--dlss=<mode>". -1 = compiled default.
int g_bootDlssMode = -1;
// Empty-HUD capture mode; see App.h. Set by main.cpp from "--no-hud".
bool g_hudHidden = false;
// Single-process settings sweep; see App.h. Set by main.cpp from "--sweep=<setting>:<v0>,...".
std::string g_sweepSetting;
std::vector<float> g_sweepValues;

#include "app/levels/JsonLevel.h"
#include "rendering/core/Screenshot.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/core/RenderStats.h"
#if WITH_EDITOR
#include "editor/EditorController.h"
#endif

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

namespace
{
    // "--sweep": apply one value of the swept setting. Deliberately a flat if-chain over string
    // names rather than a reflection system -- this is a capture harness, and a name that does not
    // match should be loud and cheap to diagnose, not silently ignored.
    // Returns false for an unknown name so the caller can say so once.
    bool ApplySweepValue(Scene& scene, const std::string& setting, float value)
    {
        render::CameraExposureSettings& e = scene.CameraExposureRef();
        render::ColorPipelineSettings& c = scene.ColorPipelineRef();

        // P4: the SCENE-side brightness knobs, so a sweep can measure them against the camera-side
        // ones. `light.exposure` is the field P4 exists to retire -- it multiplies sun AND ambient
        // in lighting_cs, which is a camera control wearing a light's name.
        if (setting == "light.exposure") { scene.DirectionalLightRef().SetExposure(value); return true; }
        if (setting == "light.ambient")  { scene.DirectionalLightRef().SetAmbient(value);  return true; }
        if (setting == "light.sunIntensity") { scene.DirectionalLightRef().SetSunIntensity(value); return true; }
        // P4 equivalence probe: put the scene back into the LEGACY split -- unit sun intensity with
        // the factor on the retired whole-scene multiplier. If the migration folds correctly this
        // must be pixel-identical to the pre-P4 capture, which is what proves the fold rather than
        // arguing about it. Kept because it is the only way to re-prove the fold after any change
        // to a light consumer.
        if (setting == "light.legacySplit")
        {
            scene.DirectionalLightRef().SetSunIntensity(1.0f);
            scene.DirectionalLightRef().SetExposure(value);
            return true;
        }
        if (setting == "light.ambientTintedBySun")
        {
            scene.DirectionalLightRef().SetAmbientTintedBySun(value != 0.0f);
            return true;
        }
        // P4 measurement hook: blend the fill colour from the sun's hue (0) toward a daylight sky
        // blue (1) at MATCHED luminance, so the sweep isolates the hue change from a brightness
        // change. Turning the tint off on its own is deliberately a no-op -- this is what actually
        // shows what a sky-coloured fill looks like.
        if (setting == "light.ambientSkyBlue")
        {
            DirectionalLight& dl = scene.DirectionalLightRef();
            const Math::float3 sun = dl.GetEffectiveColor();
            const float lum = 0.2126f * sun.x + 0.7152f * sun.y + 0.0722f * sun.z;
            // CIE-ish overcast/clear-sky ratio, normalised to unit luma so only the hue moves.
            const Math::float3 skyHue{ 0.45f, 0.66f, 1.0f };
            const float skyLum = 0.2126f * skyHue.x + 0.7152f * skyHue.y + 0.0722f * skyHue.z;
            const Math::float3 sky{ skyHue.x * lum / skyLum, skyHue.y * lum / skyLum, skyHue.z * lum / skyLum };
            const float t = value;
            dl.SetAmbientTintedBySun(false);
            dl.SetAmbientColor({ sun.x + (sky.x - sun.x) * t,
                                 sun.y + (sky.y - sun.y) * t,
                                 sun.z + (sky.z - sun.z) * t });
            return true;
        }

        if (setting == "exposure.lowPercentile")   { e.lowPercentile = value;  return true; }
        if (setting == "exposure.highPercentile")  { e.highPercentile = value; return true; }
        if (setting == "exposure.compensationEv")  { e.compensationEv = value; return true; }
        if (setting == "exposure.manualEv100")     { e.manualEv100 = value;    return true; }
        if (setting == "exposure.minEv100")        { e.minEv100 = value;       return true; }
        if (setting == "exposure.maxEv100")        { e.maxEv100 = value;       return true; }
        if (setting == "exposure.speedUp")         { e.speedUp = value;        return true; }
        if (setting == "exposure.meterMaskStrength")    { e.meterMaskStrength = value;    return true; }
        if (setting == "exposure.meterMaskInnerRadius") { e.meterMaskInnerRadius = value; return true; }
        if (setting == "exposure.meterMaskOuterRadius") { e.meterMaskOuterRadius = value; return true; }
        if (setting == "exposure.meterMaskSkyBias")     { e.meterMaskSkyBias = value;     return true; }
        if (setting == "exposure.speedDown")       { e.speedDown = value;      return true; }
        if (setting == "exposure.adaptationStartDistance") { e.adaptationStartDistance = value; return true; }
        if (setting == "exposure.blackBucketInfluence")    { e.blackBucketInfluence = value;    return true; }
        if (setting == "exposure.enabled")         { e.enabled = value != 0.0f;      return true; }
        if (setting == "exposure.autoExposure")    { e.autoExposure = value != 0.0f; return true; }
        if (setting == "color.toneCurve")
        {
            c.toneCurve = (value != 0.0f) ? render::ToneCurve::AgX : render::ToneCurve::LegacyAces;
            return true;
        }
        if (setting == "color.agxSlope")      { c.agxSlope = value;      return true; }
        if (setting == "color.agxPower")      { c.agxPower = value;      return true; }
        if (setting == "color.agxSaturation") { c.agxSaturation = value; return true; }
        if (setting == "color.gradeSaturation") { c.gradeSaturation = value; return true; }
        if (setting == "color.gradeContrast")   { c.gradeContrast = value;   return true; }
        if (setting == "color.gradeGamma")      { c.gradeGamma = value;      return true; }
        if (setting == "color.gradeGain")       { c.gradeGain = value;       return true; }
        if (setting == "color.gradeOffset")     { c.gradeOffset = value;     return true; }
        if (setting == "color.localHighlightContrast") { c.localHighlightContrast = value; return true; }
        if (setting == "color.localShadowContrast")    { c.localShadowContrast = value;    return true; }
        if (setting == "color.localDetailStrength")    { c.localDetailStrength = value;    return true; }
        // Composite: both local contrast scales at once. The two are independent branches of the
        // same function (base above / below middle grey), so sweeping them together is the only way
        // to measure the base EXPANSION the plan's P3B target needs -- one knob alone can move only
        // one end of the histogram, and the reference wants both ends moved in the same shot.
        if (setting == "color.localContrast")
        {
            c.localHighlightContrast = value;
            c.localShadowContrast = value;
            return true;
        }
        return false;
    }
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
        const bool inputStateRelease =
            message == WM_KEYUP ||
            message == WM_SYSKEYUP ||
            message == WM_LBUTTONUP ||
            message == WM_MBUTTONUP ||
            message == WM_RBUTTONUP;

        // Release messages must always reach InputManager. A modifier or mouse
        // button may have gone down before ImGui acquired capture; swallowing
        // its release would leave the engine-side state stuck indefinitely.
        if (!consumedByImGui || inputStateRelease) {
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

    // I0: materials are per-file assets in data/materials/ (one json per material, name = stem).
    // The legacy monolith loads first so per-file materials win on a name clash during migration.
    MaterialDataManager* materials = renderer.GetMaterialDataManager();
    (void)materials->LoadPresetsFromJsonFile(L"data/materials.json"); // legacy, absent post-migration
    (void)materials->LoadPresetsFromDirectory(L"data/materials");
    assert(materials->PresetCount() > 0 && "No material presets found (data/materials/)!");

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

    if (!levelManager.HasLevel(JsonLevel::kName))
    {
        std::string bootLevel = g_bootLevelPath;
#if WITH_EDITOR
        if (bootLevel.empty())
        {
            bootLevel = EditorController::LoadLastOpenedLevelPath();
        }
#endif
        if (bootLevel.empty())
        {
            bootLevel = "data/levels/demo.json";
        }
        levelManager.RegisterLevel<JsonLevel>(bootLevel);
    }

    const bool levelLoaded = levelManager.LoadLevel(JsonLevel::kName, loadCtx);
    assert(levelLoaded && "Failed to load initial level");

    // Applied after the level so it beats the level's own freeCameraStart.
    if (g_camOverride)
    {
        Camera& cam = scene.CameraRef();
        cam.SetPosition(float3(g_camPos[0], g_camPos[1], g_camPos[2]));

        // Quaternion -> the camera's yaw/pitch/roll. Recovered GEOMETRICALLY rather than by an
        // Euler-extraction formula: yaw/pitch come from where the quaternion points the forward
        // axis, then roll is the leftover twist about that axis, measured as the signed angle
        // between the un-rolled up vector and the real one. This is independent of the rotation
        // ORDER the camera happens to use, so it cannot silently break if that order is ever
        // changed - and it degrades sanely at the poles instead of producing NaNs.
        const DirectX::XMVECTOR q = DirectX::XMQuaternionNormalize(
            DirectX::XMVectorSet(g_camRot[0], g_camRot[1], g_camRot[2], g_camRot[3]));
        const DirectX::XMVECTOR fwd = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 0, 1, 0), q);
        const DirectX::XMVECTOR upQ = DirectX::XMVector3Rotate(DirectX::XMVectorSet(0, 1, 0, 0), q);
        DirectX::XMFLOAT3 f{};
        DirectX::XMStoreFloat3(&f, fwd);
        const float yaw = std::atan2(f.x, f.z);
        const float pitch = std::atan2(-f.y, std::sqrt(f.x * f.x + f.z * f.z));

        // The up vector this yaw/pitch alone would produce; the angle from it to the real up,
        // measured about forward, IS the roll.
        const mat4 noRoll = mat4::RotationRollPitchYaw(pitch, yaw, 0.0f);
        const float3 upNr3 = noRoll.TransformPoint(float3(0.0f, 1.0f, 0.0f));
        const DirectX::XMVECTOR upNr = DirectX::XMVectorSet(upNr3.x, upNr3.y, upNr3.z, 0.0f);
        const DirectX::XMVECTOR cross = DirectX::XMVector3Cross(upNr, upQ);
        const float sinR = DirectX::XMVectorGetX(DirectX::XMVector3Dot(cross, fwd));
        const float cosR = DirectX::XMVectorGetX(DirectX::XMVector3Dot(upNr, upQ));
        cam.SetYawPitchRoll(yaw, pitch, std::atan2(sinR, cosR));
    }

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
        DEFAULT_PITCH | FF_SWISS, L"Consolas");
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

        // "--dlss=<mode>": apply the boot override once the device, the DLSS handler and the
        // deferred targets exist, so this takes exactly the same path as the dev-window combo
        // (resolution update + target recreation) rather than a second, untested init order.
        if (g_bootDlssMode >= 0)
        {
            renderer.SetDlssMode(static_cast<sl::DLSSMode>(g_bootDlssMode));
        }

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
                appController_.Tick(input, renderer, scene, levelManager, deltaTime);
                scene.Tick(deltaTime);

                levelManager.Tick(deltaTime);

                if (auto pendingLevel = levelManager.ConsumePendingLevelRequest())
                {
                    renderer.WaitForPreviousFrame();

                    bool levelLoaded = false;
                    UploadBatch levelBatch;
                    if (levelBatch.Begin(&renderer))
                    {
                        LevelLoadContext levelCtx{ levelBatch, renderer, scene
#if WITH_EDITOR
                            , pendingLevel->options.editorDocument
#endif
                        };

                        levelLoaded = pendingLevel->loadFromPath
                            ? levelManager.LoadLevelFromPath(pendingLevel->sourcePath, levelCtx, pendingLevel->options)
                            : levelManager.LoadLevel(pendingLevel->levelName, levelCtx, pendingLevel->options);
                        if (levelLoaded)
                        {
                            levelBatch.SubmitAndWait(&renderer);
                        }
                    }
#if WITH_EDITOR
                    appController_.OnLevelChangeRequestCompleted(*pendingLevel, levelLoaded, renderer, scene, levelManager);
#endif
                }

                appController_.WaitForHudBuild();
                scene.Render(&renderer);
            }

            // "--shot=<path>": after the warmup delay (ocean/particle sim settling), grab the
            // just-presented backbuffer to a PNG and quit. Reliable on the flip-model swapchain.
            //
            // "--shot-count=<n> --shot-step=<sec>" turns it into a PHASE SERIES from ONE process:
            // requires --wind-freeze, whose frozen clock this advances by exactly --shot-step via
            // vfx::g_windStep between frames (path gets an _NN suffix). One boot instead of N —
            // the previous way to film the shore breathing was N full relaunches. The inter-shot
            // pause (--shot-interval) exists for the temporal stack: DLSS needs a few frames after
            // each time jump or every frame in the series carries the previous phase's ghost.
            //
            // "--sweep=<setting>:<v0>,..." reuses the same series machinery for SETTINGS instead of
            // time: value[i] is applied before shot i and the exposure adaptation is reset so each
            // shot settles on its own value. One boot for a whole slider sweep.
            if (!g_shotPath.empty())
            {
                static double shotStart = GetTimeSeconds();
                static int shotIndex = 0;
                static int appliedSweepIndex = -1;
                static bool sweepNameReported = false;

                // Apply before the settle delay is judged, so the value is in place for the whole
                // interval rather than only for the frame the shot is taken on.
                if (!g_sweepSetting.empty() && shotIndex < static_cast<int>(g_sweepValues.size()) &&
                    appliedSweepIndex != shotIndex)
                {
                    const float value = g_sweepValues[static_cast<size_t>(shotIndex)];
                    const bool known = ApplySweepValue(scene, g_sweepSetting, value);
                    if (!known && !sweepNameReported)
                    {
                        OutputDebugStringA(("[sweep] unknown setting: " + g_sweepSetting + "\n").c_str());
                        sweepNameReported = true;
                    }
                    // The camera must not carry the previous value's adaptation into this shot.
                    renderer.Exposure().RequestReset();
                    appliedSweepIndex = shotIndex;
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "[sweep] shot %d: %s = %g\n",
                        shotIndex, g_sweepSetting.c_str(), value);
                    OutputDebugStringA(msg);
                }

                const double delay = shotIndex == 0 ? g_shotDelaySec : g_shotIntervalSec;
                if (GetTimeSeconds() - shotStart >= delay)
                {
                    std::string path = g_shotPath;
                    if (g_shotCount > 1)
                    {
                        char suffix[8];
                        std::snprintf(suffix, sizeof(suffix), "_%02d", shotIndex);
                        const size_t dot = path.rfind('.');
                        if (dot != std::string::npos) { path.insert(dot, suffix); }
                        else { path += suffix; }
                    }
                    const bool ok = Screenshot::SaveBackbufferPng(renderer, path);
                    OutputDebugStringA(ok ? "[shot] saved\n" : "[shot] FAILED\n");
                    ++shotIndex;
                    if (shotIndex >= g_shotCount)
                    {
                        g_shotPath.clear();
                        isRunning_ = false;
                    }
                    else
                    {
                        vfx::g_windStep = static_cast<float>(g_shotStepSec);
                        shotStart = GetTimeSeconds();
                    }
                }
            }

            // "--profdump=<path>": temporary VSM perf harness — after the same warmup delay, dump the
            // profiler overlay to a file and quit. Independent of --shot so timings can be swept headlessly.
            if (!g_profDumpPath.empty())
            {
                static double profStart = GetTimeSeconds();
                if (GetTimeSeconds() - profStart >= g_shotDelaySec)
                {
                    const bool ok = Profiler::Get().DumpOverlay(g_profDumpPath);
                    OutputDebugStringA(ok ? "[profdump] saved\n" : "[profdump] FAILED\n");
                    g_profDumpPath.clear();
                    isRunning_ = false;
                }
            }

            // "--trace=<frames>": the same capture the CaptureTrace key requests, but headless.
            // Without it a profiler change cannot be verified without driving the GUI — which is
            // how an 82 us hole in the GPU timeline went unexplained for so long. Requests after
            // the warmup delay, then keeps running until the capture has written itself out.
            if (g_traceFrames != 0)
            {
                static double traceStart = GetTimeSeconds();
                static uint32_t framesLeft = 0;
                static bool requested = false;
                if (!requested && GetTimeSeconds() - traceStart >= g_shotDelaySec)
                {
                    Profiler::Get().RequestTraceCapture(g_traceFrames);
                    framesLeft = g_traceFrames + 30; // margin: the write happens on the last frame
                    requested = true;
                }
                else if (requested && --framesLeft == 0)
                {
                    g_traceFrames = 0;
                    isRunning_ = false;
                }
            }


            Profiler::Get().EndFrame();
        }

        TaskSystem::Get().Stop();

        renderer.WaitForPreviousFrame();
        scene.Clear();
        Systems::DestroyOceanSimulation();
        renderer.Shutdown();
    }

    Systems::Shutdown();
    systems_.reset();

    mi_register_output(MiOut, nullptr);
    mi_collect(true);
    mi_option_set(mi_option_show_stats, 1);
    //mi_stats_print(nullptr);
}
