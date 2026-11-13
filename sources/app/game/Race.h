#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/game/RaceBot.h"
#include "app/game/RaceTrack.h"
#include "physics/PhysicsWorld.h"

class DebugDrawSystem;
class Camera;
class RenderableObjectBase;

// The level's race in a play session (owner, 2026-11-11: "the start zone is lit somehow -- a translucent plane; drive into
// it and you and three bots are put on the grid, a start timer runs and off we go; three laps; after the finish a window
// with the statistics"). The Scene owns it (Scene::RaceRef).
//
//   Free       the boats lie where they are; the start zone before gate 1 is lit. A boat with somebody at its helm that
//              drives into it starts a race: it and up to three boats that carry "raceBot" are put on the grid.
//   Countdown  nobody's helm answers; 3, 2, 1.
//   Racing     the bots run the course, the player theirs. A racer has finished when it has crossed gate 1 once to start
//              its first lap and `laps` times more. The race is the player's: it ends when they finish.
//   Finished   the results are on screen; the bots still out run on to their own finish (90 s at the most). Closing the
//              window frees everybody; the zone waits again once the player's boat has left it.
namespace game
{
    // --set / MCP race.<key>, Developer Controls > Physics. One table (RaceKeys).
    struct RaceSettings
    {
        float laps = 3.0f;        // of a race
        float countdown = 3.0f;   // s on the grid before the start
        float missPenalty = 3.0f; // s added to a racer's time for a gate it went round just outside a mark; times how far from the
                                  // gate's middle it went by, in half widths of the gate (kMissWidthsMost at the most)
        float difficulty = 0.0f;  // the bots' level in the next race: 1 easy, 2 medium, 3 hard, 0 = one of each (turned by one every race)
        float finishWindow = 60.0f; // s the race still runs after the first boat's finish: who is out then did not finish
        float resetOff = 60.0f;     // m off its line ...
        float resetSeconds = 8.0f;  // ... or held (aground, against something, astern) for this long: a boat a bot steers is put back on
                                    // its line, 40 m before its next gate. 0 = never
        float resetPenalty = 10.0f; // s that costs it
        float track = 0.0f;         // the course race.start runs, by its place among the level's (the unnamed first, then by name);
                                    // a boat driven into a course's start zone picks that one itself
        float watch = 0.0f;         // the camera chases this racer (its place on the grid, 1..4; outside a race the N-th boat with a
                                    // bot); 0 = off. To watch a bots' race (owner, 2026-11-11: "hang the camera on one of the bots")
        float drill = 0.0f;         // a gate's number: a corner drill (owner, 2026-11-13): in a race every bot is put drillRun m before that
                                    // gate, at rest, runs through drillGates gates and is put back, again and again -- a corner measured
                                    // a hundred times in minutes where a lap shows it once. 0 = off
        float drillRun = 220.0f;    // m along its line before the drill's first gate it starts at (from rest: room to reach its speed)
        float drillGates = 3.0f;    // gates a passage runs through
        float drillTimeout = 60.0f; // s a passage may take before it counts as lost and starts again
        float solo = 0.0f;          // 1-4: the next race puts only that boat with a bot on the grid (fastest first, as the grid would): a time
                                    // trial, no rivals; the others go back where the level has them. 0 = everyone (owner, 2026-11-12: "build the time trial")
    };
    RaceSettings& RaceSettingsRef();

    // The editor's bot race (Inspector > a selected course, "Bot race"; owner, 2026-11-13: "set how many bots and of which
    // level race, and hang the camera on any of them"): which of the level's bot boats go on the grid, each one's level,
    // whose chase camera. Taken by the first Tick of a session that has the course and its boats, then cleared. Game-side and
    // transient, as race.* are.
    struct RaceGridBoat
    {
        std::string asset;   // the boat's kind: its mesh.json (a copy of it is spawned on the grid)
        int difficulty = 2;  // BotDifficulty: 0 easy, 1 medium, 2 hard
    };
    struct RaceGrid
    {
        bool pending = false;
        std::string track;              // the course's name ("" = the level's first)
        int laps = 3;
        std::vector<RaceGridBoat> boats; // at most 4 race
        int watch = -1;                 // index into boats: whose chase camera; -1 = none
    };

