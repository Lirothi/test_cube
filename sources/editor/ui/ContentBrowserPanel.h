#pragma once
#if WITH_EDITOR

#include <cstddef>
#include <string>
#include <vector>

#include "editor/assets/AssetRegistry.h"

class EditorExtensionRegistry;

// A spawn request raised by the content browser's context menu. The editor
// reads it after Draw and turns it into a command. Type::None means no request.
struct ContentBrowserAction
{
    std::string objectFactoryType;
    EditorAssetId asset;

    bool HasAction() const { return !objectFactoryType.empty(); }
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

    // Draws the panel as its own ImGui window. `open` backs the window's close
    // button (the editor owns it). `selectedAsset` is owned by the editor; the
    // panel highlights it and writes it when the user clicks a row. Returns the
    // action the user requested this frame (Type::None if nothing).
    ContentBrowserAction Draw(AssetRegistry& registry,
        EditorAssetId& selectedAsset,
        const EditorExtensionRegistry& extensions,
        bool* open);

private:
    void EnsureSelectedFolder(const AssetRegistry& registry);
    void SelectFolder(const AssetRegistry& registry, const std::string& folderPath, bool addHistory);
    void NavigateHistory(const AssetRegistry& registry, int delta);

    char searchBuffer_[256] = {};
    char sourceSearchBuffer_[128] = {};
    char newFolderName_[64] = {};
    std::string selectedFolder_ = "/Game";
    std::string newFolderParent_;
    std::string deleteFolderTarget_;
    std::string folderOperationMessage_;
    std::vector<std::string> folderHistory_;
    size_t folderHistoryIndex_ = 0;
    bool includeSubfolders_ = true;
    bool activeTypeFilters_[5] = {};
    ViewMode viewMode_ = ViewMode::List;
};

#endif // WITH_EDITOR
