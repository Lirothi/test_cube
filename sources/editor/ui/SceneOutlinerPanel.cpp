#include "editor/ui/SceneOutlinerPanel.h"
#if WITH_EDITOR

#include "imgui.h"

OutlinerAction SceneOutlinerPanel::Draw(EditorSceneDocument& document, EditorObjectId& selectedObject, bool* open)
{
    OutlinerAction action;

    ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene Outliner", open))
    {
        ImGui::End();
        return action;
    }

    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##outliner", 4, flags, ImVec2(0.0f, ImGui::GetContentRegionAvail().y)))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (EditorObject& obj : document.Objects())
        {
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(obj.id.value));

            ImGui::TableNextColumn();
            const bool isSelected = (selectedObject.value == obj.id.value);
            if (ImGui::Selectable(obj.name.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns))
            {
                selectedObject = obj.id;
            }

            // Right-click context menu (Unreal-style): delete this object.
            if (ImGui::BeginPopupContextItem())
            {
                selectedObject = obj.id; // right-click selects the row too
                if (ImGui::MenuItem("Delete"))
                {
                    action.type = OutlinerAction::Type::DeleteObject;
                    action.target = obj.id;
                }
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(obj.type.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(obj.id.value));

            ImGui::TableNextColumn();
            bool enabled = obj.enabled;
            if (ImGui::Checkbox("##enabled", &enabled))
            {
                obj.enabled = enabled;
                document.SetDirty(true);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::End();
    return action;
}

#endif // WITH_EDITOR
