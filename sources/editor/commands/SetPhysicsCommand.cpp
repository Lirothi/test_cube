#include "editor/commands/SetPhysicsCommand.h"
#if WITH_EDITOR

#include <algorithm>
#include <utility>

#include "app/scene/Scene.h"
#include "editor/EditorContext.h"
#include "editor/commands/CompositeCommand.h"
#include "editor/commands/TransformObjectCommand.h"
#include "physics/PhysicsWorld.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/RenderableObjectBase.h"

SetPhysicsCommand::SetPhysicsCommand(EditorObjectId id, physics::Motion motion, float mass)
    : id_(id)
    , motion_(motion)
    , mass_(mass)
{
}

bool SetPhysicsCommand::VesselAsset(EditorContext& ctx, EditorObjectId id)
{
    const RenderableObjectBase* base = ctx.scene.FindEditorObject(id.value);
    const RenderableObject* object = base ? base->AsRenderableObject() : nullptr;
    return object && object->GetVesselSettings() != nullptr;
}

physics::Motion SetPhysicsCommand::DocumentMotion(const EditorObject& obj, bool vesselAsset)
{
    physics::Motion motion = vesselAsset ? physics::Motion::Dynamic : physics::Motion::Static;
    const auto it = obj.properties.find("physics");
    if (it != obj.properties.end() && it->is_string()) { physics::MotionFromName(it->get<std::string>(), motion); }
    return motion;
}

float SetPhysicsCommand::DocumentMass(const EditorObject& obj)
{
    const auto it = obj.properties.find("mass");
    return it != obj.properties.end() && it->is_number() ? it->get<float>() : 0.0f;
}

void SetPhysicsCommand::Apply(EditorContext& ctx, EditorObjectId id, physics::Motion motion, float mass)
{
    if (EditorObject* obj = ctx.document.Find(id))
    {
        // Written only where it differs from the asset's default (a vessel's Dynamic, any other's Static).
        const physics::Motion assetDefault = VesselAsset(ctx, id) ? physics::Motion::Dynamic : physics::Motion::Static;
        if (motion == assetDefault) { obj->properties.erase("physics"); }
        else { obj->properties["physics"] = physics::MotionName(motion); }
        if (mass > 0.0f) { obj->properties["mass"] = mass; }
        else { obj->properties.erase("mass"); }
    }
    if (RenderableObjectBase* base = ctx.scene.FindEditorObject(id.value))
    {
        if (RenderableObject* object = base->AsRenderableObject())
        {
            physics::CollisionSettings collision = object->GetCollisionSettings();
            if (collision.mass != mass)
            {
                collision.mass = mass;
                object->SetCollisionSettings(std::move(collision));
            }
            object->SetPhysicsMotion(motion);
        }
    }
    ctx.document.SetDirty(true);
}

bool SetPhysicsCommand::Execute(EditorContext& ctx)
{
    const EditorObject* obj = ctx.document.Find(id_);
    if (!obj) { return false; }
    beforeMotion_ = DocumentMotion(*obj, VesselAsset(ctx, id_));
    beforeMass_ = DocumentMass(*obj);
    Apply(ctx, id_, motion_, mass_);
    return true;
}

void SetPhysicsCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, id_, beforeMotion_, beforeMass_);
}

std::unique_ptr<EditorCommand> BuildKeepSimulationCommand(EditorContext& ctx, const std::vector<EditorObjectId>* only, std::string& outStatus)
{
    std::vector<physics::SimulatedPose> poses;
    ctx.scene.Physics().CollectSimulatedPoses(poses);
    auto composite = std::make_unique<CompositeCommand>("Keep Simulation");
    std::size_t kept = 0;
    for (const physics::SimulatedPose& pose : poses)
    {
        const EditorObjectId id{ pose.object->GetEditorObjectId() };
        if (only && std::none_of(only->begin(), only->end(), [&id](const EditorObjectId& o) { return o.value == id.value; }))
        {
            continue;
        }
        const EditorObject* obj = ctx.document.Find(id);
        if (!obj) { continue; }
        EditorTransform after = obj->transform;
        after.position = pose.position;
        const float toDegrees = 180.0f / 3.14159265358979f;
        after.rotationDeg = Math::float3(pose.rotationEulerRad.x * toDegrees, pose.rotationEulerRad.y * toDegrees, pose.rotationEulerRad.z * toDegrees);
        composite->Add(std::make_unique<TransformObjectCommand>(id, obj->transform, after));
        ++kept;
    }
    if (kept == 0)
    {
        outStatus = "Nothing to keep: no body has moved";
        return nullptr;
    }
    // Says what it did to the level: it is not this session's, Stop no longer puts these back (Ctrl+Z after Stop does).
    outStatus = "Wrote the pose of " + std::to_string(kept) + (kept == 1 ? " object" : " objects") +
                " into the level (unsaved; Ctrl+Z after Stop undoes it)";
    return composite;
}

#endif // WITH_EDITOR
