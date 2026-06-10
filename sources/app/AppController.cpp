#include "app/AppController.h"

#include "app/scene/Scene.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "text/TextManager.h"
#include "core/math/Math.h"

void AppController::Tick(InputManager& input, Renderer& renderer, Scene& scene)
{
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

    scene.SetRenderSettings(settings_);
}

void AppController::BuildHud(Renderer& renderer, const Scene& scene) const
{
    auto* tb = renderer.GetTextManager();
    tb->Begin(renderer.GetWidth(), renderer.GetHeight(), 1.0f);
    tb->AddTextfShadow(8, 8, 32.0f, float4(1, 1, 1, 0.6f), true, L"FPS:%.0f MS:%0.2f Scale:%0.2f", renderer.GetFPS(), 1000.0f / renderer.GetFPS(), renderer.GetRenderResolutionScale());
    const Camera& camera = scene.CameraRef();
    const auto& camPos = camera.GetPosition();
    tb->AddTextfShadow(8, 8 + 32, 16.0f, float4(1, 1, 1, 0.9f), true, L"Cam: %0.2f %0.2f %0.2f, speed: %0.2f, DLSS: %i, SSR: %i, FXAA: %i", camPos.x, camPos.y, camPos.z, camera.GetMoveSpeedMult(),
        renderer.IsDlssActive() ? (int)renderer.GetDlssMode() : -1, (int)settings_.ssrTechnique, (int)settings_.doFxaa);
}
