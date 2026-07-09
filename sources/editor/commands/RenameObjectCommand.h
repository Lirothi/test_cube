#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Renames a regular document object. Runtime scene objects do not own display
// names, so applying and undoing this command only updates the document mirror.
class RenameObjectCommand : public EditorCommand
{
public:
    RenameObjectCommand(EditorObjectId id, std::string beforeName, std::string afterName);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Rename Object"; }

private:
    void Apply(EditorContext& ctx, const std::string& name);

    EditorObjectId id_;
    std::string beforeName_;
    std::string afterName_;
};

#endif // WITH_EDITOR
