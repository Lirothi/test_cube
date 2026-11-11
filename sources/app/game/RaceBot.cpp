#include "app/game/RaceBot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

#include "core/logging/Log.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/renderables/RenderableObject.h"

namespace game
{
    namespace
    {
        using Math::float2;

        constexpr float kPi = 3.14159265f;

        float Heading(const float2& v) { return std::atan2(v.x, v.y); }

        float WrapAngle(float a)
        {
            while (a > kPi) { a -= 2.0f * kPi; }
            while (a < -kPi) { a += 2.0f * kPi; }
            return a;
        }

        const char* StateName(RaceBot::State state)
        {
            switch (state)
            {
            case RaceBot::State::Waiting: return "waiting";
            case RaceBot::State::Racing: return "racing";
            case RaceBot::State::Reversing: return "reversing";
            default: return "idle";
            }
        }

        // "models/police_boat/police_boat.mesh.json" -> "police_boat"
        std::string VesselName(const RenderableObject* object)
        {
            if (!object) { return {}; }
            std::string name = object->GetGeometryPath();
            const std::size_t slash = name.find_last_of("/\\");
            if (slash != std::string::npos) { name = name.substr(slash + 1); }
            const std::size_t dot = name.find('.');
            if (dot != std::string::npos) { name = name.substr(0, dot); }
            return name;
        }

        // The rate (rad/s) a boat turns at in a steady turn, full ahead and hard over, at a speed -- from its own numbers
        // (PhysicsVessel). The rudder pushes the stern out with F = rudderForce x (speed + wash)^2 x sin(rudderDeg), some
        // 0.65 of the half length aft of the centre of mass. The hull then slides, and the water's push on its side is
        // what turns its path: m v w + F, acting swayLever aft -- against the turn. With the yaw drag:
        //     yawDrag w^2 + (yawLinear + m v swayLever) w = F (lever - swayLever).
        // Measured (2026-11-10, hard over at full throttle): small_yacht 13.1 deg/s at 12.8 m/s (this gives 12.8),
        // police_boat 14.7 at 14.0, fast_boat_13 14.8 at 14.1 -- a circle of 55 m for all three. Without the keel's term
        // it said 26-32 m. The circle hardly depends on the speed: a boat's turn is its own.
        float SteadyTurnRate(const physics::VesselSettings& boat, float halfLength, float speed)
        {
            const float v = std::max(speed, 0.5f);
            const float flow = v + boat.wash;
            const float push = boat.rudderForce * flow * flow * std::sin(boat.rudderDeg * 0.0174532925f);
            const float lever = std::max(0.65f * halfLength - boat.swayLever, 0.2f);
            const float linear = boat.yawLinear + boat.mass * v * boat.swayLever;
            const float quadratic = std::max(boat.yawDrag, 1.0f);
            return (-linear + std::sqrt(linear * linear + 4.0f * quadratic * push * lever)) / (2.0f * quadratic);
        }

        void FormatLap(float seconds, wchar_t* out, std::size_t size)
        {
            if (!(seconds > 0.0f)) { std::swprintf(out, size, L"-:--.-"); return; }
            const int minutes = static_cast<int>(seconds / 60.0f);
            std::swprintf(out, size, L"%d:%04.1f", minutes, seconds - 60.0f * static_cast<float>(minutes));
        }
    }

    BotSettings& BotSettingsRef()
    {
        static BotSettings settings;
        return settings;
    }

