#pragma once

#include <functional>

#include "core/math/Math.h"
#include "physics/PhysicsTypes.h" // VesselSettings, VesselState, VesselInput

// A boat's model (architecture plan R14.3a, moved out of the physics by R14.3b): the water under its hull and one step's
// forces on it, in the engine's types over physics::StepBody. app/scene/VesselModel.cpp; app/scene/VesselSystem runs it, the
// vessel gate (app/scene/VesselGates) on physics::VesselBench. The body's shape (MakeVesselShape) stays with the physics.
namespace physics { class StepBody; } // physics/PhysicsBody.h

namespace boats
{
    using physics::StepBody;
    using physics::VesselInput;
    using physics::VesselSettings;
    using physics::VesselState;

    // What the sea is doing under a boat, measured on the game's side, where the water can be read (MeasureVesselWater): the plane that fits the surface over the hull, which Jolt's buoyancy cuts the
    // hull with -- a wave shorter than the hull lifts it no more than a real one does --, and three stations along the hull for what one plane cannot say: a slope that differs at the bow and the
    // stern, a bow falling into a crest.
    struct VesselStation
    {
        float offset = 0.0f;      // m along the hull from the centre of mass: stern, middle, bow
        float height = 0.0f;      // the water's
        float speed = 0.0f;       // how fast it rises where the hull is, m/s, the hull's own way in it
        float slopeAlong = 0.0f;  // of the surface, rising toward the bow
        float slopeAcross = 0.0f; // rising toward starboard
    };
    struct VesselWater
    {
        bool valid = false;
        float height = 0.0f;      // of the plane, at the centre of mass
        float speed = 0.0f;       // m/s, as a station's
        Math::float3 normal = Math::float3(0.0f, 1.0f, 0.0f);
        Math::float3 spin = Math::float3(0.0f, 0.0f, 0.0f);     // how fast the plane turns, rad/s
        Math::float3 forward = Math::float3(1.0f, 0.0f, 0.0f);  // the hull's, level
        Math::float3 starboard = Math::float3(0.0f, 0.0f, -1.0f);
        float propellerHeight = 0.0f; // the water's, over the propeller: on a crest's back it comes out
        float calmAcross = 0.0f;  // the surface's rise toward starboard, slowly: what the form stability lies along. The chop passes under a hull faster than it can roll
        VesselStation stations[3];
    };

    // `height` answers the water's height at (x, z); `elapsed` is the time since the last measure, 0 = a first look, no rates yet. `origin` is the mesh's, `centre` the centre of mass.
    void MeasureVesselWater(VesselWater& water, const std::function<float(float, float)>& height, const Math::float3& origin, const Math::float3& centre, const Math::quat& rotation,
        const VesselSettings& settings, const Math::float3& scale, float halfLength, float halfBeam, float elapsed);
    // One step on the water: its buoyancy, the helm, the hull and the waves. R14.2: on a StepBody, in the engine's types.
    void PushVessel(StepBody& body, const VesselSettings& settings, const Math::float3& scale, VesselState& state, const VesselInput& input, const VesselWater& water, float buoyancy, const Math::float3& gravity, float dt);
    // The anchor's rode on the bow (`bowLocal`, the body's space), from `anchor` (world; its height not read): nothing within
    // anchorScope, past it the stretch's spring and the bow's run along it, never a push. One step's impulse.
    void PullAnchor(StepBody& body, const VesselSettings& settings, const Math::float3& bowLocal, const Math::float3& anchor, float dt);
    // A boat nobody steers (owner, 2026-11-13: "when we board one, the others must stand on a hard anchor"): its place along
    // the water (`at`, its origin's x and z) and its heading (`heading`, of its bow -- the settings' forward -- about the
    // vertical) held by critically damped springs on the velocities, `stiffness` and `turn` rad/s their own frequencies
    // (physics.mooring*): it heaves, pitches and rolls on the waves and stays put.
    void HoldMooring(StepBody& body, const VesselSettings& settings, const Math::float3& at, float heading, float stiffness, float turn, float dt);
} // namespace boats
