#include "editor/intent/EditorActionBuilders.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/intent/EnvironmentSettings.h"
#include "editor/commands/EditEnvironmentCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/renderables/RenderableObject.h"

// R10.3: the editor's actions -- moving things: move / rotate / scale, align / distribute / snap, randomizing, burying.
// Cut out of EditorActionRegistry.cpp; the table that names them is still there (EditorActionBuilders.h).

namespace editoractions
{
    // ---------------------------------------------------------------------- bury

    // Bury depth limits, as a fraction of the object's world height. The caller passes the tuned
    // value (Level Editor > Placement); these only keep it sane.
    //
    // THE FRACTION IS THE DEPTH, which is not obvious and is why the first attempt overshot. It is
    // the thickness of the footing band that must end up under the surface, so demanding the WHOLE
    // band go under means the object sinks at least as deep as the band is tall. At a tenth of a
    // 15 m palm that was a metre and a half of trunk, and it looked exactly as buried as it sounds.
    // It only has to swallow the ground's unevenness under the footprint -- centimetres on sand.
    //
    // It is only the EXTRA depth, though: an object hovering a metre up still travels the whole
    // metre, because the band's vertices measure their real gap to the ground. The fraction decides
    // only how far PAST contact it ends up.
    constexpr float kBuryFootingFractionMin = 0.0005f;
    constexpr float kBuryFootingFractionMax = 0.25f;
    // Ray budget per object, per keypress. The answer is a maximum over a ring of points, so a few
    // dozen evenly spread samples find it; thousands only cost time the user can feel.
    constexpr std::size_t kBuryMaxProbes = 64;

    // BURY the targets: drop each until EVERY ONE of its footing vertices is under the surface it
    // is being buried into. This replaced a "snap to surface below" that cast ONE ray from the
    // bounds centre and rested the bottom of the AABB on what it hit; the owner asked for burying
    // instead and did not want the old behaviour kept, so it was removed rather than rebound.
    //
    // IT IS THE FOOTING THAT GETS BURIED, NOT THE WHOLE MESH -- and that is the lesson of the
    // first version. Taking "every vertex ends up under the surface" literally across ALL vertices
    // lets the HIGHEST one decide, so a 15 m palm sank fifteen metres and disappeared. Correct,
    // and useless. What burying has to fix is a footing hanging in the air on uneven ground: the
    // bottom ring of the trunk must be under the sand all the way round, while the crown is none
    // of this function's business. So only vertices within the footing fraction of the object's
    // world height, measured up from its lowest point, get a vote -- a fraction of the OBJECT
    // rather than a world distance, so it means the same thing for a palm and for a pebble.
    //
    // It still cannot use the AABB: its corners are not points on the mesh, so on a slope the
    // highest footing vertex is nowhere near a corner. For each footing vertex, in world space,
    // cast straight down -- a hit means that vertex is still ABOVE the surface by exactly that
    // distance. Drop by the LARGEST such distance and the whole footing goes under. Vertices
    // already beneath the surface find nothing below them and contribute nothing.
    //
    // AND THE RAYS ARE CAPPED at kBuryMaxProbes, because the first version was slow enough for the
    // user to notice. Every ray runs the scene broad phase and then exact triangles of whatever it
    // finds, and a terrain chunk is a great many triangles; thousands of rays per keypress is a
    // visible stall for an answer that is a MAXIMUM over a ring of points, which a few dozen evenly
    // spread samples locate just as well.
    std::unique_ptr<EditorCommand> BuildBury(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to bury";
            return nullptr;
        }

        float footingFraction = NumberOr(intent.params, "depthPercent", 1.0f) * 0.01f;
        footingFraction = std::clamp(footingFraction, kBuryFootingFractionMin, kBuryFootingFractionMax);