    const std::vector<BotKey>& BotKeys()
    {
        using S = BotSettings;
        static const std::vector<BotKey> keys = {
            { "enabled", "Bots drive", "Off: a boat that carries a bot floats with its engine off.", &S::enabled, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "pace", "Pace", "The share of its own best speed the bot runs at. 1 = as fast as it can, 0.7 = a bot one can beat.", &S::pace, 0.005f, 0.2f, 1.2f, "%.2f" },
            { "lateralAccel", "Turn grip (m/s2)", "How fast it takes a bend: its speed there is sqrt(this x the bend's radius). Higher = faster, until the boat runs wide of the gates: flat out and hard over the boats make 2.9-3.6.", &S::lateralAccel, 0.01f, 0.5f, 8.0f, "%.2f" },
            { "brakeAccel", "Braking (m/s2)", "How late it slows down for a turn: higher = later. Too high and it arrives too fast.", &S::brakeAccel, 0.01f, 0.2f, 6.0f, "%.2f" },
            { "lookAhead", "Look ahead (s)", "How far along the line the helm aims, in seconds of its speed. Short = hugs the line and weaves, long = cuts the corners.", &S::lookAhead, 0.005f, 0.3f, 4.0f, "%.2f" },
            { "lookAheadMin", "Look ahead, least (m)", "The same at low speed.", &S::lookAheadMin, 0.05f, 3.0f, 60.0f, "%.1f" },
            { "steerGain", "Steering", "How eagerly the rudder goes over for the turn to the aim: 1 = just what the turn needs. Higher = sharper, until it weaves.", &S::steerGain, 0.01f, 0.2f, 8.0f, "%.2f" },
            { "yawDamping", "Steering damping", "Rudder taken back per rad/s the boat already turns. Higher = calmer, slower into a turn.", &S::yawDamping, 0.005f, 0.0f, 3.0f, "%.2f" },
            { "margin", "Room at a mark (m)", "How far from a buoy the hull's side stays where the line crosses a gate.", &S::margin, 0.02f, 0.0f, 12.0f, "%.1f" },
            { "startDelay", "Start delay (s)", "After Play, before the engine starts.", &S::startDelay, 0.05f, 0.0f, 60.0f, "%.1f" },
            { "debug", "Draw the course", "The gates (port red, starboard green) and the bot's line: green where it runs flat out, red where it slows.", &S::debug, 1.0f, 0.0f, 1.0f, "%.0f" },
        };
        return keys;
    }

    const BotKey* FindBotKey(std::string_view key)
    {
        for (const BotKey& entry : BotKeys())
        {
            if (key == entry.key) { return &entry; }
        }
        return nullptr;
    }

    void RaceBot::Reset()
    {
        line_.Clear();
        status_ = Status{};
        lineMargin_ = -1.0f;
        lineKey_[0] = -1.0f;
        located_ = false;
        joined_ = false;
        joinSeconds_ = 0.0f;
        gateIndex_ = 0;
        gateSideValid_ = false;
        started_ = false;
        elapsed_ = 0.0f;
        headingValid_ = false;
        yawRate_ = 0.0f;
        stuckSeconds_ = 0.0f;
        reverseSeconds_ = 0.0f;
        airborne_ = 0.0f;
    }

    void RaceBot::SetSoundings(float ahead, float port, float starboard)
    {
        soundings_[0] = ahead;
        soundings_[1] = port;
        soundings_[2] = starboard;
    }

    void RaceBot::Relocate()
    {
        located_ = false;
        joined_ = false;
        joinSeconds_ = 0.0f;
        gateSideValid_ = false;
        stuckSeconds_ = 0.0f;
        reverseSeconds_ = 0.0f;
    }

    void RaceBot::EnsureLine(const RaceTrack& track, const physics::VesselHull& hull)
    {
        const BotSettings& settings = BotSettingsRef();
        const float margin = hull.halfBeam + settings.margin;
        if (!line_.Valid() || margin != lineMargin_)
        {
            line_.Build(track.Gates(), margin);
            lineMargin_ = margin;
            lineKey_[0] = -1.0f;
            located_ = false;
            gateSideValid_ = false;
        }
        // Flat out the thrust meets the hull's drag on the plane: thrust = 0.77 surgeDrag v^2 (PhysicsVessel: planingLift
        // takes that share of the hull out of the water).
        const physics::VesselSettings& boat = *hull.settings;
        topSpeed_ = std::sqrt(std::max(boat.thrust, 1.0f) / std::max(0.77f * boat.surgeDrag, 1.0f));
        halfLength_ = hull.halfLength;
        const float key[3] = { settings.lateralAccel, settings.brakeAccel, topSpeed_ };
        if (key[0] == lineKey_[0] && key[1] == lineKey_[1] && key[2] == lineKey_[2]) { return; }
        for (int i = 0; i < 3; ++i) { lineKey_[i] = key[i]; }
        const float top = topSpeed_;
        // A bend is taken at the speed that asks lateralAccel across the boat's way. Never at a crawl: slower does not
        // make a boat's circle tighter -- its turn is its own, 55 m for the three here, and with the throttle eased the
        // rudder has less wash and it is 60-70 m (measured) -- it only gives the helm time to bring the bow round.
        const float lateral = std::max(settings.lateralAccel, 0.1f);
        line_.Profile([top, lateral](float curvature) { return std::clamp(std::sqrt(lateral / std::max(curvature, 1e-6f)), 0.5f * top, top); }, settings.brakeAccel);
        status_.tightestRadius = top / std::max(SteadyTurnRate(boat, halfLength_, top), 1e-4f);
        LOG_INFO(logging::LogCategory::App, "race: a line of {:.0f} m, the boat's top speed {:.1f} m/s, its tightest turn there {:.0f} m, the line's slowest place {:.1f} m/s",
                 line_.Length(), top, status_.tightestRadius, line_.SlowestSpeed());
    }

