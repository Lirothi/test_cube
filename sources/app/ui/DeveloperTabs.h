#pragma once
#if !TC_SHIPPING

#include <algorithm>
#include <string>

#include "app/ui/DeveloperWindow.h"
#include "app/GraphicsSettings.h"      // GraphicsControl, ScalabilityKnobOf, GraphicsSettingsManager::ResetControl
#include "app/settings/Scalability.h"  // the scalability dot
#include "imgui.h"
#include "imgui_internal.h"            // the reset icon is laid into the control's own item

// R10.4: what the Developer Controls' tab files (app/ui/DeveloperTab*.cpp) share with DeveloperWindow.cpp: the widget
// a graphics setting is drawn through, and the GRAPHICS_CONTROL macros that call it. Both were Draw's own (a lambda and
// its macros) while every tab was a block of Draw; the widget reaches Draw's arguments and locals through frame_.

// Draw's arguments and locals graphicsControl reaches, for the length of Draw.
struct DeveloperWindow::Frame
{
    Renderer& renderer;
    Scene& scene;
    SceneRenderSettings& settings;
    GraphicsSettingsManager& graphicsSettings;
    ImTextureID& resetIconTexture;  // made once the window is drawn, after frame_ is set
    bool& graphicsSettingsDirty;    // what Draw returns: a graphics setting changed
};

template <class DrawFn>
bool DeveloperWindow::graphicsControl(GraphicsControl control, const char* id, float explicitWidth, const DrawFn& draw)
{
    // Draw's names for what this widget reaches (the body is the lambda's, verbatim).
    Renderer& renderer = frame_->renderer;
    Scene& scene = frame_->scene;
    SceneRenderSettings& settings = frame_->settings;
    GraphicsSettingsManager& graphicsSettings = frame_->graphicsSettings;
    const ImTextureID resetIconTexture = frame_->resetIconTexture;
    bool& graphicsSettingsDirty = frame_->graphicsSettingsDirty;
    const float buttonSize = ImGui::GetFrameHeight();
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 itemStart = ImGui::GetCursorScreenPos();
    const float controlWidth = IsGraphicsToggle(control)
        ? buttonSize
        : (explicitWidth > 0.0f ? explicitWidth : ImGui::CalcItemWidth());

    // Reserve a button-sized hole between the widget frame and its visible label. The reset
    // item is submitted into that hole after the widget, so the label still belongs to the
    // native ImGui control and keeps its normal id/hover behaviour.
    ImGui::SetNextItemAllowOverlap();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing,
        ImVec2(style.ItemInnerSpacing.x + buttonSize + style.ItemSpacing.x,
               style.ItemInnerSpacing.y));
    const bool changed = draw();
    ImGui::PopStyleVar();
    ImGuiContext& context = *ImGui::GetCurrentContext();
    const ImGuiLastItemData controlItem = context.LastItemData;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 cursorPos = window->DC.CursorPos;
    const ImVec2 cursorPosPrevLine = window->DC.CursorPosPrevLine;
    const ImVec2 cursorMaxPos = window->DC.CursorMaxPos;
    const ImVec2 idealMaxPos = window->DC.IdealMaxPos;
    const ImVec2 currLineSize = window->DC.CurrLineSize;
    const ImVec2 prevLineSize = window->DC.PrevLineSize;
    const float currLineTextBaseOffset = window->DC.CurrLineTextBaseOffset;
    const float prevLineTextBaseOffset = window->DC.PrevLineTextBaseOffset;
    const bool isSameLine = window->DC.IsSameLine;
    const bool isSetPos = window->DC.IsSetPos;

    window->DC.CursorPos = ImVec2(
        itemStart.x + controlWidth + style.ItemInnerSpacing.x,
        itemStart.y);
    ImGui::PushID(id);
    if (ResetIconButton(resetIconTexture))
    {
        graphicsSettings.ResetControl(control, renderer, scene, settings);
    }
    // docs/scalability_plan.md §3.3: a control a graphics level sets wears a dot on its reset icon -- blue while the
    // level's value holds, orange once it was changed by hand over the level (an override, also listed in the
    // Scalability tab). Its reset then goes back to the level, not to the project default.
    std::string manager;
    bool overridden = false;
    if (const char* knob = ScalabilityKnobOf(control))
    {
        const settings::Scalability& sc = settings::TheScalability();
        manager = sc.ManagerLabel(knob);
        overridden = !manager.empty() && sc.Overrides().count(knob) != 0u;
    }
    if (!manager.empty())
    {
        const ImVec2 iconMax = ImGui::GetItemRectMax();
        const ImVec2 iconMin = ImGui::GetItemRectMin();
        const float r = std::max(3.0f, (iconMax.y - iconMin.y) * 0.2f);
        const ImU32 dot = overridden ? ImGui::GetColorU32(ImVec4(1.0f, 0.42f, 0.05f, 1.0f))
                                     : ImGui::GetColorU32(ImVec4(0.15f, 0.45f, 1.0f, 1.0f));
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(iconMax.x - r, iconMin.y + r), r, dot);
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        if (manager.empty()) { ImGui::SetTooltip("Reset this control to the project default."); }
        else if (overridden)
        {
            ImGui::SetTooltip("Set by hand over the graphics level (%s).\nReset: back to the level's value.",
                              manager.c_str());
        }
        else
        {
            ImGui::SetTooltip("Set by the graphics level (%s; Developer Controls > Scalability).\nA change here is kept "
                              "over the level, as set by hand. Reset: back to the level's value.", manager.c_str());
        }
    }
    ImGui::PopID();

    // The reset overlaps a hole already included in the control's item rect. Restore the
    // control's layout state so SameLine(), groups and following rows behave exactly as if the
    // button had been part of the native widget.
    window->DC.CursorPos = cursorPos;
    window->DC.CursorPosPrevLine = cursorPosPrevLine;
    window->DC.CursorMaxPos = cursorMaxPos;
    window->DC.IdealMaxPos = idealMaxPos;
    window->DC.CurrLineSize = currLineSize;
    window->DC.PrevLineSize = prevLineSize;
    window->DC.CurrLineTextBaseOffset = currLineTextBaseOffset;
    window->DC.PrevLineTextBaseOffset = prevLineTextBaseOffset;
    window->DC.IsSameLine = isSameLine;
    window->DC.IsSetPos = isSetPos;

    // Keep callers' immediately-following DevHelp()/IsItemHovered() attached to the setting,
    // not to the reset button inserted between its frame and label.
    context.LastItemData = controlItem;
    graphicsSettingsDirty |= changed;
    return changed;
}

#define GRAPHICS_CONTROL(setting, id, expression) \
    graphicsControl(GraphicsControl::setting, id, -1.0f, [&]() { return (expression); })
#define GRAPHICS_CONTROL_WIDTH(setting, id, width, expression) \
    graphicsControl(GraphicsControl::setting, id, width, [&]() \
    { \
        ImGui::SetNextItemWidth(width); \
        return (expression); \
    })

#endif // !TC_SHIPPING
