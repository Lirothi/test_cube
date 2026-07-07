#include "editor/ui/SceneOutlinerPanel.h"
#if WITH_EDITOR

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"

namespace
{
    enum class OutlinerTypeFilter
    {
        All,
        Meshes,
        StaticMesh,
        TransparentMesh,
        InstancedModels,
        Lights,
        Cameras,
        Skybox,
        Ocean,
        Other
    };

    struct TypeFilterOption
    {
        const char* label;
        OutlinerTypeFilter filter;
    };

    constexpr TypeFilterOption kTypeFilters[] = {
        { "All Types",        OutlinerTypeFilter::All },
        { "Meshes",           OutlinerTypeFilter::Meshes },
        { "Static Mesh",      OutlinerTypeFilter::StaticMesh },
        { "Transparent Mesh", OutlinerTypeFilter::TransparentMesh },
        { "Instanced Models", OutlinerTypeFilter::InstancedModels },
        { "Lights",           OutlinerTypeFilter::Lights },
        { "Cameras",          OutlinerTypeFilter::Cameras },
        { "Skybox",           OutlinerTypeFilter::Skybox },
        { "Ocean",            OutlinerTypeFilter::Ocean },
        { "Other",            OutlinerTypeFilter::Other },
    };

    constexpr const char* kSearchPropertyKeys[] = {
        "model",
        "material",
        "texture",
        "preset",
        "shader",
        "inputLayout",
    };

