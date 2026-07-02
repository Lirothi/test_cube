#pragma once
#if WITH_EDITOR

#include "editor/scene/EditorSceneDocument.h" // EditorTransform
#include "materials/Texture2D.h"

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

    // Editor icon billboards for world-positioned environment entities (point /
    // spot lights): screen-space, always-on-top ImGui overlay images drawn from
    // the icon atlas, clickable to select the entity. Loaded lazily on first use.
    Texture2D iconAtlas_;
    bool iconAtlasTried_ = false;
    bool iconAtlasReady_ = false;
};

#endif // WITH_EDITOR
