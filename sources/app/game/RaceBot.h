#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app/game/BotMemory.h"
#include "app/game/RacePlanner.h"
#include "app/game/RaceTrack.h"
#include "app/game/VesselStats.h"
#include "physics/PhysicsWorld.h"

// A bot at a boat's helm (owner, 2026-11-10: "design a water course, mark it with buoys and write a bot that runs it").
// It knows what a helmsman knows: where its boat is and how it heads (physics::VesselHull, the frame's), the gates, and
// two numbers of its boat -- its top speed, from its thrust and drag, and its beam. It orders what the keys would:
// a throttle and a rudder (physics::World::SetPilot). No depth, no other boats, no waves: the course keeps it off the
// shallows.
namespace game
{
    // --set / MCP bot.<key>, Developer Controls > Physics. One table (BotKeys).
    struct BotSettings
    {
        float enabled = 1.0f;       // 0 = the bots sit still in a race too (a boat that carries "raceBot" floats with its engine off)
        float lookAhead = 1.2f;     // s of the boat's speed the helm aims ahead along the line
        float lookAheadCircle = 0.43f; // of the radius of its boat's own circle, at least: nearer and it weaves, rudder from side to side (the yacht: 24 m of 56)
        float lookAheadMin = 12.0f;  // m, at least, whatever the boat
        float steerGain = 1.5f;     // of the rudder the turn to the aim needs (1 = just what its curvature asks of the boat's tightest)
        float yawDamping = 0.3f;    // rudder per rad/s the boat is already turning: no weaving
        float grip = 0.8f;          // of what its boat makes across its way in its own tightest turn (game::StatsOf: 2.2-4.7 m/s2 for the
                                    // boats here), the most a bend may ask: the line's speed there is sqrt(that / curvature)
        float braking = 0.35f;      // of what its boat loses with the engine off at its top speed, what it counts on to slow down for a bend
        float slip = 1.0f;          // of the angle its boat slides at in the bend ahead (game::SlipAngle), the bow pointed inside the aim by it
        float avoidBoats = 1.0f;    // 1 = a boat ahead that the bow is about to reach is steered round
        float margin = 6.0f;        // m between the hull's side and a mark where the line crosses a gate: a boat holds its line within some 5 m
        float pace = 1.0f;          // of its boat's top speed, the most it allows itself (on the straights), every bot's: on top of its level's
        float easyPace = 0.8f;      // an easy bot's cap, a share of its top speed (its bends are a hard bot's: slowed there it loses its rudder)
        float mediumPace = 0.9f;    // a medium one's (a hard one runs flat out)
        float debug = 0.0f;         // 1 = the gates and the line drawn (green fast, red slow), the aim point
        float holdThrottle = 0.0f;  // 1 = the engine flat out whatever the bend, the bot steering: a player holding the throttle down
        float learn = 1.0f;         // 1 = it fills its boat's turn map (game::BotMemory) and plans by it; 0 = the model alone
        float plan = 1.0f;          // 1 = it drives by what it sees (RacePlanner: the next gates, its boat, the others); 0 = it follows its line
        float planGates = 4.0f;     // gates it looks ahead
        float planMargin = 4.5f;    // m between the hull's side and a mark where its plan crosses a gate (its line keeps `margin`): what the helm and the sea take off it
        float planEvery = 0.1f;     // s between plans
        float gripLearn = 1.0f;     // 1 = its grip share measured hard over (gripSeen); 0 = the model's still-water grip
        float turnPower = 0.6f;     // hard over under its grip speed, the throttle never under this (the wash past the rudder); 0 = it coasts (before)
        float turnPowerOver = 3.0f; // ... while no more than this over the speed it wants (m/s)
        float steerByRudder = 0.0f; // its rudder's order a share of what the rudder turns it (1), of its grip's circle (0), between in log: no winner over 3 boats, 0 kept
        float planLead = 0.5f;      // its throttle's spool time counted ahead of the plan's limits (1 = half its swing; 0 = half a second on): fewer misses for a few s a lap
        float planTurns = 0.0f;     // 1 = the plan chooses its way through each gate too; 0 = only where it crosses (the default: A/B on reef, 6 races, the turns missed more)
        float planBrakeBend = 0.0f; // 1 = its plan counts no engine braking where its rudder will be over (it only coasts there): fewer misses, slower laps -- off
        float planBrake = 0.6f;     // of what its boat loses at a speed to its hull's drag and its engine 0.7 astern: what the plan brakes at
        float rivalClearance = 3.0f; // m between hulls the plan keeps from another boat where it will be
        float forget = 0.0f;        // 1 = every bot memory of the level emptied (and written so) at the next frame of a race; back to 0 by itself
    };
    BotSettings& BotSettingsRef();

