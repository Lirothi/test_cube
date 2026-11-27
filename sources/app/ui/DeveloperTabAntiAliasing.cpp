// R10.4: Developer Controls > AA / Scale (DeveloperWindow.h). A shipped build compiles none of this.
#if !TC_SHIPPING
#include "app/ui/DeveloperWindow.h"
#include "app/ui/DeveloperTabs.h" // GRAPHICS_CONTROL

#include <string>

#include "app/GraphicsSettings.h"
#include "app/scene/Scene.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"

namespace
{
    struct DlssModeOption
    {
        sl::DLSSMode mode;
        const char* label;
    };

    constexpr DlssModeOption kDlssModes[] = {
        { sl::DLSSMode::eOff, "Off" },
        { sl::DLSSMode::eMaxPerformance, "Max Performance" },
        { sl::DLSSMode::eBalanced, "Balanced" },
        { sl::DLSSMode::eMaxQuality, "Max Quality" },
        { sl::DLSSMode::eUltraPerformance, "Ultra Performance" },
        { sl::DLSSMode::eUltraQuality, "Ultra Quality" },
        { sl::DLSSMode::eDLAA, "DLAA" },
    };

    const char* DlssModeLabel(sl::DLSSMode mode)
    {
        for (const DlssModeOption& option : kDlssModes)
        {
            if (option.mode == mode)
            {
                return option.label;
            }
        }
        return "Unknown";
    }
}

void DeveloperWindow::DrawAntiAliasingTab(Renderer& renderer, Scene& scene, SceneRenderSettings& settings,
    GraphicsSettingsManager& graphicsSettings)
{
    const bool dlssAvailable = renderer.IsDlssAvailable();
    ImGui::Text("DLSS status: %s", renderer.IsDlssActive() ? "Active" : (dlssAvailable ? "Inactive" : "Unavailable"));

    ImGui::BeginDisabled(!dlssAvailable);
    bool dlssEnabled = renderer.IsDlssActive();
    if (GRAPHICS_CONTROL(DlssEnabled, "dlssEnabled",
                         ImGui::Checkbox("DLSS enabled", &dlssEnabled)))
    {
        if (dlssEnabled)
        {
            if (renderer.GetDlssMode() == sl::DLSSMode::eOff)
            {
                renderer.SetDlssMode(sl::DLSSMode::eBalanced);
            }
            renderer.SetDlssActive(true);
        }
        else
        {
            renderer.SetDlssActive(false);
        }
    }

    const sl::DLSSMode currentDlssMode = renderer.GetDlssMode();
    GRAPHICS_CONTROL(DlssMode, "dlssMode", [&]()
    {
        bool changed = false;
        if (ImGui::BeginCombo("DLSS quality", DlssModeLabel(currentDlssMode)))
        {
            for (const DlssModeOption& option : kDlssModes)
            {
                const bool selected = option.mode == currentDlssMode;
                if (ImGui::Selectable(option.label, selected))
                {
                    renderer.SetDlssMode(option.mode);
                    renderer.SetDlssActive(option.mode != sl::DLSSMode::eOff);
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }());
    ImGui::EndDisabled();

    if (!dlssAvailable)
    {
        ImGui::TextDisabled("Streamline DLSS is not available for this run.");
    }

    GRAPHICS_CONTROL(Fxaa, "fxaa", ImGui::Checkbox("FXAA", &settings.doFxaa));

    ImGui::Separator();
    const bool dlssControlsRenderScale = renderer.IsDlssActive();
    ImGui::BeginDisabled(dlssControlsRenderScale);
    float renderScale = renderer.GetRenderResolutionScale();
    if (GRAPHICS_CONTROL(RenderScale, "renderScale",
        ImGui::SliderFloat("Render scale", &renderScale,
                           0.1f, 1.0f, "%.2f")))
    {
        renderer.SetRenderResolutionScale(renderScale);
    }
    ImGui::EndDisabled();
    if (dlssControlsRenderScale)
    {
        ImGui::TextDisabled("DLSS quality controls the render scale while active.");
    }

    ImGui::Separator();
    if (ImGui::Button("Reset AA / scale to defaults"))
    {
        graphicsSettings.ResetUpscale(renderer, scene, settings);
    }
}

#endif // !TC_SHIPPING
