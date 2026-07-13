#pragma once
#if WITH_EDITOR

#include "editor/commands/EditorCommand.h"
#include "editor/EditorSelection.h"
#include "editor/scene/EditorSceneDocument.h"

// Duplicates one editor entity. Runtime document objects are recreated via the
// object registry; document-only markers are copied in the document; environment
// point/spot lights are duplicated as new environment entities and folded into
// the LightManager. Singletons with no meaningful copy -- camera, skybox,
// directional light, ocean -- are rejected.
class DuplicateObjectCommand : public EditorCommand
{
public:
    explicit DuplicateObjectCommand(EditorObjectId sourceId, bool addToSelection = false);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Duplicate Object"; }

private:
    EditorObjectId sourceId_;
    bool addToSelection_ = false;
    bool built_ = false;
    bool isEnvironment_ = false; // source was an environment light, not a document object
    EditorObject object_;
    EditorSelection previousSelection_;
};

#endif // WITH_EDITOR
