#include "editor/ui/ContentBrowserPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "editor/EditorExtensionRegistry.h"
#include "editor/ui/EditorDragDrop.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace
{
    namespace fs = std::filesystem;

    struct TypeFilterOption
    {
        const char* label;
        EditorAssetType type;
    };

    struct WritableContentRoot
    {
        const char* virtualRoot;
        const char* physicalRoot;
    };

    constexpr WritableContentRoot kWritableContentRoots[] = {
        { "/Game/Models",   "models" },
        { "/Game/Textures", "textures" },
        { "/Game/Levels",   "data/levels" },
        { "/Game/Shaders",  "shaders" },
    };

    enum class ContentBrowserRequestType
    {
        None,
        NewFolder,
        DeleteFolder,
        SelectFolder,
        Refresh
    };

    struct ContentBrowserUiRequest
    {
        ContentBrowserRequestType type = ContentBrowserRequestType::None;
        std::string folderPath;
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

    bool IsSameOrChildVirtualPath(const std::string& path, const char* root)
    {
        const std::string rootString(root);
        if (path == rootString)
        {
            return true;
        }
        if (path.size() <= rootString.size() ||
            path.compare(0, rootString.size(), rootString) != 0)
        {
            return false;
        }
        return path[rootString.size()] == '/';
    }

    bool IsPathInsideRoot(const fs::path& path, const fs::path& root)
    {
        const std::string pathText = LowerCopy(path.lexically_normal().generic_string());
        std::string rootText = LowerCopy(root.lexically_normal().generic_string());
        while (!rootText.empty() && rootText.back() == '/')
        {
            rootText.pop_back();
        }

        if (pathText == rootText)
        {
            return true;
        }
        return pathText.size() > rootText.size() &&
            pathText.compare(0, rootText.size(), rootText) == 0 &&
            pathText[rootText.size()] == '/';
    }

    bool TryMapVirtualFolderToPhysical(const std::string& virtualPath,
        fs::path& physicalPath,
        std::string& reason)
    {
        for (const WritableContentRoot& root : kWritableContentRoots)
        {
            if (!IsSameOrChildVirtualPath(virtualPath, root.virtualRoot))
            {
                continue;
            }

            std::error_code ec;
            fs::path physicalRoot = fs::weakly_canonical(root.physicalRoot, ec);
            if (ec)
            {
                physicalRoot = fs::absolute(root.physicalRoot, ec);
                if (ec)
                {
                    reason = "Content root cannot be resolved.";
                    return false;
                }
                physicalRoot = physicalRoot.lexically_normal();
            }

            fs::path mapped = physicalRoot;
            const std::string rootString(root.virtualRoot);
            if (virtualPath.size() > rootString.size())
            {
                std::string suffix = virtualPath.substr(rootString.size() + 1);
                size_t start = 0;
                while (start < suffix.size())
                {
                    const size_t slash = suffix.find('/', start);
                    const size_t end = slash == std::string::npos ? suffix.size() : slash;
                    const std::string part = suffix.substr(start, end - start);
                    if (part.empty() || part == "." || part == "..")
                    {
                        reason = "Virtual folder path is not safe.";
                        return false;
                    }
                    mapped /= part;
                    if (slash == std::string::npos)
                    {
                        break;
                    }
                    start = slash + 1;
                }
            }

            mapped = mapped.lexically_normal();
            if (!IsPathInsideRoot(mapped, physicalRoot))
            {
                reason = "Mapped folder escapes the approved content root.";
                return false;
            }

            physicalPath = mapped;
            return true;
        }

        reason = "This virtual folder is not backed by a writable content directory.";
        return false;
    }

    bool IsWritableRootFolder(const std::string& virtualPath)
    {
        for (const WritableContentRoot& root : kWritableContentRoots)
        {
            if (virtualPath == root.virtualRoot)
            {
                return true;
            }
        }
        return false;
    }

    std::string ParentVirtualPath(const std::string& virtualPath)
    {
        if (virtualPath.empty() || virtualPath == "/Game")
        {
            return {};
        }

        const size_t slash = virtualPath.find_last_of('/');
        if (slash == std::string::npos || slash == 0)
        {
            return {};
        }
        return virtualPath.substr(0, slash);
    }

    std::string JoinVirtualPath(const std::string& parent, const std::string& child)
    {
        if (parent.empty() || parent == "/")
        {
            return "/" + child;
        }
        return parent + "/" + child;
    }

    bool ValidateFolderName(const char* name, std::string& reason)
    {
        if (!name || name[0] == '\0')
        {
            reason = "Folder name is required.";
            return false;
        }

        const std::string text(name);
        if (text == "." || text == "..")
        {
            reason = "Folder name cannot be a relative path.";
            return false;
        }
        if (text.front() == ' ' || text.back() == ' ')
        {
            reason = "Folder name cannot start or end with a space.";
            return false;
        }

        constexpr const char* invalidChars = "\\/:*?\"<>|";
        if (text.find_first_of(invalidChars) != std::string::npos)
        {
            reason = "Folder name cannot contain path separators or reserved characters.";
            return false;
        }
        return true;
    }

    bool IsPhysicalFolderEmpty(const std::string& virtualPath, std::string& reason)
    {
        fs::path physicalPath;
        if (!TryMapVirtualFolderToPhysical(virtualPath, physicalPath, reason))
        {
            return false;
        }

        std::error_code ec;
        if (!fs::is_directory(physicalPath, ec))
        {
            reason = "Physical folder does not exist.";
            return false;
        }
        if (!fs::is_empty(physicalPath, ec))
        {
            reason = ec ? "Folder emptiness could not be checked." : "Folder is not empty.";
            return false;
        }
        return true;
    }

    bool SupportsMaterialAssignment(const EditorObject* object)
    {
        return object && object->type == "staticMesh";
    }

    bool CanAssignMaterialAsset(const EditorAssetRecord& record,
        const EditorSceneDocument& document,
        EditorObjectId selectedObject,
        std::string& reason)
    {
        if (record.id.type != EditorAssetType::MaterialPreset)
        {
            reason = "Only material assets can be assigned.";
            return false;
        }

        const EditorObject* object = document.Find(selectedObject);
        if (!object)
        {
            reason = "Select a static mesh object first.";
            return false;
        }
        if (!SupportsMaterialAssignment(object))
        {
            reason = "Selected object does not support material assignment.";
            return false;
        }

        return true;
    }

    bool CanOpenLevelAsset(const EditorAssetRecord& record,
        std::string& reason)
    {
        if (record.id.type != EditorAssetType::Level)
        {
            reason = "Only level assets can be opened.";
            return false;
        }
        return true;
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

    void RequestIfUnset(ContentBrowserUiRequest& request,
        ContentBrowserRequestType type,
        const std::string& folderPath)
    {
        if (request.type != ContentBrowserRequestType::None)
        {
            return;
        }
        request.type = type;
        request.folderPath = folderPath;
    }

    void DrawAssetDragSource(const EditorAssetRecord& record,
        EditorAssetId& selectedAsset)
    {
        if (!ImGui::BeginDragDropSource())
        {
            return;
        }

        selectedAsset = record.id;
        const EditorDragDrop::AssetPayload payload =
            EditorDragDrop::MakeAssetPayload(record.id);
        ImGui::SetDragDropPayload(EditorDragDrop::kAssetPayloadType,
            &payload,
            sizeof(payload));
        ImGui::Text("%s", record.displayName.c_str());
        ImGui::TextDisabled("%s", record.virtualPath.c_str());
        if (record.id.type == EditorAssetType::Mesh)
        {
            ImGui::TextDisabled("Drop in the viewport to spawn.");
        }
        else if (record.id.type == EditorAssetType::MaterialPreset)
        {
            ImGui::TextDisabled("Drop on the Inspector to assign.");
        }
        else
        {
            ImGui::TextDisabled("No drop target for this asset type yet.");
        }
        ImGui::EndDragDropSource();
    }

    void DrawFolderDragSource(const EditorAssetFolder& folder)
    {
        if (!ImGui::BeginDragDropSource())
        {
            return;
        }

        const EditorDragDrop::FolderPayload payload =
            EditorDragDrop::MakeFolderPayload(folder.path);
        ImGui::SetDragDropPayload(EditorDragDrop::kFolderPayloadType,
            &payload,
            sizeof(payload));
        ImGui::Text("%s", folder.name.c_str());
        ImGui::TextDisabled("%s", folder.path.c_str());
        ImGui::TextDisabled("Folder moving is disabled.");
        ImGui::EndDragDropSource();
    }

    void DrawDisabledFolderDropTarget()
    {
        if (!ImGui::BeginDragDropTarget())
        {
            return;
        }

        constexpr ImGuiDragDropFlags flags =
            ImGuiDragDropFlags_AcceptBeforeDelivery |
            ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        const ImGuiPayload* assetPayload =
            ImGui::AcceptDragDropPayload(EditorDragDrop::kAssetPayloadType, flags);
        const ImGuiPayload* folderPayload =
            ImGui::AcceptDragDropPayload(EditorDragDrop::kFolderPayloadType, flags);
        if (assetPayload || folderPayload)
        {
            ImGui::SetTooltip("Moving assets or folders is disabled until references can be repaired safely.");
        }
        ImGui::EndDragDropTarget();
    }

    void DrawFolderOperationsMenu(const std::string& folderPath,
        ContentBrowserUiRequest& request,
        bool includeOpen,
        bool allowDelete)
    {
        if (includeOpen)
        {
            if (ImGui::MenuItem("Open Folder"))
            {
                RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, folderPath);
            }
            ImGui::Separator();
        }

        std::string mapReason;
        fs::path physicalPath;
        const bool mapsToPhysical =
            TryMapVirtualFolderToPhysical(folderPath, physicalPath, mapReason);
        bool isDirectory = false;
        if (mapsToPhysical)
        {
            std::error_code ec;
            isDirectory = fs::is_directory(physicalPath, ec);
            if (!isDirectory)
            {
                mapReason = "Mapped physical folder does not exist.";
            }
        }

        if (mapsToPhysical && isDirectory)
        {
            if (ImGui::MenuItem("New Folder"))
            {
                RequestIfUnset(request, ContentBrowserRequestType::NewFolder, folderPath);
            }
        }
        else
        {
            DisabledMenuItemWithTooltip("New Folder", mapReason.c_str());
        }

        if (allowDelete)
        {
            const bool deleteAllowedRoot = !IsWritableRootFolder(folderPath) && folderPath != "/Game";
            std::string deleteReason;
            const bool emptyFolder = deleteAllowedRoot && IsPhysicalFolderEmpty(folderPath, deleteReason);
            if (emptyFolder)
            {
                if (ImGui::MenuItem("Delete Empty Folder"))
                {
                    RequestIfUnset(request, ContentBrowserRequestType::DeleteFolder, folderPath);
                }
            }
            else
            {
                if (!deleteAllowedRoot)
                {
                    deleteReason = "Content roots cannot be deleted.";
                }
                DisabledMenuItemWithTooltip("Delete Empty Folder", deleteReason.c_str());
            }
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Copy Virtual Path"))
        {
            ImGui::SetClipboardText(folderPath.c_str());
        }
        if (mapsToPhysical)
        {
            const std::string physicalText = physicalPath.generic_string();
            if (ImGui::MenuItem("Copy Physical Path"))
            {
                ImGui::SetClipboardText(physicalText.c_str());
            }
        }
        else
        {
            DisabledMenuItemWithTooltip("Copy Physical Path", mapReason.c_str());
        }
        if (ImGui::MenuItem("Refresh"))
        {
            RequestIfUnset(request, ContentBrowserRequestType::Refresh, {});
        }
    }

    void DrawEmptySpaceContextMenu(const std::string& folderPath,
        ContentBrowserUiRequest& request)
    {
        std::string mapReason;
        fs::path physicalPath;
        const bool mapsToPhysical =
            TryMapVirtualFolderToPhysical(folderPath, physicalPath, mapReason);

        bool isDirectory = false;
        if (mapsToPhysical)
        {
            std::error_code ec;
            isDirectory = fs::is_directory(physicalPath, ec);
            if (!isDirectory)
            {
                mapReason = "Mapped physical folder does not exist.";
            }
        }

        if (mapsToPhysical && isDirectory)
        {
            if (ImGui::MenuItem("New Folder"))
            {
                RequestIfUnset(request, ContentBrowserRequestType::NewFolder, folderPath);
            }
        }
        else
        {
            DisabledMenuItemWithTooltip("New Folder", mapReason.c_str());
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Copy Current Folder Path"))
        {
            ImGui::SetClipboardText(folderPath.c_str());
        }
        if (mapsToPhysical)
        {
            const std::string physicalText = physicalPath.generic_string();
            if (ImGui::MenuItem("Copy Current Physical Path"))
            {
                ImGui::SetClipboardText(physicalText.c_str());
            }
        }
        if (ImGui::MenuItem("Refresh"))
        {
            RequestIfUnset(request, ContentBrowserRequestType::Refresh, {});
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
        ContentBrowserUiRequest& request)
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
            RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, folder.path);
        }
        DrawFolderDragSource(folder);
        DrawDisabledFolderDropTarget();
        if (ImGui::BeginPopupContextItem())
        {
            DrawFolderOperationsMenu(folder.path, request, selectedFolder != folder.path, true);
            ImGui::EndPopup();
        }

        if (open && !folder.childPaths.empty())
        {
            for (const std::string& childPath : folder.childPaths)
            {
                const EditorAssetFolder* child = registry.FindFolder(childPath);
                if (child)
                {
                    DrawFolderTree(registry, *child, selectedFolder, sourceSearch, request);
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
        ContentBrowserAction& action,
        ContentBrowserUiRequest& request,
        const EditorSceneDocument& document,
        EditorObjectId selectedObject)
    {
        if (!record || !ImGui::BeginPopupContextItem())
        {
            return;
        }

        selectedAsset = record->id; // right-click also selects the row/tile
        bool wroteSpecificAction = false;
        if (record->id.type == EditorAssetType::Mesh)
        {
            for (const std::unique_ptr<IEditorObjectFactory>& factory : extensions.ObjectFactories())
            {
                if (!factory || !factory->CanBuildFromAsset(record))
                {
                    continue;
                }

                const std::string label(factory->MenuLabel());
                if (ImGui::MenuItem(label.c_str()))
                {
                    action.type = ContentBrowserAction::Type::SpawnObject;
                    action.objectFactoryType = std::string(factory->Type());
                    action.asset = record->id;
                }
                wroteSpecificAction = true;
            }
        }
        else if (record->id.type == EditorAssetType::MaterialPreset)
        {
            std::string materialReason;
            const bool canAssignMaterial =
                CanAssignMaterialAsset(*record, document, selectedObject, materialReason);
            if (canAssignMaterial)
            {
                if (ImGui::MenuItem("Assign Material to Selected"))
                {
                    action.type = ContentBrowserAction::Type::AssignMaterial;
                    action.asset = record->id;
                }
            }
            else
            {
                DisabledMenuItemWithTooltip("Assign Material to Selected", materialReason.c_str());
            }
            wroteSpecificAction = true;
        }
        else if (record->id.type == EditorAssetType::Level)
        {
            std::string levelReason;
            const bool canOpenLevel = CanOpenLevelAsset(*record, levelReason);
            if (canOpenLevel)
            {
                if (ImGui::MenuItem("Open Level"))
                {
                    action.type = ContentBrowserAction::Type::OpenLevel;
                    action.asset = record->id;
                }
                if (ImGui::MenuItem("Open Level Preserving Camera"))
                {
                    action.type = ContentBrowserAction::Type::OpenLevelPreservingCamera;
                    action.asset = record->id;
                }
            }
            wroteSpecificAction = true;
        }

        if (wroteSpecificAction)
        {
            ImGui::Separator();
        }
        if (ImGui::MenuItem("Copy Virtual Path"))
        {
            ImGui::SetClipboardText(record->virtualPath.c_str());
        }
        if (ImGui::MenuItem("Copy Physical Path"))
        {
            ImGui::SetClipboardText(record->path.c_str());
        }
        if (ImGui::MenuItem("Show In Sources"))
        {
            RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, record->virtualFolder);
        }
        if (ImGui::MenuItem("Refresh"))
        {
            RequestIfUnset(request, ContentBrowserRequestType::Refresh, {});
        }
        ImGui::EndPopup();
    }

    void DrawFolderContextMenu(const EditorAssetFolder& folder,
        ContentBrowserUiRequest& request)
    {
        if (!ImGui::BeginPopupContextItem())
        {
            return;
        }
        DrawFolderOperationsMenu(folder.path, request, true, true);
        ImGui::EndPopup();
    }

    void DrawListAssetView(const std::vector<const EditorAssetFolder*>& folders,
        const std::vector<const EditorAssetRecord*>& assets,
        EditorAssetId& selectedAsset,
        const EditorExtensionRegistry& extensions,
        ContentBrowserAction& action,
        ContentBrowserUiRequest& request,
        const EditorSceneDocument& document,
        EditorObjectId selectedObject)
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
                RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, folder->path);
            }
            DrawFolderDragSource(*folder);
            DrawDisabledFolderDropTarget();
            DrawFolderContextMenu(*folder, request);
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
                std::string levelReason;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    CanOpenLevelAsset(*record, levelReason))
                {
                    action.type = ContentBrowserAction::Type::OpenLevel;
                    action.asset = record->id;
                }
            }
            DrawAssetDragSource(*record, selectedAsset);
            DrawAssetContextMenu(record, extensions, selectedAsset, action, request,
                document, selectedObject);
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
        ContentBrowserUiRequest& request,
        const EditorSceneDocument& document,
        EditorObjectId selectedObject)
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
                RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, folder->path);
            }
            DrawFolderDragSource(*folder);
            DrawDisabledFolderDropTarget();
            DrawFolderContextMenu(*folder, request);
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
            std::string levelReason;
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                CanOpenLevelAsset(*record, levelReason))
            {
                selectedAsset = record->id;
                action.type = ContentBrowserAction::Type::OpenLevel;
                action.asset = record->id;
            }

            if (isSelected)
            {
                ImGui::PopStyleColor(2);
            }

            DrawAssetDragSource(*record, selectedAsset);
            DrawAssetContextMenu(record, extensions, selectedAsset, action, request,
                document, selectedObject);
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
    const EditorSceneDocument& document,
    EditorObjectId selectedObject,
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
    ContentBrowserUiRequest uiRequest;

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
    if (const EditorAssetFolder* root = registry.FindFolder("/Game"))
    {
        DrawFolderTree(registry, *root, selectedFolder_, sourceSearchBuffer_, uiRequest);
    }
    else
    {
        ImGui::TextDisabled("No content roots.");
    }
    ImGui::EndChild();

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
        if (viewMode_ == ViewMode::Tiles)
        {
            DrawTileAssetView(visibleFolders, visibleAssets, selectedAsset,
                extensions, action, uiRequest, document, selectedObject);
        }
        else
        {
            DrawListAssetView(visibleFolders, visibleAssets, selectedAsset,
                extensions, action, uiRequest, document, selectedObject);
        }
    }
    if (ImGui::BeginPopupContextWindow("##assetViewContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        DrawEmptySpaceContextMenu(selectedFolder_, uiRequest);
        ImGui::EndPopup();
    }
    ImGui::EndChild();

    if (uiRequest.type == ContentBrowserRequestType::SelectFolder)
    {
        SelectFolder(registry, uiRequest.folderPath, true);
    }
    else if (uiRequest.type == ContentBrowserRequestType::Refresh)
    {
        registry.Refresh();
        EnsureSelectedFolder(registry);
    }
    else if (uiRequest.type == ContentBrowserRequestType::NewFolder)
    {
        newFolderParent_ = uiRequest.folderPath;
        newFolderName_[0] = '\0';
        folderOperationMessage_.clear();
        ImGui::OpenPopup("New Folder###ContentBrowserNewFolder");
    }
    else if (uiRequest.type == ContentBrowserRequestType::DeleteFolder)
    {
        deleteFolderTarget_ = uiRequest.folderPath;
        folderOperationMessage_.clear();
        ImGui::OpenPopup("Delete Empty Folder###ContentBrowserDeleteFolder");
    }

    if (ImGui::BeginPopupModal("New Folder###ContentBrowserNewFolder",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("Parent: %s", newFolderParent_.c_str());
        ImGui::InputText("Name", newFolderName_, sizeof(newFolderName_));
        if (!folderOperationMessage_.empty())
        {
            ImGui::TextWrapped("%s", folderOperationMessage_.c_str());
        }

        if (ImGui::Button("Create"))
        {
            std::string reason;
            fs::path parentPhysical;
            if (!ValidateFolderName(newFolderName_, reason))
            {
                folderOperationMessage_ = reason;
            }
            else if (!TryMapVirtualFolderToPhysical(newFolderParent_, parentPhysical, reason))
            {
                folderOperationMessage_ = reason;
            }
            else
            {
                std::error_code ec;
                if (!fs::is_directory(parentPhysical, ec))
                {
                    folderOperationMessage_ = "Parent physical folder does not exist.";
                }
                else
                {
                    const fs::path target = (parentPhysical / newFolderName_).lexically_normal();
                    if (fs::exists(target, ec))
                    {
                        folderOperationMessage_ = "A file or folder with that name already exists.";
                    }
                    else if (!fs::create_directory(target, ec))
                    {
                        folderOperationMessage_ = ec ? ec.message() : "Folder could not be created.";
                    }
                    else
                    {
                        const std::string newVirtualFolder =
                            JoinVirtualPath(newFolderParent_, newFolderName_);
                        registry.Refresh();
                        SelectFolder(registry, newVirtualFolder, true);
                        folderOperationMessage_.clear();
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            folderOperationMessage_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete Empty Folder###ContentBrowserDeleteFolder",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextWrapped("Delete empty folder?");
        ImGui::Text("Folder: %s", deleteFolderTarget_.c_str());
        if (!folderOperationMessage_.empty())
        {
            ImGui::TextWrapped("%s", folderOperationMessage_.c_str());
        }

        if (ImGui::Button("Delete"))
        {
            std::string reason;
            fs::path physicalPath;
            if (IsWritableRootFolder(deleteFolderTarget_) || deleteFolderTarget_ == "/Game")
            {
                folderOperationMessage_ = "Content roots cannot be deleted.";
            }
            else if (!IsPhysicalFolderEmpty(deleteFolderTarget_, reason))
            {
                folderOperationMessage_ = reason;
            }
            else if (!TryMapVirtualFolderToPhysical(deleteFolderTarget_, physicalPath, reason))
            {
                folderOperationMessage_ = reason;
            }
            else
            {
                std::error_code ec;
                if (!fs::remove(physicalPath, ec))
                {
                    folderOperationMessage_ = ec ? ec.message() : "Folder could not be deleted.";
                }
                else
                {
                    const std::string parentFolder = ParentVirtualPath(deleteFolderTarget_);
                    registry.Refresh();
                    SelectFolder(registry, parentFolder.empty() ? "/Game" : parentFolder, true);
                    folderOperationMessage_.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            folderOperationMessage_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

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
