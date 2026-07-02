#pragma once
#if WITH_EDITOR

#include <string>
#include <vector>

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
struct LevelChangeRequest;

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
    void OnLevelChangeRequestCompleted(const LevelChangeRequest& request,
        bool loaded,
        Renderer& renderer,
        Scene& scene,
        LevelManager& levelManager);

    // The Ocean menu's "Preset Editor" item requests opening the F7 ocean controls
    // window (owned by DeveloperWindow). AppController routes this after Draw.
    bool ConsumeOpenOceanPresetEditorRequest()
    {
        const bool requested = openOceanPresetEditorRequested_;
        openOceanPresetEditorRequested_ = false;
        return requested;
    }

private:
    enum class LevelFileDialogMode
    {
        None,
        Open,
        SaveAs
    };

    enum class PendingLevelAction
    {
        None,
        Open,
        Save,
        Reload,
        New
    };

    bool open_ = false;
    bool firstOpenInitialized_ = false;
    bool openOceanPresetEditorRequested_ = false;
    bool showContentBrowser_ = true;
    bool showOutliner_ = true;
    bool showInspector_ = true;
    int selectionOutlineRadius_ = 1;
    AssetRegistry assetRegistry_;
    ContentBrowserPanel contentBrowser_;
    SceneOutlinerPanel outliner_;
    InspectorPanel inspector_;
    ViewportGizmo viewportGizmo_;
    EditorAssetId selectedAsset_;
    EditorSceneDocument document_;
    EditorObjectId selectedObject_;
    EditorCommandStack commandStack_;
    std::vector<std::string> recentLevelPaths_;
    std::string levelStatus_;
    std::string lastSavedCameraLevelPath_;
    Math::float3 lastSavedCameraPosition_{ 0.0f, 0.0f, 0.0f };
    float lastSavedCameraYaw_ = 0.0f;
    float lastSavedCameraPitch_ = 0.0f;
    bool lastSavedCameraStateValid_ = false;
    double nextCameraStateSaveTimeSec_ = 0.0;
    PendingLevelAction pendingLevelAction_ = PendingLevelAction::None;
    std::string pendingLevelPath_;
    nlohmann::json pendingNewLevelJson_;
    LevelFileDialogMode levelFileDialogMode_ = LevelFileDialogMode::None;
    char levelFileDialogDirectory_[1024] = {};
    char levelFileDialogFileName_[260] = {};
    std::string levelFileDialogStatus_;
};

#endif // WITH_EDITOR
