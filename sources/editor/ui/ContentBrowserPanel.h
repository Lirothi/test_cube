#pragma once
#if WITH_EDITOR

#include "editor/assets/AssetRegistry.h"

// Searchable / filterable list of discovered assets, drawn over an AssetRegistry.
// View-only for now: it reads metadata and updates the editor's selected asset.
// It never loads or spawns anything. The context-menu actions are disabled
// placeholders wired up in later steps.
class ContentBrowserPanel
{
public:
    // `selectedAsset` is owned by the editor (editor state); the panel reads it
    // to highlight the active row and writes it when the user clicks a row.
    void Draw(AssetRegistry& registry, EditorAssetId& selectedAsset);

private:
    char searchBuffer_[256] = {};
    int typeFilterIndex_ = 0; // index into the type-filter combo (0 = All)
};

#endif // WITH_EDITOR
