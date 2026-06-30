#pragma once
#if WITH_EDITOR

#include "editor/scene/EditorSceneDocument.h" // EditorObject, EditorTransform

struct EditorContext;
class EditorCommandStack;
class AssetRegistry;

// Inspector window for the selected object. Edits name/enabled and transform
// (transform is undoable), plus mesh setup fields (material preset/params for
// staticMesh, glass params for transparentMesh). `registry` supplies the material
// preset list.
class InspectorPanel
{
public:
    // Draws as its own ImGui window. `open` backs the close button.
    void Draw(EditorContext& ctx, EditorCommandStack& commandStack, const AssetRegistry& registry, bool* open);

private:
    void DrawTransformEditor(EditorContext& ctx, EditorCommandStack& commandStack, EditorObject& object);

    EditorTransform transformBeforeEdit_{}; // captured when a transform edit gesture begins
};

#endif // WITH_EDITOR