    // A race's bot boat to be made (Race asks, Scene::Sync makes it and answers Race::OnSpawned): a copy of `asset` where the
    // grid has it. The race's boats are its own (owner, 2026-11-13: "the race's boats just spawn; they are not the ones standing
    // at the shore"): made for a race, gone when it is over.
    struct RaceSpawn
    {
        std::uint64_t ticket = 0;
        std::string asset;
        Math::float3 position;
        float headingRad = 0.0f; // the bow's way, as atan2(x, z)
    };
    RaceGrid& RaceGridRef();

    struct RaceKey
    {
        const char* key;
        const char* label;
        const char* hint;
        float RaceSettings::* member;
        float speed;
        float lo;
        float hi;
        const char* format;
    };
    const std::vector<RaceKey>& RaceKeys();
    const RaceKey* FindRaceKey(std::string_view key);

    class Race
    {
    public:
        // Starting: the race's bot boats are being made (Scene::Sync), the grid laid once their hulls are in the world.
        enum class Phase : unsigned char { Free, Starting, Countdown, Racing, Finished };

        // Play starts (or the session goes on in a new level): the course as the level has it now.
        void Begin(const RaceCourse& course);
        // Stop: every pilot off its boat, the race's own boats to go.
        void End(physics::World& world);
        // The level is going (Scene::Clear): nothing of it is held any more.
        void Forget();
        // Scene::Sync, every frame: the boats to make, each answered by OnSpawned (null = it could not be made), and the race's
        // boats to take out of the world. DropSpawned: the level went with them (Scene::Clear).
        std::vector<RaceSpawn> TakeSpawns() { return std::exchange(spawns_, {}); }
        void OnSpawned(std::uint64_t ticket, RenderableObject* vessel);
        std::vector<const RenderableObjectBase*> TakeDespawns() { return std::exchange(despawns_, {}); }
        void DropSpawned()
        {
            spawns_.clear();
            despawns_.clear();
        }
        // Every frame of a session, before the helm reads its keys.
        void Tick(physics::World& world, float deltaTime);
        bool HasTrack() const { return !courses_.empty(); }
        // The course being raced (or the last one picked): its gates.
        const RaceTrack& Track() const { return track_; }
        const std::string& TrackName() const { return trackName_; }
        Phase GetPhase() const { return phase_; }

        // The player's own boat: B hands its helm to the bot and takes it back.
        bool ToggleAutopilot();
        bool Autopilot() const { return autopilot_ && helmDriven_; }
        const physics::VesselInput& HelmOrder() const { return helmOrder_; }
        // On the grid, the countdown running: the player's helm answers nothing.
        bool HoldsHelm() const { return phase_ == Phase::Countdown && playerRaces_; }
        // Once after the boats were put on the grid: the camera cuts with its boat.
        bool TakeCameraCut() { return std::exchange(cameraCut_, false); }

        // A script's (race.start, race.close): a race now -- with the player if they hold a helm, the bots alone if not;
        // the results away.
        void RequestStart() { startAsked_ = true; }
        void CloseResults();