    physics::VesselInput RaceBot::Tick(const RaceTrack& track, const physics::VesselHull& hull, float deltaTime, bool drive)
    {
        const BotSettings& settings = BotSettingsRef();
        status_.order = physics::VesselInput{};
        status_.speed = hull.speed;
        if (!track.Valid() || !hull.settings)
        {
            status_.state = State::Idle;
            return status_.order;
        }
        EnsureLine(track, hull);
        if (!line_.Valid())
        {
            status_.state = State::Idle;
            return status_.order;
        }

        const float2 position(hull.position.x, hull.position.z);
        float2 forward = float2(hull.forward.x, hull.forward.z).Normalized();
        if (forward.Dot(forward) < 0.5f) { forward = float2(0.0f, 1.0f); }
        const float heading = Heading(forward);
        if (headingValid_ && deltaTime > 1e-4f)
        {
            const float rate = WrapAngle(heading - heading_) / deltaTime;
            yawRate_ += (rate - yawRate_) * (1.0f - std::exp(-deltaTime / 0.12f));
        }
        heading_ = heading;
        headingValid_ = true;
        const float speed = hull.speed;
        elapsed_ += deltaTime;

        // ---- where on the line ----
        const std::vector<RaceGate>& gates = track.Gates();
        const float length = line_.Length();
        if (!located_)
        {
            status_.station = line_.Nearest(position);
            located_ = true;
        }
        else
        {
            status_.station = line_.Nearest(position, status_.station, 20.0f, std::max(40.0f, std::fabs(speed) * 3.0f));
        }
        const RaceLine::Sample here = line_.At(status_.station);
        status_.crossTrack = (position - here.point).Dot(float2(here.tangent.y, -here.tangent.x));

        // ---- the gates and the laps ----
        if (started_) { status_.lapSeconds += deltaTime; }
        if (!joined_)
        {
            // Not on the course yet (from its mooring, after the helm was somebody else's): the gate ahead is whichever
            // its place on the line says, and none is passed or missed.
            // On it = near it AND running along it, for a second and a half: out of its mooring it first crosses the line
            // square on, and a light boat crossed it aligned and ran on 22 m wide of the next gate.
            joinSeconds_ = std::fabs(status_.crossTrack) < 12.0f && forward.Dot(here.tangent) > 0.8f ? joinSeconds_ + deltaTime : 0.0f;
            joined_ = joinSeconds_ > 1.5f;
            gateIndex_ = 0;
            for (std::size_t i = 0; i < gates.size(); ++i)
            {
                if (line_.GateStation(i) > status_.station) { gateIndex_ = i; break; }
            }
            gateSideValid_ = false;
            status_.nextGate = gates[gateIndex_].number;
        }
        else
        {
            const RaceGate& gate = gates[gateIndex_];
            const float2 off = position - gate.center;
            const float side = off.Dot(gate.along);
            const float lateral = off.Dot(gate.across);
            bool gone = false;
            bool between = false;
            if (gateSideValid_ && gateSide_ < 0.0f && side >= 0.0f && std::fabs(lateral) < gate.halfWidth + 40.0f)
            {
                gone = true;
                between = std::fabs(lateral) <= gate.halfWidth;
            }
            else
            {
                // Far outside its marks: the place on the line says the gate is behind.
                float past = std::fmod(status_.station - line_.GateStation(gateIndex_), length);
                if (past < -0.5f * length) { past += length; }
                if (past > 0.5f * length) { past -= length; }
                gone = past > 30.0f && past < 0.4f * length;
            }
            gateSide_ = side;
            gateSideValid_ = true;
            if (gone)
            {
                if (between) { ++status_.gatesPassed; }
                else
                {
                    ++status_.gatesMissed;
                    LOG_INFO(logging::LogCategory::App, "race: gate {} missed, {:.1f} m off its middle (half width {:.1f} m)", gate.number, lateral, gate.halfWidth);
                }
                if (gateIndex_ == 0)
                {
                    if (started_)
                    {
                        ++status_.lap;
                        status_.lastLap = status_.lapSeconds;
                        status_.bestLap = status_.bestLap > 0.0f ? std::min(status_.bestLap, status_.lastLap) : status_.lastLap;
                        LOG_INFO(logging::LogCategory::App, "race: lap {} in {:.1f} s (best {:.1f}), {} gates passed, {} missed", status_.lap, status_.lastLap,
                                 status_.bestLap, status_.gatesPassed, status_.gatesMissed);
                    }
                    started_ = true;
                    status_.lapSeconds = 0.0f;
                }
                gateIndex_ = (gateIndex_ + 1) % gates.size();
                gateSideValid_ = false;
            }
            status_.nextGate = gates[gateIndex_].number;
        }

        // ---- the helm ----
        // The further off its line, the further ahead it aims: it comes back at a shallow angle instead of across the
        // line and out the other side (the start, from its mooring, used to weave through the first six gates). As much
        // further as it is off, 80 m at most: with half of that the yacht came back at 26 kn across the line, brushed
        // a gate's buoy, was thrown inside the course and ran aground on the beach.
        const float reach = std::max(settings.lookAheadMin, std::fabs(speed) * settings.lookAhead) + std::min(std::fabs(status_.crossTrack), 80.0f);
        status_.aim = line_.At(status_.station + reach).point;
        // Off the BOW, not off the way the boat moves. A hull in a turn slides, bow inside its path, so by the bow it runs
        // some 4 m wide of its line round a long bend; steered by its velocity instead (tried, 2026-11-10) it weaves from
        // gate to gate with the rudder banging from side to side -- the path answers the rudder a second after the bow does.
        const float error = WrapAngle(Heading(status_.aim - position) - heading);
        float target = line_.At(status_.station + std::max(speed, 0.0f) * 0.5f).speed * settings.pace;
        // Coming onto the course (from its mooring, the bow often pointing away from it): at the speed its own circle
        // asks lateralAccel at, so the swing out and back is as tight as the boat makes it -- flat out a light fast hull
        // settled on the line and ran 37 m wide of the next gate; at half speed the yacht took 50 s to come back.
        if (!joined_) { target = std::min(target, std::sqrt(std::max(settings.lateralAccel, 0.1f) * std::max(status_.tightestRadius, 10.0f))); }
        // The sea: a hull that spends its time in the air is going too fast for it. Out of the water more than a tenth of
        // the last seconds: the pace comes down, slowly; under a twentieth: it comes back.
        // NOT slower for the sea, and not at a crawl when the aim lies far off the bow. Both were tried (2026-11-10: the
        // pace taken down while the hull spent over 12 % of its time out of the water; 4 m/s with the aim 80 degrees
        // off) and every run that went wrong went wrong SLOWED: these boats steer by the water past the rudder,
        // (speed + wash)^2, and at 18 kn on 0.4 throttle the rudder has 44 % of its push -- the yacht drifted 10 m inside
        // its line with the rudder over and brushed a gate's buoy. The share of the time out of the water is still
        // told (flat out round wind_test's course: police_boat 2-3 %, small_yacht 9-12 %, fast_boat_13 11-23 %).
        if (deltaTime > 0.0f) { airborne_ += ((hull.immersion < 0.1f ? 1.0f : 0.0f) - airborne_) * (1.0f - std::exp(-deltaTime / 4.0f)); }
        status_.airborne = airborne_;
        status_.target = target;
        if (!drive)
        {
            status_.state = State::Idle;
            return status_.order;
        }
        if (elapsed_ < settings.startDelay)
        {
            status_.state = State::Waiting;
            return status_.order;
        }

        // The turn that brings the bow onto the aim (pure pursuit: an arc through the boat and the aim, tangent to the
        // bow), as a share of the tightest the boat makes at this speed.
        physics::VesselInput order;
        const float arc = 2.0f * (std::fabs(error) < 1.5707963f ? std::sin(error) : (error > 0.0f ? 1.0f : -1.0f)) / std::max(reach, 1.0f);
        const float pace = std::max(std::fabs(speed), 2.0f);
        const float tightest = SteadyTurnRate(*hull.settings, halfLength_, pace) / pace;
        order.rudder = settings.steerGain * arc / std::max(tightest, 1e-4f) - settings.yawDamping * yawRate_;
        // The marks are seen. One ahead that the bow would brush -- the line keeps clear of them, the boat is not always
        // on its line: coming onto the course from outside it, thrown by a sea -- is steered round, on the side its
        // gate's middle lies (dead ahead: away from the mark). A buoy is a kinematic body: hitting one stops the boat.
        {
            const float2 starboard(forward.y, -forward.x);
            const float look = std::max(25.0f, 2.5f * std::fabs(speed));
            const float clear = hull.halfBeam + settings.margin;
            for (int ahead = 0; ahead < 2; ++ahead)
            {
                const RaceGate& gate = gates[(gateIndex_ + static_cast<std::size_t>(ahead)) % gates.size()];
                for (const float2& mark : { gate.a, gate.b })
                {
                    const float2 to = mark - position;
                    const float along = to.Dot(forward);
                    const float side = to.Dot(starboard);
                    if (along <= 0.0f || along >= look || std::fabs(side) >= clear) { continue; }
                    // On its gate's side when the boat can still get there: across by (clear + how far the mark lies
                    // on that side of the bow's way) in the time to the mark, at lateralAccel. Else on whichever side is
                    // nearer -- a gate missed, not a buoy hit. "Always toward the middle" asked a boat coming onto the
                    // course from outside, already hard over, for a tighter turn than it has (the yacht met gate 4's
                    // port buoy head on); "always away from the mark" pushed one that was a little wide of a gate
                    // round the outside of its mark.
                    const float middle = (gate.center - mark).Dot(starboard) > 0.0f ? 1.0f : -1.0f;
                    const float across = std::max(side * middle + clear, 0.0f);
                    const float seconds = along / std::max(std::fabs(speed), 1.0f);
                    const bool reachable = across <= 0.5f * settings.lateralAccel * seconds * seconds;
                    const float way = reachable ? middle : (side > 0.0f ? -1.0f : 1.0f);
                    order.rudder += way * 2.5f * (1.0f - std::fabs(side) / clear);
                }
            }
        }
        order.rudder = std::clamp(order.rudder, -1.0f, 1.0f);
        // The lead line. Too little water where the bow will be (or a hull, a beach: a depth of nothing): hard over toward
        // the deeper side, the engine on -- it steers by its wash; no water on either side: astern. The line never asks
        // for this; a boat knocked off it does (the yacht, off a buoy, sat on the east beach till Stop).
        const float need = std::max(hull.settings->draft, 0.2f) * 1.5f + 0.6f;
        status_.depthAhead = soundings_[0];
        status_.shoal = soundings_[0] < need;
        if (status_.shoal && reverseSeconds_ <= 0.0f)
        {
            if (std::max(soundings_[1], soundings_[2]) >= need) { order.rudder = soundings_[2] > soundings_[1] ? 1.0f : -1.0f; }
            else
            {
                reverseSeconds_ = 3.0f;
                reverseRudder_ = error > 0.0f ? -1.0f : 1.0f;
            }
        }
        if (target >= 0.97f * topSpeed_) { order.throttle = 1.0f; }
        else
        {
            const float hold = (target / std::max(topSpeed_, 1.0f)) * (target / std::max(topSpeed_, 1.0f));
            order.throttle = std::clamp(hold + 0.5f * (target - speed), -0.7f, 1.0f);
        }
        // The rudder hard over and still not enough: the engine's wash past it is what turns the boat (measured: a circle
        // of 55 m full ahead, 60-69 m at 0.45 throttle), so easing the throttle there only runs it wider.
        // Only at the line's own speed: a boat that came in faster than the bend allows still slows first.
        if (std::fabs(order.rudder) > 0.92f && speed > 0.0f && (speed < target + 1.0f || status_.shoal)) { order.throttle = 1.0f; }
        // Aground or against something: astern for a while, the stern swung so the bow comes round to the aim.
        if (reverseSeconds_ > 0.0f)
        {
            reverseSeconds_ -= deltaTime;
            order.throttle = -1.0f;
            order.rudder = reverseRudder_;
            status_.state = State::Reversing;
        }
        else
        {
            status_.state = State::Racing;
            const bool held = order.throttle > 0.4f && std::fabs(speed) < 0.5f && elapsed_ > settings.startDelay + 4.0f;
            stuckSeconds_ = held ? stuckSeconds_ + deltaTime : 0.0f;
            if (stuckSeconds_ > 3.0f)
            {
                stuckSeconds_ = 0.0f;
                reverseSeconds_ = 4.0f;
                reverseRudder_ = error > 0.0f ? -1.0f : 1.0f;
                LOG_INFO(logging::LogCategory::App, "race: the bot is held at {:.1f} {:.1f}, going astern", position.x, position.y);
            }
        }
        status_.order = order;
        return order;
    }

