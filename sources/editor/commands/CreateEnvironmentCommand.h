#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Creates one top-level environment entity. Point/spot lights may have many
// instances; singleton environment sections such as directionalLight and skybox
// are rejected when the document already has one. Ocean is intentionally rejected;
// it has dedicated simulation lifetime handling in the Ocean menu.
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
