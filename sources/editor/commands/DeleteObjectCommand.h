#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Deletes an editor object from the document (and its runtime counterpart when it
// has a live editor-owned one). Stores a copy of the object so Undo can restore
// it with the same id.
class DeleteObjectCommand : public EditorCommand
{
public:
    explicit DeleteObjectCommand(EditorObjectId id);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;

private:
    EditorObjectId id_;
    bool captured_ = false;
    EditorObject object_;          // serialized copy for restore
    bool runtimeRemoved_ = false;  // did Execute remove a live runtime object?
    EditorObjectId previousSelection_{};
};

#endif // WITH_EDITOR