    void Race::Begin(const RaceCourse& course)
    {
        track_.Clear();
        bots_.clear();
        helmBot_.Reset();
        helmVessel_ = nullptr;
        helmOrder_ = physics::VesselInput{};
        autopilot_ = false;
        helmDriven_ = false;
        active_ = true;
        report_.clear();
        if (!course.marks.empty())
        {
            const bool ok = track_.Build(course, report_);
            if (ok) { LOG_INFO(logging::LogCategory::App, "race: {}, {} boat(s) with a bot", report_, course.bots.size()); }
            else { LOG_WARNING(logging::LogCategory::App, "race: no course -- {}", report_); }
        }
        for (const Math::float3& where : course.bots)
        {
            Entry entry;
            entry.where = where;
            bots_.push_back(std::move(entry));
        }
    }

    void Race::End(physics::World& world)
    {
        for (Entry& entry : bots_)
        {
            if (entry.vessel && entry.live) { world.SetPilot(entry.vessel, nullptr); }
        }
        Forget();
    }

    void Race::Forget()
    {
        bots_.clear();
        track_.Clear();
        helmBot_.Reset();
        helmVessel_ = nullptr;
        autopilot_ = false;
        helmDriven_ = false;
        active_ = false;
    }

    bool Race::ToggleAutopilot()
    {
        if (!helmDriven_ || !track_.Valid()) { return false; }
        autopilot_ = !autopilot_;
        if (autopilot_) { helmBot_.Relocate(); }
        return autopilot_;
    }

