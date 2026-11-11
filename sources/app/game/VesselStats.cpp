#include "app/game/VesselStats.h"

#include <algorithm>
#include <cmath>

namespace game
{
    namespace
    {
        // The share of a planing hull that is in the water: its speed carries the rest of its weight (planingLift), and
        // every drag of PhysicsVessel is times it. Measured on small_yacht: thrust = 0.77 surgeDrag v^2 at its top speed.
        constexpr float kWet = 0.77f;
        constexpr float kTwentyKnots = 10.29f;

        float SmoothStep(float a, float b, float x)
        {
            const float t = std::clamp((x - a) / std::max(b - a, 1e-4f), 0.0f, 1.0f);
            return t * t * (3.0f - 2.0f * t);
        }

        float Lever(const physics::VesselSettings& boat, float halfLength, float rudderLever)
        {
            return std::max((rudderLever > 0.0f ? rudderLever : 0.65f * halfLength) - boat.swayLever, 0.2f);
        }
    }

    // The rudder pushes the stern out with F = rudderForce (speed + wash)^2 sin(rudderDeg), `rudderLever` aft of the
    // centre of mass. The hull then slides, and the water's push on its side is what turns its path -- m v w + F -- acting
    // swayLever aft: against the turn. With the yaw drag (in the water by kWet):
    //     kWet (yawDrag w^2 + yawLinear w) + m v swayLever w = F (lever - swayLever).
    // Without the keel's term a boat's circle came out at half what it is (26-32 m against the measured 55).
    float SteadyTurnRate(const physics::VesselSettings& boat, float halfLength, float rudderLever, float speed)
    {
        const float v = std::max(speed, 0.5f);
        const float flow = v + boat.wash;
        const float push = boat.rudderForce * flow * flow * std::sin(boat.rudderDeg * 0.0174532925f);
        const float linear = kWet * boat.yawLinear + boat.mass * v * boat.swayLever;
        const float quadratic = std::max(kWet * boat.yawDrag, 1.0f);
        return (-linear + std::sqrt(linear * linear + 4.0f * quadratic * push * Lever(boat, halfLength, rudderLever))) / (2.0f * quadratic);
    }

    // The same two balances at a turn rate of speed x curvature: the rudder's force that holds that rate (the yaw's), and
    // the slide at which the water's push on the hull's side is m v w + that force (the side's).
    float SlipAngle(const physics::VesselSettings& boat, float halfLength, float rudderLever, float speed, float curvature)
    {
        const float v = std::max(speed, 0.5f);
        const float rate = std::min(v * std::fabs(curvature), SteadyTurnRate(boat, halfLength, rudderLever, v));
        const float rudder = (kWet * (boat.yawDrag * rate * rate + boat.yawLinear * rate) + boat.mass * v * rate * boat.swayLever) / Lever(boat, halfLength, rudderLever);
        const float side = boat.mass * v * rate + rudder;
        const float a = std::max(kWet * boat.swayDrag, 1e-3f);
        const float b = kWet * boat.swayLinear;
        const float slide = (-b + std::sqrt(b * b + 4.0f * a * side)) / (2.0f * a);
        return std::atan2(slide, v);
    }

    VesselStats StatsOf(const physics::VesselSettings& boat, float halfLength, float rudderLever)
    {
        VesselStats stats;
        stats.mass = boat.mass;
        stats.thrust = boat.thrust;
        stats.length = 2.0f * halfLength;
        stats.draft = boat.draft;
        const float mass = std::max(boat.mass, 1.0f);
        const float drag = std::max(kWet * boat.surgeDrag, 1e-3f);
        stats.topSpeed = std::sqrt(std::max(boat.thrust, 1.0f) / drag);
        stats.braking = boat.thrust / mass; // at its top speed the drag is the thrust

        // From rest to 20 kn: the engine spools (throttleRate), the hull climbs its hump and comes onto the plane.
        {
            float v = 0.0f;
            float t = 0.0f;
            const float dt = 0.02f;
            while (v < kTwentyKnots && t < 60.0f)
            {
                const float throttle = std::min(t * std::max(boat.throttleRate, 0.05f), 1.0f);
                const float rising = SmoothStep(0.0f, boat.humpSpeed, v);
                const float planing = SmoothStep(boat.humpSpeed, boat.planeSpeed, v);
                const float off = (v - boat.humpSpeed) / std::max(0.5f * boat.humpSpeed, 0.01f);
                const float wet = 1.0f - (1.0f - kWet) * planing;
                const float resist = wet * (boat.surgeDrag * v * v + boat.humpDrag * rising * std::exp(-off * off));
                v += (boat.thrust * throttle - resist) / mass * dt;
                t += dt;
            }
            stats.toPlane = t;
        }

        // Its tightest steady turn, hard over and full ahead. Three balances: the yaw's (SteadyTurnRate), the side's --
        // the water's push on the sliding hull, kWet (swayDrag s^2 + swayLinear s), is m v w + F -- and the way's: the
        // slide at that turn rate takes m s w off the thrust, so the boat slows by itself. A few rounds settle them.
        {
            float v = stats.topSpeed;
            float rate = 0.0f;
            for (int round = 0; round < 40; ++round)
            {
                rate = SteadyTurnRate(boat, halfLength, rudderLever, v);
                const float flow = v + boat.wash;
                const float push = boat.rudderForce * flow * flow * std::sin(boat.rudderDeg * 0.0174532925f);
                const float side = boat.mass * v * rate + push;
                const float a = std::max(kWet * boat.swayDrag, 1e-3f);
                const float b = kWet * boat.swayLinear;
                const float slide = (-b + std::sqrt(b * b + 4.0f * a * side)) / (2.0f * a);
                const float left = std::max(boat.thrust - boat.mass * slide * rate, 0.05f * boat.thrust);
                v += 0.5f * (std::sqrt(left / drag) - v);
            }
            stats.turnSpeed = v;
            stats.turnRate = rate;
            stats.turnRadius = v / std::max(rate, 1e-4f);
            stats.lateralAccel = v * rate;
        }
        return stats;
    }
}
