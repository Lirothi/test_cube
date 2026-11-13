#pragma once

#include <functional>
#include <vector>

#include "core/math/Math.h"

// What a race bot sees and how it plans (owner, 2026-11-12: "a bot that sees the gates, knows its boat's characteristics and
// sees its competitors"; nothing learned per course). Every tenth of a second it lays a path from where its boat IS -- its
// place, the way it moves, its speed -- through the next few gates, each crossed anywhere in its window, and keeps the
// fastest one its boat can really sail: the curvature along it gives the speed its boat makes there (its own speedFor: the
// model, and its turn map where that has learned more), braked back from every slow place and accelerated forward from
// its speed now; a bend it cannot make at any speed, or one it is too fast to brake for, costs time; so does a place where
// another boat will be when it gets there (their positions run on at their speeds).
namespace game
{
    struct PlanGate
    {
        Math::float2 center;
        Math::float2 across;    // unit: the offset's direction
        Math::float2 along;     // unit: the way through it
        float halfWindow = 0.0f; // m either side of the middle the hull's middle may cross at (its marks less the hull and a margin)
    };

    struct PlanRival
    {
        Math::float2 position;
        Math::float2 velocity;   // zero for a mark: the gates' buoys are rivals that do not move
        float clearance = 6.0f;  // m between the middles it wants
        bool mark = false;       // a buoy: anywhere along the path, and dearer
    };

    struct PlanBoat
    {
        Math::float2 position;
        Math::float2 way;        // unit: the way it moves now (the path leaves along it)
        float speed = 0.0f;      // m/s now
        float cap = 1.0f;        // m/s the most it allows itself (its top speed x its pace)
        float topSpeed = 1.0f;   // m/s: what its engine makes flat out (its acceleration falls to nothing there)
        float accel = 1.0f;      // m/s2 from rest, full ahead
        float brake = 1.0f;      // m/s2 it counts on to slow down from rest: its engine astern...
        float drag = 0.0f;       // ...plus this x v^2 (1/m): its hull's drag, which is most of it fast and little slow
        float reach = 0.0f;      // 1/m: the tightest curvature it makes at any speed
        float turnMost = 0.35f;  // rad its crossing direction may be turned at a gate (0 = Catmull-Rom's, from its neighbours)
        std::function<float(float)> speedFor; // the fastest it takes a curvature (1/m) at
        // The tightest it turns (1/m, its plan's share) at a speed. Given, its engine brakes only where the helm's rudder is not
        // over yet: the bend within its reach (leadMetres, leadSeconds of its speed) asking over 0.17 of that (the rudder past
        // 0.25), none past 0.4 (the rudder past 0.6) -- it never goes astern there, the rudder needs the wash. Empty = everywhere.
        std::function<float(float)> turnAt;
        float leadMetres = 12.0f;
        float leadSeconds = 1.2f;
    };

    struct RacePlan
    {
        std::vector<Math::float2> points; // from the boat, every ~kPlanStep m
        std::vector<float> along;         // m from the boat to each
        std::vector<float> curvature;     // 1/m, + = turning to starboard (RaceLine's sign)
        std::vector<float> speed;         // m/s it will have there: from its speed now, its engine and its braking (what the timing takes)
        std::vector<float> limit;         // m/s the most it may have there: each place's, braked back from the slow ones (what it steers its throttle by)
        std::vector<float> time;          // s from now
        std::vector<float> offsets;       // m off each gate's middle it crosses at
        std::vector<float> turns;         // rad its way through each gate is turned from the one its neighbours give (+ = to starboard)
        float seconds = 0.0f;             // to the last gate
        float penalty = 0.0f;             // s of it for what it cannot do (a bend too tight, too fast to brake, a rival in its way)
        bool valid = false;

        float Length() const { return along.empty() ? 0.0f : along.back(); }
        Math::float2 PointAt(float distance) const;
        float SpeedAt(float distance) const;
        float LimitAt(float distance) const;
        float CurvatureAt(float distance) const;
        // m along it nearest to a point, looked for from `from` on (the boat moves along it).
        float Project(const Math::float2& point, float from) const;
    };

    inline constexpr float kPlanStep = 2.5f;
    inline constexpr float kPlanTurnMost = 0.35f; // rad (20 degrees) a gate's crossing direction may be turned

    // The fastest path through `gates` (in order, the next first) from the boat as it is. `warm` / `warmTurns`: the offsets
    // and turns it planned last (one per gate; missing ones start in the middle, straight). The search: coordinate descent over
    // both, every step halved -- where it crosses each gate, and which way: a wide entry and a late apex are a crossing at
    // the gate's edge turned in.
    RacePlan PlanThroughGates(const PlanBoat& boat, const std::vector<PlanGate>& gates, const std::vector<PlanRival>& rivals,
                              const std::vector<float>& warm, const std::vector<float>& warmTurns = {});
    // One path, its speeds and its cost: what the search compares. Public for the gate's checks.
    RacePlan EvaluatePlan(const PlanBoat& boat, const std::vector<PlanGate>& gates, const std::vector<PlanRival>& rivals,
                          const std::vector<float>& offsets, const std::vector<float>& turns = {});
}
