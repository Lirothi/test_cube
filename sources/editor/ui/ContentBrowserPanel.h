#pragma once
#if WITH_EDITOR

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "editor/assets/AssetRegistry.h"
#include "editor/scene/EditorSceneDocument.h"
#include "materials/Texture2D.h"

class AssetThumbnailCache;
class EditorExtensionRegistry;
class Renderer;

struct ContentBrowserCollection
{
    std::string name;
    std::vector<EditorAssetId> assets;
    std::vector<std::string> folders;
};

// A spawn request raised by the content browser's context menu. The editor
// reads it after Draw and turns it into a command. Type::None means no request.
struct ContentBrowserAction
{
    enum class Type
    {
        None,
        SpawnObject,
        AssignMaterial,
        OpenLevel,
        OpenLevelPreservingCamera,
        OpenImportWindow,
        ReimportAsset
    };

    Type type = Type::None;
    std::string objectFactoryType;
    EditorAssetId asset;

    bool HasAction() const { return type != Type::None; }
};

// Searchable / filterable list of discovered assets, drawn over an AssetRegistry.
// It reads metadata, updates the editor's selected asset, and raises spawn
// requests via its context menu. It never loads or spawns anything itself.
class ContentBrowserPanel
{
public:
    enum class ViewMode
    {
        List,
        Tiles,
        Columns
    };

    struct PersistentState
    {
        std::array<bool, 5> activeTypeFilters{};
        std::string selectedFolder = "/Game";
        bool includeSubfolders = true;
        ViewMode viewMode = ViewMode::List;
        float sourcesWidth = 300.0f;
        std::vector<EditorAssetId> favoriteAssets;
        std::vector<std::string> favoriteFolders;
        std::vector<ContentBrowserCollection> collections;
    };

    // Draws the panel as its own ImGui window. `open` backs the window's close
    // button (the editor owns it). `selectedAsset` is owned by the editor; the
    // panel highlights it and writes it when the user clicks a row. Returns the
    // action the user requested this frame (Type::None if nothing).
    ContentBrowserAction Draw(AssetRegistry& registry,
        EditorAssetId& selectedAsset,
        const EditorExtensionRegistry& extensions,
        const EditorSceneDocument& document,
        EditorObjectId selectedObject,
        Renderer& renderer,
        AssetThumbnailCache& thumbnails,
        bool* open);

    // Records an external registry refresh so the browser can briefly surface
    // the otherwise silent update without changing the user's current view.
    void NotifyAutoRefresh(double timeSec);

    PersistentState GetPersistentState() const;
    void SetPersistentState(const PersistentState& state);

private:
    void EnsureSelectedFolder(const AssetRegistry& registry);
    void SelectFolder(const AssetRegistry& registry, const std::string& folderPath, bool addHistory);
    void NavigateHistory(const AssetRegistry& registry, int delta);

    char searchBuffer_[256] = {};
    char sourceSearchBuffer_[128] = {};
    char newFolderName_[64] = {};
    char newCollectionName_[64] = {};
    std::string selectedFolder_ = "/Game";
    std::string newFolderParent_;
    std::string deleteFolderTarget_;
    std::string folderOperationMessage_;
    EditorAssetId deleteAssetTarget_;      // asset pending the delete-confirmation modal
    std::string deleteAssetPath_;          // its disk path (materials: id.key is the NAME, not the path)
    // One-frame flag: expand the Sources tree along selectedFolder_ and scroll to it.
    // Raised by navigation that happens OUTSIDE the tree (asset-view double-click,
    // breadcrumbs, "Reveal in Sources", history) so both views stay in sync.
    bool syncFolderTreeToSelection_ = false;
    std::string collectionOperationMessage_;
    std::vector<std::string> folderHistory_;
    std::vector<EditorAssetId> favoriteAssets_;
    std::vector<std::string> favoriteFolders_;
    std::vector<ContentBrowserCollection> collections_;
    size_t folderHistoryIndex_ = 0;
    float sourcesWidth_ = 300.0f;
    bool includeSubfolders_ = true;
    bool activeTypeFilters_[5] = {};
    ViewMode viewMode_ = ViewMode::List;
    Texture2D iconAtlas_;
    bool iconAtlasTried_ = false;
    bool iconAtlasReady_ = false;
    double lastAutoRefreshTimeSec_ = -1000.0;

    // Cached asset-view filter result. The full-registry scan (and the per-child
    // folder rescans in FolderMatchesAssetViewFilter) runs only when the registry
    // revision or a filter input changes; otherwise it is skipped and these are
    // redrawn as-is. The cached pointers reference registry-owned storage, which
    // Refresh reallocates — so the registry revision is part of the cache key.
    std::vector<const EditorAssetFolder*> visibleFolders_;
    std::vector<const EditorAssetRecord*> visibleAssets_;
    bool assetViewCacheValid_ = false;
    std::uint64_t assetViewCacheRevision_ = 0;
    std::string assetViewCacheFolder_;
    std::string assetViewCacheSearch_;
    bool assetViewCacheIncludeSubfolders_ = false;
    std::array<bool, 5> assetViewCacheTypeFilters_{};
};

#endif // WITH_EDITOR
