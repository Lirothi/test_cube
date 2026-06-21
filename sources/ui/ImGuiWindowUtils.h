#pragma once

#include "imgui.h"

namespace ui
{
    struct ImGuiWindowMaximizeState
    {
        bool maximized = false;
        ImVec2 restorePos = ImVec2(0.0f, 0.0f);
        ImVec2 restoreSize = ImVec2(0.0f, 0.0f);
    };

    inline bool HandleWindowTitleDoubleClickMaximize(ImGuiWindowMaximizeState& state)
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (!viewport)
        {
            return false;
        }

        const ImVec2 windowPos = ImGui::GetWindowPos();
        const ImVec2 windowSize = ImGui::GetWindowSize();
        const ImVec2 titleMin = windowPos;
        const ImVec2 titleMax(windowPos.x + windowSize.x, windowPos.y + ImGui::GetFrameHeight());
        const bool titleHovered = ImGui::IsMouseHoveringRect(titleMin, titleMax, false);
        const bool windowHovered =
            ImGui::IsWindowHovered(ImGuiHoveredFlags_RootWindow | ImGuiHoveredFlags_NoPopupHierarchy);
        const bool shouldToggle =
            titleHovered && windowHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

        if (shouldToggle)
        {
            if (state.maximized)
            {
                state.maximized = false;
                ImGui::SetWindowPos(state.restorePos, ImGuiCond_Always);
                ImGui::SetWindowSize(state.restoreSize, ImGuiCond_Always);
                return true;
            }

            state.restorePos = windowPos;
            state.restoreSize = windowSize;
            state.maximized = true;
        }

        if (state.maximized)
        {
            const ImVec2 workSize(
                viewport->WorkSize.x > 1.0f ? viewport->WorkSize.x : 1.0f,
                viewport->WorkSize.y > 1.0f ? viewport->WorkSize.y : 1.0f);
            ImGui::SetWindowPos(viewport->WorkPos, ImGuiCond_Always);
            ImGui::SetWindowSize(workSize, ImGuiCond_Always);
        }

        return shouldToggle;
    }
}
