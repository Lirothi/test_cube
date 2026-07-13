#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/EditorSelection.h"
#include "editor/scene/EditorSceneDocument.h"

// Creates a serializable editor document object that has no live runtime
// renderable. Used for level markers such as FreeCameraStart.
class CreateDocumentObjectCommand : public EditorCommand
{
public:
    explicit CreateDocumentObjectCommand(EditorObject object);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Create Document Object"; }

private:
    EditorObject object_;
    EditorSelection previousSelection_;
    bool built_ = false;
};

#endif // WITH_EDITOR