    // How well a bot drives: a share of the pace it can run its boat at (BotSettings::easyPace, mediumPace; hard = all).
    enum class BotDifficulty : unsigned char { Easy, Medium, Hard };
    const char* BotDifficultyName(BotDifficulty difficulty);

    // A gate missed by a mile costs no more than this many times one missed at its mark.
    inline constexpr float kMissWidthsMost = 5.0f;

    struct BotKey
    {
        const char* key;
        const char* label;
        const char* hint;
        float BotSettings::* member;
        float speed; // of the drag
        float lo;
        float hi;
        const char* format;
    };
    const std::vector<BotKey>& BotKeys();
    const BotKey* FindBotKey(std::string_view key);
    const char* RaceBotStateName(unsigned char state);

    class RaceBot
    {
    public:
        enum class State : unsigned char { Racing, Reversing, Idle };

        struct Status
        {
            State state = State::Idle;
            int lap = 0;            // whole laps run
            bool started = false;   // gate 1 crossed: the first lap is being run
            int nextGate = 0;       // the number of the gate ahead
            int gatesPassed = 0;
            int gatesMissed = 0;    // gone by outside its marks
            float missedWidths = 0.0f; // the sum over those gates of how far from the gate's middle it went by, in half widths of
                                       // the gate: 1 just outside a mark, kMissWidthsMost at the most. What the penalty is counted in
            float lastMissWidths = 0.0f; // the last one's
            float station = 0.0f;   // m along its line from gate 1
            float speed = 0.0f;     // m/s
            float target = 0.0f;    // m/s the line asks for here
            float crossTrack = 0.0f; // m off the line, + = to starboard
            float lapSeconds = 0.0f; // of the lap being run (0 until gate 1 is first crossed)
            float lastLap = 0.0f;
            float bestLap = 0.0f;
            float tightestRadius = 0.0f; // m, the boat's own circle (game::StatsOf)
            float airborne = 0.0f;       // the share of the last seconds the hull was out of the water
            int memoryTrusted = 0;       // speeds its turn map trusts (BotMemory::TrustedBins)
            bool planned = false;        // it drives by its plan (RacePlanner) this frame
            bool recovering = false;     // pointing the wrong way: coming round slowly
            char slowedBy = 'c';         // what set its speed: c its cap (pace, level), p its plan, l its line, a the turn it needs now, r coming round, j joining
            float planSeconds = 0.0f;    // the plan's: to its last gate, and what it could not do (s)
            float planPenalty = 0.0f;
            float planOffset = 0.0f;     // m off the next gate's middle it plans to cross at
            float gripSeen = 1.0f;       // the share of its boat's side grip (game::GripAccel) it has measured in its own hard turns:
                                         // a sea throws the hull out of the water and takes some (wind_test's 1 m sea: ~0.75)
            float depthAhead = 0.0f;     // m of water where the bow will be in a couple of seconds (the sounding)
            bool shoal = false;          // too little: it is turning from it
            physics::VesselInput order;
            Math::float2 aim;
        };

        void Reset();
        // Its level (kept through Reset: a race gives it before the start). The player's own autopilot is hard.
        void SetDifficulty(BotDifficulty difficulty) { difficulty_ = difficulty; }
        BotDifficulty Difficulty() const { return difficulty_; }
        // Reset, and it stands on a race's grid: on the course from the first frame, gate 1 ahead of it.
        void Start();
        // Its place on the line is looked for afresh (it was not steering for a while); the laps stay.
        void Relocate();
        // The race put its boat back on the line, at this station: it is on the course, its gate ahead as it was.
        void PutBack(float station);
        // m along the line where the gate ahead is crossed.
        float NextGateStation() const { return line_.GateStation(gateIndex_); }
        // What lies under the water ahead, port and starboard of the bow's way (game::Race casts the rays): the water's
        // depth there in m, large over deep water, 0 or less where something stands out of it (a beach, a hull).
        void SetSoundings(float ahead, float port, float starboard);
        // One frame. `hull` is the bot's boat as the frame shows it; the order is what its helm should be told.
        // `drive` false: it only keeps the gates and the laps (somebody else steers: the player's own lap times).
        // `others`: every boat of the world this frame (its own among them, skipped): one in its way is steered round.
        physics::VesselInput Tick(const RaceTrack& track, const physics::VesselHull& hull, float deltaTime, bool drive,
                                  const std::vector<physics::VesselHull>* others = nullptr);
        const Status& GetStatus() const { return status_; }
        const RaceLine& Line() const { return line_; }
        const RacePlan* Plan() const { return planValid_ ? &plan_ : nullptr; }
        const VesselStats& Stats() const { return stats_; }
        // What it learns into and plans by (game::Race hands every bot its boat's, by the level; nullptr = the model alone).
        void SetMemory(BotMemory* memory);

