#include "app/AppController.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "app/scene/Scene.h"
#include "imgui.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "text/TextManager.h"
#include "core/math/Math.h"

void AppController::Tick(InputManager& input, Renderer& renderer, Scene& scene, float deltaTime)
{
    const bool uiCapturingMouse = renderer.ImGuiWantsMouse();
    const bool uiCapturingKeyboard = renderer.ImGuiWantsKeyboard();
    const bool uiCapturingInput = uiCapturingMouse || uiCapturingKeyboard;

    if (!uiCapturingKeyboard)
    {
        if (input.WasActionPressed("ToggleBindings"))
        {
            showBindings_ = !showBindings_;
        }
        if (input.WasActionPressed("DebugTex"))
        {
            settings_.debugTexMode = !settings_.debugTexMode;
        }
        if (input.WasActionPressed("ToggleProfiler"))
        {
            settings_.showProfiler = !settings_.showProfiler;
        }
        if (input.WasActionPressed("ToggleSSR"))
        {
            const uint32_t next = (static_cast<uint32_t>(settings_.ssrTechnique) + 1u) % static_cast<uint32_t>(SsrTechnique::Count);
            settings_.ssrTechnique = static_cast<SsrTechnique>(next);
        }
        if (input.WasActionPressed("ToggleDLSS"))
        {
            renderer.SetDlssActive(!renderer.IsDlssActive());
        }
        if (input.WasActionPressed("ToggleFXAA"))
        {
            settings_.doFxaa = !settings_.doFxaa;
        }
        if (input.WasActionPressed("Wireframe"))
        {
            renderer.SetWireframeMode(!renderer.GetWireframeMode());
        }

        const auto setDlssMode = [&renderer](sl::DLSSMode mode)
        {
            renderer.SetDlssMode(mode);
        };

        if (input.WasActionPressed("SetDlssQualityOff"))
        {
            setDlssMode(sl::DLSSMode::eOff);
        }
        if (input.WasActionPressed("SetDlssQualityMaxPerformance"))
        {
            setDlssMode(sl::DLSSMode::eMaxPerformance);
        }
        if (input.WasActionPressed("SetDlssQualityBalanced"))
        {
            setDlssMode(sl::DLSSMode::eBalanced);
        }
        if (input.WasActionPressed("SetDlssQualityMaxQuality"))
        {
            setDlssMode(sl::DLSSMode::eMaxQuality);
        }
        if (input.WasActionPressed("SetDlssQualityUltraPerformance"))
        {
            setDlssMode(sl::DLSSMode::eUltraPerformance);
        }
        if (input.WasActionPressed("SetDlssQualityUltraQuality"))
        {
            setDlssMode(sl::DLSSMode::eUltraQuality);
        }
        if (input.WasActionPressed("SetDlssQualityDLAA"))
        {
            setDlssMode(sl::DLSSMode::eDLAA);
        }
    }

    scene.SetRenderSettings(settings_);

    // Camera input runs here (before Scene::Tick) so Scene itself never touches input.
    if (!uiCapturingInput)
    {
        scene.CameraRef().UpdateFromInput(input, deltaTime);
    }
    else if (input.IsMouseCaptured())
    {
        input.SetMouseCapture(false);
    }
}

void AppController::BuildHud(Renderer& renderer, const Scene& scene, const InputManager& input) const
{
    auto* tb = renderer.GetTextManager();
    tb->Begin(renderer.GetWidth(), renderer.GetHeight(), 1.0f);
    tb->AddTextfShadow(8, 8, 32.0f, float4(1, 1, 1, 0.6f), true, L"FPS:%.0f MS:%0.2f Scale:%0.2f", renderer.GetFPS(), 1000.0f / renderer.GetFPS(), renderer.GetRenderResolutionScale());
    const Camera& camera = scene.CameraRef();
    const auto& camPos = camera.GetPosition();
    tb->AddTextfShadow(8, 8 + 32, 16.0f, float4(1, 1, 1, 0.9f), true, L"Cam: %0.2f %0.2f %0.2f, speed: %0.2f, DLSS: %i, SSR: %i, FXAA: %i", camPos.x, camPos.y, camPos.z, camera.GetMoveSpeedMult(),
        renderer.IsDlssActive() ? (int)renderer.GetDlssMode() : -1, (int)settings_.ssrTechnique, (int)settings_.doFxaa);

    if (showBindings_)
    {
        BuildBindingsOverlay(renderer, input);
    }
}

void AppController::BuildDebugUi(Renderer& renderer)
{
    static int clickCount = 0;
    static char editText[128] = "edit me";

    ImGui::Begin("Debug UI");
    ImGui::Text("Dear ImGui is integrated");
    if (ImGui::Button("Button"))
    {
        ++clickCount;
    }
    ImGui::Text("Button clicks: %d", clickCount);
    ImGui::InputText("Edit box", editText, sizeof(editText));
    ImGui::Checkbox("FXAA", &settings_.doFxaa);
    ImGui::Checkbox("Profiler", &settings_.showProfiler);
    ImGui::Text("FPS: %.1f", renderer.GetFPS());
    ImGui::End();
}

void AppController::BuildBindingsOverlay(Renderer& renderer, const InputManager& input) const
{
    const auto& descs = input.GetBindingDescs();
    if (descs.empty())
    {
        return;
    }

    constexpr float kFontPx = 14.0f;
    constexpr float kApproxCharW = 8.0f; // Consolas advance at 14px, rounded up
    constexpr int kMargin = 16;
    const std::string title = "Controls [F1]";

    size_t keysColWidth = 0;
    for (const auto& d : descs)
    {
        keysColWidth = std::max(keysColWidth, d.keys.size());
    }

    size_t maxLineChars = title.size();
    for (const auto& d : descs)
    {
        maxLineChars = std::max(maxLineChars, keysColWidth + 2 + d.action.size());
    }

    const float regionW = static_cast<float>(maxLineChars) * kApproxCharW;
    const int x = std::max(0, static_cast<int>(renderer.GetWidth()) - static_cast<int>(regionW) - kMargin);

    auto* tb = renderer.GetTextManager();
    const TextManager::RegionId region = tb->CreateRegion(x, kMargin, TextManager::Align::Left);
    tb->RegionSetBackground(region, float4(0.0f, 0.0f, 0.0f, 0.55f));
    tb->RegionSetFixedWidth(region, regionW);

    tb->AddText(region, kFontPx, float4(1.0f, 1.0f, 0.6f, 0.95f), std::string_view(title), true);

    std::string line;
    for (const auto& d : descs)
    {
        line.assign(d.keys);
        line.append(keysColWidth - d.keys.size() + 2, ' ');
        line += d.action;
        tb->AddText(region, kFontPx, float4(1.0f, 1.0f, 1.0f, 0.9f), std::string_view(line), true);
    }
}
