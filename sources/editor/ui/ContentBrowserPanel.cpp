#include "editor/ui/ContentBrowserPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "editor/EditorExtensionRegistry.h"
#include "imgui.h"
#include "imgui_internal.h"

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

    void DrawSearchInputWithClear(const char* id,
        const char* hint,
        char* buffer,
        size_t bufferSize,
        float width)
    {
        ImGui::SetNextItemWidth(width);
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

    bool ShouldShowFolder(const AssetRegistry& registry,
        const EditorAssetFolder& folder,
        const std::string& sourceSearch)
    {
        if (sourceSearch.empty())
        {
            return true;
        }

        bool exclude = false;
        std::string needle = sourceSearch;
        if (needle.size() > 1 && needle.front() == '-')
        {
            exclude = true;
            needle.erase(needle.begin());
        }
        needle = LowerCopy(needle);
        if (needle.empty())
        {
            return true;
        }

        const bool selfMatches =
            ContainsLower(folder.name, needle) || ContainsLower(folder.path, needle);
        if (exclude)
        {
            return !selfMatches;
        }
        if (selfMatches)
        {
            return true;
        }

        for (const std::string& childPath : folder.childPaths)
        {
            const EditorAssetFolder* child = registry.FindFolder(childPath);
            if (child && ShouldShowFolder(registry, *child, sourceSearch))
            {
                return true;
            }
        }
        return false;
    }

    void DrawFolderTree(const AssetRegistry& registry,
        const EditorAssetFolder& folder,
        std::string& selectedFolder,
        const std::string& sourceSearch)
    {
        if (!ShouldShowFolder(registry, folder, sourceSearch))
        {
            return;
        }

        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_OpenOnArrow |
            ImGuiTreeNodeFlags_SpanFullWidth;
        if (folder.childPaths.empty())
        {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }
        if (selectedFolder == folder.path)
        {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (folder.path == "/Game")
        {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }

        char label[256];
        std::snprintf(label, sizeof(label), "%s (%zu)",
            folder.name.c_str(), folder.recursiveAssetCount);
        const bool open = ImGui::TreeNodeEx(folder.path.c_str(), flags, "%s", label);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            selectedFolder = folder.path;
        }

        if (open && !folder.childPaths.empty())
        {
            for (const std::string& childPath : folder.childPaths)
            {
                const EditorAssetFolder* child = registry.FindFolder(childPath);
                if (child)
                {
                    DrawFolderTree(registry, *child, selectedFolder, sourceSearch);
                }
            }
            ImGui::TreePop();
        }
    }
}

