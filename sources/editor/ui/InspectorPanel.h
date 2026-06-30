#pragma once
#if WITH_EDITOR

#include "editor/scene/EditorSceneDocument.h" // EditorObject, EditorTransform

struct EditorContext;
class EditorCommandStack;

// Inspector window for the selected object. Edits name/enabled directly on the
// document; transform edits are live and undoable (one committed edit = one
// TransformObjectCommand). Material editing arrives in a later step.
class InspectorPanel
{
public:
    // Draws as its own ImGui window. `open` backs the close button.
    void Draw(EditorContext& ctx, EditorCommandStack& commandStack, bool* open);

private:
    void DrawTransformEditor(EditorContext& ctx, EditorCommandStack& commandStack, EditorObject& object);

    EditorTransform transformBeforeEdit_{}; // captured when a transform edit gesture begins
};

#endif // WITH_EDITOR
