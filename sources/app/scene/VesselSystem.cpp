#include "app/scene/VesselSystem.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "core/logging/Log.h"
#include "physics/CollisionJson.h" // VesselKeys
#include "physics/PhysicsBody.h"
#include "physics/WaterSurface.h"
#include "rendering/renderables/RenderableObject.h"

// Architecture plan R14.3b: physics::World's boats -- Entry's vessel state, SampleVesselWater, TakeHelmInputs, PushVessels,
// ShowVessels and the World API over them (PhysicsWorld.cpp before 2026-11-27) -- line by line, the poses and the
// body's sums asked of the physics (World::BodyPose, BodyToWorld, ShowBody, PlaceBody), so they come out as they did.
namespace
{
    Math::float3 AxisY() { return Math::float3(0.0f, 1.0f, 0.0f); }
    Math::float3 UnitOf(const Math::float3& v) { return v / v.Length(); } // Jolt's Vec3::Normalized
}

struct VesselSystem::Boat
{
    RenderableObject* object = nullptr;
    physics::BodyKey key = physics::kNoBody;
    physics::VesselSettings settings; // mesh.json "vessel": its own copy for --set vessel.*
    Math::float3 scale;
    float floatFactor = 1.0f;
    float halfLength = 0.0f;
    float halfBeam = 0.0f;
    float rudderLever = 0.0f; // m the propeller lies aft of the centre of mass, along the hull
    Math::float3 bowLocal;    // the stem of the hull's box at the draft, in the body's space
    physics::VesselState state; // the step's while one runs
    boats::VesselWater water;   // the step's while one runs
    // As the last show drew it, and the game's alone: who is near a boat asks every frame and must not wait for a step.
    Math::float3 shownPosition;
    Math::float3 shownForward = Math::float3(1.0f, 0.0f, 0.0f);
    Math::float3 shownUp = Math::float3(0.0f, 1.0f, 0.0f);
    float shownSpeed = 0.0f;       // state's speed as the show saw it
    float shownThrottle = 0.0f;    // and its engine and rudder
    float shownRudder = 0.0f;
    float shownImpact = 0.0f;      // and its last slam (a share of the weight, fading): the spray's burst
    float shownImmersion = 0.0f;   // how deep it sits (1 = at rest): no spray off a hull in the air
    float shownWaterHeight = 0.0f; // the surface under it: where its spray leaves and falls back to
    Math::float3 shownVelocity;    // the last step's motion of the origin and its turn: the spray's root moves
    Math::float3 shownAngular;     // with them, and a drop the side pushes leaves at least as fast
    // The anchor (ToggleAnchor, after a FinishStep; the step reads them): its rode on the bow.
    bool anchored = false;
    Math::float3 anchorPoint;
    // A pilot other than the helm (SetPilot): pilotInput is the game's, stepPilotInput taken for the steps where they
    // start, as the helm's.
    bool piloted = false;
    physics::VesselInput pilotInput;
    physics::VesselInput stepPilotInput;
    bool stepPiloted = false; // piloted, as it stood where the step started
    // The hard mooring (HoldMooring): the step's. Moored once nobody steers it and it has slowed down, where it was then.
    bool moored = false;
    Math::float3 mooredAt;
    float mooredHeading = 0.0f;
};

VesselSystem::VesselSystem(physics::World& world) : world_(world)
{
    physics::World::StepHooks hooks;
    hooks.measure = [this](float elapsed, const physics::WaterSurface* water) { Measure(elapsed, water); };
    hooks.start = [this]() { TakeOrders(); };
    hooks.fixed = [this](const physics::FixedStep& step) { Push(step); };
    hooks.shown = [this](float alpha) { Show(alpha); };
    hooks.stopped = [this]() { Stopped(); };
    hooks_ = world_.AddStepHooks(std::move(hooks));
}

VesselSystem::~VesselSystem() { world_.RemoveStepHooks(hooks_); }

void VesselSystem::EnsureCurrent()
{
    const std::uint64_t now = world_.VesselsRevision();
    if (now == revision_) { return; }
    world_.FinishStep(); // the step reads the boats
    revision_ = now;
    world_.CollectVesselBodies(scratch_);
    std::vector<std::unique_ptr<Boat>> next;
    next.reserve(scratch_.size());
    for (const physics::VesselBody& body : scratch_)
    {
        const auto kept = std::find_if(boats_.begin(), boats_.end(), [&body](const std::unique_ptr<Boat>& boat)
            { return boat && boat->object == body.object && boat->key == body.key; });
        if (kept != boats_.end())
        {
            next.push_back(std::move(*kept));
            continue;
        }
        auto boat = std::make_unique<Boat>();
        boat->object = body.object;
        boat->key = body.key;
        boat->settings = body.asset;
        boat->scale = body.scale;
        boat->floatFactor = body.floatFactor;
        boat->halfLength = body.halfLength;
        boat->halfBeam = body.halfBeam;
        boat->rudderLever = body.rudderLever;
        boat->bowLocal = body.bowLocal;
        ShowBoat(*boat, 1.0f); // where it was made
        next.push_back(std::move(boat));
    }
    if (helm_ && std::none_of(next.begin(), next.end(), [this](const std::unique_ptr<Boat>& boat) { return boat.get() == helm_; }))
    {
        helm_ = nullptr;
        helmShown_ = physics::HelmState{};
    }
    boats_ = std::move(next);
}