    private:
        void EnsureLine(const RaceTrack& track, const physics::VesselHull& hull);

        RaceLine line_;
        Status status_;
        // What the line was built for.
        float lineMargin_ = -1.0f;
        static constexpr int kLineKeys = 8;
        // grip, braking; the boat's top speed, m/s2 across, braking, circle; grip seen; the turn map's version
        float lineKey_[kLineKeys] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
        float lineBuiltAt_ = -10.0f;     // elapsed_ when it was: what its memory learned comes in at most once a second
        BotMemory* memory_ = nullptr;
        float hardOverSeconds_ = 0.0f;
        // ITS boat's speed for a curvature (TurnSpeed, the model and its turn map), on a log scale of curvature, made with
        // the line: what the line's profile and the planner look up (TurnSpeed itself tries 40 speeds).
        static constexpr int kSpeedTable = 64;
        float speedTable_[kSpeedTable] = {};
        float reachCurvature_ = 0.0f;    // 1/m: the tightest it makes at any speed, a `grip` share of it
        float recoverSpeed_ = 4.0f;      // m/s it turns tightest at: what it comes round at, pointing the wrong way
        bool recovering_ = false;
        float speedBefore_ = 0.0f;       // its speed, followed over half a second: a knock is the speed falling far under it
        float knockSeconds_ = 0.0f;
        void BuildSpeedTable(const physics::VesselSettings& boat, float share, float gripShare);
        float SpeedFor(float curvature) const;
        // What it turns (1/m, a `grip` share) at a speed, from 0 to its top speed: made with the speed table.
        static constexpr int kTurnTable = 32;
        float turnTable_[kTurnTable] = {};
        float TurnAtSpeed(float speed) const;
        // The plan (RacePlanner): what it sees ahead, made every bot.planEvery from where it is.
        void MakePlan(const physics::VesselHull& hull, const std::vector<RaceGate>& gates, const std::vector<physics::VesselHull>* others,
                      float cap);
        RacePlan plan_;
        bool planValid_ = false;
        float planAge_ = 0.0f;           // s since it was made
        std::size_t planGate_ = 0;       // gateIndex_ it was made for
        std::size_t planFirst_ = 0;      // the first gate it goes through (the one ahead, or the next when its way already carries it through that)
        float planAlong_ = 0.0f;         // m along it the boat is   // the rudder hard over: past a second the turn map takes the path's curvature
        // The rate (rad/s) its boat turns at this speed, power on: its rudder's, or `gripShare` of its side grip's (what it
        // has measured).
        float CanTurn(const physics::VesselSettings& boat, float speed, float gripShare) const;
        // The fastest speed (m/s, its top speed down to half of it) at which `share` of what it turns there makes a bend of
        // that curvature; a bend tighter than it makes at any of them: the speed it turns tightest at.
        float TurnSpeed(const physics::VesselSettings& boat, float curvature, float share, float gripShare) const;
        float pathHeading_ = 0.0f;       // the way it moves (not the bow: the hull slides in a turn), and how fast that turns
        float pathRate_ = 0.0f;
        bool pathValid_ = false;
        float gripSeen_ = 1.0f;          // Status::gripSeen. Kept through Reset: what the boat does in this sea is not the race's
        float lineGripSeen_ = 1.0f;      // the one the line's speeds were made for
        float saturatedSeconds_ = 0.0f;  // how long the rudder has been hard over, power on, past gripSpeed
        VesselStats stats_;      // its boat's, from its settings
        float topSpeed_ = 0.0f;
        float halfLength_ = 5.0f;
        float rudderLever_ = 0.0f;

        bool located_ = false;
        bool onGrid_ = false; // Start(): the first frame finds it on the course, before gate 1
        BotDifficulty difficulty_ = BotDifficulty::Hard;
        bool joined_ = false; // it has been on its line: the gates count from then (from its mooring it comes in past the first ones)
        float joinSeconds_ = 0.0f; // how long it has been near its line and heading along it
        std::size_t gateIndex_ = 0;
        float gateSide_ = 0.0f;
        bool gateSideValid_ = false;
        bool started_ = false; // gate 1 crossed once: the clock runs
        float elapsed_ = 0.0f;
        float heading_ = 0.0f;
        bool headingValid_ = false;
        float yawRate_ = 0.0f;
        float stuckSeconds_ = 0.0f;
        float reverseSeconds_ = 0.0f;
        float reverseRudder_ = 0.0f;
        float airborne_ = 0.0f; // the share of the last seconds the hull was out of the water
        float soundings_[3] = { 1.0e3f, 1.0e3f, 1.0e3f }; // ahead, port, starboard (SetSoundings)
    };
}
