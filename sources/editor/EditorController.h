#pragma once
#if WITH_EDITOR

#include "editor/assets/AssetRegistry.h"
#include "editor/scene/EditorSceneDocument.h"
#include "editor/ui/ContentBrowserPanel.h"

struct EditorContext;

// Minimal Level Editor shell: owns open/closed state and draws a placeholder
// ImGui window. The content browser, outliner, and inspector panels arrive in
// later steps.
class EditorController
{
public:
    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void ToggleOpen() { open_ = !open_; }
    void Draw(EditorContext& ctx);

private:
    bool open_ = false;
    bool firstOpenInitialized_ = false;
    AssetRegistry assetRegistry_;
    ContentBrowserPanel contentBrowser_;
    EditorAssetId selectedAsset_;
    EditorSceneDocument document_;
};

#endif // WITH_EDITOR