    void Race::Sound(physics::World& world, RaceBot& bot, const physics::VesselHull& hull) const
    {
        // Where the bow will be in a couple of seconds, and 40 degrees to either side of that. Straight down from over
        // the water: the first thing a ray meets -- the sea floor, a beach, another boat's deck -- is what stands there.
        const Math::float2 forward = Math::float2(hull.forward.x, hull.forward.z).Normalized();
        const float reach = hull.halfLength + std::max(16.0f, 2.2f * std::fabs(hull.speed));
        float depth[3];
        const float bearings[3] = { 0.0f, -0.7f, 0.7f };
        for (int i = 0; i < 3; ++i)
        {
            const float c = std::cos(bearings[i]);
            const float s = std::sin(bearings[i]);
            // A turn to starboard by the bearing: (x, z) -> (x cos + z sin, z cos - x sin).
            const Math::float2 way(forward.x * c + forward.y * s, forward.y * c - forward.x * s);
            const Math::float3 from(hull.position.x + way.x * reach, hull.waterHeight + 6.0f, hull.position.z + way.y * reach);
            physics::RayHit hit;
            const bool met = world.RayCast(from, Math::float3(0.0f, -1.0f, 0.0f), 30.0f, hit) && hit.object != hull.object;
            depth[i] = met ? hull.waterHeight - hit.point.y : 1.0e3f;
        }
        bot.SetSoundings(depth[0], depth[1], depth[2]);
    }

