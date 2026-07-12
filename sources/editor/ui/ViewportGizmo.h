#pragma once
#if WITH_EDITOR

#include "editor/scene/EditorSceneDocument.h" // EditorTransform
#include "materials/Texture2D.h"

struct EditorContext;
class EditorCommandStack;
class AssetRegistry;
class EditorExtensionRegistry;

// Viewport interaction: click-to-select picking (CPU ray vs object bounds) and an
// on-screen translate/rotate/scale gizmo (ImGuizmo) for the selected object. One
// gizmo drag = one TransformObjectCommand (undoable).
class ViewportGizmo
{
public:
    enum class Op { Select, Translate, Rotate, Scale };
    enum class TransformSpace { Local, World };

    struct PersistentState
    {
        bool snapEnabled = false;
        float translationIncrement = 0.1f;
        float rotationIncrement = 15.0f;
        float scaleIncrement = 0.1f;
        TransformSpace transformSpace = TransformSpace::World;
    };

    // Draw the Select/Translate/Rotate/Scale mode buttons (call from the editor toolbar).
    void DrawModeButtons(const char* hotkeyHintText);

    // Per-frame: draw + handle the gizmo for the selection, then click-to-pick.
    void Update(EditorContext& ctx,
        EditorCommandStack& commandStack,
        const AssetRegistry& registry,
        const EditorExtensionRegistry& extensions);

    void SetMode(Op op) { op_ = op; }
    Op GetMode() const { return op_; }
    void SetTemporarySnapInvert(bool held) { temporarySnapInvertHeld_ = held; }
    PersistentState GetPersistentState() const;
    void SetPersistentState(const PersistentState& state);
    void CycleTransformMode();
    const char* ModeLabel() const;
    static const char* ModeLabel(Op op);

private:
    Op op_ = Op::Translate;
    TransformSpace transformSpace_ = TransformSpace::World;
    bool snapEnabled_ = false;
    bool temporarySnapInvertHeld_ = false;
    float translationIncrement_ = 0.5f;
    float rotationIncrement_ = 15.0f;
    float scaleIncrement_ = 0.1f;
    bool wasUsing_ = false;
    EditorTransform transformBeforeDrag_;
    float envGizmoMatrix_[16] = {}; // persistent model matrix while dragging an env light
    bool envWasUsing_ = false;
    EditorObjectId envDragObject_{};
    nlohmann::json envPropertiesBeforeDrag_;

    // Editor icon billboards for world-positioned editor entities: screen-space,
    // always-on-top ImGui overlay images drawn from the icon atlas, clickable to
    // select the entity. Loaded lazily on first use.
    Texture2D iconAtlas_;
    bool iconAtlasTried_ = false;
    bool iconAtlasReady_ = false;
};

#endif // WITH_EDITOR
