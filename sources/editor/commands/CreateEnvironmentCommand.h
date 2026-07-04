#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Creates one top-level environment entity, currently used by the editor's
// Create menu for point/spot lights. Ocean is intentionally rejected; it has
// dedicated simulation lifetime handling in the Ocean menu.
class CreateEnvironmentCommand : public EditorCommand
{
public:
    explicit CreateEnvironmentCommand(EditorObject object);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;

private:
    EditorObject object_;
    EditorObjectId previousSelection_{};
    bool built_ = false;
};

#endif // WITH_EDITOR
