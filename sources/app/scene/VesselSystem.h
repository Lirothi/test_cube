#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "physics/PhysicsWorld.h"  // World, BodyKey, HelmState, VesselHull, VesselInput, VesselSettings
#include "app/scene/VesselModel.h" // boats::VesselWater

class RenderableObject;

// The boats (mesh.json "vessel"; docs/play_mode_plan.md, docs/agents/boats_and_water.md): what the sea under each hull is,
// who has the helm and what it says, a race's pilots, the anchor, the hard mooring, where the frame draws each boat. Moved
// out of physics::World by architecture plan R14.3b: the physics keeps the bodies (and a boat's shape, by its asset) and runs
// this system's hooks (World::AddStepHooks) -- the water measured where the steps are counted, the orders taken as they
// start, the boat's step (boats::PushVessel, the anchor, the mooring) before each, the poses where the frame shows them, and
// Stop. Owned by the scene's world (app/scene/World), beside its physics; a gate makes its own over its own world.
//
// Threads: everything here is the game thread's but the step (Push), which reads what TakeOrders copied, the water and the
// boats' step state; whatever changes those waits for a step in flight first (World::FinishStep), as the physics' own did.
class VesselSystem
{
public:
    explicit VesselSystem(physics::World& world);
    ~VesselSystem();
    VesselSystem(const VesselSystem&) = delete;
    VesselSystem& operator=(const VesselSystem&) = delete;

    // The world the boats are bodies of (a race's rays).
    physics::World& Physics() { return world_; }

    // The nearest boat whose hull is within `reach` m of the point (to the rectangle of its hull on the water): the way aboard.
    RenderableObject* FindVessel(const Math::float3& point, float reach) const;
    // Who has the helm, and what the helm says. A boat with nobody at the helm floats with its engine off.
    bool SetHelm(RenderableObject* vessel);
    void SetHelmInput(const physics::VesselInput& input);
    // A pilot other than the helm (game::RaceBot): what it orders this boat, held until told again; null = nobody, the
    // engine off. The helm, while it is this boat's, wins. Stop takes every pilot off. False = no such boat.
    bool SetPilot(RenderableObject* vessel, const physics::VesselInput* input);
    // Puts a boat somewhere, at rest: its mesh's origin at `position`, its bow heading `heading` (radians from +z toward
    // +x), level; its anchor weighed, its engine and rudder at nothing. A race's starting grid. The session's alone,
    // as every pose is: Stop puts it back where the level has it. False = no such boat, or no world.
    bool PlaceVessel(RenderableObject* vessel, const Math::float3& position, float heading);
    // Drops the boat's anchor under its bow, or weighs it: what it is after (true = down). The world's, not the level's:
    // Stop weighs every anchor with the rest of the session.
    bool ToggleAnchor(RenderableObject* vessel);
    // The boat at the helm as the frame shows it.
    const physics::HelmState& GetHelm() const;
    // Every boat as the last show drew it -- the game's alone (Scene::Sync hands them to the ocean's wake and the spray).
    void CollectHulls(std::vector<physics::VesselHull>& out) const;
    // --set / MCP vessel.<key>: on every boat in the world, transient. False = no such key.
    bool TuneVessels(const std::string& key, float value);

private:
    struct Boat;

    // The physics' boats as they stand now (World::VesselsRevision): a boat gone takes its state with it, a new one starts
    // afresh. Waits for a step in flight before it changes the list the step reads.
    void EnsureCurrent();
    void Measure(float elapsed, const physics::WaterSurface* surface);
    void TakeOrders();
    void Push(const physics::FixedStep& step);
    void Show(float alpha);
    void ShowBoat(Boat& boat, float alpha);
    void Stopped();
    Boat* Find(const RenderableObject* vessel);

    physics::World& world_;
    int hooks_ = 0;
    std::vector<std::unique_ptr<Boat>> boats_;
    std::uint64_t revision_ = ~std::uint64_t{ 0 };
    Boat* helm_ = nullptr;
    physics::VesselInput helmInput_;     // the game's
    physics::VesselInput stepHelmInput_; // taken for the steps where they start: the step's thread owns this
    float stepMooring_ = 0.0f;           // physics.mooring* as they stood where the step started
    float stepMooringTurn_ = 0.0f;
    float stepMooringSettle_ = 0.0f;
    physics::HelmState helmShown_;       // as the last show drew it: nothing here is the step's
    std::vector<physics::VesselBody> scratch_;
};
