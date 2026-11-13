#include "editor/commands/SetVesselTuneCommand.h"
#if WITH_EDITOR

#include <algorithm>
#include <cmath>
#include <utility>

#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "editor/EditorContext.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/RenderableObjectBase.h"

namespace
{
    constexpr const char* kVesselKey = "vessel";

    // The object as the factory sees it: its asset's keys under its own, the "vessel" block key by key.
    physics::VesselSettings Resolved(const nlohmann::json& properties, bool* isVessel)
    {
        physics::VesselSettings settings;
        const nlohmann::json resolved = SceneObjectFactory::ResolveMeshAsset(properties);
        const auto it = resolved.find(kVesselKey);
        const bool vessel = it != resolved.end() && it->is_object();
        if (vessel) { physics::ReadVesselSettings(*it, settings); }
        if (isVessel) { *isVessel = vessel; }
        return settings;
    }
}

SetVesselTuneCommand::SetVesselTuneCommand(EditorObjectId id, Kind kind, const physics::VesselKey* key, float value, std::string label)
    : id_(id)
    , kind_(kind)
    , key_(key)
    , value_(value)
    , label_(std::move(label))
{
}

std::unique_ptr<SetVesselTuneCommand> SetVesselTuneCommand::Key(EditorObjectId id, const physics::VesselKey& key, float value)
{
    return std::unique_ptr<SetVesselTuneCommand>(
        new SetVesselTuneCommand(id, Kind::Set, &key, std::clamp(value, key.lo, key.hi), std::string("Boat: ") + key.label));
}

std::unique_ptr<SetVesselTuneCommand> SetVesselTuneCommand::Reset(EditorObjectId id, const physics::VesselKey* key)
{
    return std::unique_ptr<SetVesselTuneCommand>(new SetVesselTuneCommand(
        id, key ? Kind::ResetKey : Kind::ResetAll, key, 0.0f, key ? std::string("Boat: ") + key->label + " as the asset's" : std::string("Boat: all as the asset's")));
}

bool SetVesselTuneCommand::IsVessel(const EditorObject& obj)
{
    bool vessel = false;
    Resolved(obj.properties, &vessel);
    return vessel;
}

const nlohmann::json* SetVesselTuneCommand::DocumentTune(const EditorObject& obj)
{
    const auto it = obj.properties.find(kVesselKey);
    return it != obj.properties.end() && it->is_object() ? &*it : nullptr;
}

bool SetVesselTuneCommand::HasOwn(const EditorObject& obj, const physics::VesselKey& key)
{
    const nlohmann::json* own = DocumentTune(obj);
    return own && own->contains(key.key);
}

physics::VesselSettings SetVesselTuneCommand::AssetSettings(const EditorObject& obj)
{
    nlohmann::json properties = obj.properties;
    properties.erase(kVesselKey);
    return Resolved(properties, nullptr);
}

physics::VesselSettings SetVesselTuneCommand::EffectiveSettings(const EditorObject& obj)
{
    return Resolved(obj.properties, nullptr);
}

bool SetVesselTuneCommand::ShapeOf(EditorContext& ctx, EditorObjectId id, HullShape& out)
{
    RenderableObjectBase* base = ctx.scene.FindEditorObject(id.value);
    const RenderableObject* object = base ? base->AsRenderableObject() : nullptr;
    if (!object) { return false; }
    std::vector<physics::VesselHull> hulls;
    ctx.scene.Physics().CollectHulls(hulls);
    for (const physics::VesselHull& hull : hulls)
    {
        if (hull.object != object || !hull.settings) { continue; }
        out.halfLength = hull.halfLength;
        out.rudderLever = hull.rudderLever;
        out.draft = hull.settings->draft * hull.scale.y;
        return true;
    }
    return false;
}

