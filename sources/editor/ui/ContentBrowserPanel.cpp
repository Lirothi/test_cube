#include "editor/ui/ContentBrowserPanel.h"
#if WITH_EDITOR

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "core/StringMatch.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/assets/AssetThumbnailCache.h"
#include "editor/ui/EditorDragDrop.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
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

    bool SameAssetId(const EditorAssetId& a, const EditorAssetId& b)
    {
        return a.type == b.type && a.key == b.key;
    }

    bool ContainsAssetId(const std::vector<EditorAssetId>& assets,
        const EditorAssetId& id)
    {
        return std::find_if(assets.begin(), assets.end(),
            [&id](const EditorAssetId& candidate)
            {
                return SameAssetId(candidate, id);
            }) != assets.end();
    }

    void ToggleAssetId(std::vector<EditorAssetId>& assets,
        const EditorAssetId& id)
    {
        const auto it = std::find_if(assets.begin(), assets.end(),
            [&id](const EditorAssetId& candidate)
            {
                return SameAssetId(candidate, id);
            });
        if (it != assets.end())
        {
            assets.erase(it);
        }
        else
        {
            assets.push_back(id);
        }
    }

    bool ContainsFolderPath(const std::vector<std::string>& folders,
        const std::string& path)
    {
        return std::find(folders.begin(), folders.end(), path) != folders.end();
    }

    void ToggleFolderPath(std::vector<std::string>& folders,
        const std::string& path)
    {
        const auto it = std::find(folders.begin(), folders.end(), path);
        if (it != folders.end())
        {
            folders.erase(it);
        }
        else
        {
            folders.push_back(path);
        }
    }

    void DrawAssetOrganizationMenu(const EditorAssetId& assetId,
        std::vector<EditorAssetId>& favoriteAssets,
        std::vector<ContentBrowserCollection>& collections,
        bool& requestNewCollection)
    {
        const bool isFavorite = ContainsAssetId(favoriteAssets, assetId);
        if (ImGui::MenuItem(isFavorite ? "Remove from Favorites" : "Add to Favorites"))
        {
            ToggleAssetId(favoriteAssets, assetId);
        }
        if (ImGui::BeginMenu("Collections"))
        {
            if (collections.empty())
            {
                ImGui::TextDisabled("No collections.");
                if (ImGui::MenuItem("New Collection..."))
                {
                    requestNewCollection = true;
                }
            }
            else
            {
                for (ContentBrowserCollection& collection : collections)
                {
                    const bool contains = ContainsAssetId(collection.assets, assetId);
                    if (ImGui::MenuItem(collection.name.c_str(), nullptr, contains))
                    {
                        ToggleAssetId(collection.assets, assetId);
                    }
                }
            }
            ImGui::EndMenu();
        }
    }

    void DrawFolderOrganizationMenu(const std::string& folderPath,
        std::vector<std::string>& favoriteFolders,
        std::vector<ContentBrowserCollection>& collections,
        bool& requestNewCollection)
    {
        const bool isFavorite = ContainsFolderPath(favoriteFolders, folderPath);
        if (ImGui::MenuItem(isFavorite ? "Remove from Favorites" : "Add to Favorites"))
        {
            ToggleFolderPath(favoriteFolders, folderPath);
        }
        if (ImGui::BeginMenu("Collections"))
        {
            if (collections.empty())
            {
                ImGui::TextDisabled("No collections.");
                if (ImGui::MenuItem("New Collection..."))
                {
                    requestNewCollection = true;
                }
            }
            else
            {
                for (ContentBrowserCollection& collection : collections)
                {
                    const bool contains =
                        ContainsFolderPath(collection.folders, folderPath);
                    if (ImGui::MenuItem(collection.name.c_str(), nullptr, contains))
                    {
                        ToggleFolderPath(collection.folders, folderPath);
                    }
                }
            }
            ImGui::EndMenu();
        }
    }

    const char* AssetTypeBadge(EditorAssetType type)
    {
        switch (type)
        {
        case EditorAssetType::Mesh:           return "[MESH]";
        case EditorAssetType::MaterialPreset: return "[MAT]";
        case EditorAssetType::Texture:        return "[TEX]";
        case EditorAssetType::Level:          return "[LEVEL]";
        case EditorAssetType::Shader:         return "[SHADER]";
        case EditorAssetType::Unknown:        return "[ASSET]";
        }
        return "[ASSET]";
    }

    enum class BrowserIcon
    {
        None = -1,
        Folder = 0,
        Level = 1,
        Shader = 2,
        Unknown = 3,
        PreviewFailed = 4
    };

    struct BrowserIconAtlas
    {
        ImTextureID texture = ImTextureID_Invalid;

        bool IsReady() const { return texture != ImTextureID_Invalid; }
    };

    std::string TruncateLabel(const std::string& text, size_t maxChars);

    BrowserIcon IconForAsset(EditorAssetType type)
    {
        switch (type)
        {
        case EditorAssetType::Level:   return BrowserIcon::Level;
        case EditorAssetType::Shader:  return BrowserIcon::Shader;
        case EditorAssetType::Unknown: return BrowserIcon::Unknown;
        case EditorAssetType::Mesh:
        case EditorAssetType::MaterialPreset:
        case EditorAssetType::Texture:
            return BrowserIcon::None;
        }
        return BrowserIcon::Unknown;
    }

    void IconUvs(BrowserIcon icon, ImVec2& uv0, ImVec2& uv1)
    {
        constexpr float kAtlasWidth = 320.0f;
        constexpr float kCellSize = 64.0f;
        const float cell = static_cast<float>(static_cast<int>(icon));
        uv0 = ImVec2((cell * kCellSize + 0.5f) / kAtlasWidth, 0.5f / kCellSize);
        uv1 = ImVec2(((cell + 1.0f) * kCellSize - 0.5f) / kAtlasWidth,
            (kCellSize - 0.5f) / kCellSize);
    }

    bool DrawBrowserIcon(ImDrawList* drawList,
        const BrowserIconAtlas& atlas,
        BrowserIcon icon,
        const ImVec2& min,
        const ImVec2& max)
    {
        if (!drawList || !atlas.IsReady() || icon == BrowserIcon::None)
        {
            return false;
        }

        ImVec2 uv0;
        ImVec2 uv1;
        IconUvs(icon, uv0, uv1);
        drawList->AddImage(atlas.texture, min, max, uv0, uv1);
        return true;
    }

    bool DrawBrowserIconItem(const BrowserIconAtlas& atlas,
        BrowserIcon icon,
        float size)
    {
        if (!atlas.IsReady() || icon == BrowserIcon::None)
        {
            return false;
        }

        ImVec2 uv0;
        ImVec2 uv1;
        IconUvs(icon, uv0, uv1);
        ImGui::Image(atlas.texture, ImVec2(size, size), uv0, uv1);
        return true;
    }

    // The renderer + thumbnail cache the browser threads into its draw helpers so
    // they can request/draw real texture previews (Step 12D).
    struct ThumbnailProvider
    {
        Renderer* renderer = nullptr;
        AssetThumbnailCache* cache = nullptr;
        std::uint64_t assetRegistryRevision = 0;
    };

    // What to draw in an asset/folder's icon slot: a real GPU thumbnail, a baked
    // atlas icon, a quiet loading placeholder, or nothing (caller draws a badge).
    struct BrowserThumbnail
    {
        ImTextureID image = ImTextureID_Invalid;
        std::uint32_t imageWidth = 0;
        std::uint32_t imageHeight = 0;
        BrowserIcon icon = BrowserIcon::None;
        bool loading = false;
        const char* failureReason = nullptr;
    };

    BrowserThumbnail FolderThumbnail()
    {
        BrowserThumbnail thumb;
        thumb.icon = BrowserIcon::Folder;
        return thumb;
    }

    BrowserThumbnail ResolveAssetThumbnail(const ThumbnailProvider& thumbs,
        const EditorAssetRecord& record)
    {
        BrowserThumbnail thumb;

        // Textures, meshes, and material presets get a real preview from the
        // thumbnail cache. Cubemaps are sampled to their +X face by Step 12F.
        const bool previewable =
            record.id.type == EditorAssetType::Texture ||
            record.id.type == EditorAssetType::Mesh ||
            record.id.type == EditorAssetType::MaterialPreset;
        if (previewable && thumbs.cache && thumbs.renderer)
        {
            const AssetThumbnailCache::View view = thumbs.cache->Request(
                *thumbs.renderer, record, thumbs.assetRegistryRevision);
            switch (view.state)
            {
            case AssetThumbnailCache::State::Ready:
                thumb.image = view.texture;
                thumb.imageWidth = view.width;
                thumb.imageHeight = view.height;
                return thumb;
            case AssetThumbnailCache::State::Failed:
                thumb.icon = BrowserIcon::PreviewFailed;
                thumb.failureReason = view.failureReason;
                return thumb;
            default:
                thumb.loading = true;
                return thumb;
            }
        }

        thumb.icon = IconForAsset(record.id.type);
        return thumb;
    }

    void DrawCheckerboard(ImDrawList* drawList,
        const ImVec2& min,
        const ImVec2& max,
        float cell)
    {
        const ImU32 lightCell = IM_COL32(96, 96, 100, 255);
        const ImU32 darkCell = IM_COL32(64, 64, 68, 255);
        drawList->AddRectFilled(min, max, darkCell);
        drawList->PushClipRect(min, max, true);
        int row = 0;
        for (float y = min.y; y < max.y; y += cell, ++row)
        {
            const float startX = min.x + ((row & 1) ? cell : 0.0f);
            for (float x = startX; x < max.x; x += cell * 2.0f)
            {
                drawList->AddRectFilled(ImVec2(x, y),
                    ImVec2(std::min(x + cell, max.x), std::min(y + cell, max.y)),
                    lightCell);
            }
        }
        drawList->PopClipRect();
    }

    // Draws a checkerboard (so alpha reads clearly) then the image aspect-fit and
    // centered inside the region.
    void DrawFittedImage(ImDrawList* drawList,
        ImTextureID image,
        std::uint32_t width,
        std::uint32_t height,
        const ImVec2& regionMin,
        const ImVec2& regionMax)
    {
        DrawCheckerboard(drawList, regionMin, regionMax, 8.0f);

        const float regionW = std::max(1.0f, regionMax.x - regionMin.x);
        const float regionH = std::max(1.0f, regionMax.y - regionMin.y);
        const float aspect = static_cast<float>(std::max(width, 1u)) /
            static_cast<float>(std::max(height, 1u));
        ImVec2 size(regionW, regionH);
        if (size.x / size.y > aspect)
        {
            size.x = size.y * aspect;
        }
        else
        {
            size.y = size.x / aspect;
        }
        const ImVec2 imageMin(regionMin.x + (regionW - size.x) * 0.5f,
            regionMin.y + (regionH - size.y) * 0.5f);
        drawList->AddImage(image, imageMin,
            ImVec2(imageMin.x + size.x, imageMin.y + size.y));
    }

    void DrawLoadingPlaceholder(ImDrawList* drawList,
        const ImVec2& min,
        const ImVec2& max)
    {
        drawList->AddRectFilled(min, max,
            ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
        const ImVec2 center((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
        const float radius =
            std::max(1.5f, std::min(max.x - min.x, max.y - min.y) * 0.09f);
        const ImU32 dot = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        drawList->AddCircleFilled(ImVec2(center.x - radius * 2.6f, center.y), radius, dot);
        drawList->AddCircleFilled(center, radius, dot);
        drawList->AddCircleFilled(ImVec2(center.x + radius * 2.6f, center.y), radius, dot);
    }

    // Draws the icon slot for one entry. Returns false only when it drew nothing,
    // so the caller can fall back to its text badge (mirrors DrawBrowserIcon).
    bool DrawThumbnailRegion(ImDrawList* drawList,
        const BrowserThumbnail& thumb,
        const BrowserIconAtlas& atlas,
        const ImVec2& min,
        const ImVec2& max)
    {
        if (!drawList)
        {
            return false;
        }
        if (thumb.image != ImTextureID_Invalid)
        {
            DrawFittedImage(drawList, thumb.image, thumb.imageWidth, thumb.imageHeight,
                min, max);
            return true;
        }
        if (thumb.loading)
        {
            DrawLoadingPlaceholder(drawList, min, max);
            return true;
        }
        return DrawBrowserIcon(drawList, atlas, thumb.icon, min, max);
    }

    void DrawSourceRowIcon(const BrowserIconAtlas& atlas, BrowserIcon icon)
    {
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        const float size = std::max(1.0f,
            std::min(18.0f, itemMax.y - itemMin.y - 2.0f));
        const ImVec2 iconMin(itemMin.x + 2.0f,
            itemMin.y + (itemMax.y - itemMin.y - size) * 0.5f);
        DrawBrowserIcon(ImGui::GetWindowDrawList(),
            atlas,
            icon,
            iconMin,
            ImVec2(iconMin.x + size, iconMin.y + size));
    }

    // Row-icon variant that can draw a real texture thumbnail instead of an icon.
    void DrawSourceRowThumbnail(const BrowserThumbnail& thumb,
        const BrowserIconAtlas& atlas)
    {
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        const float size = std::max(1.0f,
            std::min(18.0f, itemMax.y - itemMin.y - 2.0f));
        const ImVec2 iconMin(itemMin.x + 2.0f,
            itemMin.y + (itemMax.y - itemMin.y - size) * 0.5f);
        DrawThumbnailRegion(ImGui::GetWindowDrawList(),
            thumb,
            atlas,
            iconMin,
            ImVec2(iconMin.x + size, iconMin.y + size));
    }

    void DrawTileContents(const BrowserThumbnail& thumb,
        const BrowserIconAtlas& atlas,
        const char* fallbackBadge,
        const std::string& name)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();
        drawList->PushClipRect(itemMin, itemMax, true);

        constexpr float kIconSize = 56.0f;
        const ImVec2 iconMin(
            itemMin.x + (itemMax.x - itemMin.x - kIconSize) * 0.5f,
            itemMin.y + 7.0f);
        if (!DrawThumbnailRegion(drawList,
                thumb,
                atlas,
                iconMin,
                ImVec2(iconMin.x + kIconSize, iconMin.y + kIconSize)))
        {
            const ImVec2 badgeSize = ImGui::CalcTextSize(fallbackBadge);
            drawList->AddText(
                ImVec2(itemMin.x + (itemMax.x - itemMin.x - badgeSize.x) * 0.5f,
                    itemMin.y + 23.0f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled),
                fallbackBadge);
        }

        const std::string displayName = TruncateLabel(name, 20);
        const ImVec2 nameSize = ImGui::CalcTextSize(displayName.c_str());
        drawList->AddText(
            ImVec2(itemMin.x + std::max(6.0f,
                    (itemMax.x - itemMin.x - nameSize.x) * 0.5f),
                itemMax.y - nameSize.y - 8.0f),
            ImGui::GetColorU32(ImGuiCol_Text),
            displayName.c_str());
        drawList->PopClipRect();
    }

    bool BeginDelayedResourceHint()
    {
        if (ImGui::GetDragDropPayload() != nullptr ||
            ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        {
            return false;
        }
        if (!ImGui::IsItemHovered(
                ImGuiHoveredFlags_DelayNormal |
                ImGuiHoveredFlags_NoSharedDelay))
        {
            return false;
        }
        return ImGui::BeginTooltip();
    }

    void DrawAssetHoverHint(const EditorAssetRecord* record,
        const EditorAssetId& fallbackId,
        const BrowserIconAtlas& icons,
        const ThumbnailProvider& thumbs)
    {
        if (!BeginDelayedResourceHint())
        {
            return;
        }

        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        if (!record)
        {
            if (DrawBrowserIconItem(icons, BrowserIcon::PreviewFailed, 32.0f))
            {
                ImGui::SameLine();
            }
            ImGui::Text("%s  Unavailable", AssetTypeBadge(fallbackId.type));
            ImGui::Separator();
            ImGui::TextWrapped("%s", fallbackId.key.c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "This referenced asset is not in the registry.");
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
            return;
        }

        const BrowserThumbnail thumb = ResolveAssetThumbnail(thumbs, *record);
        bool drewInlineIcon = false;
        if (thumb.image != ImTextureID_Invalid)
        {
            // A larger version of the same thumbnail on its own line.
            constexpr float kMaxSize = 96.0f;
            const float width = static_cast<float>(std::max(thumb.imageWidth, 1u));
            const float height = static_cast<float>(std::max(thumb.imageHeight, 1u));
            const float aspect = width / height;
            ImVec2 size(kMaxSize, kMaxSize);
            if (aspect >= 1.0f)
            {
                size.y = kMaxSize / aspect;
            }
            else
            {
                size.x = kMaxSize * aspect;
            }
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            DrawFittedImage(ImGui::GetWindowDrawList(),
                thumb.image,
                thumb.imageWidth,
                thumb.imageHeight,
                origin,
                ImVec2(origin.x + size.x, origin.y + size.y));
            ImGui::Dummy(size);
        }
        else if (DrawBrowserIconItem(icons, thumb.icon, 32.0f))
        {
            drewInlineIcon = true;
        }
        if (drewInlineIcon)
        {
            ImGui::SameLine();
        }
        ImGui::Text("%s  %s",
            AssetTypeBadge(record->id.type),
            record->displayName.c_str());
        if (thumb.failureReason)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "Preview: %s", thumb.failureReason);
        }
        ImGui::TextDisabled("%s", ToString(record->id.type));
        ImGui::Separator();
        ImGui::TextWrapped("Virtual Path: %s", record->virtualPath.c_str());

        if (record->id.type == EditorAssetType::Texture)
        {
            const EditorTextureInfo& texture = record->texture;
            if (!texture.scanned)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                    "Metadata: Not scanned");
            }
            else if (!texture.valid)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                    "Metadata: Invalid or unreadable");
            }
            else
            {
                if (texture.depth > 1)
                {
                    ImGui::Text("Dimensions: %u x %u x %u",
                        texture.width,
                        texture.height,
                        texture.depth);
                }
                else
                {
                    ImGui::Text("Dimensions: %u x %u",
                        texture.width,
                        texture.height);
                }
                ImGui::Text("Kind: %s", ToString(texture.kind));
                ImGui::TextWrapped("Format: %s", texture.format.c_str());
                ImGui::Text("Mip Levels: %u", texture.mipLevels);
                ImGui::Text("Array Size: %u", texture.arraySize);
            }
        }
        else if (record->id.type == EditorAssetType::MaterialPreset)
        {
            ImGui::Text("Preset: %s", record->id.key.c_str());
            ImGui::TextWrapped("Definition File: %s", record->path.c_str());
        }
        else
        {
            ImGui::TextWrapped("Source: %s", record->path.c_str());
            ImGui::Text("Extension: %s",
                record->extension.empty() ? "(none)" : record->extension.c_str());
        }

        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    void DrawFolderHoverHint(const EditorAssetFolder* folder,
        const std::string& fallbackPath,
        const BrowserIconAtlas& icons)
    {
        if (!BeginDelayedResourceHint())
        {
            return;
        }

        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
        if (DrawBrowserIconItem(icons,
                folder ? BrowserIcon::Folder : BrowserIcon::PreviewFailed,
                32.0f))
        {
            ImGui::SameLine();
        }
        if (!folder)
        {
            ImGui::TextUnformatted("[DIR]  Unavailable");
            ImGui::Separator();
            ImGui::TextWrapped("%s", fallbackPath.c_str());
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "This referenced folder is not in the registry.");
        }
        else
        {
            ImGui::Text("[DIR]  %s", folder->name.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("Virtual Path: %s", folder->path.c_str());
            ImGui::Text("Child Folders: %zu", folder->childPaths.size());
            ImGui::Text("Direct Assets: %zu", folder->directAssetCount);
            ImGui::Text("All Assets: %zu", folder->recursiveAssetCount);
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }

    bool DrawReferenceRemovalMenu(const char* label)
    {
        bool remove = false;
        if (ImGui::BeginPopupContextItem())
        {
            remove = ImGui::MenuItem(label);
            ImGui::EndPopup();
        }
        return remove;
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
        return textmatch::ContainsCaseInsensitive(haystack, lowerNeedle);
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
            ImGui::TextDisabled("Drop in the viewport or Inspector to assign.");
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
        ContentBrowserUiRequest& request,
        std::vector<std::string>& favoriteFolders,
        std::vector<ContentBrowserCollection>& collections,
        bool& requestNewCollection,
        const BrowserIconAtlas& icons)
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
        std::snprintf(label, sizeof(label), icons.IsReady() ? "   %s (%zu)" : "%s (%zu)",
            folder.name.c_str(),
            folder.recursiveAssetCount);
        const float nodeCursorX = ImGui::GetCursorScreenPos().x;
        const bool open = ImGui::TreeNodeEx(folder.path.c_str(), flags, "%s", label);
        if (icons.IsReady())
        {
            const ImVec2 itemMin = ImGui::GetItemRectMin();
            const ImVec2 itemMax = ImGui::GetItemRectMax();
            const float size = std::min(18.0f, itemMax.y - itemMin.y - 2.0f);
            const float x = nodeCursorX + ImGui::GetTreeNodeToLabelSpacing();
            const ImVec2 iconMin(x,
                itemMin.y + (itemMax.y - itemMin.y - size) * 0.5f);
            DrawBrowserIcon(ImGui::GetWindowDrawList(),
                icons,
                BrowserIcon::Folder,
                iconMin,
                ImVec2(iconMin.x + size, iconMin.y + size));
        }
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        {
            RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, folder.path);
        }
        DrawFolderDragSource(folder);
        DrawDisabledFolderDropTarget();
        if (ImGui::BeginPopupContextItem())
        {
            DrawFolderOrganizationMenu(folder.path,
                favoriteFolders,
                collections,
                requestNewCollection);
            ImGui::Separator();
            DrawFolderOperationsMenu(folder.path, request, selectedFolder != folder.path, true);
            ImGui::EndPopup();
        }
        DrawFolderHoverHint(&folder, folder.path, icons);

        if (open && !folder.childPaths.empty())
        {
            for (const std::string& childPath : folder.childPaths)
            {
                const EditorAssetFolder* child = registry.FindFolder(childPath);
                if (child)
                {
                    DrawFolderTree(registry,
                        *child,
                        selectedFolder,
                        sourceSearch,
                        request,
                        favoriteFolders,
                        collections,
                        requestNewCollection,
                        icons);
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
        EditorObjectId selectedObject,
        std::vector<EditorAssetId>& favoriteAssets,
        std::vector<ContentBrowserCollection>& collections,
        bool& requestNewCollection)
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
                if (ImGui::MenuItem("Assign Material to Selected Object"))
                {
                    action.type = ContentBrowserAction::Type::AssignMaterial;
                    action.asset = record->id;
                }
            }
            else
            {
                DisabledMenuItemWithTooltip("Assign Material to Selected Object", materialReason.c_str());
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
                if (ImGui::MenuItem("Open Level (Keep Camera)"))
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
        DrawAssetOrganizationMenu(record->id,
            favoriteAssets,
            collections,
            requestNewCollection);
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Virtual Path"))
        {
            ImGui::SetClipboardText(record->virtualPath.c_str());
        }
        if (ImGui::MenuItem("Copy Physical Path"))
        {
            ImGui::SetClipboardText(record->path.c_str());
        }
        if (ImGui::MenuItem("Reveal in Sources"))
        {
            RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, record->virtualFolder);
        }
        if (ImGui::MenuItem("Refresh Asset Registry"))
        {
            RequestIfUnset(request, ContentBrowserRequestType::Refresh, {});
        }
        ImGui::EndPopup();
    }

    void DrawFolderContextMenu(const EditorAssetFolder& folder,
        ContentBrowserUiRequest& request,
        std::vector<std::string>& favoriteFolders,
        std::vector<ContentBrowserCollection>& collections,
        bool& requestNewCollection)
    {
        if (!ImGui::BeginPopupContextItem())
        {
            return;
        }
        DrawFolderOrganizationMenu(folder.path,
            favoriteFolders,
            collections,
            requestNewCollection);
        ImGui::Separator();
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
        EditorObjectId selectedObject,
        std::vector<EditorAssetId>& favoriteAssets,
        std::vector<std::string>& favoriteFolders,
        std::vector<ContentBrowserCollection>& collections,
        bool& requestNewCollection,
        const BrowserIconAtlas& icons,
        const ThumbnailProvider& thumbs)
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

        const auto drawFolderRow = [&](const EditorAssetFolder* folder)
        {
            if (!folder)
            {
                return;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID(folder->path.c_str());
            if (ImGui::Selectable(icons.IsReady() ? "##folderType" : "[DIR]", false,
                    ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
            {
                selectedAsset = {};
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, folder->path);
                }
            }
            DrawSourceRowIcon(icons, BrowserIcon::Folder);
            DrawFolderDragSource(*folder);
            DrawDisabledFolderDropTarget();
            DrawFolderContextMenu(*folder,
                request,
                favoriteFolders,
                collections,
                requestNewCollection);
            DrawFolderHoverHint(folder, folder->path, icons);
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(folder->name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(folder->path.c_str());
        };

        const auto drawAssetRow = [&](const EditorAssetRecord* record)
        {
            if (!record)
            {
                return;
            }

            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            const bool isSelected = (selectedAsset.type == record->id.type &&
                selectedAsset.key == record->id.key);
            const BrowserThumbnail thumb = ResolveAssetThumbnail(thumbs, *record);
            const bool hasVisual =
                thumb.image != ImTextureID_Invalid ||
                thumb.loading ||
                (icons.IsReady() && thumb.icon != BrowserIcon::None);

            ImGui::PushID(record->id.key.c_str());
            if (ImGui::Selectable(
                    hasVisual ? "##assetType" : AssetTypeBadge(record->id.type),
                    isSelected,
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
            DrawSourceRowThumbnail(thumb, icons);
            DrawAssetDragSource(*record, selectedAsset);
            DrawAssetContextMenu(record, extensions, selectedAsset, action, request,
                document, selectedObject, favoriteAssets, collections, requestNewCollection);
            DrawAssetHoverHint(record, record->id, icons, thumbs);
            ImGui::PopID();

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(record->displayName.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(record->virtualPath.c_str());
        };

        const int itemCount = static_cast<int>(folders.size() + assets.size());
        ImGuiListClipper clipper;
        clipper.Begin(itemCount, ImGui::GetTextLineHeightWithSpacing());
        while (clipper.Step())
        {
            for (int itemIndex = clipper.DisplayStart;
                 itemIndex < clipper.DisplayEnd;
                 ++itemIndex)
            {
                const size_t index = static_cast<size_t>(itemIndex);
                if (index < folders.size())
                {
                    drawFolderRow(folders[index]);
                }
                else
                {
                    drawAssetRow(assets[index - folders.size()]);
                }
            }
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
        EditorObjectId selectedObject,
        std::vector<EditorAssetId>& favoriteAssets,
        std::vector<std::string>& favoriteFolders,
        std::vector<ContentBrowserCollection>& collections,
        bool& requestNewCollection,
        const BrowserIconAtlas& icons,
        const ThumbnailProvider& thumbs)
    {
        const ImVec2 tileSize(132.0f, 112.0f);
        const float spacingX = ImGui::GetStyle().ItemSpacing.x;
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        const int columns = std::max(1,
            static_cast<int>((availableWidth + spacingX) / (tileSize.x + spacingX)));

        const auto drawFolderTile = [&](const EditorAssetFolder* folder)
        {
            if (!folder)
            {
                return;
            }

            ImGui::PushID(folder->path.c_str());
            if (ImGui::Button("##folderTile", tileSize))
            {
                selectedAsset = {};
            }
            DrawTileContents(FolderThumbnail(), icons, "[DIR]", folder->name);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                RequestIfUnset(request, ContentBrowserRequestType::SelectFolder, folder->path);
            }
            DrawFolderDragSource(*folder);
            DrawDisabledFolderDropTarget();
            DrawFolderContextMenu(*folder,
                request,
                favoriteFolders,
                collections,
                requestNewCollection);
            DrawFolderHoverHint(folder, folder->path, icons);
            ImGui::PopID();
        };

        const auto drawAssetTile = [&](const EditorAssetRecord* record)
        {
            if (!record)
            {
                return;
            }

            const bool isSelected = (selectedAsset.type == record->id.type &&
                selectedAsset.key == record->id.key);

            ImGui::PushID(record->id.key.c_str());
            if (isSelected)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
            }

            if (ImGui::Button("##assetTile", tileSize))
            {
                selectedAsset = record->id;
            }
            DrawTileContents(ResolveAssetThumbnail(thumbs, *record),
                icons,
                AssetTypeBadge(record->id.type),
                record->displayName);
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
                document, selectedObject, favoriteAssets, collections, requestNewCollection);
            DrawAssetHoverHint(record, record->id, icons, thumbs);
            ImGui::PopID();
        };

        const size_t itemCount = folders.size() + assets.size();
        const int rowCount = static_cast<int>((itemCount + static_cast<size_t>(columns) - 1u) /
            static_cast<size_t>(columns));
        ImGuiListClipper clipper;
        clipper.Begin(rowCount, tileSize.y + ImGui::GetStyle().ItemSpacing.y);
        while (clipper.Step())
        {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row)
            {
                const size_t firstIndex = static_cast<size_t>(row) * static_cast<size_t>(columns);
                const size_t lastIndex = std::min(firstIndex + static_cast<size_t>(columns), itemCount);
                for (size_t itemIndex = firstIndex; itemIndex < lastIndex; ++itemIndex)
                {
                    if (itemIndex != firstIndex)
                    {
                        ImGui::SameLine();
                    }
                    if (itemIndex < folders.size())
                    {
                        drawFolderTile(folders[itemIndex]);
                    }
                    else
                    {
                        drawAssetTile(assets[itemIndex - folders.size()]);
                    }
                }
            }
        }
    }
}

