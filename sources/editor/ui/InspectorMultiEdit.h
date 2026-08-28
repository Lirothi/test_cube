#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommandStack.h"
#include "editor/scene/EditorSceneDocument.h"

struct EditorContext;

// Adapts the existing single-object drawers to homogeneous selections. Idle
// frames reuse snapshots; live edits broadcast field/component deltas only.
class InspectorMultiEdit
{
public:
    struct Snapshot
    {
        EditorObject object;
        nlohmann::json authored;
        nlohmann::json effective;
        bool environment = false;
    };

    bool Begin(EditorContext& ctx, EditorCommandStack& history);
    void End(EditorContext& ctx, EditorCommandStack& history, bool itemActive);
    void Finish(EditorContext& ctx, EditorCommandStack& history);
    EditorCommandStack& DrawCommands() { return drawCommands_; }
    bool HasMixedValues() const { return mixed_; }

    static Snapshot Capture(EditorContext& ctx, const EditorObject& object, bool environment);
    static void ApplySnapshot(EditorContext& ctx, const Snapshot& snapshot);

private:
    void Refresh(EditorContext& ctx);
    std::vector<Snapshot> current_;
    std::vector<Snapshot> beforeGesture_;
    EditorCommandStack drawCommands_;
    std::string levelPath_;
    uint64_t version_ = ~uint64_t(0);
    uint32_t staticVersionBeforeDraw_ = 0;
    bool mixed_ = false;
};

#endif