        // The HUD: the race's line, and the countdown's number or "GO!" in the middle of the screen.
        std::wstring HudLine() const;
        std::wstring BigText() const;
        // The results (ImGui, the frame's): true while they are on screen. The mouse is theirs meanwhile.
        bool ResultsOpen() const { return resultsOpen_; }
        void DrawResults();
        // On the grid and in the race's first seconds: over every boat who it is -- "YOU", or a bot's level (ImGui).
        void DrawLabels(const Camera& camera) const;
        // bot.debug: over every bot what it decides and why -- speed and the speed it wants (and who set that), the throttle's
        // and the rudder's rule, its grip, its plan's time and penalty (ImGui).
        void DrawBotCards(const Camera& camera) const;
        std::string ReadoutJson() const;
        // The start zone while it waits (always), the gates and a bot's line (bot.debug).
        void DebugDraw(DebugDrawSystem& draw) const;
        // The boat race.watch names (null = none, or the player holds a helm: their own camera rules), and the last Tick's
        // dt: the spectator's chase camera follows it (Scene::Sync).
        RenderableObject* WatchedVessel() const;
        float LastDeltaTime() const { return lastDelta_; }
        // Tab on foot in Play: the camera on the next racer (the next boat with a bot outside a race), past the last one free
        // again. What it is on now, for the status line ("" = free).
        std::string WatchNext();
        // Who the camera can be hung on, in race.watch's order (the racers by their place on the grid, else the bot boats),
        // each "name" or "name -- P2"; the one it is on (1-based, 0 = none); hang it on the N-th (0 = give it back).
        std::vector<std::string> WatchNames() const;
        int WatchIndex() const;
        void SetWatch(int index);

    private:
        // A bot of the race being run: its own boat, spawned for it (RaceSpawn).
        struct Entry
        {
            std::string asset;               // the boat's kind
            std::uint64_t ticket = 0;        // its spawn's
            int level = -1;                  // BotDifficulty, or -1 = race.difficulty's rule
            bool watched = false;            // the editor's grid hangs the camera on it
            RenderableObject* vessel = nullptr;
            RaceBot bot;
            bool yielded = false;            // the player holds its helm
            bool live = false;               // its boat was in the world this frame
            float waterHeight = 0.0f;
            Math::float3 position;
            std::string name;
        };

        struct Racer
        {
            RenderableObject* vessel = nullptr;
            const RaceBot* bot = nullptr;    // an entry's, or the helm's
            std::string name;
            bool player = false;
            std::vector<float> laps;         // each lap's seconds, the first from the start
            float lapStart = 0.0f;
            float finish = -1.0f;            // the race's clock at its finish; < 0 = still out
            int missed = 0;
            float missedWidths = 0.0f;       // RaceBot::Status::missedWidths at its finish (or now): x missPenalty = its penalty
            float lostSeconds = 0.0f;        // it has been off the course or held for this long
            int difficulty = -1;             // a bot's level (BotDifficulty); -1 = the player
            int resets = 0;                  // times it was put back on its line: x resetPenalty
            float progress = 0.0f;           // m along the course since the start line
            // race.drill: this passage's
            float drillStart = -1.0f;        // the race's clock when it was put at the drill's start; < 0 = not yet
            int drillPrevGate = 0;           // its gate ahead last frame, its misses then
            int drillPrevMissed = 0;
            int drillMisses = 0;
            int drillContacts = 0;
            int drillResetsAt = 0;
            float drillEntry = 0.0f;         // m/s across the drill's first gate
            float drillSpeedAvg = 0.0f;      // over the last half second: a contact is the speed falling far under it next to a mark
            float drillContactAt = -10.0f;
        };
        // race.drill: what each boat did in it since the race started.
        struct DrillStats
        {
            std::string vessel;
            int passages = 0;
            int finished = 0;  // not timed out
            int clean = 0;     // no miss, no contact, not put back
            int contacts = 0;
            int timeouts = 0;
            int resets = 0;
            double seconds = 0.0;  // the finished passages'
            double seconds2 = 0.0;
            double entry = 0.0;
            int missed[8] = {};    // at each drill gate
        };
        std::vector<DrillStats> drillStats_;
        DrillStats& DrillOf(const std::string& vessel);
        void Drill(physics::World& world, float deltaTime);

        // One of the level's courses: its gates and its start zone.
        struct Course
        {
            std::string name; // "" = the level's first
            RaceTrack track;
            RaceStart start;
        };
        // The course the race runs: its gates and zone become track_ / start_, and every bot looks for its line afresh.
        void SelectCourse(std::size_t index);
        // The course whose start zone holds the point; courses_.size() = none.
        std::size_t ZoneAt(const Math::float2& point) const;

