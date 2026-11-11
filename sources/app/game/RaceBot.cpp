#include "app/game/RaceBot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

#include "core/logging/Log.h"
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

    }

    const char* RaceBotStateName(unsigned char state)
    {
        switch (static_cast<RaceBot::State>(state))
        {
        case RaceBot::State::Racing: return "racing";
        case RaceBot::State::Reversing: return "reversing";
        default: return "idle";
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
            { "enabled", "Bots drive", "Off: a boat that carries a bot floats with its engine off, in a race too.", &S::enabled, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "pace", "Pace", "The share of its own best speed the bot runs at. 1 = as fast as it can, 0.7 = a bot one can beat.", &S::pace, 0.005f, 0.2f, 1.2f, "%.2f" },
            { "easyPace", "Easy bot's pace", "The share of the line's speed an EASY bot runs at (a race gives each bot a level: race.difficulty).", &S::easyPace, 0.005f, 0.3f, 1.0f, "%.2f" },
            { "mediumPace", "Medium bot's pace", "The same for a MEDIUM bot. A hard one runs all of it.", &S::mediumPace, 0.005f, 0.3f, 1.0f, "%.2f" },
            { "grip", "Turn share", "How much of its boat's own tightest turn (the boat's card: its circle) a bend may ask. Higher = faster through the bends, until the boat runs wide of the gates; a boat that turns better takes them faster by itself.", &S::grip, 0.005f, 0.3f, 1.3f, "%.2f" },
            { "braking", "Braking share", "How late it slows down for a bend, as a share of what its boat loses with the engine off at its top speed: higher = later. Too high and it arrives too fast.", &S::braking, 0.005f, 0.05f, 1.0f, "%.2f" },
            { "lookAhead", "Look ahead (s)", "How far along the line the helm aims, in seconds of its speed. Short = hugs the line and weaves, long = cuts the corners.", &S::lookAhead, 0.005f, 0.3f, 4.0f, "%.2f" },
            { "lookAheadCircle", "Look ahead, least (of its circle)", "At least this share of the radius of the boat's own tightest circle: a boat makes the same path through a change of turn at any speed, a length of its own, so one that turns wider starts its turn earlier. Lower = hugs the line and weaves, higher = cuts the corners.", &S::lookAheadCircle, 0.005f, 0.1f, 1.5f, "%.2f" },
            { "lookAheadMin", "Look ahead, least (m)", "And never nearer than this, whatever the boat.", &S::lookAheadMin, 0.05f, 3.0f, 60.0f, "%.1f" },
            { "steerGain", "Steering", "How eagerly the rudder goes over for the turn to the aim: 1 = just what the turn needs. Higher = sharper, until it weaves.", &S::steerGain, 0.01f, 0.2f, 8.0f, "%.2f" },
            { "yawDamping", "Steering damping", "Rudder taken back per rad/s the boat already turns. Higher = calmer, slower into a turn.", &S::yawDamping, 0.005f, 0.0f, 3.0f, "%.2f" },
            { "slip", "Slide allowance", "A hull in a turn slides, bow inside its way. 1 = the bow is pointed inside the aim by the angle its boat's own numbers give for the bend, so its WAY goes to the aim; 0 = the bow at the aim itself: the boat runs outside its line in every bend, a heavy one by metres.", &S::slip, 0.01f, 0.0f, 2.0f, "%.2f" },
            { "avoidBoats", "Steer round boats", "A boat ahead that the bow would reach within two seconds is steered round, on the side it does not lie on. Off: the bots hold their line whoever is on it.", &S::avoidBoats, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "margin", "Room at a mark (m)", "How far from a buoy the hull's side stays where the line crosses a gate.", &S::margin, 0.02f, 0.0f, 12.0f, "%.1f" },
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

    const char* BotDifficultyName(BotDifficulty difficulty)
    {
        switch (difficulty)
        {
        case BotDifficulty::Easy: return "easy";
        case BotDifficulty::Medium: return "medium";
        default: return "hard";
        }
    }

    void RaceBot::Reset()
    {
        line_.Clear();
        status_ = Status{};
        lineMargin_ = -1.0f;
        std::fill(lineKey_, lineKey_ + kLineKeys, -1.0f);
        located_ = false;
        onGrid_ = false;
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

    void RaceBot::Start()
    {
        Reset();
        onGrid_ = true;
    }

    void RaceBot::PutBack(float station)
    {
        status_.station = station;
        status_.crossTrack = 0.0f;
        located_ = true;
        onGrid_ = false;
        joined_ = true;
        joinSeconds_ = 0.0f;
        gateSideValid_ = false;
        stuckSeconds_ = 0.0f;
        reverseSeconds_ = 0.0f;
        yawRate_ = 0.0f;
        headingValid_ = false;
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
            std::fill(lineKey_, lineKey_ + kLineKeys, -1.0f);
            located_ = false;
            gateSideValid_ = false;
        }
        // ITS boat's numbers (game::StatsOf, from the vessel's own settings): the top speed, the circle it turns in and
        // what that asks across its way, what it loses with the engine off. A boat that turns better takes a bend
        // faster, a heavy one brakes earlier -- nothing here is tuned to one hull.
        const physics::VesselSettings& boat = *hull.settings;
        halfLength_ = hull.halfLength;
        rudderLever_ = hull.rudderLever;
        stats_ = StatsOf(boat, halfLength_, rudderLever_);
        topSpeed_ = stats_.topSpeed;
        // Every number the profile below is made of: a boat retuned (--set vessel.*, a copy's own numbers) is a new line
        // the frame it changes. The mass alone moves the braking and not the top speed.
        const float key[kLineKeys] = { settings.grip, settings.braking, stats_.topSpeed, stats_.lateralAccel, stats_.braking, stats_.turnRadius };
        if (std::equal(key, key + kLineKeys, lineKey_)) { return; }
        std::copy(key, key + kLineKeys, lineKey_);
        const float top = topSpeed_;
        // A bend is taken at the speed that asks `grip` of what the boat's own tightest turn makes across its way. Never
        // at a crawl: slower does not make a boat's circle tighter -- its turn is its own, and with the throttle eased
        // the rudder has less wash (measured: 55 m flat out, 60-70 m at 0.45) -- it only gives the helm time.
        const float lateral = std::max(settings.grip * stats_.lateralAccel, 0.3f);
        line_.Profile([top, lateral](float curvature) { return std::clamp(std::sqrt(lateral / std::max(curvature, 1e-6f)), 0.5f * top, top); },
                      std::max(settings.braking * stats_.braking, 0.2f));
        status_.tightestRadius = stats_.turnRadius;
        // Where the line turns tightest, and the gate it comes before: a course that asks more than a boat's circle
        // shows there.
        Math::float2 tightAt;
        float tightStation = 0.0f;
        const float tightRadius = line_.TightestRadius(&tightAt, &tightStation);
        int tightGate = track.Gates().empty() ? 0 : track.Gates()[0].number;
        for (std::size_t i = 0; i < track.Gates().size(); ++i)
        {
            if (line_.GateStation(i) >= tightStation)
            {
                tightGate = track.Gates()[i].number;
                break;
            }
        }
        LOG_INFO(logging::LogCategory::App,
                 "race: '{}': a line of {:.0f} m for a boat of {:.1f} m/s flat out, {:.1f} s to 20 kn, a circle of {:.0f} m at {:.1f} m/s ({:.1f} m/s2 across, {:.2f} braking); "
                 "the line's slowest place {:.1f} m/s, its tightest r {:.0f} m at {:.0f} {:.0f} (before gate {})",
                 hull.object ? hull.object->GetGeometryPath() : std::string(), line_.Length(), top, stats_.toPlane, stats_.turnRadius, stats_.turnSpeed,
                 stats_.lateralAccel, stats_.braking, line_.SlowestSpeed(), tightRadius, tightAt.x, tightAt.y, tightGate);
    }

    physics::VesselInput RaceBot::Tick(const RaceTrack& track, const physics::VesselHull& hull, float deltaTime, bool drive,
                                       const std::vector<physics::VesselHull>* others)
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
            if (onGrid_) // a race's start: it stands before gate 1, on the course whatever slot of the grid it has
            {
                onGrid_ = false;
                joined_ = true;
                gateIndex_ = 0;
                gateSideValid_ = false;
            }
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
                    // How far from its middle, in its own half widths: 1 at a mark. That is what the penalty goes by.
                    status_.lastMissWidths = std::clamp(std::fabs(lateral) / std::max(gate.halfWidth, 1.0f), 1.0f, kMissWidthsMost);
                    status_.missedWidths += status_.lastMissWidths;
                    LOG_INFO(logging::LogCategory::App, "race: '{}' missed gate {}, {:.1f} m off its middle (half width {:.1f} m): x{:.2f} of the penalty",
                             hull.object ? hull.object->GetGeometryPath() : std::string(), gate.number, lateral, gate.halfWidth, status_.lastMissWidths);
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
                    status_.started = true;
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
        // At least a share of the boat's own circle: the water's forces go as the speed squared, so the path a hull makes
        // through a change of turn is the same at any speed, a length of its own -- one with a wider circle starts its
        // turn that much earlier (police_boat, 7.5 t, ran 9-17 m wide through the slalom with the yacht's 24 m).
        // The extension with the distance off the line only where the line runs straight (and while coming onto the
        // course): in a bend a far aim asks for LESS rudder than the bend itself does -- police_boat, 30-50 m outside the
        // far turn, held its rudder at 0.4-0.6 and stayed there.
        const float bendHere = std::fabs(here.curvature);
        const float straight = joined_ ? std::clamp(1.0f - 2.0f * bendHere * stats_.turnRadius, 0.0f, 1.0f) : 1.0f;
        const float reach = std::max({ settings.lookAheadMin, settings.lookAheadCircle * stats_.turnRadius, std::fabs(speed) * settings.lookAhead }) +
                            std::min(std::fabs(status_.crossTrack), 80.0f) * straight;
        const RaceLine::Sample ahead = line_.At(status_.station + reach);
        status_.aim = ahead.point;
        // Off the BOW, not off the way the boat moves. A hull in a turn slides, bow inside its path, so by the bow it runs
        // some 4 m wide of its line round a long bend; steered by its velocity instead (tried, 2026-11-10) it weaves from
        // gate to gate with the rudder banging from side to side -- the path answers the rudder a second after the bow does.
        // And not AT the aim: inside it, by the angle this boat slides at in the bend between here and there (its own
        // numbers: game::SlipAngle) -- a hull's way lies outside its bow in a turn, and with the bow on the aim the boat ran
        // outside its line in every bend by its reach times that angle: police_boat (7.5 t, 11 degrees in the coast's
        // 140 m bend) 7-9 m, onto the outer marks. From the LINE's bend, not from how the boat moves: that lags.
        const float bend = 0.5f * (here.curvature + ahead.curvature);
        const float slide = settings.slip * SlipAngle(*hull.settings, halfLength_, rudderLever_, std::max(std::fabs(speed), 2.0f), bend);
        const float error = WrapAngle(Heading(status_.aim - position) + (bend > 0.0f ? slide : -slide) - heading);
        const float level = difficulty_ == BotDifficulty::Easy ? settings.easyPace : (difficulty_ == BotDifficulty::Medium ? settings.mediumPace : 1.0f);
        float target = line_.At(status_.station + std::max(speed, 0.0f) * 0.5f).speed * settings.pace * level;
        // Coming onto the course (from its mooring, the bow often pointing away from it): at the speed its own circle
        // asks lateralAccel at, so the swing out and back is as tight as the boat makes it -- flat out a light fast hull
        // settled on the line and ran 37 m wide of the next gate; at half speed the yacht took 50 s to come back.
        if (!joined_) { target = std::min(target, std::max(stats_.turnSpeed, 3.0f)); }
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
        // The turn that brings the bow onto the aim (pure pursuit: an arc through the boat and the aim, tangent to the
        // bow), as a share of the tightest the boat makes at this speed.
        physics::VesselInput order;
        const float arc = 2.0f * (std::fabs(error) < 1.5707963f ? std::sin(error) : (error > 0.0f ? 1.0f : -1.0f)) / std::max(reach, 1.0f);
        const float pace = std::max(std::fabs(speed), 2.0f);
        const float tightest = SteadyTurnRate(*hull.settings, halfLength_, rudderLever_, pace) / pace;
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
                    const bool reachable = across <= 0.5f * stats_.lateralAccel * seconds * seconds;
                    const float way = reachable ? middle : (side > 0.0f ? -1.0f : 1.0f);
                    order.rudder += way * 2.5f * (1.0f - std::fabs(side) / clear);
                }
            }
        }
        // Another boat in its way that the bow is about to reach -- within two seconds at the speed the two close at:
        // round it, on the side it does not lie on. Not "ahead and a little slower": four equal boats run one line nose
        // to tail, closing and opening at a metre a second with the sea, and steering round a stern 40 m off threw the
        // rudder over all the way round the course (lap 1 of a race: three boats off the far turn in the same second).
        if (others && settings.avoidBoats > 0.5f)
        {
            const float2 starboard(forward.y, -forward.x);
            for (const physics::VesselHull& other : *others)
            {
                if (other.object == hull.object) { continue; }
                const float2 to = float2(other.position.x, other.position.z) - position;
                const float along = to.Dot(forward);
                const float side = to.Dot(starboard);
                const float clear = hull.halfBeam + other.halfBeam + 2.5f;
                const float closing = speed - float2(other.velocity.x, other.velocity.z).Dot(forward);
                const float gap = along - halfLength_ - other.halfLength; // bow to stern
                if (along <= 0.0f || std::fabs(side) >= clear || closing < 0.5f || gap > 2.0f * closing) { continue; }
                order.rudder += (side > 0.0f ? -1.0f : 1.0f) * 2.0f * (1.0f - std::fabs(side) / clear);
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
        const float hold = (target / std::max(topSpeed_, 1.0f)) * (target / std::max(topSpeed_, 1.0f));
        if (target >= 0.97f * topSpeed_) { order.throttle = 1.0f; }
        else { order.throttle = std::clamp(hold + 0.5f * (target - speed), -0.7f, 1.0f); }
        // It brakes on the straight, not in the bend: the rudder works by the water past it, the boat's speed AND the
        // propeller's wash, so with the rudder asked for much the engine is not eased under what holds the line's speed,
        // let alone put astern -- astern at 23 kn took four fifths of the rudder's push, and police_boat, hard over,
        // went on out of the far turn. In the bend the slide takes the speed off by itself.
        {
            const float steering = std::clamp((std::fabs(order.rudder) - 0.25f) / 0.35f, 0.0f, 1.0f);
            order.throttle = std::max(order.throttle, -0.7f + (std::min(hold, 1.0f) + 0.7f) * steering);
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
            const bool held = order.throttle > 0.4f && std::fabs(speed) < 0.5f && elapsed_ > 4.0f;
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
}
