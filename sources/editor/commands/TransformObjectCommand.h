#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h" // EditorObjectId, EditorTransform

// Undoable transform edit for one object: stores before/after transforms. One
// committed inspector edit (or, later, one gizmo drag) becomes one of these.
class TransformObjectCommand : public EditorCommand
{
public:
    TransformObjectCommand(EditorObjectId id, const EditorTransform& before, const EditorTransform& after);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;

    // Apply a transform to both the document object and its runtime object (when
    // it has a live editor-owned one). Used for live editing and by the command.
    static void ApplyTransform(EditorContext& ctx, EditorObjectId id, const EditorTransform& t);

private:
    EditorObjectId id_;
    EditorTransform before_;
    EditorTransform after_;
};

#endif // WITH_EDITOR
