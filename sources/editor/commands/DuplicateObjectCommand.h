#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Duplicates one serialized level object into the live editor scene. Environment
// entities such as the top-level ocean are not document objects, so they are not
// duplicable through this command.
class DuplicateObjectCommand : public EditorCommand
{
public:
    explicit DuplicateObjectCommand(EditorObjectId sourceId);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;

private:
    EditorObjectId sourceId_;
    bool built_ = false;
    EditorObject object_;
    EditorObjectId previousSelection_{};
};

#endif // WITH_EDITOR
