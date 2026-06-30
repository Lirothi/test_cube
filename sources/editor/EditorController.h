#pragma once
#if WITH_EDITOR

#include "editor/assets/AssetRegistry.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/scene/EditorSceneDocument.h"
#include "editor/ui/ContentBrowserPanel.h"
#include "editor/ui/InspectorPanel.h"
#include "editor/ui/SceneOutlinerPanel.h"
#include "editor/ui/ViewportGizmo.h"

class Renderer;
class Scene;
class LevelManager;

// Minimal Level Editor shell: owns open/closed state and draws a placeholder
// ImGui window. The content browser, outliner, and inspector panels arrive in
// later steps.
class EditorController
{
public:
    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void ToggleOpen() { open_ = !open_; }
    void Draw(Renderer& renderer, Scene& scene, LevelManager& levelManager);

private:
    bool open_ = false;
    bool firstOpenInitialized_ = false;
    bool showContentBrowser_ = true;
    bool showOutliner_ = true;
    bool showInspector_ = true;
    AssetRegistry assetRegistry_;
    ContentBrowserPanel contentBrowser_;
    SceneOutlinerPanel outliner_;
    InspectorPanel inspector_;
    ViewportGizmo viewportGizmo_;
    EditorAssetId selectedAsset_;
    EditorSceneDocument document_;
    EditorObjectId selectedObject_;
    EditorCommandStack commandStack_;
};

#endif // WITH_EDITOR
