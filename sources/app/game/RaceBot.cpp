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
        constexpr float kTableLeast = 1.0f / 3000.0f; // 1/m: the speed table's curvatures (RaceBot::BuildSpeedTable)
        constexpr float kTableMost = 0.2f;

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
            { "pace", "Pace", "The share of its boat's top speed the bot allows itself on the straights (every bot, on top of its level). The bends it takes as fast as its line says whatever this is: slowed there the rudder loses its wash. 1 = flat out.", &S::pace, 0.005f, 0.2f, 1.0f, "%.2f" },
            { "easyPace", "Easy bot's pace", "The share of its top speed an EASY bot allows itself on the straights (a race gives each bot a level: race.difficulty). Its bends are a hard bot's.", &S::easyPace, 0.005f, 0.3f, 1.0f, "%.2f" },
            { "mediumPace", "Medium bot's pace", "The same for a MEDIUM bot. A hard one runs flat out.", &S::mediumPace, 0.005f, 0.3f, 1.0f, "%.2f" },
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
            { "debug", "Bots' vision (debug)", "What every bot sees and decides, drawn in Play: the gates (port red, starboard green); its plan coloured by the speed it may have there (red slow, green fast) with a fence a metre tall per 4 m/s and its expected speed (white); the buoys and boats it plans round (orange / pink rings, a boat's next 6 s); its aim (a mast); and a card over each racing bot -- speed and target, throttle and rudder with WHY. Also the editor's course section, \"Bot race\".", &S::debug, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "holdThrottle", "Hold the throttle", "The bots steer and the engine stays flat out, bends and all: a player holding the throttle down, to see what a course asks of a boat that never slows.", &S::holdThrottle, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "learn", "Learn", "The bots learn as they drive how tight their boat really turns at each speed (its turn map: the boat's, not a course's) and plan by it. Kept per boat and level in data/bot_memory, written when a race ends and when Play stops. 0 = the model alone, nothing learned.", &S::learn, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "gripLearn", "Measure its grip", "1 = with the rudder hard over it measures what its boat really turns against what its model says the grip gives in still water (a sea takes some), and plans its bends by that share. 0 = the model's grip.", &S::gripLearn, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "plan", "Drive by sight", "1 = the bot drives by what it sees: every tenth of a second a path from where its boat is through the next gates, crossing each anywhere in its window, the fastest its boat can really sail (its turn, its grip, its braking), round where the other boats will be. 0 = it follows its line through the gates' middles.", &S::plan, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "planGates", "Gates it looks ahead", "How many gates ahead its plan goes through. More = it sets up a bend from further off; each is a little more to compute.", &S::planGates, 1.0f, 1.0f, 8.0f, "%.0f" },
            { "planMargin", "Plan margin to a mark (m)", "Between the hull's side and a buoy where its plan crosses a gate. Less = it uses more of the gate (wider entries, tighter apexes), and brushes a buoy in a sea.", &S::planMargin, 0.05f, 0.5f, 10.0f, "%.1f" },
            { "planEvery", "Plan every (s)", "Seconds between plans.", &S::planEvery, 0.01f, 0.02f, 1.0f, "%.2f" },
            { "turnPower", "Power in a hard turn", "Hard over under its grip speed, its throttle is never under this while it is no more than 'Power over its speed' past the speed it wants: these boats turn by the propeller's wash past the rudder, and coasting hard over at 6-9 m/s the yacht made circles of 55-180 m where with the power on it makes 27-44 m. 0 = it coasts down to the speed it wants (before).", &S::turnPower, 0.05f, 0.0f, 1.0f, "%.2f" },
            { "turnPowerOver", "Power over its speed (m/s)", "How far over the speed it wants a boat hard over still keeps 'Power in a hard turn' on; past it it coasts to slow down first.", &S::turnPowerOver, 0.1f, 0.0f, 10.0f, "%.1f" },
            { "markRule", "Buoys: last moment only", "1 = with a plan (which keeps clear of the buoys itself) the helm steers round a buoy only when a contact is about to happen: within its half beam and 1.5 m, 1.2 s ahead. 0 = it keeps its line's room ('Room at a mark') from every buoy ahead, fighting a plan that crosses a narrow gate near a mark on purpose.", &S::markRule, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "stepSteer", "Steer in the physics steps", "1 = its steering law (a pure pursuit of its aim, damped by the hull's turn) is evaluated in every physics step from the hull as that step has it: the same helm at any frame rate and time scale. 0 = once a frame, the rudder standing for every step the frame holds (4 at 60 fps and x4).", &S::stepSteer, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "turnWash", "Wash to the grip", "1 = with its rudder hard over its throttle is never under the share whose propeller wash takes the rudder to what the boat's grip holds at that speed (coasting, a rudder without wash turned the yacht on an 86 m circle where its grip allows 35). 0 = it may coast hard over (before).", &S::turnWash, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "planSwitch", "Plan switch margin (s)", "A new plan replaces the last one (run on from where the boat is) only when it is that many seconds cheaper: two near-equal ways through a gate swapped every tenth of a second and the rudder went from one side to the other. 0 = the cheapest plan every time (before).", &S::planSwitch, 0.05f, 0.0f, 2.0f, "%.2f" },
            { "steerByRudder", "Steer by its rudder", "Its rudder's order is a share of the turn it makes at this speed: 1 = what its rudder turns it, 0 = what its grip lets it (past its grip speed a couple of degrees off then asked for full rudder: it sawed from side to side on a straight), between them in log.", &S::steerByRudder, 0.05f, 0.0f, 1.0f, "%.2f" },
            { "planLead", "Plan ahead of its engine", "1 = it brakes as early as its engine needs: the least its plan allows over the stretch it covers in half the time its throttle takes from full ahead to astern (police_boat 1.9 s, the yacht 0.85 s): the braking ramps in as the engine swings. 0 = the plan's limit half a second on.", &S::planLead, 0.05f, 0.0f, 2.0f, "%.2f" },
            { "planTurns", "Plan its way through a gate", "1 = its plan chooses which way it crosses each gate too (up to 20 degrees off the line through its neighbours): a wide entry and a late apex. 0 = only where it crosses.", &S::planTurns, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "planBrakeBend", "No engine braking in a bend", "1 = its plan counts no braking from its engine where its rudder will already be over for the bend ahead (it never goes astern there: the rudder needs the propeller's wash), so it brakes for a tight corner on the straight before it. 0 = its plan counts the engine everywhere (the default: on, reef's time trials missed fewer gates -- T1's apex 2 times against 5 -- and lapped 9 s slower).", &S::planBrakeBend, 1.0f, 0.0f, 1.0f, "%.0f" },
            { "planBrake", "Plan braking share", "Of what its boat loses at a speed to its hull's drag and its engine 0.7 astern: what its plan counts on to slow down (little when slow, so a slow bend is braked for from further off). Higher = later braking.", &S::planBrake, 0.01f, 0.1f, 1.5f, "%.2f" },
            { "rivalClearance", "Room to a rival (m)", "Between hulls, what its plan keeps from where another boat will be when it gets there. More = it passes wider or waits behind.", &S::rivalClearance, 0.1f, 0.0f, 20.0f, "%.1f" },
            { "forget", "Forget", "1 = every bot memory of this level emptied (and written so) at the next frame of Play: the bots start learning again from the model.", &S::forget, 1.0f, 0.0f, 1.0f, "%.0f" },
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
        speedBefore_ = 0.0f;
        knockSeconds_ = 0.0f;
        recovering_ = false;
        pathValid_ = false;
        pathRate_ = 0.0f;
        saturatedSeconds_ = 0.0f;
        status_.gripSeen = gripSeen_; // kept: what its boat does in this sea is not the race's
        hardOverSeconds_ = 0.0f;
        planValid_ = false;
        planAge_ = 0.0f;
    }

    float RaceBot::CanTurn(const physics::VesselSettings& boat, float speed, float gripShare) const
    {
        const float v = std::max(speed, 0.5f);
        const float model = std::min(SteadyTurnRate(boat, halfLength_, rudderLever_, v), gripShare * GripAccel(boat, v) / v);
        // What its boat has really made hard over at this speed stands in for the model as far as the turn map trusts it.
        float curvature = 0.0f;
        const float trust = memory_ && BotSettingsRef().learn >= 0.5f ? memory_->TurnTrust(v, &curvature) : 0.0f;
        return trust > 0.0f ? model + (curvature * v - model) * trust : model;
    }

    float RaceBot::TurnSpeed(const physics::VesselSettings& boat, float curvature, float share, float gripShare) const
    {
        // Down to the speed it turns tightest at (BuildSpeedTable), not half its top speed: under its grip's reach a slower boat
        // DOES turn tighter -- its turn map knows (the yacht r 34 m at 8 m/s, r 10 at 4) -- and a bend it ran wide of at
        // 10-12 m/s with its rudder hard over cost the reef's gates (2026-11-12, owner: "when many are missed, fix the bot").
        const float top = topSpeed_;
        const float bend = std::max(curvature, 1e-6f);
        const float least = std::min(std::max(recoverSpeed_, 3.0f), 0.5f * top);
        float best = least;
        float bestRadius = 1.0e9f;
        for (float v = top; v >= least; v -= 0.25f)
        {
            const float rate = share * CanTurn(boat, v, gripShare);
            if (rate >= v * bend) { return v; }
            const float radius = v / std::max(rate, 1e-4f);
            // A tie goes to the slower: a bend it cannot make is better met with time to steer (a turn map that knows the
            // same circle at every speed would otherwise send it in flat out).
            if (radius < 1.001f * bestRadius)
            {
                bestRadius = radius;
                best = v;
            }
        }
        return best;
    }

    void RaceBot::Start()
    {
        Reset();
        onGrid_ = true;
    }

    void RaceBot::PutBack(float station, std::size_t gateAhead)
    {
        if (gateAhead != static_cast<std::size_t>(-1)) { gateIndex_ = gateAhead; }
        recovering_ = false;
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
        hardOverSeconds_ = 0.0f;
        planValid_ = false; // a jump: the plan was made from where it was
        speedBefore_ = 0.0f; // put back at rest: no knock
        knockSeconds_ = 0.0f;
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
        hardOverSeconds_ = 0.0f;
        planValid_ = false;
    }

    void RaceBot::SetMemory(BotMemory* memory)
    {
        if (memory == memory_) { return; }
        memory_ = memory;
        std::fill(lineKey_, lineKey_ + kLineKeys, -1.0f); // its line was planned without it
    }

    void RaceBot::BuildSpeedTable(const physics::VesselSettings& boat, float share, float gripShare)
    {
        // The speed it turns tightest at, from walking pace up (its turn map knows the slow end: the yacht r 10 m at 4 m/s,
        // r 55 at 10): what it comes round at when it is pointing the wrong way, and the slowest a bend is taken at.
        recoverSpeed_ = 3.5f;
        float tightest = 1.0e9f;
        for (float v = 3.5f; v <= 0.6f * topSpeed_; v += 0.25f)
        {
            const float radius = v / std::max(CanTurn(boat, v, gripShare), 1e-4f);
            if (radius < 0.97f * tightest)
            {
                tightest = radius;
                recoverSpeed_ = v;
            }
        }
        reachCurvature_ = 0.0f;
        for (float v = topSpeed_; v >= recoverSpeed_; v -= 0.25f) { reachCurvature_ = std::max(reachCurvature_, share * CanTurn(boat, v, gripShare) / std::max(v, 0.5f)); }
        // What it turns at each speed, 0 to its top speed (the plan's braking asks where its rudder will be over).
        for (int i = 0; i < kTurnTable; ++i)
        {
            const float v = std::max(topSpeed_ * static_cast<float>(i) / static_cast<float>(kTurnTable - 1), 0.5f);
            turnTable_[i] = share * CanTurn(boat, v, gripShare) / v;
        }
        // Curvature from 1/3000 m to 1/5 m on a log scale; under it the straight's speed, over it the tightest's.
        for (int i = 0; i < kSpeedTable; ++i)
        {
            const float bend = kTableLeast * std::pow(kTableMost / kTableLeast, static_cast<float>(i) / static_cast<float>(kSpeedTable - 1));
            speedTable_[i] = TurnSpeed(boat, bend, share, gripShare);
        }
    }

    float RaceBot::WashToGrip(const physics::VesselSettings& boat, float speed) const
    {
        // The rudder's turn (SteadyTurnRate) with its wash at the root of the throttle, against the grip's turn at the share it
        // has measured: the least throttle at which the first reaches the second (bisection; the rudder's turn grows with it).
        const float v = std::max(speed, 0.5f);
        const float grip = gripSeen_ * GripAccel(boat, v) / v;
        const auto rudderAt = [&](float throttle)
        {
            physics::VesselSettings eased = boat;
            eased.wash *= std::sqrt(std::max(throttle, 0.0f));
            return SteadyTurnRate(eased, halfLength_, rudderLever_, v);
        };
        if (rudderAt(1.0f) <= grip) { return 1.0f; }
        if (rudderAt(0.0f) >= grip) { return 0.0f; }
        float low = 0.0f;
        float high = 1.0f;
        for (int i = 0; i < 12; ++i)
        {
            const float mid = 0.5f * (low + high);
            (rudderAt(mid) >= grip ? high : low) = mid;
        }
        return high;
    }

    float RaceBot::TurnAtSpeed(float speed) const
    {
        const float at = std::clamp(speed / std::max(topSpeed_, 1.0f), 0.0f, 1.0f) * static_cast<float>(kTurnTable - 1);
        const int below = std::min(static_cast<int>(at), kTurnTable - 2);
        const float t = at - static_cast<float>(below);
        return turnTable_[below] + (turnTable_[below + 1] - turnTable_[below]) * t;
    }

    float RaceBot::SpeedFor(float curvature) const
    {
        if (curvature <= kTableLeast) { return speedTable_[0]; }
        if (curvature >= kTableMost) { return speedTable_[kSpeedTable - 1]; }
        const float at = std::log(curvature / kTableLeast) / std::log(kTableMost / kTableLeast) * static_cast<float>(kSpeedTable - 1);
        const int below = std::min(static_cast<int>(at), kSpeedTable - 2);
        const float t = at - static_cast<float>(below);
        return speedTable_[below] + (speedTable_[below + 1] - speedTable_[below]) * t;
    }

    void RaceBot::MakePlan(const physics::VesselHull& hull, const std::vector<RaceGate>& gates, const std::vector<physics::VesselHull>* others, float cap)
    {
        const BotSettings& settings = BotSettingsRef();
        const std::size_t count = std::min(static_cast<std::size_t>(std::clamp(settings.planGates, 1.0f, 8.0f)), gates.size());
        PlanBoat boat;
        boat.position = float2(hull.position.x, hull.position.z);
        const float2 way(hull.velocity.x, hull.velocity.z);
        const float moving = std::sqrt(way.Dot(way));
        boat.way = moving > 1.0f ? way / moving : float2(hull.forward.x, hull.forward.z).Normalized();
        boat.speed = std::max(moving, std::fabs(hull.speed));
        std::vector<PlanGate> ahead;
        ahead.reserve(count + 1);
        bool skipped = false;
        for (std::size_t k = 0; k < count + 1 && ahead.size() < count; ++k)
        {
            const RaceGate& gate = gates[(gateIndex_ + k) % gates.size()];
            // The margin is for gates it plans to: the one it is about to cross takes any crossing the hull clears its marks at
            // by 3 m. Held to the planned window there it dragged a boat already safely inside the gate back to its edge in
            // the last 30 m -- an S-bend it could not make, so it braked to 5 m/s before every gate (the toy boat's circle);
            // at 1.5 m the hulls, sliding 20-30 degrees, hit the marks.
            const bool coming = ahead.empty() && (gate.center - boat.position).Dot(gate.along) < 60.0f;
            const float window = std::max(gate.halfWidth - (hull.halfBeam + (coming ? 3.0f : settings.planMargin)), 0.5f);
            // A gate a second or so ahead that its way already carries it through inside the window is passed as it goes:
            // a path bent to cross it elsewhere a few metres off is a bend no boat makes. The plan starts at the next.
            if (k == 0)
            {
                const float2 off = gate.center - boat.position;
                const float ahead1 = off.Dot(gate.along);
                const float closing = boat.way.Dot(gate.along);
                if (ahead1 > 0.0f && closing > 0.5f && ahead1 < std::max(15.0f, boat.speed))
                {
                    const float2 crossing = boat.position + boat.way * (ahead1 / closing);
                    if (std::fabs((crossing - gate.center).Dot(gate.across)) < gate.halfWidth - hull.halfBeam - 1.0f)
                    {
                        skipped = true;
                        continue;
                    }
                }
            }
            ahead.push_back(PlanGate{ gate.center, gate.across, gate.along, window });
        }
        boat.cap = cap;
        boat.topSpeed = topSpeed_;
        boat.accel = 0.85f * stats_.thrust / std::max(stats_.mass, 1.0f);
        // Its hull's drag (the share in the water, 0.77, of surgeDrag v^2) and its engine at 0.7 astern -- a share of both:
        // the drag is most of it fast and little slow, so a slow bend is braked for from further off (a constant 1.9 m/s2
        // left police_boat too fast into the reef's first hairpin at 8 m/s).
        const float mass = std::max(stats_.mass, 1.0f);
        boat.brake = std::max(settings.planBrake * 0.7f * hull.settings->reverseShare * stats_.thrust / mass, 0.1f);
        boat.drag = settings.planBrake * 0.77f * hull.settings->surgeDrag / mass;
        boat.reach = reachCurvature_;
        boat.turnMost = settings.planTurns >= 0.5f ? kPlanTurnMost : 0.0f;
        boat.switchCost = std::max(settings.planSwitch, 0.0f);
        boat.speedFor = [this](float curvature) { return SpeedFor(curvature); };
        if (settings.planBrakeBend >= 0.5f)
        {
            boat.turnAt = [this](float speed) { return TurnAtSpeed(speed); };
            boat.leadMetres = settings.lookAheadMin;
            boat.leadSeconds = settings.lookAhead;
        }
        std::vector<PlanRival> rivals;
        if (others && settings.avoidBoats > 0.5f)
        {
            for (const physics::VesselHull& other : *others)
            {
                if (other.object == hull.object) { continue; }
                const float2 at(other.position.x, other.position.z);
                const float2 off = at - boat.position;
                if (off.Dot(off) > 200.0f * 200.0f) { continue; }
                // Nose to tail half their lengths count too: a boat 6 m dead ahead is in the way, one 6 m abeam is not much.
                const float clearance = 0.5f * (hull.halfLength + other.halfLength) + hull.halfBeam + other.halfBeam + settings.rivalClearance;
                rivals.push_back(PlanRival{ at, float2(other.velocity.x, other.velocity.z), clearance });
            }
        }
        // The marks of the gates it plans through, and of the one before them (it may still be beside it): what a hull sliding
        // in a turn sweeps is wider than its beam -- its half beam and 0.35 of its half length (20 degrees of slide) -- and a
        // metre and a half more for the buoy and the sea.
        const float markClear = hull.halfBeam + 0.35f * hull.halfLength + 1.5f;
        for (std::size_t k = 0; k <= count && k < gates.size(); ++k)
        {
            const RaceGate& gate = gates[(gateIndex_ + gates.size() - 1 + k) % gates.size()];
            rivals.push_back(PlanRival{ gate.a, float2(0.0f, 0.0f), markClear, true });
            rivals.push_back(PlanRival{ gate.b, float2(0.0f, 0.0f), markClear, true });
        }
        // It starts from what it planned: the same gates' offsets, or shifted by the gate it has passed since.
        const std::size_t first = (gateIndex_ + (skipped ? 1 : 0)) % gates.size();
        std::vector<float> warm;
        std::vector<float> warmTurns;
        if (planValid_ && planFirst_ == first)
        {
            warm = plan_.offsets;
            warmTurns = plan_.turns;
        }
        else if (planValid_ && (planFirst_ + 1) % gates.size() == first && plan_.offsets.size() > 1)
        {
            warm.assign(plan_.offsets.begin() + 1, plan_.offsets.end());
            warmTurns.assign(plan_.turns.begin() + 1, plan_.turns.end());
        }
        planRivals_ = rivals;
        plan_ = PlanThroughGates(boat, ahead, rivals, warm, warmTurns);
        planValid_ = plan_.valid;
        planAge_ = 0.0f;
        planGate_ = gateIndex_;
        planFirst_ = first;
        planAlong_ = 0.0f;
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
        // The grip it has measured in its own turns comes in by steps (each a new profile), not every frame.
        if (std::fabs(gripSeen_ - lineGripSeen_) > 0.04f) { lineGripSeen_ = gripSeen_; }
        const bool learning = memory_ && settings.learn >= 0.5f;
        const float key[kLineKeys] = { settings.grip, settings.braking, stats_.topSpeed, stats_.lateralAccel, stats_.braking, stats_.turnRadius, lineGripSeen_,
                                       learning ? static_cast<float>(memory_->Version()) : -2.0f };
        if (std::equal(key, key + kLineKeys, lineKey_)) { return; }
        // Only its turn map moved: at most once a second (the map learns every frame it is hard over).
        const bool learnedOnly = std::equal(key, key + 7, lineKey_);
        if (learnedOnly && elapsed_ - lineBuiltAt_ < 1.0f) { return; }
        std::copy(key, key + kLineKeys, lineKey_);
        lineBuiltAt_ = elapsed_;
        BuildSpeedTable(boat, settings.grip, lineGripSeen_);
        const float top = topSpeed_;
        // A bend is taken at the fastest speed at which `grip` of what the boat turns AT THAT SPEED is the bend's curvature:
        // its rudder's turn, or past gripSpeed its side grip's (2026-11-12: there the hull lets go and runs wide, its circle
        // growing as the speed squared, so the bot lifts before a tight bend) -- the grip as it has measured it in this sea.
        // A bend tighter than the boat turns at any speed is taken at the speed it turns tightest at. Never at a crawl:
        // under gripSpeed slower does not make the circle tighter -- the rudder's own hardly changes with the speed, and
        // with the throttle eased the rudder has less wash (measured: 55 m flat out, 60-70 m at 0.45).
        line_.Profile([this](float curvature) { return SpeedFor(curvature); }, std::max(settings.braking * stats_.braking, 0.2f));
        status_.tightestRadius = stats_.turnRadius;
        if (learnedOnly) { return; } // the line is the one it logged
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
        const physics::VesselInput last = status_.order; // what it told the helm a frame ago: its grip is measured under it
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
        // Its grip as it finds it (2026-11-12, owner: "teach the bot"). The model's GripAccel is still water's; a sea throws
        // the hull out of the water and its grip goes with it (measured: the yacht's circle flat out 90 m in a calm, 136 m
        // in wind_test's 1 m sea). With the rudder hard over and the power on past gripSpeed for a second and a half, the
        // turn its way makes (the path's, not the bow's: the hull slides) is all its grip gives: that against the model,
        // followed over a few seconds of such turns. The line's speeds follow it by steps (EnsureLine).
        {
            const float2 way(hull.velocity.x, hull.velocity.z);
            const float wayLength = std::sqrt(way.Dot(way));
            const float wayHeading = wayLength > 1.0f ? Heading(way / wayLength) : pathHeading_;
            if (pathValid_ && deltaTime > 1e-4f)
            {
                const float rate = WrapAngle(wayHeading - pathHeading_) / deltaTime;
                pathRate_ += (rate - pathRate_) * (1.0f - std::exp(-deltaTime / 0.25f));
            }
            pathHeading_ = wayHeading;
            pathValid_ = wayLength > 1.0f;
            // At its grip's limit: hard over, and the way turns clearly less than the rudder at the throttle it had turns a boat
            // (its wash goes as the root of the throttle) -- it is the grip, not the rudder, that holds it. What it then makes
            // against the model's still-water grip. Until 2026-11-12 it also asked for the speed past gripSpeed and the rudder
            // 1.25 over the grip: a planned bot was never there that long, the share stayed 1, and police_boat planned reef's
            // T1 at 12 m/s on a sea that gives three quarters of that grip, ran 10 m wide and hit the apex buoy.
            // Against the less of the two the model says hold it there -- the rudder at that throttle, the still-water grip: a
            // boat that makes less than either has a sea taking its grip (its hull leaving the water takes the keel and the
            // rudder with it). The share comes into its grip only (CanTurn); the rudder's own error is the turn map's.
            const bool hardOver = drive && std::fabs(last.rudder) > 0.9f && last.throttle >= 0.0f && speed > recoverSpeed_ + 2.0f && wayLength > 2.0f;
            saturatedSeconds_ = hardOver ? saturatedSeconds_ + deltaTime : 0.0f;
            if (settings.gripLearn < 0.5f) { gripSeen_ = 1.0f; }
            else if (saturatedSeconds_ > 1.0f)
            {
                physics::VesselSettings eased = *hull.settings;
                eased.wash *= std::sqrt(last.throttle);
                const float rudder = speed * SteadyTurnRate(eased, halfLength_, rudderLever_, speed);
                const float made = wayLength * std::fabs(pathRate_);
                const float seen = made / std::max(std::min(rudder, GripAccel(*hull.settings, speed)), 0.1f);
                gripSeen_ += (std::clamp(seen, 0.4f, 1.2f) - gripSeen_) * (1.0f - std::exp(-deltaTime / 4.0f));
            }
            status_.gripSeen = gripSeen_;
            // The turn map (BotMemory, 2026-11-12, owner: "make the table"): the rudder hard over for a second, whatever the
            // throttle -- the curvature its way then makes is what the boat turns at this speed, in this sea, the way this
            // bot drives it. A path tighter than 5 m is a knock, not a turn.
            hardOverSeconds_ = drive && std::fabs(last.rudder) > 0.95f && reverseSeconds_ <= 0.0f && speed > 2.0f ? hardOverSeconds_ + deltaTime : 0.0f;
            const float pathCurvature = wayLength > 2.0f ? std::fabs(pathRate_) / wayLength : 0.0f;
            // Power on only (0.6 and over): coasting takes the wash off the rudder, and a map learned so said its boat turned
            // wider than it does, so the bot planned slower, coasted more and learned wider still (reef, 2026-11-12: its
            // target under 7.5 m/s for 26-47 % of a lap).
            if (memory_ && settings.learn >= 0.5f && hardOverSeconds_ > 1.0f && pathValid_ && pathCurvature < 0.2f && last.throttle >= 0.6f)
            {
                memory_->LearnTurn(speed, pathCurvature, deltaTime);
            }
        }

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

        status_.memoryTrusted = memory_ ? memory_->TrustedBins() : 0;

        // ---- what it sees (RacePlanner): a path from where it is through the next gates, made every bot.planEvery ----
        // Once it is on the course (coming onto it from its mooring it follows its line). A new plan at once when the gate
        // ahead changed, or when the boat has run past the plan's middle.
        const float level = difficulty_ == BotDifficulty::Easy ? settings.easyPace : (difficulty_ == BotDifficulty::Medium ? settings.mediumPace : 1.0f);
        const float cap = settings.pace * level * topSpeed_;
        planAge_ += deltaTime;
        const bool planning = settings.plan >= 0.5f && joined_ && drive;
        if (!planning) { planValid_ = false; }
        else if (!planValid_ || planAge_ >= settings.planEvery || planGate_ != gateIndex_ || planAlong_ > 0.5f * plan_.Length())
        {
            MakePlan(hull, gates, others, cap);
        }
        status_.planned = planning && planValid_;
        status_.planSeconds = status_.planned ? plan_.seconds : 0.0f;
        status_.planPenalty = status_.planned ? plan_.penalty : 0.0f;
        status_.planOffset = status_.planned && !plan_.offsets.empty() ? plan_.offsets[0] : 0.0f;

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
        // The circle it turns at THIS speed: its rudder's, or its grip's past gripSpeed (that grows as the speed squared).
        const float pace = std::max(std::fabs(speed), 2.0f);
        const float radiusNow = pace / std::max(CanTurn(*hull.settings, pace, gripSeen_), 1e-3f);
        const float bendHere = std::fabs(here.curvature);
        const float straight = joined_ ? std::clamp(1.0f - 2.0f * bendHere * radiusNow, 0.0f, 1.0f) : 1.0f;
        // By its plan: the plan starts where the boat is, so no reach for being off it.
        const bool planned = status_.planned;
        const float reach = std::max({ settings.lookAheadMin, settings.lookAheadCircle * radiusNow, std::fabs(speed) * settings.lookAhead }) +
                            (planned ? 0.0f : std::min(std::fabs(status_.crossTrack), 80.0f) * straight);
        if (planned) { planAlong_ = plan_.Project(position, planAlong_); }
        const RaceLine::Sample ahead = line_.At(status_.station + reach);
        status_.aim = planned ? plan_.PointAt(planAlong_ + reach) : ahead.point;
        // Off the BOW, not off the way the boat moves. A hull in a turn slides, bow inside its path, so by the bow it runs
        // some 4 m wide of its line round a long bend; steered by its velocity instead (tried, 2026-11-10) it weaves from
        // gate to gate with the rudder banging from side to side -- the path answers the rudder a second after the bow does.
        // And not AT the aim: inside it, by the angle this boat slides at in the bend between here and there (its own
        // numbers: game::SlipAngle) -- a hull's way lies outside its bow in a turn, and with the bow on the aim the boat ran
        // outside its line in every bend by its reach times that angle: police_boat (7.5 t, 11 degrees in the coast's
        // 140 m bend) 7-9 m, onto the outer marks. From the LINE's bend, not from how the boat moves: that lags.
        const float bend = planned ? 0.5f * (plan_.CurvatureAt(planAlong_) + plan_.CurvatureAt(planAlong_ + reach)) : 0.5f * (here.curvature + ahead.curvature);
        const float slide = settings.slip * SlipAngle(*hull.settings, halfLength_, rudderLever_, std::max(std::fabs(speed), 2.0f), bend);
        float error = WrapAngle(Heading(status_.aim - position) + (bend > 0.0f ? slide : -slide) - heading);
        // A level (and bot.pace) caps how fast it runs -- on the straights -- not a share of every speed of its line. Slowed
        // in a bend these boats lose their rudder: its push is (speed + wash)^2, the wash is the throttle's. The owner's race
        // (2026-11-12, reef) had the easy police_boat and the medium yacht at 0.8 / 0.9 of their line's speed everywhere:
        // 4 and 6 gates missed, nearly all in the shore's slalom and the hairpin, where the hard bots missed none.
        // By its plan: the most its plan lets it have half a second on (its own braking points), not the speed the plan
        // expects it to have there: steered by that, it never asked for more than a little over its speed now.
        const float ahead05 = std::max(speed, 0.0f) * 0.5f;
        float target = std::min(planned ? plan_.LimitAt(planAlong_ + ahead05) : line_.At(status_.station + ahead05).speed, cap);
        // Its engine takes time to come off and go astern (throttleRate: the yacht 1/s, police_boat 0.45/s -- full ahead to
        // 0.7 astern is nearly 4 s): the least the plan allows over the stretch it covers meanwhile, so it starts braking
        // when the engine needs to, not when the limit is half a second off.
        if (planned && settings.planLead > 0.0f && hull.settings)
        {
            const float spool = settings.planLead * 0.85f / std::max(hull.settings->throttleRate, 0.1f); // half the swing: the braking ramps in
            const float reachLead = ahead05 + std::max(speed, 0.0f) * spool;
            for (int k = 1; k <= 8; ++k) { target = std::min(target, plan_.LimitAt(planAlong_ + ahead05 + (reachLead - ahead05) * static_cast<float>(k) / 8.0f)); }
        }
        status_.slowedBy = target >= cap - 1e-3f ? 'c' : (planned ? 'p' : 'l');
        // Coming onto the course (from its mooring, the bow often pointing away from it): at the speed its own circle
        // asks lateralAccel at, so the swing out and back is as tight as the boat makes it -- flat out a light fast hull
        // settled on the line and ran 37 m wide of the next gate; at half speed the yacht took 50 s to come back.
        // Since the side grip (2026-11-12) the swing is tightest at gripSpeed: faster its grip lets go and it runs wide.
        if (!joined_ && std::max(std::min(stats_.turnSpeed, stats_.gripSpeed), 3.0f) < target)
        {
            target = std::max(std::min(stats_.turnSpeed, stats_.gripSpeed), 3.0f);
            status_.slowedBy = 'j';
        }
        // Pointing the wrong way -- a buoy or another boat knocked it round, it came onto the course badly: it comes round
        // SLOWLY, at the speed it turns tightest at, and only then goes on (owner, 2026-11-12: "a bot that hit a buoy then
        // tears off somewhere flat out instead of coming back to the course carefully at low speed" -- its line and its plan
        // never ask for less than half its top speed, and half its top speed is a circle of 50-100 m). In past 57 degrees
        // off its aim, out under 23.
        // A knock -- the speed gone by 4 m/s and more in a moment (a buoy, a boat; braking for a bend takes a few seconds to
        // lose that) -- counts as pointing the wrong way for a second and a half, however the bow lies after it.
        if (deltaTime > 0.0f)
        {
            if (speedBefore_ - speed > 4.0f && reverseSeconds_ <= 0.0f) { knockSeconds_ = 1.5f; }
            speedBefore_ += (speed - speedBefore_) * (1.0f - std::exp(-deltaTime / 0.5f));
            knockSeconds_ = std::max(knockSeconds_ - deltaTime, 0.0f);
        }
        // Coming round it aims at the gate ahead itself, not along its plan: a plan leaves along the way the boat moves, so
        // pointing the wrong way it is a wide loop (the light boat circled 9 s at 4 m/s on one). Before the gate: where its
        // plan crosses it; past it without having crossed: 25 m before it, to come at it the right way. Out when its bow is
        // on that point and the knock is over.
        float2 back = status_.aim;
        if (joined_ && !gates.empty())
        {
            const RaceGate& gate = gates[gateIndex_];
            const bool before = (position - gate.center).Dot(gate.along) < -5.0f;
            const float offset = planned && planFirst_ == gateIndex_ && !plan_.offsets.empty() ? plan_.offsets[0] : 0.0f;
            back = before ? gate.center + gate.across * offset : gate.center - gate.along * 25.0f;
        }
        const float backError = WrapAngle(Heading(back - position) - heading);
        // The wrong way is against the COURSE where the boat is (its line's way there), not against its aim: in a hairpin the
        // aim lies 60-90 degrees off the bow by design, and coming round at a crawl there cost 14-32 % of a lap.
        const float againstCourse = std::fabs(WrapAngle(Heading(here.tangent) - heading));
        recovering_ = joined_ && (againstCourse > 1.3f || knockSeconds_ > 0.0f || (recovering_ && (againstCourse > 1.0f || std::fabs(backError) > 0.6f)));
        status_.recovering = recovering_;
        if (recovering_)
        {
            if (recoverSpeed_ < target) { status_.slowedBy = 'r'; }
            target = std::min(target, recoverSpeed_);
            status_.aim = back;
            error = backError;
        }
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
        const float tightest = 1.0f / radiusNow; // its rudder's, or its grip's past it: what it can do, not what the rudder alone would
        // The turn it needs NOW, to its aim, against what it turns at this speed: past it, the speed it can make that turn
        // at (the line's own rule, TurnSpeed). The line's speed is the line's: a boat 20 m outside it in a bend, or
        // knocked off it, needs a tighter turn than the line does there -- and flat out with the rudder over it only ran
        // wider (reef, 2026-11-12: 75 m outside the esses at 14 m/s, the line asking 17.5 there).
        if (joined_ && !planned && std::fabs(arc) > settings.grip * tightest) // a plan holds its own speeds (its curvature, braked back): the pursuit's arc wobbles
        {
            const float now = std::max(TurnSpeed(*hull.settings, std::fabs(arc), settings.grip, gripSeen_), 0.5f * topSpeed_); // its old floor: the arc wobbles
            if (now < target) { status_.slowedBy = 'a'; }
            target = std::min(target, now);
            status_.target = target;
        }
        // The rudder's order is a share of what the RUDDER turns it at this speed, not of what its grip lets it: past
        // gripSpeed the grip's circle is two or three times the rudder's, and a share of that put a couple of degrees of
        // error at full rudder -- police_boat sawed hard over from side to side down reef's 400 m straight and lost 3 m/s
        // to it (2026-11-12). bot.steerByRudder 0 = by the grip's (before).
        // Between the two by bot.steerByRudder, in log: 0.5 their geometric mean (the yacht, with twice its rudder, steered too
        // softly by the rudder's alone and lost 38 s a lap on reef).
        const float rudderCurve = std::max(SteadyTurnRate(*hull.settings, halfLength_, rudderLever_, pace) / pace, tightest);
        const float by = std::clamp(settings.steerByRudder, 0.0f, 1.0f);
        const float steerBy = std::exp((1.0f - by) * std::log(std::max(tightest, 1e-4f)) + by * std::log(std::max(rudderCurve, 1e-4f)));
        const float steerGainNow = settings.steerGain / std::max(steerBy, 1e-4f);
        order.rudder = steerGainNow * arc - settings.yawDamping * yawRate_;
        const char* rudderBy = recovering_ ? "coming round" : (planned ? "its plan" : "its line");
        const float rudderAimed = order.rudder;
        // The marks are seen. One ahead that the bow would brush -- the line keeps clear of them, the boat is not always
        // on its line: coming onto the course from outside it, thrown by a sea -- is steered round, on the side its
        // gate's middle lies (dead ahead: away from the mark). A buoy is a kinematic body: hitting one stops the boat.
        {
            const float2 starboard(forward.y, -forward.x);
            // With a plan the marks are already its to keep clear of (rivals that stand still, all along its path), and it
            // crosses a gate 4.5 m from a mark on purpose; this rule then catches only a contact about to happen -- the hull's
            // half beam and 1.5 m, 1.2 s of its speed ahead (bot.markRule). Held to its line's 6 m (bot.margin) it fought the
            // plan at every narrow gate: hard over one way and the other at reef's gate 16 at 13.8 m/s, the boat swung 15-20 m
            // each side through the slalom at 5-8 m/s, and it was steering "round a buoy" at every one of 25 contacts logged
            // (2026-11-13).
            const bool emergency = planned && settings.markRule >= 0.5f;
            const float look = emergency ? std::max(10.0f, 1.2f * std::fabs(speed)) : std::max(25.0f, 2.5f * std::fabs(speed));
            const float clear = hull.halfBeam + (emergency ? 1.5f : settings.margin);
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
                    const bool reachable = across <= 0.5f * gripSeen_ * stats_.lateralAccel * seconds * seconds;
                    const float way = reachable ? middle : (side > 0.0f ? -1.0f : 1.0f);
                    order.rudder += way * 2.5f * (1.0f - std::fabs(side) / clear);
                }
            }
            if (order.rudder != rudderAimed) { rudderBy = "round a buoy"; }
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
                rudderBy = "round a boat";
            }
        }
        const float rudderAvoid = order.rudder - rudderAimed; // round a buoy, round a boat
        order.rudder = std::clamp(order.rudder, -1.0f, 1.0f);
        // The law itself goes to the physics steps (bot.stepSteer): they evaluate it from the hull as each step has it, at their
        // fixed rate. The frame keeps its own evaluation for its rules (the throttle's "hard over") and the readout.
        if (settings.stepSteer >= 0.5f)
        {
            order.steer = true;
            order.aimX = status_.aim.x;
            order.aimZ = status_.aim.y;
            order.reach = std::max(reach, 1.0f);
            order.slip = recovering_ ? 0.0f : (bend > 0.0f ? slide : -slide);
            order.gain = steerGainNow;
            order.yawDamping = settings.yawDamping;
            order.rudderOffset = rudderAvoid;
        }
        // The lead line. Too little water where the bow will be (or a hull, a beach: a depth of nothing): hard over toward
        // the deeper side, the engine on -- it steers by its wash; no water on either side: astern. The line never asks
        // for this; a boat knocked off it does (the yacht, off a buoy, sat on the east beach till Stop).
        const float need = std::max(hull.settings->draft, 0.2f) * 1.5f + 0.6f;
        status_.depthAhead = soundings_[0];
        status_.shoal = soundings_[0] < need;
        if (status_.shoal && reverseSeconds_ <= 0.0f)
        {
            if (std::max(soundings_[1], soundings_[2]) >= need)
            {
                order.rudder = soundings_[2] > soundings_[1] ? 1.0f : -1.0f;
                order.steer = false;
                rudderBy = "shoal: to deeper water";
            }
            else
            {
                reverseSeconds_ = 3.0f;
                reverseRudder_ = error > 0.0f ? -1.0f : 1.0f;
            }
        }
        const float hold = (target / std::max(topSpeed_, 1.0f)) * (target / std::max(topSpeed_, 1.0f));
        const char* throttleBy = "flat out";
        if (target >= 0.97f * topSpeed_) { order.throttle = 1.0f; }
        else
        {
            order.throttle = std::clamp(hold + 0.5f * (target - speed), -0.7f, 1.0f);
            throttleBy = order.throttle < -0.05f ? "brake: astern" : (order.throttle > 0.95f ? "speed up" : "hold the speed");
        }
        // Which rule moved it last: the card says why the throttle is what it is.
        float throttleWas = order.throttle;
        const auto because = [&](const char* rule) {
            if (order.throttle != throttleWas) { throttleBy = rule; }
            throttleWas = order.throttle;
        };
        // With the rudder asked for much it is never put astern: the rudder works by the water past it, the boat's speed AND
        // the propeller's wash -- astern at 23 kn took four fifths of the rudder's push, and police_boat, hard over, went on
        // out of the far turn. At the line's speed or under it the engine is not eased under what holds that speed; over it
        // (the side grip, 2026-11-12: a boat that comes into a bend faster than its grip holds runs wide of it, and the
        // reach sees a bend coming 50 m off, so the rudder is over before the bend is) it is eased to nothing: it coasts.
        // Before the side grip the slide took the speed off in the bend by itself and the circle did not care.
        {
            const float steering = std::clamp((std::fabs(order.rudder) - 0.25f) / 0.35f, 0.0f, 1.0f);
            const float least = speed > target + 0.5f ? 0.0f : std::min(hold, 1.0f);
            order.throttle = std::max(order.throttle, -0.7f + (least + 0.7f) * steering);
            because(least <= 0.0f ? "rudder over: coast, no astern" : "rudder over: hold, no less");
        }
        // Coming round slowly: power on for the wash past the rudder -- what turns a boat at a crawl (coasting at 3.5 m/s with
        // the rudder hard over, police_boat sat 5 s against its turn) -- and nothing past a little over that speed.
        if (recovering_)
        {
            if (speed > target + 2.0f) { order.throttle = std::min(order.throttle, 0.0f); }
            else if (std::fabs(order.rudder) > 0.8f) { order.throttle = std::max(order.throttle, 0.5f); }
            because("coming round");
        }
        // The rudder hard over and still not enough, under gripSpeed: the engine's wash past it is what turns the boat
        // (measured: a circle of 55 m full ahead, 60-69 m at 0.45 throttle), so easing the throttle there only runs it
        // wider. Past gripSpeed the hull lets go before the rudder runs out, and full ahead only takes it wider: the power
        // that holds the line's speed. Only at the line's own speed: a boat that came in faster than the bend allows
        // still slows first.
        if (std::fabs(order.rudder) > 0.92f && speed > 0.0f && (speed < target + (recovering_ ? 0.0f : 1.0f) || status_.shoal) &&
            (speed <= 1.05f * stats_.gripSpeed || status_.shoal))
        {
            order.throttle = 1.0f;
            because("full: rudder short of its grip");
        }
        // And a little over the plan's speed, still not coasting (bot.turnPower): under gripSpeed what turns these boats is
        // the wash, (speed + wash sqrt(throttle))^2 -- the yacht coasting at 6-9 m/s hard over made circles of 55-180 m, at
        // 0.6 and more of the throttle 27-44 m (its turn map). Coasting down to a plan that asked 4 m/s (a bend tighter than
        // it turns) the throttle went 0, 1, 0, 1 about the target + 1 and the yacht circled at 5 m/s 25-37 m off its line
        // for 10 s a gate: reef's T1 and esses, three gates missed in a row after one knock (2026-11-12). Past the target by
        // turnPowerOver it still coasts: a boat that came in too fast slows first.
        else if (!recovering_ && settings.turnPower > 0.0f && std::fabs(order.rudder) > 0.92f && speed > 0.0f && speed <= 1.05f * stats_.gripSpeed &&
                 speed < target + settings.turnPowerOver)
        {
            order.throttle = std::max(order.throttle, std::min(settings.turnPower, 1.0f));
            because("power in the turn");
        }
        // Hard over, at any speed: never less throttle than takes its rudder to what its grip holds here (bot.turnWash). Its
        // plan counts on that turn -- the rudder at full wash or the grip, the less -- and gripSpeed is where the two meet AT
        // FULL THROTTLE: coasting, the rudder has no wash and runs out long before the grip does. The yacht came out of reef's
        // T1 straight at 9-10 m/s, the plan asking 4 and the throttle at 0, its circle 86 m where the grip allows 35, and missed
        // gate 4 (2026-11-13). The least throttle that does it turns as tight as the boat can at this speed; more only speeds
        // it up (and widens a grip-limited circle), less leaves the turn to a rudder without wash.
        status_.washThrottle = settings.turnWash > 0.0f || settings.debug >= 0.5f ? WashToGrip(*hull.settings, speed) : 0.0f;
        if (!recovering_ && settings.turnWash > 0.0f && std::fabs(order.rudder) > 0.92f && speed > 0.0f && reverseSeconds_ <= 0.0f)
        {
            order.throttle = std::max(order.throttle, std::min(status_.washThrottle, 1.0f));
            because("wash to its grip");
        }
        // Aground or against something: astern for a while, the stern swung so the bow comes round to the aim.
        if (reverseSeconds_ > 0.0f)
        {
            reverseSeconds_ -= deltaTime;
            order.throttle = -1.0f;
            order.rudder = reverseRudder_;
            order.steer = false;
            status_.state = State::Reversing;
            throttleBy = "astern: held or aground";
            rudderBy = "astern: stern round";
        }
        else
        {
            status_.state = State::Racing;
            // A hand holding the throttle down (bot.holdThrottle): the bot steers, the engine is flat out whatever the bend --
            // what a player does, to measure a course against.
            if (settings.holdThrottle >= 0.5f)
            {
                order.throttle = 1.0f;
                throttleBy = "held flat out (bot.holdThrottle)";
            }
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
        status_.throttleBy = throttleBy;
        status_.rudderBy = rudderBy;
        return order;
    }
}
