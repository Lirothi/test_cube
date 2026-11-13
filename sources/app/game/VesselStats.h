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
        float gripSpeed = 0.0f;    // m/s where its rudder's turn meets its grip: slower it turns no tighter (the rudder's circle hardly
                                   // changes with the speed), faster it runs wide (its grip's, the speed squared). Its top speed if it never lets go
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

    // The four bars of the boat's card in Play (PlaySession::DrawBoatCard) -- and the casual edit of Inspector > Boat.
    struct VesselCard
    {
        float topSpeed = 0.0f;   // m/s
        float toPlane = 0.0f;    // s from rest to 20 kn
        float circle = 0.0f;     // m across its tightest circle (2 x turnRadius)
        float seaKeeping = 0.0f; // the bar's share: half its draft against 0.8 m, half its weight against 8 t
    };
    // `draft`: as the hull floats, m (the settings' times the copy's scale).
    float SeaKeeping(float mass, float draft);
    // The bars' lengths, one scale for every boat, longer = better (the card's and the Inspector's).
    inline float TopSpeedBar(float topSpeed) { return topSpeed * 1.94384f / 45.0f; }
    inline float AccelerationBar(float toPlane) { return (7.0f - toPlane) / 5.0f; }
    // Flat out, so its grip's circle since the side grip (2026-11-12): 75 m full, 250 m empty (fast_boat_13 126, the yacht 181).
    inline float HandlingBar(float circle) { return (250.0f - circle) / 175.0f; }
    VesselCard CardOf(const physics::VesselSettings& boat, float halfLength, float rudderLever, float draft);
    // The other way (owner, 2026-11-11: "I want to drag the card's characteristics; if one moves a set of parameters,
    // that must happen by itself"): the numbers that make the card read `want`, from `boat` as it is. Each bar has its
    // number -- sea keeping the mass, the acceleration the thrust, the top speed the drag ahead (tied to the thrust:
    // thrust = 0.77 drag v^2), the circle the rudder force -- solved in that order, each against what the ones before it
    // say, so one bar dragged leaves the other three where they were (the heavier boat gets the thrust and the rudder that
    // keep its acceleration and its circle). A bar out of reach stops at its number's range. Returns what it reached.
    VesselCard SolveCard(physics::VesselSettings& boat, float halfLength, float rudderLever, float draft, const VesselCard& want);
    // The rate (rad/s) its rudder turns it at in a steady turn at a speed, full ahead and hard over.
    float SteadyTurnRate(const physics::VesselSettings& boat, float halfLength, float rudderLever, float speed);
    // m/s2: the most its hull holds across its way at a speed (VesselSettings::sideGrip, less on the plane; PushVessel's limit).
    float GripAccel(const physics::VesselSettings& boat, float speed);
    // The rate it turns at in a steady turn at a speed: its rudder's, or its grip's (GripAccel / speed) where that is less --
    // past it the boat runs wide. What a boat can do; the bot's rudder is a share of it.
    float TurnRateAt(const physics::VesselSettings& boat, float halfLength, float rudderLever, float speed);
    // The angle (rad) between its bow and its way in a steady turn of that curvature (1/m) at a speed: the hull slides,
    // bow inside its path. A curvature past what it turns at that speed counts as its tightest.
    float SlipAngle(const physics::VesselSettings& boat, float halfLength, float rudderLever, float speed, float curvature);
}
