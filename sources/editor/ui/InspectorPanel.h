#pragma once
#if WITH_EDITOR

#include <string>

#include "editor/scene/EditorSceneDocument.h" // EditorObject, EditorTransform
#include "editor/ui/InspectorMultiEdit.h"

struct EditorContext;
class EditorCommandStack;
class AssetRegistry;
class EditorExtensionRegistry;

// Inspector window for the selected object. Edits name/enabled and transform
// (transform is undoable), plus mesh setup fields (material preset/params for
// staticMesh, glass params for transparentMesh). `registry` supplies the material
// preset list.
class InspectorPanel
{
public:
    // Draws as its own ImGui window. `open` backs the close button.
    void Draw(EditorContext& ctx,
        EditorCommandStack& commandStack,
        const AssetRegistry& registry,
        const EditorExtensionRegistry& extensions,
        bool* open);

private:
    void DrawTransformEditor(EditorContext& ctx, EditorCommandStack& commandStack, EditorObject& object);

    InspectorMultiEdit multiEdit_;
    EditorObjectId nameEditObject_{};
    char nameEditBuffer_[256] = {};
    std::string nameBeforeEdit_;
    bool nameEditActive_ = false;
    EditorObjectId environmentEditObject_{};
    nlohmann::json environmentPropertiesBeforeEdit_;
    EditorTransform transformBeforeEdit_{}; // captured when a transform edit gesture begins
};

#endif // WITH_EDITOR
