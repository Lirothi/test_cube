#pragma once

#include <string>
#include <string_view>
#include <vector>

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
        float pace = 1.0f;          // of the line's speed, every bot's: on top of its own level's
        float easyPace = 0.8f;      // an easy bot's share of the line's speed
        float mediumPace = 0.9f;    // a medium one's (a hard one runs all of it)
        float debug = 0.0f;         // 1 = the gates and the line drawn (green fast, red slow), the aim point
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
        const VesselStats& Stats() const { return stats_; }

    private:
        void EnsureLine(const RaceTrack& track, const physics::VesselHull& hull);

        RaceLine line_;
        Status status_;
        // What the line was built for.
        float lineMargin_ = -1.0f;
        static constexpr int kLineKeys = 6;
        float lineKey_[kLineKeys] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f }; // grip, braking; the boat's top speed, m/s2 across, braking, circle
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
