#pragma once
#if WITH_EDITOR

#include <string>

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
    // Draws the panel as its own ImGui window. `open` backs the window's close
    // button (the editor owns it). `selectedAsset` is owned by the editor; the
    // panel highlights it and writes it when the user clicks a row. Returns the
    // action the user requested this frame (Type::None if nothing).
    ContentBrowserAction Draw(AssetRegistry& registry,
        EditorAssetId& selectedAsset,
        const EditorExtensionRegistry& extensions,
        bool* open);

private:
    char searchBuffer_[256] = {};
    int typeFilterIndex_ = 0; // index into the type-filter combo (0 = All)
};

#endif // WITH_EDITOR
