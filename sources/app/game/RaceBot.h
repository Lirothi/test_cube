#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "app/game/RaceTrack.h"
#include "physics/PhysicsWorld.h"

class DebugDrawSystem;

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
        float enabled = 1.0f;       // 0 = the bots sit still (a boat that carries "raceBot" floats with its engine off)
        float lookAhead = 1.2f;     // s of the boat's speed the helm aims ahead along the line
        float lookAheadMin = 24.0f; // m, at least: nearer than some 0.4 of the boat's circle (55 m) and it weaves, rudder from side to side
        float steerGain = 1.5f;     // of the rudder the turn to the aim needs (1 = just what its curvature asks of the boat's tightest)
        float yawDamping = 0.3f;    // rudder per rad/s the boat is already turning: no weaving
        float lateralAccel = 3.0f;  // m/s2 across its way a bend is taken at: the line's speed there is sqrt(this / curvature). The three boats
                                    // measure 2.9-3.6 flat out and hard over (a circle of 55 m at 13-14 m/s)
        float brakeAccel = 1.5f;    // m/s2 the bot counts on to slow down for a turn (the engine off, the hull's drag)
        float margin = 6.0f;        // m between the hull's side and a mark where the line crosses a gate: a boat holds its line within some 5 m
        float pace = 1.0f;          // of the line's speed: 0.7 = a bot one can beat
        float startDelay = 3.0f;    // s after Play before the engine starts
        float debug = 0.0f;         // 1 = the gates and the line drawn (green fast, red slow), the aim point
    };
    BotSettings& BotSettingsRef();

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

    class RaceBot
    {
    public:
        enum class State : unsigned char { Waiting, Racing, Reversing, Idle };

        struct Status
        {
            State state = State::Idle;
            int lap = 0;            // whole laps run
            int nextGate = 0;       // the number of the gate ahead
            int gatesPassed = 0;
            int gatesMissed = 0;    // gone by outside its marks
            float station = 0.0f;   // m along its line from gate 1
            float speed = 0.0f;     // m/s
            float target = 0.0f;    // m/s the line asks for here
            float crossTrack = 0.0f; // m off the line, + = to starboard
            float lapSeconds = 0.0f; // of the lap being run (0 until gate 1 is first crossed)
            float lastLap = 0.0f;
            float bestLap = 0.0f;
            float tightestRadius = 0.0f; // m, the boat's own at its top speed, hard over
            float airborne = 0.0f;       // the share of the last seconds the hull was out of the water
            float depthAhead = 0.0f;     // m of water where the bow will be in a couple of seconds (the sounding)
            bool shoal = false;          // too little: it is turning from it
            physics::VesselInput order;
            Math::float2 aim;
        };

        void Reset();
        // Its place on the line is looked for afresh (it was not steering for a while); the laps stay.
        void Relocate();
        // What lies under the water ahead, port and starboard of the bow's way (game::Race casts the rays): the water's
        // depth there in m, large over deep water, 0 or less where something stands out of it (a beach, a hull).
        void SetSoundings(float ahead, float port, float starboard);
        // One frame. `hull` is the bot's boat as the frame shows it; the order is what its helm should be told.
        // `drive` false: it only keeps the gates and the laps (somebody else steers: the player's own lap times).
        physics::VesselInput Tick(const RaceTrack& track, const physics::VesselHull& hull, float deltaTime, bool drive);
        const Status& GetStatus() const { return status_; }
        const RaceLine& Line() const { return line_; }

    private:
        void EnsureLine(const RaceTrack& track, const physics::VesselHull& hull);

        RaceLine line_;
        Status status_;
        // What the line was built for.
        float lineMargin_ = -1.0f;
        float lineKey_[3] = { -1.0f, -1.0f, -1.0f }; // lateralAccel, brakeAccel, the boat's top speed
        float topSpeed_ = 0.0f;
        float halfLength_ = 5.0f;

        bool located_ = false;
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

    // The level's race in a play session: the track, a bot for every boat the level says, and one for the boat the
    // player holds the helm of (its laps; its helm too, on autopilot). The Scene owns it (Scene::RaceRef).
    class Race
    {
    public:
        // Play starts (or the session goes on in a new level): the course as the level has it now.
        void Begin(const RaceCourse& course);
        // Stop: every pilot off its boat.
        void End(physics::World& world);
        // The level is going (Scene::Clear): nothing of it is held any more.
        void Forget();
        // Every frame of a session, before the helm reads its keys.
        void Tick(physics::World& world, float deltaTime);
        bool HasTrack() const { return track_.Valid(); }
        const RaceTrack& Track() const { return track_; }

        // The player's own boat: B hands its helm to the bot and takes it back.
        bool ToggleAutopilot();
        bool Autopilot() const { return autopilot_ && helmDriven_; }
        const physics::VesselInput& HelmOrder() const { return helmOrder_; }

        std::wstring HudLine() const;
        std::string ReadoutJson() const;
        void DebugDraw(DebugDrawSystem& draw) const;

    private:
        struct Entry
        {
            Math::float3 where;              // the boat's place in the level
            RenderableObject* vessel = nullptr;
            RaceBot bot;
            bool yielded = false;            // the player holds its helm
            bool live = false;               // its boat was in the world this frame
            float waterHeight = 0.0f;
            Math::float3 position;
            std::string name;
        };

        // The soundings of one boat: three rays straight down ahead of its bow.
        void Sound(physics::World& world, RaceBot& bot, const physics::VesselHull& hull) const;

        RaceTrack track_;
        std::string report_;
        float soundSeconds_ = 0.0f; // since the rays were last cast: four times a second (each waits for a step in flight)
        std::vector<physics::VesselHull> hulls_; // a frame's, kept for its capacity
        std::vector<Entry> bots_;
        RaceBot helmBot_;
        RenderableObject* helmVessel_ = nullptr;
        physics::VesselInput helmOrder_;
        bool autopilot_ = false;
        bool helmDriven_ = false; // somebody is at a helm this frame
        float helmWaterHeight_ = 0.0f;
        bool active_ = false;
    };
}