ContentBrowserAction ContentBrowserPanel::Draw(AssetRegistry& registry,
    EditorAssetId& selectedAsset,
    const EditorExtensionRegistry& extensions,
    bool* open)
{
    ContentBrowserAction action;

    ImGui::SetNextWindowSize(ImVec2(760.0f, 480.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Content Browser", open))
    {
        ImGui::End();
        return action;
    }

    if (!registry.FindFolder(selectedFolder_))
    {
        selectedFolder_ = registry.FindFolder("/Game") ? "/Game" :
            (registry.Folders().empty() ? std::string() : registry.Folders().front().path);
    }

    // Toolbar: refresh, search, type filter, and recursive folder display.
    if (ImGui::Button("Refresh"))
    {
        registry.Refresh();
        if (!registry.FindFolder(selectedFolder_))
        {
            selectedFolder_ = registry.FindFolder("/Game") ? "/Game" :
                (registry.Folders().empty() ? std::string() : registry.Folders().front().path);
        }
    }
    ImGui::SameLine();
    DrawSearchInputWithClear("##search", "Search assets...", searchBuffer_,
        sizeof(searchBuffer_), 220.0f);
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
    ImGui::SameLine();
    ImGui::Checkbox("Include Subfolders", &includeSubfolders_);

    const EditorAssetType typeFilter = kTypeFilters[typeFilterIndex_].type;
    ImGui::Separator();

    // Split browser. Reserve space at the bottom for selected-asset details.
    const float detailHeight = ImGui::GetTextLineHeightWithSpacing() * 7.0f;
    ImGui::BeginChild("##sourcesPanel", ImVec2(190.0f, -detailHeight), true);
    ImGui::TextUnformatted("Sources");
    DrawSearchInputWithClear("##sourceSearch", "Search folders...", sourceSearchBuffer_,
        sizeof(sourceSearchBuffer_), -1.0f);
    ImGui::Separator();
    if (const EditorAssetFolder* root = registry.FindFolder("/Game"))
    {
        DrawFolderTree(registry, *root, selectedFolder_, sourceSearchBuffer_);
    }
    else
    {
        ImGui::TextDisabled("No content roots.");
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##assetView", ImVec2(0.0f, -detailHeight), false);

    const EditorAssetFolder* selectedFolder = registry.FindFolder(selectedFolder_);
    const std::vector<const EditorAssetRecord*> results =
        registry.SearchInFolder(selectedFolder_, includeSubfolders_, searchBuffer_, typeFilter);
    const size_t folderAssetCount = selectedFolder ?
        (includeSubfolders_ ? selectedFolder->recursiveAssetCount : selectedFolder->directAssetCount) :
        registry.Assets().size();

    ImGui::Text("Path: %s", selectedFolder_.empty() ? "(none)" : selectedFolder_.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| Showing %d of %d assets",
        static_cast<int>(results.size()),
        static_cast<int>(folderAssetCount));

    // Asset table.
    const ImGuiTableFlags tableFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
    if (ImGui::BeginTable("##assetTable", 3, tableFlags, ImVec2(0.0f, 0.0f)))
    {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Virtual Path", ImGuiTableColumnFlags_WidthStretch);
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

            // Context menu. Spawn actions come from registered object factories;
            // material assign remains disabled until multi-target editing exists.
            if (ImGui::BeginPopupContextItem())
            {
                selectedAsset = record->id; // right-click also selects the row
                for (const std::unique_ptr<IEditorObjectFactory>& factory : extensions.ObjectFactories())
                {
                    if (!factory)
                    {
                        continue;
                    }

                    const std::string label(factory->MenuLabel());
                    const bool enabled = factory->CanBuildFromAsset(record);
                    ImGui::BeginDisabled(!enabled);
                    if (ImGui::MenuItem(label.c_str()))
                    {
                        action.objectFactoryType = std::string(factory->Type());
                        action.asset = record->id;
                    }
                    ImGui::EndDisabled();
                }
                ImGui::BeginDisabled(true);
                ImGui::MenuItem("Assign Material to Selected");
                ImGui::EndDisabled();
                ImGui::EndPopup();
            }
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(record->displayName.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(record->virtualPath.c_str());
        }

        ImGui::EndTable();
    }
    ImGui::EndChild();

    // Selected-asset details. Resolved against the full registry so the details
    // persist even if the current search/filter hides the row.
    ImGui::Separator();
    if (const EditorAssetRecord* selected = FindById(registry, selectedAsset))
    {
        ImGui::Text("Type: %s", ToString(selected->id.type));
        ImGui::Text("Name: %s", selected->displayName.c_str());
        ImGui::Text("Virtual Path: %s", selected->virtualPath.c_str());
        ImGui::Text("Source Path: %s", selected->path.c_str());
        if (selected->id.type == EditorAssetType::Texture)
        {
            const EditorTextureInfo& tex = selected->texture;
            if (tex.valid)
            {
                ImGui::Text("Texture: %s | %s", ToString(tex.kind), tex.format.c_str());
                if (tex.depth > 1)
                {
                    ImGui::Text("Size: %u x %u x %u | Mips: %u | Array: %u",
                        tex.width, tex.height, tex.depth, tex.mipLevels, tex.arraySize);
                }
                else
                {
                    ImGui::Text("Size: %u x %u | Mips: %u | Array: %u",
                        tex.width, tex.height, tex.mipLevels, tex.arraySize);
                }
            }
            else
            {
                ImGui::TextDisabled("Texture metadata unavailable.");
            }
        }
        else if (!selected->extension.empty())
        {
            ImGui::Text("Extension: %s", selected->extension.c_str());
        }
        ImGui::Text("Key:  %s", selected->id.key.c_str());
    }
    else
    {
        ImGui::TextDisabled("No asset selected.");
    }

    ImGui::End();
    return action;
}

#endif // WITH_EDITOR