        // The whole target set is ignored, not just the object being moved: burying one palm into
        // the sand must not measure against another palm that is being buried with it.
        std::vector<Scene::SceneObjectId> ignoredObjectIds;
        ignoredObjectIds.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            ignoredObjectIds.push_back(id.value);
        }

        const Math::float3 rayDirection(0.0f, -1.0f, 0.0f);
        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        std::size_t meshCount = 0;
        std::size_t noSurfaceCount = 0;
        std::size_t noGeometryCount = 0;
        std::size_t alreadyBuriedCount = 0;

        for (const EditorObjectId id : targets)
        {
            EditorObject* object = ctx.document.Find(id);
            RenderableObjectBase* runtime = ctx.scene.FindEditorObject(id.value);
            RenderableObject* renderable = runtime ? runtime->As<RenderableObject>() : nullptr;
            if (!object || !renderable)
            {
                continue;
            }
            ++meshCount;

            const Mesh* mesh = renderable->GetMesh();
            if (!mesh || !mesh->HasRaycastTriangles())
            {
                // No CPU geometry (a runtime generator, an instanced batch): there is no honest
                // per-vertex answer, and quietly falling back to the AABB would bury it wrong.
                ++noGeometryCount;
                continue;
            }

            // Authored: bury rewrites the transform the level saves, and a floating object's model
            // matrix also carries this frame's wave.
            const Math::mat4 world = renderable->GetAuthoredModelMatrix();
            const std::vector<Math::float3>& localPositions = mesh->RaycastPositions();

            // The footing band is measured in WORLD height: the object can be rotated, so the
            // mesh's own local Y is not the direction gravity cares about.
            float lowestY = std::numeric_limits<float>::max();
            float highestY = std::numeric_limits<float>::lowest();
            for (const Math::float3& local : localPositions)
            {
                const float y = world.TransformPoint(local).y;
                lowestY = std::min(lowestY, y);
                highestY = std::max(highestY, y);
            }
            if (!(highestY >= lowestY))
            {
                ++noGeometryCount;
                continue;
            }
            const float footingTopY = lowestY + (highestY - lowestY) * footingFraction;

            // Gather the footing FIRST and thin it after: striding the raw vertex list would spend
            // the ray budget on the crown and leave the ring underneath barely sampled.
            std::vector<Math::float3> footing;
            for (const Math::float3& local : localPositions)
            {
                const Math::float3 worldPos = world.TransformPoint(local);
                if (worldPos.y <= footingTopY)
                {
                    footing.push_back(worldPos);
                }
            }
            if (footing.empty())
            {
                ++noGeometryCount;
                continue;
            }
            const std::size_t stride = (footing.size() + kBuryMaxProbes - 1) / kBuryMaxProbes;

            // A PROBE THAT IS ALREADY UNDERGROUND MUST NOT VOTE, and finding that out needs the
            // upward ray. Casting only downwards cannot tell "hovering above the sand" from
            // "buried in it": a vertex inside the terrain still reports a hit below it -- the far
            // side of the surface, or the slope further down -- so pressing the key again would
            // sink an already-buried object deeper every time, without limit.
            //
            // Anything hit going UP means this vertex has surface over it, which is the definition
            // of buried. Overhanging geometry (a neighbour's crown) can answer this too, and that
            // is the safe direction to be wrong in: such a probe abstains, so the object is buried
            // slightly less rather than run away downwards.
            const Math::float3 rayUp(0.0f, 1.0f, 0.0f);
            float deepest = 0.0f;
            bool hitAnything = false;
            std::size_t probeCount = 0;
            std::size_t coveredProbes = 0;
            for (std::size_t v = 0; v < footing.size(); v += stride)
            {
                ++probeCount;
                float upDistance = 0.0f;
                if (ctx.scene.RaycastEditorObject(
                        footing[v], rayUp, &upDistance, 0, &ignoredObjectIds) != 0)
                {
                    ++coveredProbes;
                    continue;   // already under a surface: contributes nothing
                }
                float hitDistance = 0.0f;
                const Scene::SceneObjectId hit = ctx.scene.RaycastEditorObject(
                    footing[v], rayDirection, &hitDistance, 0, &ignoredObjectIds);
                if (hit == 0 || !std::isfinite(hitDistance))
                {
                    continue;   // nothing below this vertex either
                }
                hitAnything = true;
                deepest = std::max(deepest, hitDistance);
            }

            // Every probe covered: the footing is fully under. Leave the object exactly where it
            // is -- re-running the tool on a finished object is a no-op, not a nudge.
            if (probeCount > 0 && coveredProbes == probeCount)
            {
                ++alreadyBuriedCount;
                continue;
            }

            if (!hitAnything)
            {
                ++noSurfaceCount;
                continue;
            }
            if (deepest <= 1.0e-4f)
            {
                ++alreadyBuriedCount;
                continue;
            }

            EditorTransform after = object->transform;
            // A hair past contact, so the highest vertex ends up INSIDE rather than coplanar with
            // the surface -- coplanar is where z-fighting lives.
            after.position.y -= deepest + 1.0e-3f;
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (meshCount == 0)
        {
            outStatus = "Select one or more meshes to bury";
            return nullptr;
        }
        if (commands.empty())
        {
            if (noGeometryCount == meshCount)
            {
                outStatus = "Selected object has no CPU geometry to bury";
            }
            else if (alreadyBuriedCount > 0 && noSurfaceCount == 0)
            {
                outStatus = meshCount == 1 ? "Selection is already buried"
                                           : "Selected meshes are already buried";
            }
            else
            {
                outStatus = "No visible editor object below selected meshes";
            }
            return nullptr;
        }

        const std::size_t buriedCount = commands.size();
        outStatus = buriedCount == 1 ? "Buried selection below surface"
                                     : "Buried " + std::to_string(buriedCount) + " objects below surface";
        return FoldIntoOneEntry(std::move(commands),
            "Bury " + std::to_string(buriedCount) + " Objects");
    }

    // --------------------------------------------------------------- transforms

    using TransformFn = void (*)(EditorTransform& t, const Math::float3& value, bool relative);

    void ApplyMove(EditorTransform& t, const Math::float3& value, bool relative)
    {
        t.position = relative
            ? Math::float3(t.position.x + value.x, t.position.y + value.y, t.position.z + value.z)
            : value;
    }

    void ApplyRotate(EditorTransform& t, const Math::float3& value, bool relative)
    {
        t.rotationDeg = relative
            ? Math::float3(t.rotationDeg.x + value.x, t.rotationDeg.y + value.y, t.rotationDeg.z + value.z)
            : value;
    }

    void ApplyScale(EditorTransform& t, const Math::float3& value, bool relative)
    {
        // Relative scaling MULTIPLIES. "make them 20% bigger" is x1.2, not +1.2, and the
        // additive reading would flip a 0.05 pebble into a boulder.
        t.scale = relative
            ? Math::float3(t.scale.x * value.x, t.scale.y * value.y, t.scale.z * value.z)
            : value;
    }

    std::unique_ptr<EditorCommand> BuildTransform(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        TransformFn apply,
        const char* verbPast,
        const char* historyVerb,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to transform";
            return nullptr;
        }

        const Math::float3 value = Vec3Or(intent.params, "value", Math::float3(0.0f, 0.0f, 0.0f));
        const bool relative = BoolOr(intent.params, "relative", true);

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                // Environment entities have no EditorTransform to edit through this path.
                continue;
            }
            EditorTransform after = object->transform;
            apply(after, value, relative);
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (commands.empty())
        {
            outStatus = "No object in the target set has a transform";
            return nullptr;
        }
        outStatus = std::string(verbPast) + " " + CountedObjects(commands.size());
        return FoldIntoOneEntry(std::move(commands),
            std::string(historyVerb) + " " + std::to_string(commands.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildMove(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus)
    {
        // EditorActionDesc::reachesLights: the lights among the targets move by their "position" property, through the
        // command the Inspector and the light gizmo use (its runtime apply rebuilds the light from the properties).
        const EditorSceneDocument& document = actionCtx.editor.document;
        const Math::float3 value = Vec3Or(intent.params, "value", Math::float3(0.0f, 0.0f, 0.0f));
        const bool relative = BoolOr(intent.params, "relative", true);
        std::vector<EditorObjectId> objects;
        std::vector<std::unique_ptr<EditorCommand>> lights;
        for (const EditorObjectId id : targets)
        {
            const EditorObject* light = FindEnvironmentObject(document, id);
            if (!light || (light->type != "pointLight" && light->type != "spotLight"))
            {
                objects.push_back(id);
                continue;
            }
            const nlohmann::json& before = light->properties;
            Math::float3 position(0.0f, 0.0f, 0.0f);
            if (const auto it = before.find("position"); it != before.end() && it->is_array() && it->size() == 3)
            {
                position = Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
            }
            position = relative ? Math::float3(position.x + value.x, position.y + value.y, position.z + value.z) : value;
            nlohmann::json after = before;
            after["position"] = nlohmann::json::array({ position.x, position.y, position.z });
            lights.push_back(std::make_unique<EditEnvironmentCommand>(id, before, std::move(after), "Move Light"));
        }
        if (lights.empty())
        {
            return BuildTransform(actionCtx, objects, intent, &ApplyMove, "Moved", "Move", outStatus);
        }
        std::vector<std::unique_ptr<EditorCommand>> commands = std::move(lights);
        const std::size_t lightCount = commands.size();
        std::string objectStatus;
        if (!objects.empty())
        {
            if (auto rest = BuildTransform(actionCtx, objects, intent, &ApplyMove, "Moved", "Move", objectStatus))
            {
                commands.push_back(std::move(rest));
            }
        }
        const bool movedObjects = commands.size() > lightCount;
        outStatus = "Moved " + std::to_string(lightCount) + (lightCount == 1 ? " light" : " lights") +
                    (movedObjects ? " and " + objectStatus.substr(objectStatus.find(' ') + 1) : std::string());
        return FoldIntoOneEntry(std::move(commands), "Move " + std::to_string(lightCount + objects.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildRotate(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus)
    {
        return BuildTransform(actionCtx, targets, intent, &ApplyRotate, "Rotated", "Rotate", outStatus);
    }

    std::unique_ptr<EditorCommand> BuildScale(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets, const EditorIntent& intent, std::string& outStatus)
    {
        EditorIntent adjusted = intent;
        if (!adjusted.params.contains("value"))
        {
            adjusted.params["value"] = nlohmann::json::array({ 1.0f, 1.0f, 1.0f });
        }
        return BuildTransform(actionCtx, targets, adjusted, &ApplyScale, "Scaled", "Scale", outStatus);
    }

    // ----------------------------------------------------- align / distribute / snap

    // Which component of a position an axis name refers to. Shared by all three placement
    // actions so "y" cannot mean one thing to align and another to snap.
    int AxisIndex(const std::string& axis)
    {
        if (axis == "x") { return 0; }
        if (axis == "y") { return 1; }
        return 2;   // "z"
    }

    float AxisValue(const Math::float3& v, int axis)
    {
        return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
    }

    // Math::float3 has no operator==, and these want an EXACT comparison anyway: the
    // question is "did this transform change at all", so that a no-op does not become an
    // undo step that undoes nothing.
    bool SameVec3(const Math::float3& a, const Math::float3& b)
    {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    void SetAxisValue(Math::float3& v, int axis, float value)
    {
        if (axis == 0) { v.x = value; }
        else if (axis == 1) { v.y = value; }
        else { v.z = value; }
    }

    // The objects a placement action can actually move: document objects with a transform.
    // Environment entities have no transform to line up.
    std::vector<EditorObject*> CollectMovable(EditorContext& ctx,
        const std::vector<EditorObjectId>& targets)
    {
        std::vector<EditorObject*> movable;
        movable.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            if (EditorObject* object = ctx.document.Find(id))
            {
                movable.push_back(object);
            }
        }
        return movable;
    }

    std::unique_ptr<EditorCommand> FoldTransforms(std::vector<EditorObject*>& movable,
        const std::vector<Math::float3>& after,
        const std::string& label,
        std::string& outStatus,
        const std::string& doneWord)
    {
        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(movable.size());
        for (std::size_t i = 0; i < movable.size(); ++i)
        {
            if (SameVec3(movable[i]->transform.position, after[i]))
            {
                continue;   // already there; a no-op entry is an undo step that does nothing
            }
            EditorTransform t = movable[i]->transform;
            t.position = after[i];
            commands.push_back(std::make_unique<TransformObjectCommand>(
                movable[i]->id, movable[i]->transform, t));
        }
        if (commands.empty())
        {
            outStatus = "Nothing moved -- they were already " + doneWord;
            return nullptr;
        }
        outStatus = doneWord + " " + CountedObjects(commands.size());
        return FoldIntoOneEntry(std::move(commands),
            label + " " + std::to_string(commands.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildAlign(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        std::vector<EditorObject*> movable = CollectMovable(actionCtx.editor, targets);
        if (movable.size() < 2)
        {
            outStatus = "Aligning needs at least two objects with a transform";
            return nullptr;
        }

        const int axis = AxisIndex(StringOr(intent.params, "axis", "x"));
        const std::string to = StringOr(intent.params, "to", "average");

        float lo = AxisValue(movable.front()->transform.position, axis);
        float hi = lo;
        float sum = 0.0f;
        for (const EditorObject* object : movable)
        {
            const float value = AxisValue(object->transform.position, axis);
            lo = std::min(lo, value);
            hi = std::max(hi, value);
            sum += value;
        }

        float target = sum / static_cast<float>(movable.size());
        if (to == "min")   { target = lo; }
        else if (to == "max")   { target = hi; }
        else if (to == "center") { target = (lo + hi) * 0.5f; }
        else if (to == "first")  { target = AxisValue(movable.front()->transform.position, axis); }

        std::vector<Math::float3> after;
        after.reserve(movable.size());
        for (const EditorObject* object : movable)
        {
            Math::float3 position = object->transform.position;
            SetAxisValue(position, axis, target);
            after.push_back(position);
        }
        return FoldTransforms(movable, after, "Align", outStatus, "aligned");
    }

    std::unique_ptr<EditorCommand> BuildDistribute(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        std::vector<EditorObject*> movable = CollectMovable(actionCtx.editor, targets);
        if (movable.size() < 3)
        {
            // Two objects are already evenly spaced, whatever the spacing is.
            outStatus = "Distributing needs at least three objects";
            return nullptr;
        }

        const int axis = AxisIndex(StringOr(intent.params, "axis", "x"));
        // Order along the axis FIRST. Distributing in selection order would shuffle them
        // past each other, which is never what the request means.
        std::sort(movable.begin(), movable.end(),
            [axis](const EditorObject* a, const EditorObject* b)
            {
                return AxisValue(a->transform.position, axis) <
                       AxisValue(b->transform.position, axis);
            });

        const float first = AxisValue(movable.front()->transform.position, axis);
        const float last = AxisValue(movable.back()->transform.position, axis);
        const float spanSteps = static_cast<float>(movable.size() - 1);
        // An explicit spacing walks out from the first one; without it the two ends stay
        // put and everything between them is evened out -- which is what "space them
        // evenly" means to someone who has already placed the ends deliberately.
        float step = (last - first) / spanSteps;
        const auto spacingIt = intent.params.find("spacing");
        if (spacingIt != intent.params.end() && spacingIt->is_number())
        {
            step = spacingIt->get<float>();
        }

        std::vector<Math::float3> after;
        after.reserve(movable.size());
        for (std::size_t i = 0; i < movable.size(); ++i)
        {
            Math::float3 position = movable[i]->transform.position;
            SetAxisValue(position, axis, first + step * static_cast<float>(i));
            after.push_back(position);
        }
        return FoldTransforms(movable, after, "Distribute", outStatus, "spaced");
    }

    std::unique_ptr<EditorCommand> BuildSnap(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        std::vector<EditorObject*> movable = CollectMovable(actionCtx.editor, targets);
        if (movable.empty())
        {
            outStatus = "Nothing with a transform to snap";
            return nullptr;
        }

        const float grid = std::max(0.0001f, NumberOr(intent.params, "grid", 1.0f));
        // DEFAULT IS THE HORIZONTAL PLANE, and that is a decision, not an oversight.
        // Snapping height to a grid on uneven ground lifts objects off it -- it undoes
        // exactly what `bury` exists to do. Someone who wants it can ask for "all"; nobody
        // should get it by typing "snap to grid" on a hillside.
        const std::string axes = StringOr(intent.params, "axes", "xz");
        const bool snapX = axes != "y" && axes != "z";
        const bool snapY = axes == "all" || axes == "y";
        const bool snapZ = axes != "x" && axes != "y";
        const float rotationStep = std::max(0.0f, NumberOr(intent.params, "rotationStep", 0.0f));

        const auto quantise = [grid](float value)
        {
            return std::round(value / grid) * grid;
        };

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(movable.size());
        for (EditorObject* object : movable)
        {
            EditorTransform t = object->transform;
            if (snapX) { t.position.x = quantise(t.position.x); }
            if (snapY) { t.position.y = quantise(t.position.y); }
            if (snapZ) { t.position.z = quantise(t.position.z); }
            if (rotationStep > 0.0f)
            {
                t.rotationDeg.y = std::round(t.rotationDeg.y / rotationStep) * rotationStep;
            }
            if (SameVec3(t.position, object->transform.position) &&
                SameVec3(t.rotationDeg, object->transform.rotationDeg))
            {
                continue;
            }
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, t));
        }

        if (commands.empty())
        {
            outStatus = "Nothing moved -- already on the grid";
            return nullptr;
        }
        outStatus = "Snapped " + CountedObjects(commands.size()) + " to a " +
            envsettings::ToText(grid) + " m grid";
        return FoldIntoOneEntry(std::move(commands),
            "Snap " + std::to_string(commands.size()) + " Objects");
    }

    // -------------------------------------------------------------- randomizing
    //
    // This is what makes a scattered set look scattered. Ten palms spawned from one asset
    // with one rotation and one scale are ten copies of the same palm standing in the same
    // pose; the eye reads the repetition instantly, long before it reads the positions.
    // So it is not decoration on top of `spawn` -- it is half of what "scatter" means, and
    // it is separately useful on props that were placed by hand.

    std::unique_ptr<EditorCommand> BuildRandomizeRotation(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to randomize";
            return nullptr;
        }

        float lo = 0.0f;
        float hi = 360.0f;
        RangeOr(intent.params, "range", lo, hi);
        const bool allAxes = StringOr(intent.params, "axis", "yaw") == "all";

        std::mt19937 rng(SeedFor(intent.params, ctx.document));
        std::uniform_real_distribution<float> dist(lo, hi);

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                continue;
            }
            EditorTransform after = object->transform;
            // Yaw only by default: a randomly pitched tree lies on its side. "all" exists
            // for rocks and debris, where any orientation is a legal one.
            after.rotationDeg.y = dist(rng);
            if (allAxes)
            {
                after.rotationDeg.x = dist(rng);
                after.rotationDeg.z = dist(rng);
            }
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (commands.empty())
        {
            outStatus = "No object in the target set has a transform";
            return nullptr;
        }
        outStatus = "Randomized rotation on " + CountedObjects(commands.size());
        return FoldIntoOneEntry(std::move(commands),
            "Randomize Rotation " + std::to_string(commands.size()) + " Objects");
    }

    std::unique_ptr<EditorCommand> BuildRandomizeScale(const EditorActionContext& actionCtx,
        const std::vector<EditorObjectId>& targets,
        const EditorIntent& intent,
        std::string& outStatus)
    {
        EditorContext& ctx = actionCtx.editor;
        if (targets.empty())
        {
            outStatus = "Nothing to randomize";
            return nullptr;
        }

        float lo = 0.9f;
        float hi = 1.1f;
        RangeOr(intent.params, "range", lo, hi);
        const bool uniform = BoolOr(intent.params, "uniform", true);

        std::mt19937 rng(SeedFor(intent.params, ctx.document));
        std::uniform_real_distribution<float> dist(lo, hi);

        std::vector<std::unique_ptr<EditorCommand>> commands;
        commands.reserve(targets.size());
        for (const EditorObjectId id : targets)
        {
            EditorObject* object = ctx.document.Find(id);
            if (!object)
            {
                continue;
            }
            EditorTransform after = object->transform;
            // The range MULTIPLIES whatever scale the object already has, so a mesh whose
            // asset spawns at 0.03 stays at its own size and only varies around it.
            const float fx = dist(rng);
            const float fy = uniform ? fx : dist(rng);
            const float fz = uniform ? fx : dist(rng);
            after.scale = Math::float3(after.scale.x * fx, after.scale.y * fy, after.scale.z * fz);
            commands.push_back(std::make_unique<TransformObjectCommand>(
                object->id, object->transform, after));
        }

        if (commands.empty())
        {
            outStatus = "No object in the target set has a transform";
            return nullptr;
        }
        outStatus = "Randomized scale on " + CountedObjects(commands.size());
        return FoldIntoOneEntry(std::move(commands),
            "Randomize Scale " + std::to_string(commands.size()) + " Objects");
    }
}

#endif // WITH_EDITOR
