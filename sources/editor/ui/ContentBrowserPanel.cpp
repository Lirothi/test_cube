#include "editor/ui/ContentBrowserPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
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

    constexpr TypeFilterOption kTypeFilters[] = {
        { "Meshes",    EditorAssetType::Mesh },
        { "Materials", EditorAssetType::MaterialPreset },
        { "Textures",  EditorAssetType::Texture },
        { "Levels",    EditorAssetType::Level },
        { "Shaders",   EditorAssetType::Shader },
    };

    static_assert(IM_ARRAYSIZE(kTypeFilters) == 5,
        "ContentBrowserPanel::activeTypeFilters_ must match kTypeFilters.");

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

    const char* ViewModeLabel(ContentBrowserPanel::ViewMode mode)
    {
        switch (mode)
        {
        case ContentBrowserPanel::ViewMode::List:    return "List";
        case ContentBrowserPanel::ViewMode::Tiles:   return "Tiles";
        case ContentBrowserPanel::ViewMode::Columns: return "Columns";
        }
        return "List";
    }

    bool AnyTypeFilterActive(const bool* activeTypeFilters)
    {
        for (int i = 0; i < IM_ARRAYSIZE(kTypeFilters); ++i)
        {
            if (activeTypeFilters[i])
            {
                return true;
            }
        }
        return false;
    }

    bool MatchesActiveTypeFilters(const EditorAssetRecord& record,
        const bool* activeTypeFilters)
    {
        bool any = false;
        for (int i = 0; i < IM_ARRAYSIZE(kTypeFilters); ++i)
        {
            if (!activeTypeFilters[i])
            {
                continue;
            }

            any = true;
            if (record.id.type == kTypeFilters[i].type)
            {
                return true;
            }
        }
        return !any;
    }

    void ClearTypeFilters(bool* activeTypeFilters)
    {
        for (int i = 0; i < IM_ARRAYSIZE(kTypeFilters); ++i)
        {
            activeTypeFilters[i] = false;
        }
    }

    void DrawActiveFilterChips(bool* activeTypeFilters)
    {
        bool any = false;
        ImGui::TextUnformatted("Active Filters");
        ImGui::SameLine();
        for (int i = 0; i < IM_ARRAYSIZE(kTypeFilters); ++i)
        {
            if (!activeTypeFilters[i])
            {
                continue;
            }

            any = true;
            ImGui::PushID(i);
            std::string label(kTypeFilters[i].label);
            label += " x";
            if (ImGui::SmallButton(label.c_str()))
            {
                activeTypeFilters[i] = false;
            }
            ImGui::PopID();
            ImGui::SameLine();
        }

        if (any)
        {
            if (ImGui::SmallButton("Clear All"))
            {
                ClearTypeFilters(activeTypeFilters);
            }
        }
        else
        {
            ImGui::TextDisabled("All Types");
        }
    }

    void DisabledButtonWithTooltip(const char* label, const char* tooltip)
    {
        ImGui::BeginDisabled(true);
        ImGui::Button(label);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", tooltip);
        }
    }

    void DisabledMenuItemWithTooltip(const char* label, const char* tooltip)
    {
        ImGui::BeginDisabled(true);
        ImGui::MenuItem(label);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        {
            ImGui::SetTooltip("%s", tooltip);
        }
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
        const std::string& selectedFolder,
        const std::string& sourceSearch,
        std::string& requestedFolder)
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
            requestedFolder = folder.path;
        }

        if (open && !folder.childPaths.empty())
        {
            for (const std::string& childPath : folder.childPaths)
            {
                const EditorAssetFolder* child = registry.FindFolder(childPath);
                if (child)
                {
                    DrawFolderTree(registry, *child, selectedFolder, sourceSearch, requestedFolder);
                }
            }
            ImGui::TreePop();
        }
    }

    std::vector<std::pair<std::string, std::string>> BuildBreadcrumbs(const std::string& folderPath)
    {
        std::vector<std::pair<std::string, std::string>> crumbs;
        if (folderPath.empty())
        {
            return crumbs;
        }

        size_t start = 0;
        if (folderPath.front() == '/')
        {
            start = 1;
        }

        std::string current;
        while (start < folderPath.size())
        {
            const size_t slash = folderPath.find('/', start);
            const size_t end = slash == std::string::npos ? folderPath.size() : slash;
            if (end > start)
            {
                const std::string part = folderPath.substr(start, end - start);
                current += "/";
                current += part;
                crumbs.push_back({ part, current });
            }
            if (slash == std::string::npos)
            {
                break;
            }
            start = slash + 1;
        }
        return crumbs;
    }

    std::string DrawBreadcrumbBar(const std::string& selectedFolder)
    {
        std::string requestedFolder;
        const std::vector<std::pair<std::string, std::string>> crumbs =
            BuildBreadcrumbs(selectedFolder);

        ImGui::TextUnformatted("Path");
        ImGui::SameLine();
        for (size_t i = 0; i < crumbs.size(); ++i)
        {
            const auto& crumb = crumbs[i];
            ImGui::PushID(crumb.second.c_str());
            if (ImGui::SmallButton(crumb.first.c_str()))
            {
                requestedFolder = crumb.second;
            }
            ImGui::PopID();

            if (i + 1 < crumbs.size())
            {
                ImGui::SameLine();
                ImGui::TextDisabled(">");
                ImGui::SameLine();
            }
        }

        if (crumbs.empty())
        {
            ImGui::TextDisabled("(none)");
        }

        return requestedFolder;
    }

    bool FolderHasVisibleAsset(const AssetRegistry& registry,
        const EditorAssetFolder& folder,
        std::string_view searchText,
        const bool* activeTypeFilters)
    {
        const std::vector<const EditorAssetRecord*> assets =
            registry.SearchInFolder(folder.path, true, searchText, EditorAssetType::Unknown);
        for (const EditorAssetRecord* asset : assets)
        {
            if (asset && MatchesActiveTypeFilters(*asset, activeTypeFilters))
            {
                return true;
            }
        }
        return false;
    }

    bool FolderMatchesAssetViewFilter(const AssetRegistry& registry,
        const EditorAssetFolder& folder,
        std::string_view searchText,
        const bool* activeTypeFilters)
    {
        if (!searchText.empty())
        {
            const std::string needle = LowerCopy(std::string(searchText));
            if (!ContainsLower(folder.name, needle) &&
                !ContainsLower(folder.path, needle) &&
                !FolderHasVisibleAsset(registry, folder, searchText, activeTypeFilters))
            {
                return false;
            }
        }

        if (AnyTypeFilterActive(activeTypeFilters) &&
            !FolderHasVisibleAsset(registry, folder, {}, activeTypeFilters))
        {
            return false;
        }

        return true;
    }

    std::string TruncateLabel(const std::string& text, size_t maxChars)
    {
        if (text.size() <= maxChars)
        {
            return text;
        }
        if (maxChars <= 3)
        {
            return text.substr(0, maxChars);
        }
        return text.substr(0, maxChars - 3) + "...";
    }

    void DrawAssetContextMenu(const EditorAssetRecord* record,
        const EditorExtensionRegistry& extensions,
        EditorAssetId& selectedAsset,
        ContentBrowserAction& action)
    {
        if (!record || !ImGui::BeginPopupContextItem())
        {
            return;
        }

        selectedAsset = record->id; // right-click also selects the row/tile
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

    void DrawFolderContextPlaceholder()
    {
        if (!ImGui::BeginPopupContextItem())
        {
            return;
        }
        ImGui::TextDisabled("Folder actions are planned for Step 9.");
        ImGui::EndPopup();
    }

    void DrawListAssetView(const std::vector<const EditorAssetFolder*>& folders,
        const std::vector<const EditorAssetRecord*>& assets,
        EditorAssetId& selectedAsset,
        const EditorExtensionRegistry& extensions,
        ContentBrowserAction& action,
        std::string& requestedFolder)
    {
        const ImGuiTableFlags tableFlags =
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
        if (!ImGui::BeginTable("##assetTable", 3, tableFlags, ImVec2(0.0f, 0.0f)))
        {
            return;
        }

        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 180.0f);
        ImGui::TableSetupColumn("Virtual Path", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const EditorAssetFolder* folder : folders)
        {
            if (!folder)
            {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(folder->path.c_str());
            if (ImGui::Selectable("Folder", false,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                requestedFolder = folder->path;
            }
            DrawFolderContextPlaceholder();
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(folder->name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(folder->path.c_str());
        }

        for (const EditorAssetRecord* record : assets)
        {
            if (!record)
            {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const bool isSelected = (selectedAsset.type == record->id.type &&
                selectedAsset.key == record->id.key);

            ImGui::PushID(record->id.key.c_str());
            if (ImGui::Selectable(ToString(record->id.type), isSelected,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                selectedAsset = record->id;
            }
            DrawAssetContextMenu(record, extensions, selectedAsset, action);
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(record->displayName.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(record->virtualPath.c_str());
        }

        ImGui::EndTable();
    }

    void DrawTileAssetView(const std::vector<const EditorAssetFolder*>& folders,
        const std::vector<const EditorAssetRecord*>& assets,
        EditorAssetId& selectedAsset,
        const EditorExtensionRegistry& extensions,
        ContentBrowserAction& action,
        std::string& requestedFolder)
    {
        const ImVec2 tileSize(132.0f, 72.0f);
        const float spacingX = ImGui::GetStyle().ItemSpacing.x;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const int columns = std::max(1,
            static_cast<int>((availableWidth + spacingX) / (tileSize.x + spacingX)));

        int itemIndex = 0;
        auto advanceTile = [&itemIndex, columns]()
        {
            if (itemIndex > 0 && (itemIndex % columns) != 0)
            {
                ImGui::SameLine();
            }
            ++itemIndex;
        };

        for (const EditorAssetFolder* folder : folders)
        {
            if (!folder)
            {
                continue;
            }

            advanceTile();
            ImGui::PushID(folder->path.c_str());
            const std::string label = std::string("[Folder]\n") +
                TruncateLabel(folder->name, 24);
            ImGui::Button(label.c_str(), tileSize);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                requestedFolder = folder->path;
            }
            DrawFolderContextPlaceholder();
            ImGui::PopID();
        }

        for (const EditorAssetRecord* record : assets)
        {
            if (!record)
            {
                continue;
            }

            advanceTile();
            const bool isSelected = (selectedAsset.type == record->id.type &&
                selectedAsset.key == record->id.key);

            ImGui::PushID(record->id.key.c_str());
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
            }

            const std::string label = std::string("[") + ToString(record->id.type) + "]\n" +
                TruncateLabel(record->displayName, 24);
            if (ImGui::Button(label.c_str(), tileSize))
            {
                selectedAsset = record->id;
            }

            if (isSelected)
            {
                ImGui::PopStyleColor(2);
            }

            DrawAssetContextMenu(record, extensions, selectedAsset, action);
            ImGui::PopID();
        }
    }
}

void ContentBrowserPanel::EnsureSelectedFolder(const AssetRegistry& registry)
{
    std::string fallback;
    if (registry.FindFolder("/Game"))
    {
        fallback = "/Game";
    }
    else if (!registry.Folders().empty())
    {
        fallback = registry.Folders().front().path;
    }

    bool resetHistory = false;
    if (selectedFolder_.empty() || !registry.FindFolder(selectedFolder_))
    {
        selectedFolder_ = fallback;
        resetHistory = true;
    }

    if (folderHistory_.empty())
    {
        if (!selectedFolder_.empty())
        {
            folderHistory_.push_back(selectedFolder_);
        }
        folderHistoryIndex_ = 0;
        return;
    }

    if (resetHistory)
    {
        folderHistory_.clear();
        if (!selectedFolder_.empty())
        {
            folderHistory_.push_back(selectedFolder_);
        }
        folderHistoryIndex_ = 0;
        return;
    }

    if (folderHistoryIndex_ >= folderHistory_.size())
    {
        folderHistoryIndex_ = folderHistory_.size() - 1;
    }

    if (selectedFolder_.empty() || registry.FindFolder(folderHistory_[folderHistoryIndex_]))
    {
        return;
    }

    folderHistory_.clear();
    if (!selectedFolder_.empty())
    {
        folderHistory_.push_back(selectedFolder_);
    }
    folderHistoryIndex_ = 0;
}

void ContentBrowserPanel::SelectFolder(const AssetRegistry& registry,
    const std::string& folderPath,
    bool addHistory)
{
    if (folderPath.empty() || !registry.FindFolder(folderPath))
    {
        return;
    }

    if (selectedFolder_ == folderPath)
    {
        if (folderHistory_.empty())
        {
            folderHistory_.push_back(folderPath);
            folderHistoryIndex_ = 0;
        }
        return;
    }

    selectedFolder_ = folderPath;
    if (!addHistory)
    {
        return;
    }

    if (!folderHistory_.empty() && folderHistoryIndex_ + 1 < folderHistory_.size())
    {
        folderHistory_.erase(folderHistory_.begin() + static_cast<std::ptrdiff_t>(folderHistoryIndex_ + 1),
            folderHistory_.end());
    }

    if (folderHistory_.empty() || folderHistory_.back() != folderPath)
    {
        folderHistory_.push_back(folderPath);
    }
    folderHistoryIndex_ = folderHistory_.size() - 1;
}

void ContentBrowserPanel::NavigateHistory(const AssetRegistry& registry, int delta)
{
    if (folderHistory_.empty() || delta == 0)
    {
        return;
    }

    const int current = static_cast<int>(folderHistoryIndex_);
    const int next = current + delta;
    if (next < 0 || next >= static_cast<int>(folderHistory_.size()))
    {
        return;
    }

    const std::string& target = folderHistory_[static_cast<size_t>(next)];
    if (!registry.FindFolder(target))
    {
        EnsureSelectedFolder(registry);
        return;
    }

    folderHistoryIndex_ = static_cast<size_t>(next);
    selectedFolder_ = target;
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

    EnsureSelectedFolder(registry);

    const float navHeight = ImGui::GetFrameHeightWithSpacing() * 4.4f;
    ImGui::BeginChild("##navigationBar", ImVec2(0.0f, navHeight), true);
    ImGui::TextUnformatted("Navigation Bar");
    ImGui::SameLine();
    DisabledButtonWithTooltip("Add", "Asset creation is planned for a later Content Browser step.");
    ImGui::SameLine();
    DisabledButtonWithTooltip("Import", "Import is planned for a later Content Browser step.");
    ImGui::SameLine();
    DisabledButtonWithTooltip("Save All", "Assets are raw files; save integration is not available yet.");

    ImGui::SameLine();
    const bool canGoBack = folderHistoryIndex_ > 0 && !folderHistory_.empty();
    ImGui::BeginDisabled(!canGoBack);
    if (ImGui::Button("<"))
    {
        NavigateHistory(registry, -1);
    }
    ImGui::EndDisabled();
    if (!canGoBack && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("No previous folder.");
    }

    ImGui::SameLine();
    const bool canGoForward =
        !folderHistory_.empty() && folderHistoryIndex_ + 1 < folderHistory_.size();
    ImGui::BeginDisabled(!canGoForward);
    if (ImGui::Button(">"))
    {
        NavigateHistory(registry, 1);
    }
    ImGui::EndDisabled();
    if (!canGoForward && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("No next folder.");
    }

    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
    {
        registry.Refresh();
        EnsureSelectedFolder(registry);
    }
    ImGui::SameLine();
    if (ImGui::Button("Settings"))
    {
        ImGui::OpenPopup("##contentBrowserSettings");
    }
    if (ImGui::BeginPopup("##contentBrowserSettings"))
    {
        ImGui::TextUnformatted("Content Browser Settings");
        ImGui::Separator();
        if (ImGui::MenuItem("List View", nullptr, viewMode_ == ViewMode::List))
        {
            viewMode_ = ViewMode::List;
        }
        if (ImGui::MenuItem("Tile View", nullptr, viewMode_ == ViewMode::Tiles))
        {
            viewMode_ = ViewMode::Tiles;
        }
        DisabledMenuItemWithTooltip("Column View", "Column view is planned after the list/tile data model settles.");
        ImGui::Separator();
        ImGui::Checkbox("Include Subfolders", &includeSubfolders_);
        DisabledMenuItemWithTooltip("Collections", "Collections are scheduled for the later details/favorites step.");
        ImGui::EndPopup();
    }

    const std::string breadcrumbRequest = DrawBreadcrumbBar(selectedFolder_);
    if (!breadcrumbRequest.empty())
    {
        SelectFolder(registry, breadcrumbRequest, true);
    }

    DrawSearchInputWithClear("##search", "Search assets...", searchBuffer_,
        sizeof(searchBuffer_), 220.0f);
    ImGui::SameLine();
    if (ImGui::Button("Filters"))
    {
        ImGui::OpenPopup("##contentBrowserFilters");
    }
    if (ImGui::BeginPopup("##contentBrowserFilters"))
    {
        ImGui::TextUnformatted("Asset Type Filters");
        ImGui::Separator();
        for (int i = 0; i < IM_ARRAYSIZE(kTypeFilters); ++i)
        {
            if (ImGui::MenuItem(kTypeFilters[i].label, nullptr, activeTypeFilters_[i]))
            {
                activeTypeFilters_[i] = !activeTypeFilters_[i];
            }
        }
        if (AnyTypeFilterActive(activeTypeFilters_))
        {
            ImGui::Separator();
            if (ImGui::MenuItem("Clear Filters"))
            {
                ClearTypeFilters(activeTypeFilters_);
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Include Subfolders##toolbar", &includeSubfolders_);
    ImGui::SameLine();
    ImGui::TextDisabled("View: %s", ViewModeLabel(viewMode_));
    DrawActiveFilterChips(activeTypeFilters_);
    ImGui::EndChild();

    ImGui::Separator();

    // Split browser. Reserve space at the bottom for selected-asset details.
    const float detailHeight = ImGui::GetTextLineHeightWithSpacing() * 7.0f;
    ImGui::BeginChild("##sourcesPanel", ImVec2(190.0f, -detailHeight), true);
    ImGui::TextUnformatted("Sources");
    DrawSearchInputWithClear("##sourceSearch", "Search folders...", sourceSearchBuffer_,
        sizeof(sourceSearchBuffer_), -1.0f);
    ImGui::Separator();
    std::string requestedSourceFolder;
    if (const EditorAssetFolder* root = registry.FindFolder("/Game"))
    {
        DrawFolderTree(registry, *root, selectedFolder_, sourceSearchBuffer_, requestedSourceFolder);
    }
    else
    {
        ImGui::TextDisabled("No content roots.");
    }
    ImGui::EndChild();
    if (!requestedSourceFolder.empty())
    {
        SelectFolder(registry, requestedSourceFolder, true);
    }

    ImGui::SameLine();
    ImGui::BeginChild("##assetView", ImVec2(0.0f, -detailHeight), false);

    const EditorAssetFolder* selectedFolder = registry.FindFolder(selectedFolder_);
    std::vector<const EditorAssetFolder*> visibleFolders;
    if (selectedFolder)
    {
        for (const std::string& childPath : selectedFolder->childPaths)
        {
            const EditorAssetFolder* child = registry.FindFolder(childPath);
            if (child && FolderMatchesAssetViewFilter(registry, *child,
                    searchBuffer_, activeTypeFilters_))
            {
                visibleFolders.push_back(child);
            }
        }
    }

    std::vector<const EditorAssetRecord*> visibleAssets;
    const std::vector<const EditorAssetRecord*> assetCandidates =
        registry.SearchInFolder(selectedFolder_, includeSubfolders_,
            searchBuffer_, EditorAssetType::Unknown);
    for (const EditorAssetRecord* record : assetCandidates)
    {
        if (record && MatchesActiveTypeFilters(*record, activeTypeFilters_))
        {
            visibleAssets.push_back(record);
        }
    }

    const size_t visibleEntryCount = visibleFolders.size() + visibleAssets.size();

    ImGui::TextUnformatted("Asset View");
    ImGui::SameLine();
    ImGui::TextDisabled("%d entries (%d folders, %d assets) | %s",
        static_cast<int>(visibleEntryCount),
        static_cast<int>(visibleFolders.size()),
        static_cast<int>(visibleAssets.size()),
        selectedFolder_.empty() ? "(none)" : selectedFolder_.c_str());

    if (visibleEntryCount == 0)
    {
        ImGui::TextDisabled("No entries match the current folder, search, and filters.");
        if (AnyTypeFilterActive(activeTypeFilters_))
        {
            ImGui::TextDisabled("Active type filters are limiting results.");
        }
    }
    else
    {
        std::string requestedAssetViewFolder;
        if (viewMode_ == ViewMode::Tiles)
        {
            DrawTileAssetView(visibleFolders, visibleAssets, selectedAsset,
                extensions, action, requestedAssetViewFolder);
        }
        else
        {
            DrawListAssetView(visibleFolders, visibleAssets, selectedAsset,
                extensions, action, requestedAssetViewFolder);
        }

        if (!requestedAssetViewFolder.empty())
        {
            SelectFolder(registry, requestedAssetViewFolder, true);
        }
    }
    ImGui::EndChild();

    // Selected-asset details. Resolved against the full registry so the details
    // persist even if the current search/filter hides the row.
    ImGui::Separator();
    ImGui::TextUnformatted("Details / Preview");
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