VesselSystem::Boat* VesselSystem::Find(const RenderableObject* vessel)
{
    for (const std::unique_ptr<Boat>& boat : boats_)
    {
        if (boat->object == vessel) { return boat.get(); }
    }
    return nullptr;
}

void VesselSystem::Measure(float elapsed, const physics::WaterSurface* surface)
{
    EnsureCurrent();
    if (boats_.empty()) { return; }
    const bool sea = surface && surface->Present();
    const physics::WaterSurface* readFrom = sea ? surface : nullptr;
    const bool waves = sea && readFrom->HasWaves();
    const float level = sea ? readFrom->Level() : 0.0f;
    for (const std::unique_ptr<Boat>& one : boats_)
    {
        Boat& boat = *one;
        Math::float3 origin;
        Math::float3 centre;
        Math::quat rotation;
        if (!sea || !world_.BodyPose(boat.key, origin, centre, rotation)) // fallen out of the world: a push wakes it, and under the world it would sink for ever
        {
            boat.water.valid = false;
            continue;
        }
        // The boats' wake (OceanWake), as the free height: a boat lying still does not float in its own hollow and
        // rocks in another's wake. Under way it leaves the wake out -- its own bow wave and stern trough are what the
        // hump, the planing and their trim already stand for, and taken twice they would trim it twice -- and so
        // another boat's too, the price of one field for all of them. Read here, between the steps (no step runs).
        const float speed = std::fabs(boat.state.speed);
        const float t = std::clamp((speed - 1.0f) / 2.0f, 0.0f, 1.0f);
        const float wakeShare = 1.0f - t * t * (3.0f - 2.0f * t);
        const std::function<float(float, float)> height = [readFrom, waves, level, wakeShare](float x, float z)
            { return waves ? readFrom->SampleHeight(x, z, wakeShare) : level; };
        boats::MeasureVesselWater(boat.water, height, origin, centre, rotation, boat.settings, boat.scale, boat.halfLength, boat.halfBeam, elapsed);
    }
}

// The orders as they stand where a step starts: the step's thread owns the copies.
void VesselSystem::TakeOrders()
{
    EnsureCurrent();
    stepHelmInput_ = helmInput_;
    for (const std::unique_ptr<Boat>& boat : boats_)
    {
        boat->stepPilotInput = boat->piloted ? boat->pilotInput : physics::VesselInput{};
        boat->stepPiloted = boat->piloted;
    }
    const physics::Settings& settings = physics::GameSettings();
    stepMooring_ = settings.mooring;
    stepMooringTurn_ = settings.mooringTurn;
    stepMooringSettle_ = settings.mooringSettle;
}

void VesselSystem::Push(const physics::FixedStep& step)
{
    const float dt = step.Dt();
    for (const std::unique_ptr<Boat>& one : boats_)
    {
        Boat& boat = *one;
        if (!boat.water.valid) { continue; }
        step.With(boat.key, [&](physics::StepBody& pushed)
        {
            boats::PushVessel(pushed, boat.settings, boat.scale, boat.state, &boat == helm_ ? stepHelmInput_ : boat.stepPilotInput, boat.water, boat.floatFactor, step.Gravity(), dt);
            if (boat.anchored) { boats::PullAnchor(pushed, boat.settings, boat.bowLocal, boat.anchorPoint, dt); }
            // Nobody steers it and its own anchor is up: the hard mooring, once it has slowed down, where it was then.
            if (&boat == helm_ || boat.stepPiloted || boat.anchored || (stepMooring_ <= 0.0f && stepMooringTurn_ <= 0.0f))
            {
                boat.moored = false;
                return;
            }
            if (!boat.moored)
            {
                const Math::float3 velocity = pushed.LinearVelocity();
                if (velocity.x * velocity.x + velocity.z * velocity.z > stepMooringSettle_ * stepMooringSettle_) { return; }
                const Math::float3 position = pushed.Position();
                const Math::float3 bow = pushed.Rotate(UnitOf(boat.settings.forward));
                boat.moored = true;
                boat.mooredAt = Math::float3(position.x, 0.0f, position.z);
                boat.mooredHeading = std::atan2(bow.x, bow.z);
            }
            boats::HoldMooring(pushed, boat.settings, boat.mooredAt, boat.mooredHeading, stepMooring_, stepMooringTurn_, dt);
        });
    }
}