    void Race::Tick(physics::World& world, float deltaTime)
    {
        if (!active_) { return; }
        std::vector<physics::VesselHull>& hulls = hulls_;
        world.CollectHulls(hulls);
        const BotSettings& settings = BotSettingsRef();
        soundSeconds_ += deltaTime;
        const bool sound = track_.Valid() && soundSeconds_ >= 0.25f;
        if (sound) { soundSeconds_ = 0.0f; }

        // The boat the player holds: its gates and laps always, its helm on autopilot.
        helmDriven_ = false;
        for (const physics::VesselHull& hull : hulls)
        {
            if (!hull.atHelm) { continue; }
            if (helmVessel_ != hull.object)
            {
                helmBot_.Reset();
                helmVessel_ = hull.object;
                autopilot_ = false;
            }
            helmDriven_ = true;
            helmWaterHeight_ = hull.waterHeight;
            if (sound && autopilot_) { Sound(world, helmBot_, hull); }
            helmOrder_ = helmBot_.Tick(track_, hull, deltaTime, autopilot_);
        }
        if (!helmDriven_)
        {
            helmVessel_ = nullptr;
            autopilot_ = false;
            helmOrder_ = physics::VesselInput{};
        }

        for (Entry& entry : bots_)
        {
            if (!entry.vessel)
            {
                entry.vessel = world.FindVessel(entry.where, 2.0f);
                if (!entry.vessel) { continue; }
                entry.name = VesselName(entry.vessel);
                LOG_INFO(logging::LogCategory::App, "race: a bot at the helm of '{}'", entry.name);
            }
            entry.live = false;
            for (const physics::VesselHull& hull : hulls)
            {
                if (hull.object != entry.vessel) { continue; }
                entry.live = true;
                entry.waterHeight = hull.waterHeight;
                entry.position = hull.position;
                if (hull.atHelm) // the player took it: the bot stands down until the helm is free again
                {
                    if (!entry.yielded)
                    {
                        world.SetPilot(entry.vessel, nullptr);
                        entry.yielded = true;
                    }
                    break;
                }
                if (entry.yielded)
                {
                    entry.yielded = false;
                    entry.bot.Relocate();
                }
                if (settings.enabled < 0.5f)
                {
                    world.SetPilot(entry.vessel, nullptr);
                    break;
                }
                if (sound) { Sound(world, entry.bot, hull); }
                const physics::VesselInput order = entry.bot.Tick(track_, hull, deltaTime, true);
                world.SetPilot(entry.vessel, &order);
                break;
            }
            if (!entry.live) { entry.vessel = nullptr; } // gone with its world: looked for again
        }
    }

