#include "app/game/RacePlanner.h"

#include <algorithm>
#include <cmath>

namespace game
{
    namespace
    {
        using Math::float2;

        constexpr float kPi = 3.14159265f;
        constexpr float kTooTight = 300.0f; // s per m of path per 1/m of curvature past what the boat makes at any speed
        constexpr float kOverSpeed = 0.5f;  // s per m of path per m/s over what it takes the bend at (it could not brake in time)
        constexpr float kRival = 1.0f;      // s per m of path run right where another boat will be
        constexpr float kMark = 4.0f;       // s per m of path run right over a buoy (it stops the boat dead: seconds lost, a gate)
        constexpr float kRivalSeconds = 6.0f; // s ahead it runs the others on: past that a straight line is a guess
        constexpr float kEdge = 0.2f;        // s for crossing a gate at the very edge of its window (its share of the window to the fourth)
        constexpr float kSettle = 6.0f;      // m of the plan from the boat that its curvature limits nothing (the helm settles them)

        float Heading(const float2& v) { return std::atan2(v.x, v.y); }

        float WrapAngle(float a)
        {
            while (a > kPi) { a -= 2.0f * kPi; }
            while (a < -kPi) { a += 2.0f * kPi; }
            return a;
        }

        float Length(const float2& v) { return std::sqrt(v.Dot(v)); }

        // The sample at or before a distance along the plan, and how far past it.
        std::size_t Below(const std::vector<float>& along, float distance, float& t)
        {
            if (along.size() < 2)
            {
                t = 0.0f;
                return 0;
            }
            const float d = std::clamp(distance, 0.0f, along.back());
            const std::size_t i = std::min(static_cast<std::size_t>(std::upper_bound(along.begin(), along.end(), d) - along.begin()), along.size() - 1);
            const std::size_t a = i > 0 ? i - 1 : 0;
            const float span = along[a + 1] - along[a];
            t = span > 1e-4f ? (d - along[a]) / span : 0.0f;
            return a;
        }
    }

    Math::float2 RacePlan::PointAt(float distance) const
    {
        if (points.empty()) { return Math::float2(0.0f, 0.0f); }
        float t = 0.0f;
        const std::size_t a = Below(along, distance, t);
        if (a + 1 >= points.size()) { return points.back(); }
        return points[a] + (points[a + 1] - points[a]) * t;
    }

    float RacePlan::SpeedAt(float distance) const
    {
        if (speed.empty()) { return 0.0f; }
        float t = 0.0f;
        const std::size_t a = Below(along, distance, t);
        if (a + 1 >= speed.size()) { return speed.back(); }
        return speed[a] + (speed[a + 1] - speed[a]) * t;
    }

    float RacePlan::LimitAt(float distance) const
    {
        if (limit.empty()) { return 0.0f; }
        float t = 0.0f;
        const std::size_t a = Below(along, distance, t);
        if (a + 1 >= limit.size()) { return limit.back(); }
        return limit[a] + (limit[a + 1] - limit[a]) * t;
    }

    float RacePlan::CurvatureAt(float distance) const
    {
        if (curvature.empty()) { return 0.0f; }
        float t = 0.0f;
        const std::size_t a = Below(along, distance, t);
        if (a + 1 >= curvature.size()) { return curvature.back(); }
        return curvature[a] + (curvature[a + 1] - curvature[a]) * t;
    }