    std::string LowerCopy(std::string text)
    {
        for (char& ch : text)
        {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text;
    }

    bool ContainsLower(const std::string& haystack, const std::string& lowerNeedle)
    {
        return LowerCopy(haystack).find(lowerNeedle) != std::string::npos;
    }

    void DrawSearchInputWithClear(const char* id, const char* hint, char* buffer, size_t bufferSize)
    {
        ImGui::SetNextItemWidth(-1.0f);
        const ImVec2 inputMin = ImGui::GetCursorScreenPos();
        const float inputWidth = ImGui::CalcItemWidth();
        const float frameHeight = ImGui::GetFrameHeight();
        const float buttonSize = frameHeight > 4.0f ? frameHeight - 4.0f : frameHeight;
        ImVec2 buttonPos(
            inputMin.x + inputWidth - buttonSize - 3.0f,
            inputMin.y + (frameHeight - buttonSize) * 0.5f);
        ImVec2 buttonMax(buttonPos.x + buttonSize, buttonPos.y + buttonSize);

        if (buffer[0] != '\0' &&
            ImGui::IsMouseHoveringRect(buttonPos, buttonMax, true) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            buffer[0] = '\0';
            ImGui::ClearActiveID();
        }

        ImGui::InputTextWithHint(id, hint, buffer, bufferSize);
        if (buffer[0] == '\0')
        {
            return;
        }

        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        const float actualFrameHeight = itemMax.y - itemMin.y;
        const float actualButtonSize = actualFrameHeight > 4.0f ? actualFrameHeight - 4.0f : actualFrameHeight;
        buttonPos = ImVec2(
            itemMax.x - actualButtonSize - 3.0f,
            itemMin.y + (actualFrameHeight - actualButtonSize) * 0.5f);
        buttonMax = ImVec2(buttonPos.x + actualButtonSize, buttonPos.y + actualButtonSize);

        const bool clearHovered = ImGui::IsMouseHoveringRect(buttonPos, buttonMax, true);
        const ImU32 color = ImGui::GetColorU32(clearHovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        const float pad = actualButtonSize > 12.0f ? 5.0f : 4.0f;
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        drawList->AddLine(
            ImVec2(buttonPos.x + pad, buttonPos.y + pad),
            ImVec2(buttonPos.x + actualButtonSize - pad, buttonPos.y + actualButtonSize - pad),
            color,
            1.5f);
        drawList->AddLine(
            ImVec2(buttonPos.x + actualButtonSize - pad, buttonPos.y + pad),
            ImVec2(buttonPos.x + pad, buttonPos.y + actualButtonSize - pad),
            color,
            1.5f);
    }

    bool IsLightType(const std::string& type)
    {
        return type == "pointLight" || type == "spotLight" || type == "directionalLight";
    }

    bool IsCameraType(const std::string& type)
    {
        return type == "camera" || type == "freeCameraStart";
    }

    bool IsMeshType(const std::string& type)
    {
        return type == "staticMesh" || type == "transparentMesh" || type == "instancedModels";
    }

    bool IsKnownType(const std::string& type)
    {
        return IsMeshType(type) || IsLightType(type) || IsCameraType(type) ||
            type == "skybox" || type == "ocean";
    }

    bool MatchesTypeFilter(const EditorObject& object, OutlinerTypeFilter filter)
    {
        const std::string& type = object.type;
        switch (filter)
        {
        case OutlinerTypeFilter::All:             return true;
        case OutlinerTypeFilter::Meshes:          return IsMeshType(type);
        case OutlinerTypeFilter::StaticMesh:      return type == "staticMesh";
        case OutlinerTypeFilter::TransparentMesh: return type == "transparentMesh";
        case OutlinerTypeFilter::InstancedModels: return type == "instancedModels";
        case OutlinerTypeFilter::Lights:          return IsLightType(type);
        case OutlinerTypeFilter::Cameras:         return IsCameraType(type);
        case OutlinerTypeFilter::Skybox:          return type == "skybox";
        case OutlinerTypeFilter::Ocean:           return type == "ocean";
        case OutlinerTypeFilter::Other:           return !IsKnownType(type);
        }
        return true;
    }

    bool MatchesSearch(const EditorObject& object, const std::string& lowerNeedle)
    {
        if (lowerNeedle.empty())
        {
            return true;
        }

        if (ContainsLower(object.name, lowerNeedle) ||
            ContainsLower(object.type, lowerNeedle))
        {
            return true;
        }

        char idText[32];
        std::snprintf(idText, sizeof(idText), "%llu",
            static_cast<unsigned long long>(object.id.value));
        if (ContainsLower(idText, lowerNeedle))
        {
            return true;
        }

        if (object.properties.is_object())
        {
            for (const char* key : kSearchPropertyKeys)
            {
                const auto it = object.properties.find(key);
                if (it != object.properties.end() && it->is_string() &&
                    ContainsLower(it->get<std::string>(), lowerNeedle))
                {
                    return true;
                }
            }
        }

        return false;
    }
}

OutlinerAction SceneOutlinerPanel::Draw(EditorSceneDocument& document, EditorObjectId& selectedObject, bool* open)
{
    OutlinerAction action;

    ImGui::SetNextWindowSize(ImVec2(360.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene Outliner", open))
    {
        ImGui::End();
        return action;
    }

    if (typeFilterIndex_ < 0 || typeFilterIndex_ >= IM_ARRAYSIZE(kTypeFilters))
    {
        typeFilterIndex_ = 0;
    }

    DrawSearchInputWithClear("##outlinerSearch", "Search name, type, id, or asset...", searchBuffer_,
        sizeof(searchBuffer_));

    ImGui::Checkbox("Objects", &showObjects_);
    ImGui::SameLine();
    ImGui::Checkbox("Environment", &showEnvironment_);
    ImGui::SameLine();
    ImGui::TextUnformatted("Type");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##outlinerTypeFilter", kTypeFilters[typeFilterIndex_].label))
    {
        for (int i = 0; i < IM_ARRAYSIZE(kTypeFilters); ++i)
        {
            const bool selected = (typeFilterIndex_ == i);
            if (ImGui::Selectable(kTypeFilters[i].label, selected))
            {
                typeFilterIndex_ = i;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    const std::string lowerNeedle = LowerCopy(searchBuffer_);
    const OutlinerTypeFilter typeFilter = kTypeFilters[typeFilterIndex_].filter;
    const auto rowVisible = [&](const EditorObject& object, bool environment)
    {
        if ((!environment && !showObjects_) || (environment && !showEnvironment_))
        {
            return false;
        }
        return MatchesTypeFilter(object, typeFilter) && MatchesSearch(object, lowerNeedle);
    };

    const int totalRows = static_cast<int>(document.Objects().size() + document.Environment().size());
    int visibleRows = 0;
    int visibleEnvironmentRows = 0;
    bool selectedExists = false;
    bool selectedVisible = false;
    for (const EditorObject& obj : document.Objects())
    {
        const bool visible = rowVisible(obj, false);
        visibleRows += visible ? 1 : 0;
        if (selectedObject.value == obj.id.value)
        {
            selectedExists = true;
            selectedVisible = visible;
        }
    }
    for (const EditorObject& env : document.Environment())
    {
        const bool visible = rowVisible(env, true);
        visibleRows += visible ? 1 : 0;
        visibleEnvironmentRows += visible ? 1 : 0;
        if (selectedObject.value == env.id.value)
        {
            selectedExists = true;
            selectedVisible = visible;
        }
    }

    ImGui::Text("Showing %d of %d rows", visibleRows, totalRows);

    const float footerHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##outliner", 4, flags, ImVec2(0.0f, -footerHeight)))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        if (visibleRows == 0)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled(totalRows == 0 ? "No rows in document." : "No rows match current filters.");
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
        }

        for (EditorObject& obj : document.Objects())
        {
            if (!rowVisible(obj, false))
            {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(obj.id.value));

            ImGui::TableNextColumn();
            const bool isSelected = (selectedObject.value == obj.id.value);
            // AllowOverlap so the "On" checkbox in a later column stays clickable
            // instead of being covered by this row-spanning selectable.
            if (ImGui::Selectable(obj.name.c_str(), isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
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
                action.type = OutlinerAction::Type::SetEnabled;
                action.target = obj.id;
                action.enabledValue = enabled;
            }

            ImGui::PopID();
        }

        // Environment entities (camera/lights/skybox/ocean): selectable under a
        // label. Some expose an enable toggle, but they are not deletable here.
        if (visibleEnvironmentRows > 0)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("Environment");
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();

            for (EditorObject& env : document.Environment())
            {
                if (!rowVisible(env, true))
                {
                    continue;
                }

                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(env.id.value));

                ImGui::TableNextColumn();
                const bool isSelected = (selectedObject.value == env.id.value);
                // AllowOverlap so the enable checkbox stays clickable under the
                // row-spanning selectable (same as object rows).
                if (ImGui::Selectable(env.name.c_str(), isSelected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    selectedObject = env.id;
                }

                ImGui::TableNextColumn();
                ImGui::TextUnformatted(env.type.c_str());

                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(env.id.value));

                ImGui::TableNextColumn();
                // Enable toggle for lights + ocean (camera/skybox have none).
                const bool supportsEnable =
                    env.type == "pointLight" || env.type == "spotLight" ||
                    env.type == "directionalLight" || env.type == "ocean";
                if (supportsEnable)
                {
                    bool enabled = env.properties.value("enabled", true);
                    if (ImGui::Checkbox("##enabled", &enabled))
                    {
                        action.type = OutlinerAction::Type::SetEnvEnabled;
                        action.target = env.id;
                        action.enabledValue = enabled;
                    }
                }

                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }

    if (selectedObject.value != 0 && selectedExists && !selectedVisible)
    {
        ImGui::TextDisabled("Selected ID %llu is hidden by the current filters.",
            static_cast<unsigned long long>(selectedObject.value));
    }
    else if (selectedObject.value != 0 && !selectedExists)
    {
        ImGui::TextDisabled("Selected ID %llu is not in the document.",
            static_cast<unsigned long long>(selectedObject.value));
    }

    ImGui::End();
    return action;
}

#endif // WITH_EDITOR
