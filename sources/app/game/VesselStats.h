#pragma once

#include "physics/PhysicsWorld.h"

// What a boat does, worked out of its own numbers (physics::VesselSettings, PhysicsVessel's forces at their steady state):
// what its card says to a player who walks up to it in Play, and what a bot at its helm counts on. One model for both
// (owner, 2026-11-11: "balance the boats -- one heavier but more powerful, another turns better", "teach the bot to
// take the characteristics into account so it drives any boat well").
namespace game
{
    struct VesselStats
    {
        float topSpeed = 0.0f;     // m/s, flat out: the thrust against the hull's drag on the plane
        float toPlane = 0.0f;      // s from rest to 20 kn, full ahead (the engine spooling, the hump on the way)
        float turnSpeed = 0.0f;    // m/s it keeps in its tightest steady turn: hard over, full ahead
        float turnRate = 0.0f;     // rad/s there
        float turnRadius = 0.0f;   // m: its circle
        float lateralAccel = 0.0f; // m/s2 across its way in that turn: what a bend may ask of it
        float braking = 0.0f;      // m/s2 it loses at its top speed with the engine off
        float mass = 0.0f;         // kg
        float thrust = 0.0f;       // N
        float length = 0.0f;       // m
        float draft = 0.0f;        // m its keel sits under the water at rest: what keeps it IN the water over a wave (a hull is out of
                                   // it when the sea has dropped away by about that) -- its sea keeping
    };

    // `halfLength`: of the hull's box. `rudderLever`: m the propeller (where the rudder pushes) lies aft of the centre of
    // mass, physics::VesselHull's; 0 or less = not known, 0.65 of the half length stands in.
    VesselStats StatsOf(const physics::VesselSettings& boat, float halfLength, float rudderLever);
    // The rate (rad/s) it turns at in a steady turn at a speed, full ahead and hard over.
    float SteadyTurnRate(const physics::VesselSettings& boat, float halfLength, float rudderLever, float speed);
    // The angle (rad) between its bow and its way in a steady turn of that curvature (1/m) at a speed: the hull slides,
    // bow inside its path. A curvature past what it turns at that speed counts as its tightest.
    float SlipAngle(const physics::VesselSettings& boat, float halfLength, float rudderLever, float speed, float curvature);
}
