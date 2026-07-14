#pragma once
#if WITH_EDITOR

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "editor/EditorSelection.h"
#include "editor/scene/EditorSceneDocument.h"

struct ImGuiTableSortSpecs;

// A delete request raised by the outliner; the editor turns it into a command.
struct OutlinerAction
{
    enum class Type
    {
        None,
        DeleteObject,
        DuplicateObject,
        FrameSelection,
        RenameObject,
        SetEnabled,
        SetEnvEnabled
    };
    Type type = Type::None;
    EditorObjectId target;
    bool enabledValue = false; // for SetEnabled / SetEnvEnabled
    std::string nameValue; // for RenameObject
};

// Lists document and environment entities in a searchable/filterable table.
// Selecting a row writes the editor's selected object; row controls raise
// actions that the editor turns into commands or live environment updates.
class SceneOutlinerPanel
{
public:
    struct PersistentState
    {
        bool meshesGroupOpen = true;
        bool lightsGroupOpen = true;
        bool camerasGroupOpen = true;
        bool environmentGroupOpen = true;
        bool otherGroupOpen = true;
    };

    // Draws the panel as its own ImGui window. `open` backs the window's close
    // button (the editor owns it).
    OutlinerAction Draw(EditorSceneDocument& document, EditorSelection& selection, bool* open);

    PersistentState GetPersistentState() const;
    void SetPersistentState(const PersistentState& state);

private:
    // One filtered row (a document object or an environment entry). Bucketed into
    // the per-group scratch vectors below during Draw.
    struct OutlinerRowRef
    {
        EditorObject* object = nullptr;
        bool environment = false;
    };

    static void SortRows(std::vector<OutlinerRowRef>& rows,
        const ImGuiTableSortSpecs* sortSpecs);

    char searchBuffer_[256] = {};
    bool showObjects_ = true;
    bool showEnvironment_ = true;
    bool meshesGroupOpen_ = true;
    bool lightsGroupOpen_ = true;
    bool camerasGroupOpen_ = true;
    bool environmentGroupOpen_ = true;
    bool otherGroupOpen_ = true;
    int typeFilterIndex_ = 0;
    EditorObjectId rangeAnchor_{};
    EditorObjectId renamingObject_{};
    char renameBuffer_[256] = {};
    bool renameFocusRequested_ = false;

    // Per-frame scratch, reused across Draw calls (cleared each frame, capacity
    // retained) so the row buckets and display order don't reallocate every frame.
    std::vector<OutlinerRowRef> scratchMeshes_;
    std::vector<OutlinerRowRef> scratchLights_;
    std::vector<OutlinerRowRef> scratchCameras_;
    std::vector<OutlinerRowRef> scratchEnvironment_;
    std::vector<OutlinerRowRef> scratchOther_;
    std::vector<EditorObjectId> scratchDisplayedOrder_;

    // Filtered-bucket cache. The buckets hold EditorObject* into the document's
    // vectors, so the key snapshots those vectors' storage identity (data + size)
    // for pointer safety across add/remove/realloc, plus the document content
    // version for in-place field edits (rename/enable) and the filter inputs.
    // Sorting is re-applied only when the buckets change or the user re-sorts.
    bool bucketCacheValid_ = false;
    std::uint64_t cacheContentVersion_ = 0;
    const void* cacheObjectsData_ = nullptr;
    std::size_t cacheObjectsSize_ = 0;
    const void* cacheEnvironmentData_ = nullptr;
    std::size_t cacheEnvironmentSize_ = 0;
    int cacheTypeFilterIndex_ = -1;
    bool cacheShowObjects_ = false;
    bool cacheShowEnvironment_ = false;
    std::string cacheSearch_;
};

#endif // WITH_EDITOR
