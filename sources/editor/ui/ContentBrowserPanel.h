#pragma once
#if WITH_EDITOR

#include "editor/assets/AssetRegistry.h"

// A spawn request raised by the content browser's context menu. The editor
// reads it after Draw and turns it into a command. Type::None means no request.
struct ContentBrowserAction
{
    enum class Type { None, SpawnStaticMesh, SpawnTransparentMesh };
    Type type = Type::None;
    EditorAssetId asset;
};

// Searchable / filterable list of discovered assets, drawn over an AssetRegistry.
// It reads metadata, updates the editor's selected asset, and raises spawn
// requests via its context menu. It never loads or spawns anything itself.
class ContentBrowserPanel
{
public:
    // `selectedAsset` is owned by the editor (editor state); the panel reads it
    // to highlight the active row and writes it when the user clicks a row.
    // Returns the action the user requested this frame (Type::None if nothing).
    ContentBrowserAction Draw(AssetRegistry& registry, EditorAssetId& selectedAsset);

private:
    char searchBuffer_[256] = {};
    int typeFilterIndex_ = 0; // index into the type-filter combo (0 = All)
};

#endif // WITH_EDITOR
