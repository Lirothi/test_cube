#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Creates a serializable editor document object that has no live runtime
// renderable. Used for level markers such as FreeCameraStart.
class CreateDocumentObjectCommand : public EditorCommand
{
public:
    explicit CreateDocumentObjectCommand(EditorObject object);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;

private:
    EditorObject object_;
    EditorObjectId previousSelection_{};
    bool built_ = false;
};

#endif // WITH_EDITOR