ContentBrowserPanel::PersistentState ContentBrowserPanel::GetPersistentState() const
{
    PersistentState state;
    std::copy(std::begin(activeTypeFilters_), std::end(activeTypeFilters_), state.activeTypeFilters.begin());
    state.selectedFolder = selectedFolder_;
    state.includeSubfolders = includeSubfolders_;
    state.viewMode = viewMode_;
    state.sourcesWidth = sourcesWidth_;
    state.favoriteAssets = favoriteAssets_;
    state.favoriteFolders = favoriteFolders_;
    state.collections = collections_;
    return state;
}

void ContentBrowserPanel::SetPersistentState(const PersistentState& state)
{
    std::copy(state.activeTypeFilters.begin(), state.activeTypeFilters.end(), std::begin(activeTypeFilters_));
    selectedFolder_ = state.selectedFolder.empty() ? "/Game" : state.selectedFolder;
    folderHistory_.clear();
    folderHistoryIndex_ = 0;
    includeSubfolders_ = state.includeSubfolders;
    viewMode_ = state.viewMode == ViewMode::Tiles ? ViewMode::Tiles : ViewMode::List;
    if (std::isfinite(state.sourcesWidth))
    {
        sourcesWidth_ = std::clamp(state.sourcesWidth, 160.0f, 4096.0f);
    }
    favoriteAssets_ = state.favoriteAssets;
    favoriteFolders_ = state.favoriteFolders;
    collections_ = state.collections;
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

void ContentBrowserPanel::NotifyAutoRefresh(double timeSec)
{
    lastAutoRefreshTimeSec_ = timeSec;
}

ContentBrowserAction ContentBrowserPanel::Draw(AssetRegistry& registry,
    EditorAssetId& selectedAsset,
    const EditorExtensionRegistry& extensions,
    const EditorSceneDocument& document,
    EditorObjectId selectedObject,
    Renderer& renderer,
    AssetThumbnailCache& thumbnails,
    bool* open)
{
    CPU_SCOPE(ProfilerScopes::kContentBrowserDraw);
    ContentBrowserAction action;
    BrowserIconAtlas icons;
    const ThumbnailProvider thumbs{ &renderer, &thumbnails, registry.Revision() };
    thumbnails.BeginFrame(renderer);

    if (!iconAtlasTried_)
    {
        iconAtlasTried_ = true;
        renderer.WaitForPreviousFrame();
        UploadBatch uploads;
        if (uploads.Begin(&renderer))
        {
            Texture2D::CreateDesc desc;
            desc.path = L"textures/editor/content_browser_icons.png";
            desc.usage = Texture2D::Usage::AlbedoSRGB;
            iconAtlasReady_ = iconAtlas_.CreateFromFile(
                &renderer,
                uploads.CommandList(),
                desc,
                uploads.KeepAlive());
            uploads.SubmitAndWait(&renderer);
        }
    }

    if (iconAtlasReady_)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = iconAtlas_.GetSrvFormat();
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        renderer.MarkImGuiTextureShaderReadable(iconAtlas_.GetResource());
        icons.texture =
            renderer.CreateImGuiTextureId(iconAtlas_.GetResource(), srvDesc);
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 480.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(520.0f, 320.0f),
        ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin("Content Browser", open))
    {
        ImGui::End();
        return action;
    }

    EnsureSelectedFolder(registry);
    ContentBrowserUiRequest uiRequest;
    bool requestNewCollection = false;
    int collectionToDelete = -1;

    const float navHeight = ImGui::GetFrameHeightWithSpacing() * 4.4f;
    ImGui::BeginChild("##navigationBar", ImVec2(0.0f, navHeight), true);
    ImGui::TextUnformatted("Navigation");
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
        selectedAsset = {};
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
        selectedAsset = {};
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
    if (ImGui::Button("View Options"))
    {
        ImGui::OpenPopup("##contentBrowserSettings");
    }
    if (ImGui::BeginPopup("##contentBrowserSettings"))
    {
        ImGui::TextUnformatted("View Options");
        ImGui::Separator();
        if (ImGui::MenuItem("List", nullptr, viewMode_ == ViewMode::List))
        {
            viewMode_ = ViewMode::List;
        }
        if (ImGui::MenuItem("Tiles", nullptr, viewMode_ == ViewMode::Tiles))
        {
            viewMode_ = ViewMode::Tiles;
        }
        DisabledMenuItemWithTooltip("Columns", "Column view is planned after the list and tile data model settles.");
        ImGui::Separator();
        ImGui::Checkbox("Include Subfolders", &includeSubfolders_);
        if (ImGui::MenuItem("New Collection..."))
        {
            requestNewCollection = true;
        }
        ImGui::EndPopup();
    }

    const std::string breadcrumbRequest = DrawBreadcrumbBar(selectedFolder_);
    if (!breadcrumbRequest.empty())
    {
        SelectFolder(registry, breadcrumbRequest, true);
        selectedAsset = {};
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

    constexpr float kMinSourcesWidth = 160.0f;
    constexpr float kMinAssetViewWidth = 320.0f;
    constexpr float kSplitterWidth = 6.0f;
    const ImVec2 workspaceSize = ImGui::GetContentRegionAvail();
    const float maxSourcesWidth = std::max(kMinSourcesWidth,
        workspaceSize.x - kMinAssetViewWidth - kSplitterWidth);
    sourcesWidth_ = std::clamp(sourcesWidth_, kMinSourcesWidth, maxSourcesWidth);
    float assetViewWidth =
        std::max(kMinAssetViewWidth, workspaceSize.x - sourcesWidth_ - kSplitterWidth);

    const ImVec2 workspaceOrigin = ImGui::GetCursorScreenPos();
    const ImRect splitterRect(
        ImVec2(workspaceOrigin.x + sourcesWidth_, workspaceOrigin.y),
        ImVec2(workspaceOrigin.x + sourcesWidth_ + kSplitterWidth,
            workspaceOrigin.y + workspaceSize.y));
    ImGui::SplitterBehavior(splitterRect,
        ImGui::GetID("##sourcesAssetSplitter"),
        ImGuiAxis_X,
        &sourcesWidth_,
        &assetViewWidth,
        kMinSourcesWidth,
        kMinAssetViewWidth,
        3.0f,
        0.1f);

    std::optional<Profiler::ScopedCpu> drawSourcesScope(
        std::in_place, ProfilerScopes::kContentBrowserDrawSources);
    ImGui::BeginChild("##sourcesPanel",
        ImVec2(sourcesWidth_, workspaceSize.y),
        true);
    ImGui::TextUnformatted("Sources");
    DrawSearchInputWithClear("##sourceSearch", "Search folders...", sourceSearchBuffer_,
        sizeof(sourceSearchBuffer_), -1.0f);
    ImGui::Separator();

    const std::string sourceNeedle = LowerCopy(sourceSearchBuffer_);
    std::string favoriteFolderToRemove;
    EditorAssetId favoriteAssetToRemove;
    if (ImGui::CollapsingHeader("Favorites", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (favoriteFolders_.empty() && favoriteAssets_.empty())
        {
            ImGui::TextDisabled("No favorites.");
        }
        for (const std::string& folderPath : favoriteFolders_)
        {
            const EditorAssetFolder* folder = registry.FindFolder(folderPath);
            const std::string label = folder ? folder->name : folderPath;
            if (!sourceNeedle.empty() &&
                !ContainsLower(label, sourceNeedle) &&
                !ContainsLower(folderPath, sourceNeedle))
            {
                continue;
            }

            ImGui::PushID(folderPath.c_str());
            const BrowserIcon folderIcon =
                folder ? BrowserIcon::Folder : BrowserIcon::PreviewFailed;
            const std::string rowLabel =
                icons.IsReady() ?
                    std::string("   ") + label :
                    std::string("[DIR] ") + label;
            if (ImGui::Selectable(rowLabel.c_str(),
                    selectedFolder_ == folderPath && selectedAsset.key.empty()))
            {
                if (folder)
                {
                    SelectFolder(registry, folderPath, true);
                }
                selectedAsset = {};
            }
            DrawSourceRowIcon(icons, folderIcon);
            if (DrawReferenceRemovalMenu("Remove from Favorites"))
            {
                favoriteFolderToRemove = folderPath;
            }
            DrawFolderHoverHint(folder, folderPath, icons);
            ImGui::PopID();
        }
        for (const EditorAssetId& assetId : favoriteAssets_)
        {
            const EditorAssetRecord* record = registry.FindById(assetId);
            const std::string label = record ? record->displayName : assetId.key;
            if (!sourceNeedle.empty() &&
                !ContainsLower(label, sourceNeedle) &&
                (!record || !ContainsLower(record->virtualPath, sourceNeedle)))
            {
                continue;
            }

            ImGui::PushID(assetId.key.c_str());
            const BrowserIcon assetIcon =
                record ? IconForAsset(record->id.type) : BrowserIcon::PreviewFailed;
            const std::string rowLabel =
                icons.IsReady() && assetIcon != BrowserIcon::None ?
                    std::string("   ") + label :
                    std::string(AssetTypeBadge(assetId.type)) + " " + label;
            if (ImGui::Selectable(rowLabel.c_str(), SameAssetId(selectedAsset, assetId)))
            {
                selectedAsset = assetId;
            }
            DrawSourceRowIcon(icons, assetIcon);
            if (DrawReferenceRemovalMenu("Remove from Favorites"))
            {
                favoriteAssetToRemove = assetId;
            }
            DrawAssetHoverHint(record, assetId, icons, thumbs);
            ImGui::PopID();
        }
    }
    if (!favoriteFolderToRemove.empty())
    {
        const auto it = std::find(favoriteFolders_.begin(),
            favoriteFolders_.end(),
            favoriteFolderToRemove);
        if (it != favoriteFolders_.end())
        {
            favoriteFolders_.erase(it);
        }
    }
    if (!favoriteAssetToRemove.key.empty())
    {
        const auto it = std::find_if(favoriteAssets_.begin(),
            favoriteAssets_.end(),
            [&favoriteAssetToRemove](const EditorAssetId& id)
            {
                return SameAssetId(id, favoriteAssetToRemove);
            });
        if (it != favoriteAssets_.end())
        {
            favoriteAssets_.erase(it);
        }
    }

    if (ImGui::SmallButton("+##newCollection"))
    {
        requestNewCollection = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("New Collection");
    }
    ImGui::SameLine();
    const bool collectionsOpen =
        ImGui::CollapsingHeader("Collections", ImGuiTreeNodeFlags_DefaultOpen);
    if (collectionsOpen)
    {
        if (collections_.empty())
        {
            ImGui::TextDisabled("No collections.");
        }
        for (size_t collectionIndex = 0;
             collectionIndex < collections_.size();
             ++collectionIndex)
        {
            ContentBrowserCollection& collection = collections_[collectionIndex];
            ImGui::PushID(static_cast<int>(collectionIndex));
            const bool collectionOpen =
                ImGui::TreeNodeEx(collection.name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            if (ImGui::BeginPopupContextItem())
            {
                if (ImGui::MenuItem("Delete Collection"))
                {
                    collectionToDelete = static_cast<int>(collectionIndex);
                }
                ImGui::EndPopup();
            }

            if (collectionOpen)
            {
                std::string collectionFolderToRemove;
                EditorAssetId collectionAssetToRemove;
                if (collection.folders.empty() && collection.assets.empty())
                {
                    ImGui::TextDisabled("Empty");
                }
                for (const std::string& folderPath : collection.folders)
                {
                    const EditorAssetFolder* folder = registry.FindFolder(folderPath);
                    const std::string label = folder ? folder->name : folderPath;
                    ImGui::PushID(folderPath.c_str());
                    const BrowserIcon folderIcon =
                        folder ? BrowserIcon::Folder : BrowserIcon::PreviewFailed;
                    const std::string rowLabel =
                        icons.IsReady() ?
                            std::string("   ") + label :
                            std::string("[DIR] ") + label;
                    if (ImGui::Selectable(rowLabel.c_str(),
                            selectedFolder_ == folderPath && selectedAsset.key.empty()))
                    {
                        if (folder)
                        {
                            SelectFolder(registry, folderPath, true);
                        }
                        selectedAsset = {};
                    }
                    DrawSourceRowIcon(icons, folderIcon);
                    if (DrawReferenceRemovalMenu("Remove from Collection"))
                    {
                        collectionFolderToRemove = folderPath;
                    }
                    DrawFolderHoverHint(folder, folderPath, icons);
                    ImGui::PopID();
                }
                for (const EditorAssetId& assetId : collection.assets)
                {
                    const EditorAssetRecord* record = registry.FindById(assetId);
                    const std::string label = record ? record->displayName : assetId.key;
                    ImGui::PushID(assetId.key.c_str());
                    const BrowserIcon assetIcon =
                        record ? IconForAsset(record->id.type) : BrowserIcon::PreviewFailed;
                    const std::string rowLabel =
                        icons.IsReady() && assetIcon != BrowserIcon::None ?
                            std::string("   ") + label :
                            std::string(AssetTypeBadge(assetId.type)) + " " + label;
                    if (ImGui::Selectable(rowLabel.c_str(), SameAssetId(selectedAsset, assetId)))
                    {
                        selectedAsset = assetId;
                    }
                    DrawSourceRowIcon(icons, assetIcon);
                    if (DrawReferenceRemovalMenu("Remove from Collection"))
                    {
                        collectionAssetToRemove = assetId;
                    }
                    DrawAssetHoverHint(record, assetId, icons, thumbs);
                    ImGui::PopID();
                }
                if (!collectionFolderToRemove.empty())
                {
                    const auto it = std::find(collection.folders.begin(),
                        collection.folders.end(),
                        collectionFolderToRemove);
                    if (it != collection.folders.end())
                    {
                        collection.folders.erase(it);
                    }
                }
                if (!collectionAssetToRemove.key.empty())
                {
                    const auto it = std::find_if(collection.assets.begin(),
                        collection.assets.end(),
                        [&collectionAssetToRemove](const EditorAssetId& id)
                        {
                            return SameAssetId(id, collectionAssetToRemove);
                        });
                    if (it != collection.assets.end())
                    {
                        collection.assets.erase(it);
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
    }
    if (collectionToDelete >= 0)
    {
        collections_.erase(collections_.begin() + collectionToDelete);
    }

    ImGui::SeparatorText("Folders");
    if (const EditorAssetFolder* root = registry.FindFolder("/Game"))
    {
        DrawFolderTree(registry,
            *root,
            selectedFolder_,
            sourceSearchBuffer_,
            uiRequest,
            favoriteFolders_,
            collections_,
            requestNewCollection,
            icons);
    }
    else
    {
        ImGui::TextDisabled("No content roots.");
    }
    ImGui::EndChild();
    drawSourcesScope.reset();

    ImGui::SameLine(0.0f, kSplitterWidth);
    ImGui::BeginChild("##assetView",
        ImVec2(0.0f, workspaceSize.y),
        false);

    {
        std::optional<Profiler::ScopedCpu> buildVisibleEntriesScope(
            std::in_place, ProfilerScopes::kContentBrowserBuildVisibleEntries);

        // Rebuild the filtered folder/asset lists only when an input actually
        // changed. In steady state this skips the full-registry scan in
        // SearchInFolder and the per-child-folder rescans in
        // FolderMatchesAssetViewFilter entirely. The cached pointers reference
        // registry-owned storage, which Refresh reallocates, so the revision
        // (bumped by every Refresh) is part of the key and invalidates them.
        const std::uint64_t revision = registry.Revision();
        const bool cacheHit = assetViewCacheValid_ &&
            assetViewCacheRevision_ == revision &&
            assetViewCacheIncludeSubfolders_ == includeSubfolders_ &&
            assetViewCacheFolder_ == selectedFolder_ &&
            assetViewCacheSearch_ == searchBuffer_ &&
            std::equal(std::begin(activeTypeFilters_), std::end(activeTypeFilters_),
                assetViewCacheTypeFilters_.begin());
        if (!cacheHit)
        {
            visibleFolders_.clear();
            visibleAssets_.clear();

            if (const EditorAssetFolder* selectedFolder = registry.FindFolder(selectedFolder_))
            {
                for (const std::string& childPath : selectedFolder->childPaths)
                {
                    const EditorAssetFolder* child = registry.FindFolder(childPath);
                    if (child && FolderMatchesAssetViewFilter(registry, *child,
                            searchBuffer_, activeTypeFilters_))
                    {
                        visibleFolders_.push_back(child);
                    }
                }
            }

            const std::vector<const EditorAssetRecord*> assetCandidates =
                registry.SearchInFolder(selectedFolder_, includeSubfolders_,
                    searchBuffer_, EditorAssetType::Unknown);
            for (const EditorAssetRecord* record : assetCandidates)
            {
                if (record && MatchesActiveTypeFilters(*record, activeTypeFilters_))
                {
                    visibleAssets_.push_back(record);
                }
            }

            assetViewCacheValid_ = true;
            assetViewCacheRevision_ = revision;
            assetViewCacheIncludeSubfolders_ = includeSubfolders_;
            assetViewCacheFolder_ = selectedFolder_;
            assetViewCacheSearch_ = searchBuffer_;
            std::copy(std::begin(activeTypeFilters_), std::end(activeTypeFilters_),
                assetViewCacheTypeFilters_.begin());
        }
    }

    const std::vector<const EditorAssetFolder*>& visibleFolders = visibleFolders_;
    const std::vector<const EditorAssetRecord*>& visibleAssets = visibleAssets_;

    std::optional<Profiler::ScopedCpu> drawAssetViewScope(
        std::in_place, ProfilerScopes::kContentBrowserDrawAssetView);
    const size_t visibleEntryCount = visibleFolders.size() + visibleAssets.size();

    ImGui::TextUnformatted("Asset View");
    ImGui::SameLine();
    ImGui::TextDisabled("%d entries (%d folders, %d assets) | %s",
        static_cast<int>(visibleEntryCount),
        static_cast<int>(visibleFolders.size()),
        static_cast<int>(visibleAssets.size()),
        selectedFolder_.empty() ? "(none)" : selectedFolder_.c_str());
    if (ImGui::GetTime() - lastAutoRefreshTimeSec_ < 4.0)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("Auto-refreshed");
    }
    if (const EditorAssetRecord* selectedRecord = FindById(registry, selectedAsset))
    {
        const std::string selectedLabel = TruncateLabel(selectedRecord->displayName, 48);
        ImGui::TextDisabled("Selected: %s", selectedLabel.c_str());
    }

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
            DrawTileAssetView(visibleFolders,
                visibleAssets,
                selectedAsset,
                extensions,
                action,
                uiRequest,
                document,
                selectedObject,
                favoriteAssets_,
                favoriteFolders_,
                collections_,
                requestNewCollection,
                icons,
                thumbs);
        }
        else
        {
            DrawListAssetView(visibleFolders,
                visibleAssets,
                selectedAsset,
                extensions,
                action,
                uiRequest,
                document,
                selectedObject,
                favoriteAssets_,
                favoriteFolders_,
                collections_,
                requestNewCollection,
                icons,
                thumbs);
        }
    }
    if (ImGui::BeginPopupContextWindow("##assetViewContext",
            ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    {
        DrawEmptySpaceContextMenu(selectedFolder_, uiRequest);
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    drawAssetViewScope.reset();

    if (uiRequest.type == ContentBrowserRequestType::SelectFolder)
    {
        SelectFolder(registry, uiRequest.folderPath, true);
        selectedAsset = {};
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

    if (requestNewCollection)
    {
        newCollectionName_[0] = '\0';
        collectionOperationMessage_.clear();
        ImGui::OpenPopup("New Collection###ContentBrowserNewCollection");
    }
    if (ImGui::BeginPopupModal("New Collection###ContentBrowserNewCollection",
            nullptr,
            ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::InputText("Name", newCollectionName_, sizeof(newCollectionName_));
        if (!collectionOperationMessage_.empty())
        {
            ImGui::TextWrapped("%s", collectionOperationMessage_.c_str());
        }
        if (ImGui::Button("Create"))
        {
            std::string name(newCollectionName_);
            const size_t first = name.find_first_not_of(" \t");
            const size_t last = name.find_last_not_of(" \t");
            if (first == std::string::npos)
            {
                collectionOperationMessage_ = "Collection name cannot be empty.";
            }
            else
            {
                name = name.substr(first, last - first + 1);
                const bool duplicate = std::any_of(collections_.begin(), collections_.end(),
                    [&name](const ContentBrowserCollection& collection)
                    {
                        return LowerCopy(collection.name) == LowerCopy(name);
                    });
                if (duplicate)
                {
                    collectionOperationMessage_ = "A collection with that name already exists.";
                }
                else
                {
                    ContentBrowserCollection collection;
                    collection.name = std::move(name);
                    collections_.push_back(std::move(collection));
                    collectionOperationMessage_.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
        {
            collectionOperationMessage_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Generate a bounded batch of the thumbnails requested above (no GPU stall
    // when nothing is queued). Runs while the editor is drawing, before the frame
    // is submitted, matching the icon-atlas load path above.
    thumbnails.ProcessPending(renderer);

    ImGui::End();
    return action;
}

#endif // WITH_EDITOR
