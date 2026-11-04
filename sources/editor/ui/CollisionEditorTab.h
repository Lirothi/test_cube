#pragma once
#if WITH_EDITOR

#include <cstdint>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "imgui.h"
#include "physics/PhysicsWorld.h"
#include "third_party/json/json.hpp"

// Mesh Editor > Collision: how an asset meets the physics world (docs/physics_plan.md section 9). It edits the mesh.json being edited, its
// "collision" block and nothing else, and writes only what differs from the defaults, so an asset nobody touched stays byte-identical. With
// "Custom shapes" the asset's collision is a list of boxes, spheres, capsules and cylinders (UE's simple collision), moved with a gizmo over the preview.
// No GPU and no panel state of the Mesh Editor: tools/inspector_regression drives it with real ImGui clicks.
class CollisionEditorTab
{
public:
    // Another asset: the selection and the preview start over.
    void Reset();

    // The tab's body. `slots` names the asset's material slots.
    void Draw(nlohmann::json& doc, const std::vector<std::string>& slots);

    // The shape's edges in mesh space, pairs of points, for the preview to draw against the mesh's depth; the selected primitive's apart. The revision moves when they do, and is
    // never 0 (the preview's "no lines").
    const std::vector<Math::float3>& Lines() const { return lines_; }
    const std::vector<Math::float3>& SelectedLines() const { return selectedLines_; }
    std::uint64_t LinesRevision() const { return linesRevision_; }

    // The selected primitive's gizmo over the preview. Call after the picture, with the matrices it was rendered with.
    void DrawGizmo(nlohmann::json& doc, ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const Math::mat4& view, const Math::mat4& proj);

    // The gizmo holds the mouse: the preview does not orbit and the window does not move.
    bool GizmoWantsMouse() const { return gizmoWantsMouse_; }

    int SelectedPrimitive() const { return selected_; }

private:
    enum class GizmoMode { Move, Rotate, Size };

    void DrawPrimitives(physics::CollisionSettings& settings, const nlohmann::json& doc, const std::vector<std::string>& slots, bool& changed);
    void RebuildOverlay(const physics::CollisionSettings& settings);

    int selected_ = -1;
    int fitSlot_ = -1; // what Fit measures: -1 = every solid slot
    bool previewAsDynamic_ = false;
    bool gizmoWantsMouse_ = false;
    GizmoMode gizmoMode_ = GizmoMode::Move;
    std::string previewKey_;
    physics::CollisionPreview preview_;
    std::vector<Math::float3> lines_;         // pairs of points
    std::vector<Math::float3> selectedLines_; // the selected primitive's
    std::uint64_t linesRevision_ = 1;
};

#endif // WITH_EDITOR
