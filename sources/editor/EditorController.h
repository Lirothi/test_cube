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
#include "editor/ui/CommandBarPanel.h"
#include "editor/ui/CommandHistoryPanel.h"
#include "editor/ui/ModelChatPanel.h"
#include "editor/ui/ContentBrowserPanel.h"
#include "editor/intent/LlmReaper.h"
#include "editor/ui/ImportPanel.h"
#include "editor/ui/InspectorPanel.h"
#include "editor/ui/MaterialEditorPanel.h"
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
    // The MRU list is already persisted in editor_state.json after every successful open/save.
    // Startup uses its first valid entry unless an explicit --level override was supplied.
    static std::string LoadLastOpenedLevelPath();

    bool IsOpen() const { return open_; }
    void SetOpen(bool open) { open_ = open; }
    void ToggleOpen() { open_ = !open_; }
    void Draw(Renderer& renderer, Scene& scene, LevelManager& levelManager, bool logWindowOpen);
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

    // Window > Session Log toggles the viewer owned by DeveloperWindow. AppController routes the
    // request after Draw so the editor does not take ownership of an app-level window.
    bool ConsumeToggleLogWindowRequest()
    {
        const bool requested = toggleLogWindowRequested_;
        toggleLogWindowRequested_ = false;
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
        bool showCommandBar = false;
        bool showModelChat = false;
        ContentBrowserPanel::PersistentState contentBrowser;
        SceneOutlinerPanel::PersistentState outliner;
        MeshEditorPanel::PersistentState meshEditor;
        ViewportGizmo::PersistentState viewportGizmo;
    };

    PanelStateSnapshot CapturePanelState() const;
    static bool PanelStateMatches(const PanelStateSnapshot& a, const PanelStateSnapshot& b);

    bool open_ = false;
    bool firstOpenInitialized_ = false;
    bool extensionsRegistered_ = false;
    bool openOceanPresetEditorRequested_ = false;
    bool toggleLogWindowRequested_ = false;
    bool showContentBrowser_ = true;
    bool showOutliner_ = true;
    bool showInspector_ = true;
    bool showCommandHistory_ = true;
    // E1/E6: type a phrase, preview what it reaches, run it as one undoable entry. Off by
    // default -- it is a power tool beside the panels, not a fourth thing always on screen.
    bool showCommandBar_ = false;
    // A plain conversation with the local model, beside the command bar rather than inside
    // it -- see ModelChatPanel for why the two are not one panel.
    bool showModelChat_ = false;
    // "--intent=<phrase>" is in flight: submitted, waiting for the source to settle.
    bool headlessIntentPending_ = false;
    // Editor frames drawn since boot. The harness waits for a few before submitting: firing
    // on frame zero ran a command while the renderer was still standing its resources up,
    // and took the process with it. Nobody can type that fast, so this is a harness-only
    // hazard -- but a harness that crashes is not a harness.
    int headlessIntentWarmupFrames_ = 0;
    // --intent-repeat: which pass this is, the phrase to repeat, and when it was submitted.
    // The elapsed time is the whole point of repeating -- the first pass carries the system
    // prompt's prefill and no later one does.
    int headlessIntentPass_ = 0;
    std::string headlessIntentPhrase_;
    double headlessIntentStartedSec_ = 0.0;
    // True from the first pass's submission until the last one's verdict. Separate from
    // headlessIntentPending_, which is only true while ONE pass is in flight -- between two
    // passes both the phrase and the pending flag are clear, and without this the run
    // silently ended after pass 1.
    bool headlessIntentRunning_ = false;
    // Same warmup reasoning as the intent harness, counted separately because --chat can be
    // used without --intent and that counter only advances while the intent block runs.
    int headlessChatWarmupFrames_ = 0;
    // The model's own warmup: server started and system prompt prefilled, before anyone
    // types. Same frame delay as the harness, for the same reason -- the level's runtime
    // objects are still arriving over the first few frames and the prompt is built from them.
    bool modelWarmed_ = false;
    int modelWarmupFrames_ = 0;
    bool showImportPanel_ = false; // H3: import_staging -> engine assets window
    bool showMeshEditor_ = false;  // J: dedicated Mesh Editor window (edits a .mesh.json)
    bool showMaterialEditor_ = false; // I2: Material Editor window (edits a data/materials/<name>.json)
    bool showLevelErrors_ = false; // J: level-errors window (missing geometry/material/textures)
    // Bury depth, as a PERCENT of the object's world height (End). It is the thickness of the
    // footing band that must end up under the surface, which is the same thing as how deep the
    // object ends up past contact -- see BurySelectionBelowSurface.
    float buryDepthPercent_ = 1.0f;
    int selectionOutlineRadius_ = 1;

    // J: per-object missing-asset problems (objectId.value -> messages). Rescanned when the loaded
    // level or an edit changes the document (see RefreshAssetErrorsIfStale). Consumed by the Level
    // Errors window and the outliner "Bad Assets" group.
    std::unordered_map<std::uint64_t, std::vector<std::string>> assetErrors_;
    std::uint64_t assetErrorsVersion_ = ~0ull;
    std::string   assetErrorsLevel_;
    std::size_t   assetErrorsCount_ = ~0ull;
    // Debounce for the scan above: it serialises every mesh object and stats the filesystem, while
    // ContentVersion bumps once per frame for the whole of a slider drag.
    std::uint64_t assetErrorsPendingVersion_ = ~0ull;
    double        assetErrorsDueTimeSec_ = 0.0;
    void RefreshAssetErrors();
    void RefreshAssetErrorsIfStale();
    AssetRegistry assetRegistry_;
    AssetThumbnailCache thumbnailCache_;
    ContentBrowserPanel contentBrowser_;
    ImportPanel importPanel_;
    MeshEditorPanel meshEditor_;
    MaterialEditorPanel materialEditor_;
    SceneOutlinerPanel outliner_;
    InspectorPanel inspector_;
    CommandHistoryPanel commandHistory_;
    // Held for the whole editor session, so the model watchdog can tell that somebody is
    // still editing. FIRST among the members that matter here and deliberately not tied to
    // the command bar: an editor that never types a phrase is still an editor, and the
    // watchdog's rule is about sessions, not about model use.
    llmreaper::EditorSessionMark modelSessionMark_;
    CommandBarPanel commandBar_;
    ModelChatPanel modelChat_;
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
