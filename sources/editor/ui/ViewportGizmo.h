#pragma once
#if WITH_EDITOR

#include "editor/scene/EditorSceneDocument.h" // EditorTransform

struct EditorContext;
class EditorCommandStack;

// Viewport interaction: click-to-select picking (CPU ray vs object bounds) and an
// on-screen translate/rotate/scale gizmo (ImGuizmo) for the selected object. One
// gizmo drag = one TransformObjectCommand (undoable).
class ViewportGizmo
{
public:
    enum class Op { Translate, Rotate, Scale };

    // Draw the Translate/Rotate/Scale mode buttons (call from the editor toolbar).
    void DrawModeButtons();

    // Per-frame: draw + handle the gizmo for the selection, then click-to-pick.
    void Update(EditorContext& ctx, EditorCommandStack& commandStack);

private:
    Op op_ = Op::Translate;
    bool wasUsing_ = false;
    EditorTransform transformBeforeDrag_;
};

#endif // WITH_EDITOR
