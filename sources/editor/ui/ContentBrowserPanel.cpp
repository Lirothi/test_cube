#include "editor/ui/ContentBrowserPanel.h"

#include "imgui.h"

namespace
{
    struct TypeFilterOption
    {
        const char* label;
        EditorAssetType type;
    };

    // First entry is "All": EditorAssetType::Unknown means "no type filter" to
    // AssetRegistry::Search.
    constexpr TypeFilterOption kTypeFilters[] = {
        { "All",       EditorAssetType::Unknown },
        { "Meshes",    EditorAssetType::Mesh },
        { "Materials", EditorAssetType::MaterialPreset },
        { "Textures",  EditorAssetType::Texture },
        { "Levels",    EditorAssetType::Level },
        { "Shaders",   EditorAssetType::Shader },
    };

    const EditorAssetRecord* FindById(const AssetRegistry& registry,
        const EditorAssetId& id)
    {
        if (id.key.empty())
        {
            return nullptr;
        }
        for (const EditorAssetRecord& record : registry.Assets())
        {
            if (record.id.type == id.type && record.id.key == id.key)
            {
                return &record;
            }
        }
        return nullptr;
    }
}

void ContentBrowserPanel::Draw(AssetRegistry& registry, EditorAssetId& selectedAsset)
{
    // Toolbar: refresh, search, type filter.
    if (ImGui::Button("Refresh"))
    {
        registry.Refresh();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##search", "Search name or path...", searchBuffer_,
        sizeof(searchBuffer_));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##typeFilter", kTypeFilters[typeFilterIndex_].label))
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

    const EditorAssetType typeFilter = kTypeFilters[typeFilterIndex_].type;
    const std::vector<const EditorAssetRecord*> results =
        registry.Search(searchBuffer_, typeFilter);

    ImGui::Text("Showing %d of %d assets", static_cast<int>(results.size()),
        static_cast<int>(registry.Assets().size()));

    // Asset table. Reserve space at the bottom for the selected-asset details.
    const float detailHeight = ImGui::GetTextLineHeightWithSpacing() * 6.0f;
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##assetTable", 3, tableFlags, ImVec2(0.0f, -detailHeight)))
    {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        int rowIndex = 0;
        for (const EditorAssetRecord* record : results)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const bool isSelected = (selectedAsset.type == record->id.type &&
                selectedAsset.key == record->id.key);

            ImGui::PushID(rowIndex++);
            if (ImGui::Selectable(ToString(record->id.type), isSelected,
                    ImGuiSelectableFlags_SpanAllColumns))
            {
                selectedAsset = record->id;
            }

            // Placeholder context menu (actions become live in later steps).
            if (ImGui::BeginPopupContextItem())
            {
                selectedAsset = record->id; // right-click also selects the row
                ImGui::BeginDisabled();
                ImGui::MenuItem("Spawn Static Mesh");
                ImGui::MenuItem("Spawn Transparent Mesh");
                ImGui::MenuItem("Assign Material to Selected");
                ImGui::EndDisabled();
                ImGui::TextDisabled("(available in later steps)");
                ImGui::EndPopup();
            }
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(record->displayName.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(record->path.c_str());
        }

        ImGui::EndTable();
    }

    // Selected-asset details. Resolved against the full registry so the details
    // persist even if the current search/filter hides the row.
    ImGui::Separator();
    if (const EditorAssetRecord* selected = FindById(registry, selectedAsset))
    {
        ImGui::Text("Type: %s", ToString(selected->id.type));
        ImGui::Text("Name: %s", selected->displayName.c_str());
        ImGui::Text("Path: %s", selected->path.c_str());
        if (!selected->extension.empty())
        {
            ImGui::Text("Extension: %s", selected->extension.c_str());
        }
        ImGui::Text("Key:  %s", selected->id.key.c_str());
    }
    else
    {
        ImGui::TextDisabled("No asset selected.");
    }
}
