#include "editor/ui/SceneOutlinerPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

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

    enum OutlinerColumnId
    {
        OutlinerColumn_Name = 1,
        OutlinerColumn_Type,
        OutlinerColumn_Id,
        OutlinerColumn_Enabled
    };

    enum class OutlinerGroup
    {
        Meshes,
        Lights,
        Cameras,
        Environment,
        Other
    };

    struct OutlinerRowRef
    {
        EditorObject* object = nullptr;
        bool environment = false;
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

    bool SupportsEnvironmentEnable(const EditorObject& object)
    {
        return IsLightType(object.type) || object.type == "ocean";
    }

    bool RowEnabledValue(const EditorObject& object, bool environment)
    {
        if (environment)
        {
            return SupportsEnvironmentEnable(object) && object.properties.value("enabled", true);
        }
        return object.enabled;
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

    OutlinerGroup GroupForObject(const EditorObject& object)
    {
        if (IsMeshType(object.type))
        {
            return OutlinerGroup::Meshes;
        }
        if (IsLightType(object.type))
        {
            return OutlinerGroup::Lights;
        }
        if (IsCameraType(object.type))
        {
            return OutlinerGroup::Cameras;
        }
        if (object.type == "skybox" || object.type == "ocean")
        {
            return OutlinerGroup::Environment;
        }
        return OutlinerGroup::Other;
    }

    bool SupportsDuplicate(const EditorObject& object, bool environment)
    {
        return environment ?
            (object.type == "pointLight" || object.type == "spotLight") :
            object.type != "ocean";
    }

    bool SupportsFrameSelection(const EditorObject& object, bool environment)
    {
        if (!environment)
        {
            return true;
        }

        const auto position = object.properties.find("position");
        return position != object.properties.end() &&
            position->is_array() &&
            position->size() >= 3;
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

    void PushEditorObjectId(EditorObjectId id)
    {
        char idText[32];
        std::snprintf(idText, sizeof(idText), "%llu",
            static_cast<unsigned long long>(id.value));
        ImGui::PushID(idText);
    }

    int CompareCaseInsensitive(const std::string& a, const std::string& b)
    {
        const std::string lowerA = LowerCopy(a);
        const std::string lowerB = LowerCopy(b);
        if (lowerA < lowerB)
        {
            return -1;
        }
        if (lowerA > lowerB)
        {
            return 1;
        }
        return 0;
    }

    int CompareRows(const EditorObject* a,
        bool aEnvironment,
        const EditorObject* b,
        bool bEnvironment,
        ImGuiID columnId)
    {
        if (!a || !b)
        {
            return a ? -1 : (b ? 1 : 0);
        }

        int result = 0;
        switch (columnId)
        {
        case OutlinerColumn_Name:
            result = CompareCaseInsensitive(a->name, b->name);
            break;
        case OutlinerColumn_Type:
            result = CompareCaseInsensitive(a->type, b->type);
            break;
        case OutlinerColumn_Id:
            if (a->id.value < b->id.value) { result = -1; }
            else if (a->id.value > b->id.value) { result = 1; }
            break;
        case OutlinerColumn_Enabled:
        {
            const bool aEnabled = RowEnabledValue(*a, aEnvironment);
            const bool bEnabled = RowEnabledValue(*b, bEnvironment);
            if (aEnabled != bEnabled)
            {
                result = aEnabled ? 1 : -1;
            }
            break;
        }
        default:
            break;
        }

        if (result == 0)
        {
            if (a->id.value < b->id.value) { result = -1; }
            else if (a->id.value > b->id.value) { result = 1; }
        }
        return result;
    }

    void SortRows(std::vector<OutlinerRowRef>& rows,
        const ImGuiTableSortSpecs* sortSpecs)
    {
        if (!sortSpecs || sortSpecs->SpecsCount <= 0)
        {
            return;
        }

        std::stable_sort(rows.begin(), rows.end(),
            [sortSpecs](const OutlinerRowRef& a, const OutlinerRowRef& b)
            {
                for (int n = 0; n < sortSpecs->SpecsCount; ++n)
                {
                    const ImGuiTableColumnSortSpecs& spec = sortSpecs->Specs[n];
                    int result = CompareRows(a.object,
                        a.environment,
                        b.object,
                        b.environment,
                        spec.ColumnUserID);
                    if (result == 0)
                    {
                        continue;
                    }
                    if (spec.SortDirection == ImGuiSortDirection_Descending)
                    {
                        result = -result;
                    }
                    return result < 0;
                }
                return false;
            });
    }
}

SceneOutlinerPanel::PersistentState SceneOutlinerPanel::GetPersistentState() const
{
    return PersistentState{
        meshesGroupOpen_,
        lightsGroupOpen_,
        camerasGroupOpen_,
        environmentGroupOpen_,
        otherGroupOpen_
    };
}

void SceneOutlinerPanel::SetPersistentState(const PersistentState& state)
{
    meshesGroupOpen_ = state.meshesGroupOpen;
    lightsGroupOpen_ = state.lightsGroupOpen;
    camerasGroupOpen_ = state.camerasGroupOpen;
    environmentGroupOpen_ = state.environmentGroupOpen;
    otherGroupOpen_ = state.otherGroupOpen;
}

OutlinerAction SceneOutlinerPanel::Draw(EditorSceneDocument& document, EditorSelection& selection, bool* open)
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
    std::vector<OutlinerRowRef> meshes;
    std::vector<OutlinerRowRef> lights;
    std::vector<OutlinerRowRef> cameras;
    std::vector<OutlinerRowRef> environmentRows;
    std::vector<OutlinerRowRef> other;
    bool selectedExists = false;
    bool selectedVisible = false;
    std::string selectedName;
    const EditorObjectId primarySelection = selection.Primary();
    const auto addVisibleRow = [&](EditorObject& object, bool environment)
    {
        std::vector<OutlinerRowRef>* group = nullptr;
        switch (GroupForObject(object))
        {
        case OutlinerGroup::Meshes:      group = &meshes; break;
        case OutlinerGroup::Lights:      group = &lights; break;
        case OutlinerGroup::Cameras:     group = &cameras; break;
        case OutlinerGroup::Environment: group = &environmentRows; break;
        case OutlinerGroup::Other:       group = &other; break;
        }
        if (group)
        {
            group->push_back(OutlinerRowRef{ &object, environment });
        }
    };

    for (EditorObject& obj : document.Objects())
    {
        const bool visible = rowVisible(obj, false);
        if (visible)
        {
            addVisibleRow(obj, false);
        }
        if (primarySelection.value == obj.id.value)
        {
            selectedExists = true;
            selectedVisible = visible;
            selectedName = obj.name;
        }
    }
    for (EditorObject& env : document.Environment())
    {
        const bool visible = rowVisible(env, true);
        if (visible)
        {
            addVisibleRow(env, true);
        }
        if (primarySelection.value == env.id.value)
        {
            selectedExists = true;
            selectedVisible = visible;
            selectedName = env.name;
        }
    }

    const int visibleRows = static_cast<int>(
        meshes.size() +
        lights.size() +
        cameras.size() +
        environmentRows.size() +
        other.size());
    ImGui::Text("Showing %d of %d rows", visibleRows, totalRows);
    if (selectedExists)
    {
        if (selection.Size() > 1)
        {
            ImGui::TextDisabled("Selected: %zu objects | Primary: %s",
                selection.Size(), selectedName.c_str());
        }
        else
        {
            ImGui::TextDisabled("Selected: %s", selectedName.c_str());
        }
    }

    const float footerHeight = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Sortable;
    if (ImGui::BeginTable("##outliner", 4, flags, ImVec2(0.0f, -footerHeight)))
    {
        ImGui::TableSetupColumn("Name",
            ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_DefaultSort,
            0.0f,
            OutlinerColumn_Name);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 110.0f, OutlinerColumn_Type);
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 48.0f, OutlinerColumn_Id);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28.0f, OutlinerColumn_Enabled);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs();
        SortRows(meshes, sortSpecs);
        SortRows(lights, sortSpecs);
        SortRows(cameras, sortSpecs);
        SortRows(environmentRows, sortSpecs);
        SortRows(other, sortSpecs);

        std::vector<EditorObjectId> displayedOrder;
        displayedOrder.reserve(static_cast<std::size_t>(visibleRows));
        const auto appendDisplayedRows = [&](const std::vector<OutlinerRowRef>& rows, bool groupOpen)
        {
            if (!groupOpen)
            {
                return;
            }
            for (const OutlinerRowRef& row : rows)
            {
                if (row.object)
                {
                    displayedOrder.push_back(row.object->id);
                }
            }
        };
        appendDisplayedRows(meshes, meshesGroupOpen_);
        appendDisplayedRows(lights, lightsGroupOpen_);
        appendDisplayedRows(cameras, camerasGroupOpen_);
        appendDisplayedRows(environmentRows, environmentGroupOpen_);
        appendDisplayedRows(other, otherGroupOpen_);

        const auto selectRow = [&](EditorObjectId id)
        {
            const ImGuiIO& io = ImGui::GetIO();
            if (io.KeyShift)
            {
                EditorObjectId anchor = rangeAnchor_.value != 0 ? rangeAnchor_ : selection.Primary();
                const auto anchorIt = std::find_if(displayedOrder.begin(), displayedOrder.end(),
                    [anchor](EditorObjectId item) { return item.value == anchor.value; });
                const auto clickedIt = std::find_if(displayedOrder.begin(), displayedOrder.end(),
                    [id](EditorObjectId item) { return item.value == id.value; });
                if (anchorIt != displayedOrder.end() && clickedIt != displayedOrder.end())
                {
                    auto first = anchorIt < clickedIt ? anchorIt : clickedIt;
                    auto last = anchorIt < clickedIt ? clickedIt : anchorIt;
                    std::vector<EditorObjectId> range(first, last + 1);
                    selection.SetOrdered(std::move(range), id);
                    return;
                }
            }

            rangeAnchor_ = id;
            if (io.KeyCtrl)
            {
                selection.Toggle(id);
            }
            else
            {
                selection.Replace(id);
            }
        };

        if (visibleRows == 0)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled(totalRows == 0 ? "No rows in document." : "No rows match current filters.");
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
            ImGui::TableNextColumn();
        }

        const auto drawRow = [&](const OutlinerRowRef& row)
        {
            EditorObject* obj = row.object;
            if (!obj)
            {
                return;
            }

            ImGui::TableNextRow();
            PushEditorObjectId(obj->id);

            ImGui::TableNextColumn();
            const bool isSelected = selection.Contains(obj->id);
            const bool renaming =
                !row.environment && renamingObject_.value == obj->id.value;
            if (renaming)
            {
                if (renameFocusRequested_)
                {
                    ImGui::SetKeyboardFocusHere();
                    renameFocusRequested_ = false;
                }

                ImGui::SetNextItemWidth(-1.0f);
                const bool submitted = ImGui::InputText(
                    "##rename",
                    renameBuffer_,
                    sizeof(renameBuffer_),
                    ImGuiInputTextFlags_EnterReturnsTrue |
                        ImGuiInputTextFlags_AutoSelectAll);
                const bool cancelRename = ImGui::IsKeyPressed(ImGuiKey_Escape);
                const bool finishRename = submitted || ImGui::IsItemDeactivated();
                if (cancelRename)
                {
                    renamingObject_ = EditorObjectId{};
                }
                else if (finishRename)
                {
                    if (std::string(renameBuffer_) != obj->name)
                    {
                        action.type = OutlinerAction::Type::RenameObject;
                        action.target = obj->id;
                        action.nameValue = renameBuffer_;
                    }
                    renamingObject_ = EditorObjectId{};
                }
            }
            else
            {
                // AllowOverlap so the "On" checkbox in a later column stays
                // clickable instead of being covered by this row-spanning item.
                if (ImGui::Selectable(obj->name.c_str(), isSelected,
                        ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                {
                    selectRow(obj->id);
                }

                if (ImGui::BeginPopupContextItem())
                {
                    if (!selection.Contains(obj->id))
                    {
                        selection.Replace(obj->id);
                        rangeAnchor_ = obj->id;
                    }

                    bool hasAction = false;
                    if (!row.environment)
                    {
                        if (ImGui::MenuItem("Rename"))
                        {
                            std::snprintf(renameBuffer_, sizeof(renameBuffer_), "%s", obj->name.c_str());
                            renamingObject_ = obj->id;
                            renameFocusRequested_ = true;
                        }
                        hasAction = true;
                    }
                    if (SupportsDuplicate(*obj, row.environment))
                    {
                        if (ImGui::MenuItem("Duplicate"))
                        {
                            action.type = OutlinerAction::Type::DuplicateObject;
                            action.target = obj->id;
                        }
                        hasAction = true;
                    }
                    if (SupportsFrameSelection(*obj, row.environment))
                    {
                        if (ImGui::MenuItem("Frame Selected"))
                        {
                            action.type = OutlinerAction::Type::FrameSelection;
                            action.target = obj->id;
                        }
                        hasAction = true;
                    }

                    const bool supportsEnable =
                        row.environment ? SupportsEnvironmentEnable(*obj) : true;
                    if (supportsEnable || !row.environment)
                    {
                        if (hasAction)
                        {
                            ImGui::Separator();
                        }
                        const bool enabled = RowEnabledValue(*obj, row.environment);
                        if (ImGui::MenuItem(enabled ? "Disable" : "Enable"))
                        {
                            action.type = row.environment ?
                                OutlinerAction::Type::SetEnvEnabled :
                                OutlinerAction::Type::SetEnabled;
                            action.target = obj->id;
                            action.enabledValue = !enabled;
                        }
                        hasAction = true;
                    }

                    if (!row.environment || SupportsDuplicate(*obj, true))
                    {
                        if (ImGui::MenuItem("Delete"))
                        {
                            action.type = OutlinerAction::Type::DeleteObject;
                            action.target = obj->id;
                        }
                        hasAction = true;
                    }

                    if (!hasAction)
                    {
                        ImGui::TextDisabled("No available actions.");
                    }
                    ImGui::EndPopup();
                }
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(obj->type.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(obj->id.value));

            ImGui::TableNextColumn();
            if (row.environment)
            {
                if (SupportsEnvironmentEnable(*obj))
                {
                    bool enabled = obj->properties.value("enabled", true);
                    if (ImGui::Checkbox("##enabled", &enabled))
                    {
                        if (action.type == OutlinerAction::Type::None)
                        {
                            action.type = OutlinerAction::Type::SetEnvEnabled;
                            action.target = obj->id;
                            action.enabledValue = enabled;
                        }
                    }
                }
            }
            else
            {
                bool enabled = obj->enabled;
                if (ImGui::Checkbox("##enabled", &enabled))
                {
                    if (action.type == OutlinerAction::Type::None)
                    {
                        action.type = OutlinerAction::Type::SetEnabled;
                        action.target = obj->id;
                        action.enabledValue = enabled;
                    }
                }
            }

            ImGui::PopID();
        };

        const auto drawGroup = [&](const char* id,
            const char* label,
            const std::vector<OutlinerRowRef>& rows,
            bool& groupOpen)
        {
            if (rows.empty())
            {
                return;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::SetNextItemOpen(groupOpen, ImGuiCond_Always);
            const ImGuiTreeNodeFlags groupFlags =
                ImGuiTreeNodeFlags_SpanAllColumns |
                ImGuiTreeNodeFlags_LabelSpanAllColumns |
                ImGuiTreeNodeFlags_FramePadding |
                ImGuiTreeNodeFlags_NoTreePushOnOpen;
            const bool openNow =
                ImGui::TreeNodeEx(id, groupFlags, "%s (%zu)", label, rows.size());
            if (ImGui::IsItemToggledOpen())
            {
                groupOpen = openNow;
            }
            if (!openNow)
            {
                return;
            }
            for (const OutlinerRowRef& row : rows)
            {
                drawRow(row);
            }
        };

        drawGroup("##meshesGroup", "Meshes", meshes, meshesGroupOpen_);
        drawGroup("##lightsGroup", "Lights", lights, lightsGroupOpen_);
        drawGroup("##camerasGroup", "Cameras", cameras, camerasGroupOpen_);
        drawGroup("##environmentGroup", "Environment", environmentRows, environmentGroupOpen_);
        drawGroup("##otherGroup", "Other", other, otherGroupOpen_);

        ImGui::EndTable();
    }

    if (primarySelection.value != 0 && selectedExists && !selectedVisible)
    {
        ImGui::TextDisabled("Selected ID %llu is hidden by the current filters.",
            static_cast<unsigned long long>(primarySelection.value));
    }
    else if (primarySelection.value != 0 && !selectedExists)
    {
        ImGui::TextDisabled("Selected ID %llu is not in the document.",
            static_cast<unsigned long long>(primarySelection.value));
    }

    ImGui::End();
    return action;
}

#endif // WITH_EDITOR
