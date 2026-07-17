#pragma once
#if WITH_EDITOR

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/assets/AssetRegistry.h"
#include "editor/assets/AssetThumbnailCache.h"
#include "editor/commands/EditorCommandStack.h"
#include "editor/EditorHotkeys.h"
#include "editor/EditorExtensionRegistry.h"
#include "editor/EditorSelection.h"
#include "editor/scene/EditorSceneDocument.h"
#include "editor/ui/CommandHistoryPanel.h"
#include "editor/ui/ContentBrowserPanel.h"
#include "editor/ui/ImportPanel.h"
#include "editor/ui/InspectorPanel.h"
#include "editor/ui/MeshEditorPanel.h"
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
    bool RequestOpenLevelPath(LevelManager& levelManager,
        const std::string& path,
        bool preserveCameraTransform,
        bool bypassUnsavedChangesConfirmation = false);
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

    struct PanelStateSnapshot
    {
        bool showContentBrowser = true;
        bool showOutliner = true;
        bool showInspector = true;
        bool showCommandHistory = true;
        ContentBrowserPanel::PersistentState contentBrowser;
        SceneOutlinerPanel::PersistentState outliner;
        ViewportGizmo::PersistentState viewportGizmo;
    };

    PanelStateSnapshot CapturePanelState() const;
    static bool PanelStateMatches(const PanelStateSnapshot& a, const PanelStateSnapshot& b);

    bool open_ = false;
    bool firstOpenInitialized_ = false;
    bool extensionsRegistered_ = false;
    bool openOceanPresetEditorRequested_ = false;
    bool showContentBrowser_ = true;
    bool showOutliner_ = true;
    bool showInspector_ = true;
    bool showCommandHistory_ = true;
    bool showImportPanel_ = false; // H3: import_staging -> engine assets window
    bool showMeshEditor_ = false;  // J: dedicated Mesh Editor window (edits a .mesh.json)
    bool showLevelErrors_ = false; // J: level-errors window (missing geometry/material/textures)
    int selectionOutlineRadius_ = 1;

    // J: per-object missing-asset problems (objectId.value -> messages). Rescanned when the loaded
    // level or an edit changes the document (see RefreshAssetErrorsIfStale). Consumed by the Level
    // Errors window and the outliner "Bad Assets" group.
    std::unordered_map<std::uint64_t, std::vector<std::string>> assetErrors_;
    std::uint64_t assetErrorsVersion_ = ~0ull;
    std::string   assetErrorsLevel_;
    std::size_t   assetErrorsCount_ = ~0ull;
    void RefreshAssetErrors();
    void RefreshAssetErrorsIfStale();
    AssetRegistry assetRegistry_;
    AssetThumbnailCache thumbnailCache_;
    ContentBrowserPanel contentBrowser_;
    ImportPanel importPanel_;
    MeshEditorPanel meshEditor_;
    SceneOutlinerPanel outliner_;
    InspectorPanel inspector_;
    CommandHistoryPanel commandHistory_;
    ViewportGizmo viewportGizmo_;
    EditorHotkeys hotkeys_;
    EditorExtensionRegistry extensions_;
    EditorAssetId selectedAsset_;
    EditorSceneDocument document_;
    EditorSelection selection_;
    EditorCommandStack commandStack_;
    std::vector<std::string> recentLevelPaths_;
    std::string objectClipboard_;
    std::string levelStatus_;
    std::string lastSavedCameraLevelPath_;
    Math::float3 lastSavedCameraPosition_{ 0.0f, 0.0f, 0.0f };
    float lastSavedCameraYaw_ = 0.0f;
    float lastSavedCameraPitch_ = 0.0f;
    std::array<bool, 9> cameraBookmarkSlots_{};
    bool lastSavedCameraStateValid_ = false;
    double nextCameraStateSaveTimeSec_ = 0.0;
    double nextAssetRegistryPollTimeSec_ = 0.0;
    PendingLevelAction pendingLevelAction_ = PendingLevelAction::None;
    std::string pendingLevelPath_;
    nlohmann::json pendingNewLevelJson_;
    bool confirmOpenLevelPopupRequested_ = false;
    bool confirmOpenLevelPreserveCamera_ = false;
    std::string confirmOpenLevelPath_;
    nlohmann::json lastObservedPanelState_;
    PanelStateSnapshot lastObservedPanelStateSnapshot_;
    bool panelStateLoaded_ = false;
    bool panelStateDirty_ = false;
    double nextPanelStateSaveTimeSec_ = 0.0;
    LevelFileDialogMode levelFileDialogMode_ = LevelFileDialogMode::None;
    char levelFileDialogDirectory_[1024] = {};
    char levelFileDialogFileName_[260] = {};
    std::string levelFileDialogStatus_;
};

#endif // WITH_EDITOR
