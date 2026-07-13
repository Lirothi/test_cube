#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/commands/EditorCommand.h"
#include "editor/scene/EditorSceneDocument.h"

// Creates a fresh object from serialized clipboard JSON. Regular document
// objects are rebuilt through SceneObjectRegistry; point/spot lights are added
// as environment entities. The paste owns selection and runtime undo/redo.
class PasteObjectCommand : public EditorCommand
{
public:
    explicit PasteObjectCommand(nlohmann::json objectJson);

    static bool Validate(const nlohmann::json& objectJson, std::string& outReason);

    bool Execute(EditorContext& ctx) override;
    void Undo(EditorContext& ctx) override;
    std::string_view HistoryLabel() const override { return "Paste Object"; }

private:
    nlohmann::json objectJson_;
    EditorObject object_;
    EditorObjectId previousSelection_{};
    bool built_ = false;
    bool isEnvironment_ = false;
};

#endif // WITH_EDITOR
