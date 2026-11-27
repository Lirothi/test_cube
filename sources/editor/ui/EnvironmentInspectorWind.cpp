#include "editor/ui/EnvironmentInspector.h"
#if WITH_EDITOR

#include <string>

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "imgui.h"

// R10.3: the Inspector's wind -- flow, gusts, distance fade, the debug freeze (EnvironmentInspector.h).

void EnvironmentInspector::DrawWind()
{
    const vfx::WindState& wind = ctx.scene.GetWindState();

    ImGui::SeparatorText("Flow");
    dragF("Direction", "directionDeg", wind.directionDeg,
        0.5f, -360.0f, 360.0f, "%.1f deg");

    float strength = JsonFloat(tgt(), "strength", wind.strength);
    const bool strengthChanged = ImGui::SliderFloat("Strength", &strength, 0.0f, 1.0f);
    beginContinuousEdit(strengthChanged);
    if (strengthChanged) { tgt()["strength"] = strength; }
    trackContinuousEdit(strengthChanged);

    dragF("Sway Frequency", "swayFrequency", wind.swayFrequency,
        0.01f, 0.0f, 10.0f, "%.2f");
    dragF("Foliage Sway", "foliageSwayMeters", wind.foliageSwayMeters,
        0.01f, 0.0f, 5.0f, "%.2f m");

    ImGui::SeparatorText("Gusts");
    const auto dragGust = [&](const char* label, const char* key, float def,
        float speed, float lo, float hi, const char* fmt)
    {
        float value = def;
        const auto gustIt = tgt().find("gust");
        if (gustIt != tgt().end() && gustIt->is_object())
        {
            value = JsonFloat(*gustIt, key, def);
        }
        const bool changed = ImGui::DragFloat(label, &value, speed, lo, hi, fmt);
        beginContinuousEdit(changed);
        if (changed)
        {
            if (!tgt().contains("gust") || !tgt()["gust"].is_object())
            {
                tgt()["gust"] = nlohmann::json::object();
            }
            tgt()["gust"][key] = value;
        }
        trackContinuousEdit(changed);
    };
    dragGust("Amplitude", "amplitude", wind.gustAmplitude,
        0.01f, 0.0f, 2.0f, "%.2f");
    dragGust("Frequency", "frequencyHz", wind.gustFrequencyHz,
        0.005f, 0.0f, 2.0f, "%.3f Hz");
    ImGui::TextDisabled("Current envelope: %.2fx", wind.gustMul);

    // W8 distance fade. Also global rather than level data: the right distance depends on
    // the shot, and a value baked into a level would freeze foliage someone else framed
    // deliberately. Off while End <= Start.
    ImGui::SeparatorText("Distance Fade");
    ImGui::DragFloat("Fade Start", &vfx::g_windFadeStart_game, 1.0f, 0.0f, 5000.0f, "%.0f m");
    ImGui::DragFloat("Fade End", &vfx::g_windFadeEnd_game, 1.0f, 0.0f, 5000.0f, "%.0f m");
    if (!(vfx::g_windFadeEnd_game > vfx::g_windFadeStart_game))
    {
        ImGui::TextDisabled("Disabled (End must exceed Start)");
    }

    // W8 debug freeze. Deliberately NOT written into the object properties: this is a viewing
    // data, so it must never end up saved in the level or land in the undo stack.
    ImGui::SeparatorText("Debug");
    ImGui::Checkbox("Freeze time", &vfx::g_windFreeze_game);
    ImGui::BeginDisabled(!vfx::g_windFreeze_game);
    ImGui::SameLine();
    static float s_windStepSeconds = 1.0f; // UI-only; the freeze itself is a global
    if (ImGui::Button("Step")) { vfx::g_windStep_game = s_windStepSeconds; }
    ImGui::SetNextItemWidth(120.0f);
    ImGui::DragFloat("Step size", &s_windStepSeconds, 0.01f, 0.001f, 10.0f, "%.3f s");
    ImGui::EndDisabled();
    ImGui::TextDisabled("Wind clock: %.3f s%s", wind.time,
        vfx::g_windFreeze_game ? " (frozen)" : "");
}

#endif // WITH_EDITOR