// Where the frame draws the boats, for whoever asks about them on the game's side.
void VesselSystem::Show(float alpha)
{
    EnsureCurrent();
    for (const std::unique_ptr<Boat>& boat : boats_) { ShowBoat(*boat, alpha); }
}

void VesselSystem::ShowBoat(Boat& boat, float alpha)
{
    physics::ShownBody shown;
    if (!world_.ShowBody(boat.key, alpha, boat.settings.forward, shown)) { return; }
    boat.shownPosition = shown.position;
    boat.shownForward = shown.forward;
    boat.shownUp = shown.up;
    boat.shownSpeed = boat.state.speed;
    boat.shownThrottle = boat.state.throttle;
    boat.shownRudder = boat.state.rudder;
    // The last step's motion, from its two poses (the step's own velocities are the step thread's).
    boat.shownVelocity = shown.velocity;
    boat.shownAngular = shown.angular;
    boat.shownImpact = boat.state.impact;
    boat.shownImmersion = boat.state.immersion;
    boat.shownWaterHeight = boat.state.waterHeight;
    // With a margin, so a boat crawling at the limit does not go in and out of them.
    const float probeSpeed = boat.settings.giProbeSpeed;
    const float speed = std::fabs(boat.state.speed);
    if (probeSpeed > 0.0f && speed > probeSpeed) { boat.object->SetLeavesGiProbes(true); }
    else if (probeSpeed <= 0.0f || speed < 0.7f * probeSpeed) { boat.object->SetLeavesGiProbes(false); }
    if (&boat != helm_) { return; }
    helmShown_.exists = true;
    helmShown_.object = boat.object;
    helmShown_.settings = &boat.settings;
    helmShown_.position = boat.shownPosition;
    helmShown_.rotation = shown.rotation;
    helmShown_.forward = shown.forward;
    helmShown_.scale = boat.scale;
    helmShown_.starboardY = shown.up.Cross(shown.forward).y;
    helmShown_.upY = shown.up.y;
    helmShown_.halfLength = boat.halfLength;
    helmShown_.halfBeam = boat.halfBeam;
    helmShown_.anchored = boat.anchored;
    helmShown_.vessel = boat.state;
}

// Stop: the bodies are back where the level put them (the physics did that); the boats' session goes with it.
void VesselSystem::Stopped()
{
    EnsureCurrent();
    for (const std::unique_ptr<Boat>& one : boats_)
    {
        Boat& boat = *one;
        boat.object->SetLeavesGiProbes(false);
        boat.piloted = false;
        boat.pilotInput = physics::VesselInput{};
        boat.stepPilotInput = physics::VesselInput{};
        boat.state = physics::VesselState{};
        boat.water = boats::VesselWater{};
        boat.anchored = false; // the session's, weighed with it
        boat.moored = false;   // and the mooring: the next session moors it where it is then
        ShowBoat(boat, 1.0f);  // at rest where the level has it: no speed, no motion, no slam
    }
}

RenderableObject* VesselSystem::FindVessel(const Math::float3& point, float reach) const
{
    const_cast<VesselSystem*>(this)->EnsureCurrent();
    RenderableObject* best = nullptr;
    float bestDistance = reach;
    for (const std::unique_ptr<Boat>& boat : boats_)
    {
        // To the rectangle of its hull on the water, not to its middle: a boat is long.
        const float dx = point.x - boat->shownPosition.x;
        const float dz = point.z - boat->shownPosition.z;
        const float fx = boat->shownForward.x;
        const float fz = boat->shownForward.z;
        const float pastEnds = std::max(std::fabs(dx * fx + dz * fz) - boat->halfLength, 0.0f);
        const float pastSides = std::max(std::fabs(dx * fz - dz * fx) - boat->halfBeam, 0.0f);
        const float distance = std::sqrt(pastEnds * pastEnds + pastSides * pastSides);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = boat->object;
        }
    }
    return best;
}

bool VesselSystem::SetHelm(RenderableObject* vessel)
{
    world_.FinishStep();
    EnsureCurrent();
    helm_ = nullptr;
    helmInput_ = physics::VesselInput{};
    stepHelmInput_ = physics::VesselInput{};
    helmShown_ = physics::HelmState{};
    if (!vessel) { return false; }
    Boat* boat = Find(vessel);
    if (!boat) { return false; }
    helm_ = boat;
    Show(1.0f);
    return true;
}