    std::wstring Race::HudLine() const
    {
        if (!active_ || !track_.Valid()) { return {}; }
        wchar_t line[320];
        wchar_t last[16];
        wchar_t best[16];
        wchar_t now[16];
        const int gates = static_cast<int>(track_.Gates().size());
        if (helmDriven_)
        {
            const RaceBot::Status& s = helmBot_.GetStatus();
            FormatLap(s.lapSeconds, now, 16);
            FormatLap(s.lastLap, last, 16);
            FormatLap(s.bestLap, best, 16);
            std::swprintf(line, sizeof(line) / sizeof(line[0]), L"RACE: lap %d   gate %d of %d   %ls   last %ls   best %ls   missed %d   %ls", s.lap + 1, s.nextGate, gates,
                          now, last, best, s.gatesMissed, autopilot_ ? L"AUTOPILOT, B takes the helm back" : L"B autopilot");
            return line;
        }
        for (const Entry& entry : bots_)
        {
            if (!entry.live || entry.yielded) { continue; }
            const RaceBot::Status& s = entry.bot.GetStatus();
            FormatLap(s.lapSeconds, now, 16);
            FormatLap(s.lastLap, last, 16);
            FormatLap(s.bestLap, best, 16);
            std::swprintf(line, sizeof(line) / sizeof(line[0]), L"BOT %hs: %hs, %.1f kn   lap %d   gate %d of %d   %ls   last %ls   best %ls   missed %d", entry.name.c_str(),
                          StateName(s.state), s.speed * 1.94384f, s.lap + 1, s.nextGate, gates, now, last, best, s.gatesMissed);
            return line;
        }
        return {};
    }