        void Sound(physics::World& world, RaceBot& bot, const physics::VesselHull& hull) const;
        // Its boat's memory on this level (game::BotMemoryFor), what every bot at the helm of such a boat learns into.
        BotMemory* MemoryOf(const RenderableObject* vessel) const;
        const physics::VesselHull* HullOf(const RenderableObject* vessel) const;
        // A race asked for: its bot boats asked of the scene (Phase::Starting) -- the grid's, or the fleet's kinds in turn, three
        // beside the player, four alone (race.solo: one). FinishStart lays the grid once their hulls are in the world.
        void BeginStart(const RaceGrid* grid);
        void FinishStart(physics::World& world);
        // The race's boats out of the world (TakeDespawns), their entries gone.
        void DespawnBots(physics::World& world);
        void StopRace(physics::World& world, const char* why);
        void UpdateRacers();
        // The thresholds: a boat a bot steers that is lost (far off its line, or held) is put back on the course.
        void WatchRacers(physics::World& world, float deltaTime);
        RaceBot* BotOf(const Racer& racer);
        // The race still runs: nobody has finished, or the first did less than finishWindow ago.
        bool WindowOpen() const;
        const Racer* PlayerRacer() const;
        int PlaceOf(const Racer& racer) const; // 1 = leading
        float TimeOf(const Racer& racer) const; // its finish with the penalties; < 0 = still out
        std::vector<const Racer*> Standing() const;

        std::vector<Course> courses_;
        std::vector<std::string> fleet_; // RaceCourse::fleet: the kinds of boat the bots race in
        std::vector<RaceSpawn> spawns_;  // asked of the scene this frame
        std::vector<const RenderableObjectBase*> despawns_; // to go: survives Forget (Stop), the scene takes them at its next Sync
        std::uint64_t nextTicket_ = 1;
        float startWait_ = 0.0f;         // s Phase::Starting has waited for the boats
        bool startByGrid_ = false;       // the editor's grid started it: its camera, its levels
        std::string level_;      // RaceCourse::level: whose bot memories
        std::size_t current_ = 0;
        RaceTrack track_;        // courses_[current_]'s
        RaceStart start_;
        std::string trackName_;
        std::string report_;
        float soundSeconds_ = 0.0f; // since the rays were last cast: four times a second (each waits for a step in flight)
        std::vector<physics::VesselHull> hulls_; // a frame's, kept for its capacity
        std::vector<Entry> bots_;
        RaceBot helmBot_;
        RenderableObject* helmVessel_ = nullptr;
        physics::VesselInput helmOrder_;
        bool autopilot_ = false;
        bool helmDriven_ = false; // somebody is at a helm this frame
        float seaLevel_ = 0.0f;   // the water under the boats, smoothed: where the zone and the lines are drawn
        bool seaLevelValid_ = false;
        bool active_ = false;

        Phase phase_ = Phase::Free;
        std::vector<Racer> racers_;
        bool playerRaces_ = false;
        bool armed_ = false;      // the player's boat has been outside the zone since the last race
        bool startAsked_ = false;
        bool cameraCut_ = false;
        bool resultsOpen_ = false;
        bool resultsFresh_ = false; // the window's first frame: it takes the focus
        float countdown_ = 0.0f;
        float clock_ = 0.0f;        // the race's, from the start
        float sinceFinish_ = 0.0f;  // since the player (or the last bot of a race without one) finished
        float firstFinish_ = -1.0f; // the clock at the first boat's finish; < 0 = nobody yet
        float pulse_ = 0.0f;        // the zone's glow
        float missShown_ = 0.0f;    // s the player's last missed gate still says so on screen
        float missShownWidths_ = 1.0f; // and how far from its middle that one was
        int laps_ = 3;              // of the race being run
        int raceCount_ = 0;         // races started this session: the mixed levels turn by one each
        float lastDelta_ = 0.0f;
    };
}