void VesselSystem::SetHelmInput(const physics::VesselInput& input) { helmInput_ = input; }

bool VesselSystem::SetPilot(RenderableObject* vessel, const physics::VesselInput* input)
{
    // The game's own fields: no step reads them (TakeOrders copies them where a step starts), so no FinishStep.
    EnsureCurrent();
    Boat* boat = Find(vessel);
    if (!boat) { return false; }
    boat->piloted = input != nullptr;
    boat->pilotInput = input ? *input : physics::VesselInput{};
    return true;
}

bool VesselSystem::PlaceVessel(RenderableObject* vessel, const Math::float3& position, float heading)
{
    world_.FinishStep(); // the step owns the bodies and the boats' step state
    EnsureCurrent();
    Boat* boat = Find(vessel);
    if (!boat) { return false; }
    // About the vertical, by what takes the hull's own bow (its settings' forward) to the heading: a turn of t
    // takes +z to (sin t, 0, cos t).
    const Math::float3 bow = UnitOf(boat->settings.forward);
    const Math::quat rotation = physics::RotationAbout(AxisY(), heading - std::atan2(bow.x, bow.z));
    if (!world_.PlaceBody(boat->key, position, rotation)) { return false; }
    boat->state = physics::VesselState{};
    boat->water = boats::VesselWater{}; // measured afresh where it is now: the rates it followed were another place's
    boat->anchored = false;
    boat->moored = false; // moors again where it was put
    Show(1.0f);
    return true;
}

bool VesselSystem::ToggleAnchor(RenderableObject* vessel)
{
    world_.FinishStep(); // the step reads the anchor
    EnsureCurrent();
    Boat* boat = Find(vessel);
    if (!boat) { return false; }
    if (boat->anchored)
    {
        boat->anchored = false;
        if (boat == helm_) { helmShown_.anchored = false; } // the HUD before the next show
        LOG_INFO(logging::LogCategory::Physics, "physics: '{}' weighed its anchor", vessel->GetGeometryPath());
        return false;
    }
    // Dropped straight down from the bow, where it is now: the boat lays out its scope as it drifts back.
    if (!world_.BodyToWorld(boat->key, boat->bowLocal, boat->anchorPoint)) { return false; }
    boat->anchored = true;
    if (boat == helm_) { helmShown_.anchored = true; }
    LOG_INFO(logging::LogCategory::Physics, "physics: '{}' dropped its anchor at {:.2f} {:.2f}, {:.1f} m of slack", vessel->GetGeometryPath(),
             boat->anchorPoint.x, boat->anchorPoint.z, boat->settings.anchorScope);
    return true;
}

const physics::HelmState& VesselSystem::GetHelm() const
{
    const_cast<VesselSystem*>(this)->EnsureCurrent();
    return helmShown_;
}

void VesselSystem::CollectHulls(std::vector<physics::VesselHull>& out) const
{
    const_cast<VesselSystem*>(this)->EnsureCurrent();
    out.clear();
    for (const std::unique_ptr<Boat>& one : boats_)
    {
        const Boat& boat = *one;
        if (!boat.object) { continue; }
        physics::VesselHull hull;
        hull.object = boat.object;
        hull.settings = &boat.settings;
        hull.position = boat.shownPosition;
        hull.forward = boat.shownForward;
        hull.up = boat.shownUp;
        hull.scale = boat.scale;
        hull.speed = boat.shownSpeed;
        hull.velocity = boat.shownVelocity;
        hull.angularVelocity = boat.shownAngular;
        hull.impact = boat.shownImpact;
        hull.immersion = boat.shownImmersion;
        hull.waterHeight = boat.shownWaterHeight;
        hull.atHelm = &boat == helm_;
        hull.anchored = boat.anchored;
        hull.piloted = boat.piloted;
        hull.halfLength = boat.halfLength;
        hull.halfBeam = boat.halfBeam;
        hull.rudderLever = boat.rudderLever;
        hull.throttle = boat.shownThrottle;
        hull.rudder = boat.shownRudder;
        out.push_back(hull);
    }
}

bool VesselSystem::TuneVessels(const std::string& key, float value)
{
    for (const physics::VesselKey& entry : physics::VesselKeys())
    {
        if (key != entry.key) { continue; }
        world_.FinishStep();
        EnsureCurrent();
        for (const std::unique_ptr<Boat>& boat : boats_) { boat->settings.*(entry.member) = std::clamp(value, entry.lo, entry.hi); }
        return true;
    }
    return false;
}