std::vector<std::unique_ptr<SetVesselTuneCommand>> SetVesselTuneCommand::Card(EditorContext& ctx, EditorObjectId id, const game::VesselCard& want,
                                                                                unsigned which, game::VesselCard* reached)
{
    std::vector<std::unique_ptr<SetVesselTuneCommand>> commands;
    if (reached) { *reached = game::VesselCard{}; }
    const EditorObject* obj = ctx.document.Find(id);
    HullShape shape;
    if (!obj || !ShapeOf(ctx, id, shape)) { return commands; }
    const physics::VesselSettings now = EffectiveSettings(*obj);
    game::VesselCard target = game::CardOf(now, shape.halfLength, shape.rudderLever, shape.draft);
    if (which & kCardTopSpeed) { target.topSpeed = want.topSpeed; }
    if (which & kCardToPlane) { target.toPlane = want.toPlane; }
    if (which & kCardCircle) { target.circle = want.circle; }
    if (which & kCardSeaKeeping) { target.seaKeeping = want.seaKeeping; }
    physics::VesselSettings solved = now;
    const game::VesselCard got = game::SolveCard(solved, shape.halfLength, shape.rudderLever, shape.draft, target);
    if (reached) { *reached = got; }
    for (const char* name : { "mass", "thrust", "surgeDrag", "rudderForce", "sideGrip" })
    {
        const physics::VesselKey* key = physics::FindVesselKey(name);
        if (key && solved.*(key->member) != now.*(key->member)) { commands.push_back(Key(id, *key, solved.*(key->member))); }
    }
    return commands;
}

void SetVesselTuneCommand::Apply(EditorContext& ctx, EditorObjectId id, const nlohmann::json& block)
{
    EditorObject* obj = ctx.document.Find(id);
    if (!obj) { return; }
    if (block.is_object() && !block.empty()) { obj->properties[kVesselKey] = block; }
    else { obj->properties.erase(kVesselKey); }
    ctx.document.SetDirty(true);
    // The running copy: the same numbers the factory would give it, and the world rebuilds its body from them.
    if (RenderableObjectBase* base = ctx.scene.FindEditorObject(id.value))
    {
        RenderableObject* object = base->AsRenderableObject();
        if (object && object->GetVesselSettings())
        {
            object->SetVesselSettings(std::make_shared<const physics::VesselSettings>(EffectiveSettings(*obj)));
        }
    }
}

nlohmann::json SetVesselTuneCommand::After(const EditorObject& obj) const
{
    const nlohmann::json* own = DocumentTune(obj);
    nlohmann::json after = own ? *own : nlohmann::json::object();
    switch (kind_)
    {
    case Kind::Set:
    {
        const float asset = AssetSettings(obj).*(key_->member);
        // At the asset's own number the copy has nothing of its own to say (a drag back to where it was).
        if (std::fabs(value_ - asset) <= 1.0e-6f * std::max(1.0f, std::fabs(asset))) { after.erase(key_->key); }
        else { after[key_->key] = value_; }
        break;
    }
    case Kind::ResetKey:
        after.erase(key_->key);
        break;
    case Kind::ResetAll:
        // Only what a copy may say: a key of another group written into the level by hand is left as it is.
        for (const physics::VesselKey& key : physics::VesselKeys())
        {
            if (physics::VesselGroupPerCopy(key.group)) { after.erase(key.key); }
        }
        break;
    }
    if (after.empty()) { after = nlohmann::json(); }
    return after;
}

bool SetVesselTuneCommand::Changes(const EditorObject& obj) const
{
    const nlohmann::json* own = DocumentTune(obj);
    return After(obj) != (own ? *own : nlohmann::json());
}

bool SetVesselTuneCommand::Execute(EditorContext& ctx)
{
    const EditorObject* obj = ctx.document.Find(id_);
    if (!obj || !Changes(*obj)) { return false; } // nothing changes: no history entry
    const nlohmann::json* own = DocumentTune(*obj);
    before_ = own ? *own : nlohmann::json();
    const nlohmann::json after = After(*obj);
    Apply(ctx, id_, after);
    return true;
}

void SetVesselTuneCommand::Undo(EditorContext& ctx)
{
    Apply(ctx, id_, before_);
}

#endif // WITH_EDITOR