    std::string Race::ReadoutJson() const
    {
        std::string out = ",\"race\":{";
        char item[640];
        std::snprintf(item, sizeof(item), "\"active\":%s,\"gates\":%zu,\"autopilot\":%s,\"atHelm\":%s", active_ ? "true" : "false", track_.Gates().size(),
                      autopilot_ ? "true" : "false", helmDriven_ ? "true" : "false");
        out += item;
        const auto status = [&](const char* name, const RaceBot& bot, const Math::float3& position, bool comma)
        {
            const RaceBot::Status& s = bot.GetStatus();
            std::snprintf(item, sizeof(item),
                          "%s{\"vessel\":\"%s\",\"state\":\"%s\",\"lap\":%d,\"nextGate\":%d,\"passed\":%d,\"missed\":%d,\"station\":%.1f,\"length\":%.1f,\"speed\":%.2f,"
                          "\"target\":%.2f,\"slowest\":%.2f,\"tightestRadius\":%.1f,\"airborne\":%.3f,\"depthAhead\":%.1f,\"shoal\":%s,\"crossTrack\":%.2f,\"lapSeconds\":%.2f,\"lastLap\":%.2f,\"bestLap\":%.2f,\"throttle\":%.3f,\"rudder\":%.3f,"
                          "\"position\":[%.2f,%.2f,%.2f]}",
                          comma ? "," : "", name, StateName(s.state), s.lap, s.nextGate, s.gatesPassed, s.gatesMissed, s.station, bot.Line().Length(), s.speed, s.target,
                          bot.Line().SlowestSpeed(), s.tightestRadius, s.airborne, std::min(s.depthAhead, 999.0f), s.shoal ? "true" : "false", s.crossTrack, s.lapSeconds, s.lastLap, s.bestLap, s.order.throttle, s.order.rudder, position.x, position.y, position.z);
            out += item;
        };
        if (helmDriven_)
        {
            out += ",\"helm\":";
            status(VesselName(helmVessel_).c_str(), helmBot_, Math::float3(0.0f, 0.0f, 0.0f), false);
        }
        out += ",\"bots\":[";
        bool comma = false;
        for (const Entry& entry : bots_)
        {
            if (!entry.live) { continue; }
            status(entry.name.c_str(), entry.bot, entry.position, comma);
            comma = true;
        }
        out += "]}";
        return out;
    }

    void Race::DebugDraw(DebugDrawSystem& draw) const
    {
        if (!active_ || !track_.Valid() || BotSettingsRef().debug < 0.5f) { return; }
        const RaceBot* bot = helmDriven_ ? &helmBot_ : nullptr;
        float water = helmWaterHeight_;
        for (const Entry& entry : bots_)
        {
            if (bot) { break; }
            if (entry.live) { bot = &entry.bot; water = entry.waterHeight; }
        }
        const float y = water + 0.6f;
        const Math::float4 port(1.0f, 0.15f, 0.1f, 1.0f);
        const Math::float4 starboard(0.1f, 1.0f, 0.2f, 1.0f);
        for (const RaceGate& gate : track_.Gates())
        {
            const Math::float3 a(gate.a.x, y, gate.a.y);
            const Math::float3 b(gate.b.x, y, gate.b.y);
            draw.AddLine(a, b, Math::float4(1.0f, 1.0f, 1.0f, 0.8f));
            draw.AddLine(a, Math::float3(a.x, y + 5.0f, a.z), port);
            draw.AddLine(b, Math::float3(b.x, y + 5.0f, b.z), starboard);
        }
        if (!bot || !bot->Line().Valid()) { return; }
        const RaceLine& line = bot->Line();
        const std::vector<Math::float2>& points = line.Points();
        float top = 1.0f;
        for (std::size_t i = 0; i < points.size(); ++i) { top = std::max(top, line.SpeedAt(i)); }
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            const Math::float2& a = points[i];
            const Math::float2& b = points[(i + 1) % points.size()];
            const float fast = std::clamp(line.SpeedAt(i) / top, 0.0f, 1.0f);
            draw.AddLine(Math::float3(a.x, y, a.y), Math::float3(b.x, y, b.y), Math::float4(1.0f - fast, fast, 0.1f, 1.0f));
        }
        const Math::float2 aim = bot->GetStatus().aim;
        draw.AddLine(Math::float3(aim.x, y, aim.y), Math::float3(aim.x, y + 3.0f, aim.y), Math::float4(1.0f, 1.0f, 0.2f, 1.0f));
    }
}
