#include "editor/ui/CommandHistoryPanel.h"
#if WITH_EDITOR

#include <limits>
#include <string>

#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "editor/EditorContext.h"
#include "editor/commands/EditorCommand.h"
#include "editor/commands/EditorCommandStack.h"
#include "imgui.h"

void CommandHistoryPanel::Draw(
    EditorContext& ctx,
    EditorCommandStack& commandStack,
    bool* open)
{
    CPU_SCOPE(ProfilerScopes::kCommandHistoryDraw);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Command History", open))
    {
        ImGui::End();
        return;
    }

    ImGui::BeginDisabled(!commandStack.CanUndo());
    if (ImGui::ArrowButton("##historyUndo", ImGuiDir_Left))
    {
        commandStack.Undo(ctx);
        jumpFailed_ = false;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Undo");
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(!commandStack.CanRedo());
    if (ImGui::ArrowButton("##historyRedo", ImGuiDir_Right))
    {
        commandStack.Redo(ctx);
        jumpFailed_ = false;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("Redo");
    }

    ImGui::SameLine();
    ImGui::Text(
        "State %zu / %zu",
        commandStack.AppliedCount(),
        commandStack.HistorySize());
    if (jumpFailed_)
    {
        ImGui::TextDisabled("State restore failed.");
    }
    ImGui::Separator();

    const std::size_t historySize = commandStack.HistorySize();
    const std::size_t appliedCount = commandStack.AppliedCount();
    const bool scrollToCurrent = appliedCount != lastAppliedCount_;
    std::size_t requestedState = std::numeric_limits<std::size_t>::max();

    if (ImGui::BeginChild("##commandHistoryEntries", ImVec2(0.0f, 0.0f), false))
    {
        const bool initialSelected = appliedCount == 0;
        if (ImGui::Selectable("Initial State", initialSelected))
        {
            requestedState = 0;
        }
        if (initialSelected && scrollToCurrent)
        {
            ImGui::SetScrollHereY(0.5f);
        }

        for (std::size_t index = 0; index < historySize; ++index)
        {
            const EditorCommand* command = commandStack.HistoryEntry(index);
            if (!command)
            {
                continue;
            }

            ImGui::PushID(static_cast<int>(index));
            const bool applied = index < appliedCount;
            const bool selected = index + 1 == appliedCount;
            if (!applied)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            }

            std::string label = std::to_string(index + 1);
            label += "  ";
            label += command->HistoryLabel();
            if (ImGui::Selectable(label.c_str(), selected))
            {
                requestedState = index + 1;
            }

            if (!applied)
            {
                ImGui::PopStyleColor();
            }
            if (selected && scrollToCurrent)
            {
                ImGui::SetScrollHereY(0.5f);
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    if (requestedState != std::numeric_limits<std::size_t>::max())
    {
        jumpFailed_ = !commandStack.MoveTo(ctx, requestedState);
        lastAppliedCount_ = std::numeric_limits<std::size_t>::max();
    }
    else
    {
        lastAppliedCount_ = commandStack.AppliedCount();
    }

    ImGui::End();
}

#endif // WITH_EDITOR