    float RacePlan::Project(const Math::float2& point, float from) const
    {
        if (points.size() < 2) { return 0.0f; }
        float t = 0.0f;
        const std::size_t first = Below(along, from, t);
        float best = from;
        float bestDistance = 1.0e30f;
        for (std::size_t i = first; i + 1 < points.size() && along[i] < from + 60.0f; ++i)
        {
            const float2 piece = points[i + 1] - points[i];
            const float length2 = std::max(piece.Dot(piece), 1e-6f);
            const float u = std::clamp((point - points[i]).Dot(piece) / length2, 0.0f, 1.0f);
            const float2 nearest = points[i] + piece * u;
            const float2 off = point - nearest;
            const float d = off.Dot(off);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = along[i] + (along[i + 1] - along[i]) * u;
            }
        }
        return std::max(best, from);
    }

    RacePlan EvaluatePlan(const PlanBoat& boat, const std::vector<PlanGate>& gates, const std::vector<PlanRival>& rivals,
                          const std::vector<float>& offsets, const std::vector<float>& turns)
    {
        RacePlan plan;
        const std::size_t n = gates.size();
        if (n == 0 || !boat.speedFor) { return plan; }
        plan.offsets.resize(n);
        plan.turns.resize(n);
        const float turnMost = std::clamp(boat.turnMost, 0.0f, kPlanTurnMost);
        for (std::size_t i = 0; i < n; ++i) { plan.turns[i] = std::clamp(i < turns.size() ? turns[i] : 0.0f, -turnMost, turnMost); }
        // The knots: the boat, then where it crosses each gate. Their tangents: the way it moves now (it cannot leave in
        // another), Catmull-Rom's through the gates, the last gate's own way at the end.
        std::vector<float2> knot(n + 1);
        knot[0] = boat.position;
        for (std::size_t i = 0; i < n; ++i)
        {
            const float limit = std::max(gates[i].halfWindow, 0.0f);
            plan.offsets[i] = std::clamp(i < offsets.size() ? offsets[i] : 0.0f, -limit, limit);
            knot[i + 1] = gates[i].center + gates[i].across * plan.offsets[i];
        }
        // Directions, scaled by each piece's own chord: a Catmull-Rom tangent the length of the long piece beside a short one
        // (the boat a few metres before a gate) threw an S-bend into the short one, a bend no boat makes.
        std::vector<float2> direction(n + 1);
        direction[0] = boat.way;
        for (std::size_t i = 1; i < n; ++i)
        {
            const float2 chord = knot[i + 1] - knot[i - 1];
            const float length = Length(chord);
            direction[i] = length > 1e-3f ? chord / length : gates[i - 1].along;
        }
        direction[n] = gates[n - 1].along;
        for (std::size_t i = 1; i <= n; ++i)
        {
            const float heading = Heading(direction[i]) + plan.turns[i - 1];
            direction[i] = float2(std::sin(heading), std::cos(heading));
        }

        plan.points.push_back(knot[0]);
        for (std::size_t k = 0; k < n; ++k)
        {
            const float chord = Length(knot[k + 1] - knot[k]);
            const float2 leave = direction[k] * chord;
            const float2 arrive = direction[k + 1] * chord;
            const int pieces = std::max(2, static_cast<int>(std::ceil(chord / kPlanStep)));
            for (int j = 1; j <= pieces; ++j)
            {
                const float t = static_cast<float>(j) / static_cast<float>(pieces);
                const float t2 = t * t;
                const float t3 = t2 * t;
                const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
                const float h10 = t3 - 2.0f * t2 + t;
                const float h01 = -2.0f * t3 + 3.0f * t2;
                const float h11 = t3 - t2;
                plan.points.push_back(knot[k] * h00 + leave * h10 + knot[k + 1] * h01 + arrive * h11);
            }
        }
        const std::size_t count = plan.points.size();
        plan.along.assign(count, 0.0f);
        for (std::size_t k = 1; k < count; ++k) { plan.along[k] = plan.along[k - 1] + Length(plan.points[k] - plan.points[k - 1]); }

        // How hard it turns: the heading's change from one piece to the next (RaceLine's measure and sign), lightly smoothed.
        std::vector<float> raw(count, 0.0f);
        for (std::size_t k = 1; k + 1 < count; ++k)
        {
            const float2 in = plan.points[k] - plan.points[k - 1];
            const float2 out = plan.points[k + 1] - plan.points[k];
            const float ds = 0.5f * (Length(in) + Length(out));
            raw[k] = ds > 1e-3f ? WrapAngle(Heading(out) - Heading(in)) / ds : 0.0f;
        }
        if (count > 2)
        {
            raw[0] = raw[1];
            raw[count - 1] = raw[count - 2];
        }
        plan.curvature.assign(count, 0.0f);
        for (std::size_t k = 0; k < count; ++k)
        {
            const float before = raw[k > 0 ? k - 1 : k];
            const float after = raw[k + 1 < count ? k + 1 : k];
            plan.curvature[k] = 0.25f * before + 0.5f * raw[k] + 0.25f * after;
        }

        // The speeds: what each place allows, braked back from the slow ones; then from its speed now, as fast as its engine
        // takes it there and as slow as its braking can make it. Over what a place allows it runs wide there: that costs.
        // The first metres are the helm's: the path leaves along the way it moves now, and a degree of that off what the
        // bends ahead want reads there as a turn no boat makes (the toy boat's plans cost 50-100 s a gate before this).
        std::vector<float> most(count);
        for (std::size_t k = 0; k < count; ++k)
        {
            if (plan.along[k] < kSettle)
            {
                most[k] = boat.cap;
                continue;
            }
            const float bend = std::fabs(plan.curvature[k]);
            most[k] = std::min(boat.speedFor(bend), boat.cap);
            const float ds = k > 0 ? plan.along[k] - plan.along[k - 1] : 0.0f;
            if (bend > boat.reach) { plan.penalty += (bend - boat.reach) * ds * kTooTight; }
        }
        // Its engine's share of the braking at each place: none where the helm already has its rudder over for the bend
        // ahead (PlanBoat::turnAt). Counted everywhere, the plan braked for reef's T1 as if the engine went astern while the
        // helm, its rudder over 30 m before the apex, only coasted: 12-13 m/s at gate 3, 15-20 m outside the apex at 6-8 m/s,
        // gate 4 missed in most time trials (2026-11-12).
        std::vector<float> engine(count, 1.0f);
        const auto engineAt = [&](std::size_t k, float v)
        {
            if (!boat.turnAt) { return 1.0f; }
            const float window = std::max(boat.leadMetres, boat.leadSeconds * v);
            float bend = 0.0f;
            for (std::size_t j = k; j < count && plan.along[j] - plan.along[k] <= window; ++j) { bend = std::max(bend, std::fabs(plan.curvature[j])); }
            const float share = bend / std::max(boat.turnAt(v), 1e-4f);
            return 1.0f - std::clamp((share - 0.17f) / 0.23f, 0.0f, 1.0f);
        };
        std::vector<float> wanted = most;
        for (std::size_t k = count - 1; k-- > 0;)
        {
            const float ds = plan.along[k + 1] - plan.along[k];
            const float v = wanted[k + 1];
            engine[k] = engineAt(k, v);
            wanted[k] = std::min(wanted[k], std::sqrt(v * v + 2.0f * (engine[k] * boat.brake + boat.drag * v * v) * ds));
        }
        plan.limit = wanted;
        plan.speed.assign(count, 0.0f);
        plan.time.assign(count, 0.0f);
        plan.speed[0] = std::max(boat.speed, 0.5f);
        const float top = std::max(boat.topSpeed, 1.0f);
        for (std::size_t k = 1; k < count; ++k)
        {
            const float ds = plan.along[k] - plan.along[k - 1];
            const float v = plan.speed[k - 1];
            const float push = boat.accel * std::max(1.0f - (v / top) * (v / top), 0.0f);
            const float up = std::sqrt(v * v + 2.0f * push * ds);
            const float down = std::sqrt(std::max(v * v - 2.0f * (engine[k - 1] * boat.brake + boat.drag * v * v) * ds, 0.0f));
            plan.speed[k] = std::max(std::clamp(wanted[k], down, std::max(up, down)), 0.5f);
            if (plan.speed[k] > most[k] + 0.2f) { plan.penalty += (plan.speed[k] - most[k]) * ds * kOverSpeed; }
            plan.time[k] = plan.time[k - 1] + 2.0f * ds / (v + plan.speed[k]);
        }

        // The others, run on at their speeds: a place it reaches when one of them is there costs, the more the nearer. The
        // gates' buoys stand still and count all along the path: between two gates a path may cut over the next one's
        // inner mark (10 of 13 knocks on a reef lap were buoys, 1-5 m from the hull's middle).
        for (const PlanRival& rival : rivals)
        {
            for (std::size_t k = 1; k < count && (rival.mark || plan.time[k] < kRivalSeconds); ++k)
            {
                const float2 there = rival.position + rival.velocity * plan.time[k];
                const float d = Length(plan.points[k] - there);
                if (d < rival.clearance) { plan.penalty += (rival.mark ? kMark : kRival) * (1.0f - d / rival.clearance) * (plan.along[k] - plan.along[k - 1]); }
            }
        }
        // A crossing at the very edge of a window leaves nothing for the sea or the helm's lag: it costs a little, so the
        // plan goes there only for time it really gains (on a circle the shortest path hugs every inner mark, and a
        // tenth of a second bought there cost a gate in the toy boat's two laps).
        for (std::size_t i = 0; i < n; ++i)
        {
            const float share = gates[i].halfWindow > 0.0f ? plan.offsets[i] / gates[i].halfWindow : 0.0f;
            plan.penalty += kEdge * share * share * share * share;
        }
        plan.seconds = plan.time.back();
        plan.valid = true;
        return plan;
    }

    RacePlan PlanThroughGates(const PlanBoat& boat, const std::vector<PlanGate>& gates, const std::vector<PlanRival>& rivals,
                              const std::vector<float>& warm, const std::vector<float>& warmTurns)
    {
        // The parameters: an offset per gate (its window) and a turn per gate (kPlanTurnMost).
        const std::size_t n = gates.size();
        std::vector<float> x(2 * n, 0.0f);
        std::vector<float> limit(2 * n, std::clamp(boat.turnMost, 0.0f, kPlanTurnMost));
        for (std::size_t i = 0; i < n; ++i)
        {
            limit[i] = std::max(gates[i].halfWindow, 0.0f);
            if (i < warm.size()) { x[i] = warm[i]; }
            if (i < warmTurns.size()) { x[n + i] = warmTurns[i]; }
        }
        const auto evaluate = [&](const std::vector<float>& p)
        {
            return EvaluatePlan(boat, gates, rivals, std::vector<float>(p.begin(), p.begin() + static_cast<std::ptrdiff_t>(n)),
                                std::vector<float>(p.begin() + static_cast<std::ptrdiff_t>(n), p.end()));
        };
        const auto cost = [](const RacePlan& plan) { return plan.seconds + plan.penalty; };
        RacePlan best = evaluate(x);
        if (!best.valid) { return best; }
        {
            RacePlan middle = evaluate(std::vector<float>(2 * n, 0.0f));
            if (cost(middle) < cost(best))
            {
                best = std::move(middle);
                std::fill(x.begin(), x.end(), 0.0f);
            }
        }
        for (std::size_t i = 0; i < n; ++i)
        {
            x[i] = best.offsets[i];
            x[n + i] = best.turns[i];
        }
        float step = 0.5f;
        for (int sweep = 0; sweep < 4; ++sweep)
        {
            for (std::size_t i = 0; i < 2 * n; ++i)
            {
                for (const float direction : { -1.0f, 1.0f })
                {
                    std::vector<float> y = x;
                    y[i] = std::clamp(x[i] + direction * step * limit[i], -limit[i], limit[i]);
                    if (y[i] == x[i]) { continue; }
                    RacePlan trial = evaluate(y);
                    if (cost(trial) < cost(best))
                    {
                        best = std::move(trial);
                        x = y;
                    }
                }
            }
            step *= 0.5f;
        }
        return best;
    }
}
