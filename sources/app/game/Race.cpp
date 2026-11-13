#include "app/game/Race.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

#include "imgui.h"

#include <DirectXMath.h>

#include "app/camera/Camera.h"
#include "core/logging/Log.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "rendering/debug/DebugDraw.h"
#include "rendering/renderables/RenderableObject.h"

namespace game
{
    namespace
    {
        // Where a lost boat is put back: this far before its next gate, m along its line.
        constexpr float kPutBackBefore = 40.0f;
        constexpr float kGoSeconds = 1.2f; // "GO!" stays on screen
        constexpr float kLabelSeconds = 8.0f; // into the race the boats still wear who they are
        constexpr float kMissSeconds = 2.0f; // and so does a missed gate's penalty

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

        // 83.4 -> "1:23.4"; nothing run -> "-:--.-"
        std::string LapText(float seconds)
        {
            if (!(seconds > 0.0f)) { return "-:--.-"; }
            char text[24];
            const int minutes = static_cast<int>(seconds / 60.0f);
            std::snprintf(text, sizeof(text), "%d:%04.1f", minutes, seconds - 60.0f * static_cast<float>(minutes));
            return text;
        }

        std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

        float TopSpeed(const physics::VesselHull& hull)
        {
            return hull.settings ? std::sqrt(std::max(hull.settings->thrust, 1.0f) / std::max(0.77f * hull.settings->surgeDrag, 1.0f)) : 0.0f;
        }

        const char* PhaseName(Race::Phase phase)
        {
            switch (phase)
            {
            case Race::Phase::Starting: return "starting";
            case Race::Phase::Countdown: return "countdown";
            case Race::Phase::Racing: return "racing";
            case Race::Phase::Finished: return "finished";
            default: return "free";
            }
        }
    }

    RaceSettings& RaceSettingsRef()
    {
        static RaceSettings settings;
        return settings;
    }

    RaceGrid& RaceGridRef()
    {
        static RaceGrid grid;
        return grid;
    }

    const std::vector<RaceKey>& RaceKeys()
    {
        using S = RaceSettings;
        static const std::vector<RaceKey> keys = {
            { "laps", "Laps", "Of a race. The next race takes it.", &S::laps, 1.0f, 1.0f, 20.0f, "%.0f" },
            { "countdown", "Countdown (s)", "On the grid, before the start.", &S::countdown, 0.1f, 0.0f, 30.0f, "%.0f" },
            { "difficulty", "Bots' level", "Of the next race's bots: 1 easy, 2 medium, 3 hard (bot.easyPace / mediumPace: their share of the pace). 0 = one of each, turned by one boat every race. Each boat wears its level on the grid.", &S::difficulty, 1.0f, 0.0f, 3.0f, "%.0f" },
            { "finishWindow", "Finish window (s)", "Once the first boat has finished, the race runs this long: who is still out then did not finish.", &S::finishWindow, 1.0f, 5.0f, 600.0f, "%.0f" },
            { "resetOff", "Lost: off the line (m)", "A boat a bot steers (a bot's, or yours on autopilot) that has been further than this from its line -- or held: aground, against something, going astern -- for the time below is put back on its line, 40 m before its next gate.", &S::resetOff, 1.0f, 15.0f, 500.0f, "%.0f" },
            { "resetSeconds", "Lost: for (s)", "How long it has to be lost before it is put back. 0 = never: a boat aground stays there.", &S::resetSeconds, 0.1f, 0.0f, 120.0f, "%.0f" },
            { "resetPenalty", "Put back penalty (s)", "Added to a racer's time each time it is put back on the course.", &S::resetPenalty, 0.1f, 0.0f, 120.0f, "%.0f" },
            { "missPenalty", "Missed gate penalty (s)", "Added to a racer's time for a gate it went round just outside a mark. It grows with how far from the gate's middle the boat went by: twice the gate's half width off costs twice this, five times at the most.", &S::missPenalty, 0.1f, 0.0f, 60.0f, "%.1f" },
            { "track", "Course (race.start)", "Which of the level's courses a race started by a script runs: 0 = the first (the unnamed one), then the named ones by name. A boat driven into a course's start zone picks that course itself.", &S::track, 1.0f, 0.0f, 15.0f, "%.0f" },
            { "watch", "Watch a bot", "The camera chases this racer -- its place on the grid, 1 to 4 (outside a race the N-th boat with a bot). 0 = off. Not while you hold a helm.", &S::watch, 1.0f, 0.0f, 4.0f, "%.0f" },
            { "drill", "Corner drill: gate", "A gate's number: in a race every bot is put 'Drill run-in' metres before that gate, at rest, runs through 'Drill gates' gates and is put back, again and again -- one corner measured a hundred times (the readout's drill: passages, clean ones, misses at each gate, buoy contacts, the time). 0 = off.", &S::drill, 1.0f, 0.0f, 64.0f, "%.0f" },
            { "drillRun", "Drill run-in (m)", "How far along its line before the drill's gate a bot starts, at rest.", &S::drillRun, 5.0f, 20.0f, 1000.0f, "%.0f" },
            { "drillGates", "Drill gates", "How many gates a drill passage runs through.", &S::drillGates, 1.0f, 1.0f, 8.0f, "%.0f" },
            { "drillTimeout", "Drill timeout (s)", "A passage taking longer counts as lost and starts again.", &S::drillTimeout, 1.0f, 5.0f, 600.0f, "%.0f" },
            { "solo", "Time trial: one boat", "1 to 4: the next race puts only that boat with a bot on the grid (fastest first, as the grid would have them) -- a time trial, its laps with no rivals in its way; the others go back where the level has them. 0 = everyone.", &S::solo, 1.0f, 0.0f, 4.0f, "%.0f" },
        };
        return keys;
    }

    const RaceKey* FindRaceKey(std::string_view key)
    {
        for (const RaceKey& entry : RaceKeys())
        {
            if (key == entry.key) { return &entry; }
        }
        return nullptr;
    }

    void Race::Begin(const RaceCourse& course)
    {
        Forget();
        level_ = course.level;
        active_ = true;
        for (const std::string& name : course.TrackNames())
        {
            Course one;
            one.name = name;
            std::string report;
            if (one.track.Build(course, name, report))
            {
                one.start = StartOf(one.track);
                LOG_INFO(logging::LogCategory::App, "race: course '{}': {}; the start zone at {:.1f} {:.1f}", TrackDisplayName(name), report, one.start.center.x,
                         one.start.center.y);
                courses_.push_back(std::move(one));
            }
            else { LOG_WARNING(logging::LogCategory::App, "race: no course '{}' -- {}", TrackDisplayName(name), report); }
            report_ += (report_.empty() ? "" : "; ") + TrackDisplayName(name) + ": " + report;
        }
        fleet_ = course.fleet;
        if (!courses_.empty())
        {
            SelectCourse(0);
            std::string kinds;
            for (const std::string& asset : fleet_) { kinds += (kinds.empty() ? "" : ", ") + asset; }
            LOG_INFO(logging::LogCategory::App, "race: {} course(s); the bots race in {}", courses_.size(), kinds.empty() ? std::string("nothing (no boat carries raceBot)") : kinds);
        }
    }

    void Race::End(physics::World& world)
    {
        SaveBotMemories(); // what the bots learned this session
        DespawnBots(world);
        Forget();
    }

    void Race::DespawnBots(physics::World& world)
    {
        for (Entry& entry : bots_)
        {
            if (!entry.vessel) { continue; }
            world.SetPilot(entry.vessel, nullptr);
            despawns_.push_back(entry.vessel);
        }
        if (!bots_.empty()) { LOG_INFO(logging::LogCategory::App, "race: its {} boat(s) go", bots_.size()); }
        bots_.clear();
        spawns_.clear(); // not made yet: an answer that still comes (OnSpawned) finds no entry and goes too
    }

    void Race::OnSpawned(std::uint64_t ticket, RenderableObject* vessel)
    {
        for (Entry& entry : bots_)
        {
            if (entry.ticket != ticket) { continue; }
            entry.vessel = vessel;
            entry.name = VesselName(vessel);
            if (!vessel) { LOG_WARNING(logging::LogCategory::App, "race: a boat of '{}' could not be made", entry.asset); }
            return;
        }
        if (vessel) { despawns_.push_back(vessel); } // its race went before it came
    }

    BotMemory* Race::MemoryOf(const RenderableObject* vessel) const
    {
        return vessel ? &BotMemoryFor(level_, vessel->GetGeometryPath()) : nullptr;
    }

    void Race::Forget()
    {
        bots_.clear();
        racers_.clear();
        courses_.clear();
        fleet_.clear();
        spawns_.clear();
        startWait_ = 0.0f;
        startByGrid_ = false;
        current_ = 0;
        track_.Clear();
        start_ = RaceStart{};
        trackName_.clear();
        report_.clear();
        helmBot_.Reset();
        helmVessel_ = nullptr;
        helmOrder_ = physics::VesselInput{};
        autopilot_ = false;
        helmDriven_ = false;
        seaLevelValid_ = false;
        active_ = false;
        phase_ = Phase::Free;
        playerRaces_ = false;
        armed_ = false;
        startAsked_ = false;
        cameraCut_ = false;
        resultsOpen_ = false;
        resultsFresh_ = false;
    }

    bool Race::ToggleAutopilot()
    {
        if (!helmDriven_ || !track_.Valid()) { return false; }
        autopilot_ = !autopilot_;
        // In a race the bot goes on from where the player is; out of one it looks for its place on the course afresh.
        if (autopilot_ && phase_ == Phase::Free) { helmBot_.Relocate(); }
        return autopilot_;
    }

    void Race::CloseResults() { resultsOpen_ = false; }

    void Race::SelectCourse(std::size_t index)
    {
        if (index >= courses_.size()) { return; }
        const bool same = index == current_ && track_.Valid();
        current_ = index;
        track_ = courses_[index].track;
        start_ = courses_[index].start;
        trackName_ = courses_[index].name;
        if (same) { return; }
        // Another course: every line was the old one's.
        helmBot_.Reset();
        for (Entry& entry : bots_) { entry.bot.Reset(); }
        LOG_INFO(logging::LogCategory::App, "race: the course is '{}', {} gates", TrackDisplayName(trackName_), track_.Gates().size());
    }

    std::size_t Race::ZoneAt(const Math::float2& point) const
    {
        for (std::size_t i = 0; i < courses_.size(); ++i)
        {
            if (courses_[i].start.Holds(point)) { return i; }
        }
        return courses_.size();
    }

    const physics::VesselHull* Race::HullOf(const RenderableObject* vessel) const
    {
        for (const physics::VesselHull& hull : hulls_)
        {
            if (hull.object == vessel) { return &hull; }
        }
        return nullptr;
    }

    void Race::Sound(physics::World& world, RaceBot& bot, const physics::VesselHull& hull) const
    {
        CPU_SCOPE(ProfilerScopes::kRaceSound);
        // Where the bow will be in a couple of seconds, and 40 degrees to either side of that. Straight down from over
        // the water: the first thing a ray meets -- the sea floor, a beach, a buoy -- is what stands there. Another boat
        // does not count: in a race there is one ahead most of the time, and it is steered round by where it is.
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
            const bool met = world.RayCast(from, Math::float3(0.0f, -1.0f, 0.0f), 30.0f, hit) && !HullOf(hit.object);
            depth[i] = met ? hull.waterHeight - hit.point.y : 1.0e3f;
        }
        bot.SetSoundings(depth[0], depth[1], depth[2]);
    }

    void Race::BeginStart(const RaceGrid* grid)
    {
        RaceSettings& settings = RaceSettingsRef();
        racers_.clear();
        drillStats_.clear();
        laps_ = std::clamp(grid ? grid->laps : static_cast<int>(std::lround(settings.laps)), 1, 20);
        playerRaces_ = helmDriven_;
        startByGrid_ = grid != nullptr;
        // Who: the editor's grid, else the fleet's kinds in turn -- three beside the player, four alone, a time trial one.
        std::vector<Entry> picks;
        if (grid)
        {
            for (std::size_t i = 0; i < grid->boats.size(); ++i)
            {
                Entry entry;
                entry.asset = grid->boats[i].asset;
                entry.level = std::clamp(grid->boats[i].difficulty, 0, 2);
                entry.watched = static_cast<int>(i) == grid->watch;
                picks.push_back(std::move(entry));
            }
        }
        else if (!fleet_.empty())
        {
            const int solo = static_cast<int>(std::lround(settings.solo));
            const int count = playerRaces_ ? 3 : solo >= 1 ? 1 : 4;
            for (int i = 0; i < count; ++i)
            {
                Entry entry;
                entry.asset = fleet_[static_cast<std::size_t>((solo >= 1 && !playerRaces_ ? solo - 1 + i : i)) % fleet_.size()];
                picks.push_back(std::move(entry));
            }
        }
        // Asked of the scene where the grid will have them (FinishStart puts each on its place once its hull is in the world).
        const float heading = std::atan2(start_.along.x, start_.along.y);
        int slot = playerRaces_ ? 1 : 0;
        bots_.clear();
        spawns_.clear();
        for (Entry& entry : picks)
        {
            if (slot >= 4) { break; }
            entry.ticket = nextTicket_++;
            const Math::float2& at = start_.slots[static_cast<std::size_t>(slot)];
            spawns_.push_back(RaceSpawn{ entry.ticket, entry.asset, Math::float3(at.x, seaLevel_, at.y), heading });
            bots_.push_back(std::move(entry));
            ++slot;
        }
        phase_ = Phase::Starting;
        startWait_ = 0.0f;
        LOG_INFO(logging::LogCategory::App, "race: on '{}', {} boat(s) of its own to be made{}", TrackDisplayName(trackName_), bots_.size(),
                 playerRaces_ ? " beside the player's" : "");
    }

    void Race::FinishStart(physics::World& world)
    {
        RaceSettings& settings = RaceSettingsRef();
        const float heading = std::atan2(start_.along.x, start_.along.y);
        int slot = 0;
        const auto place = [&](RenderableObject* vessel, const RaceBot* bot, std::string name, bool player)
        {
            const physics::VesselHull* hull = HullOf(vessel);
            if (!hull || slot >= 4) { return; }
            // Afloat: its keel its draft under the water that stands where it is now (the buoyancy settles the rest).
            const float draft = hull->settings ? hull->settings->draft * hull->scale.y : 0.4f;
            const Math::float2& at = start_.slots[static_cast<std::size_t>(slot)];
            if (!world.PlaceVessel(vessel, Math::float3(at.x, seaLevel_ - draft, at.y), heading)) { return; }
            int same = 1;
            for (const Racer& racer : racers_) { same += racer.name.rfind(name, 0) == 0 ? 1 : 0; }
            if (same > 1) { name += " " + std::to_string(same); }
            Racer racer;
            racer.vessel = vessel;
            racer.bot = bot;
            racer.name = std::move(name);
            racer.player = player;
            racers_.push_back(std::move(racer));
            ++slot;
        };
        // The player on the front row, to port; the bots after them, the faster boat the further forward: nobody starts
        // with a faster boat on their stern.
        if (playerRaces_)
        {
            helmBot_.Start();
            place(helmVessel_, &helmBot_, VesselName(helmVessel_), true);
            cameraCut_ = true;
        }
        std::vector<Entry*> rivals;
        for (Entry& entry : bots_)
        {
            if (entry.vessel && HullOf(entry.vessel)) { rivals.push_back(&entry); }
        }
        std::sort(rivals.begin(), rivals.end(), [this](const Entry* a, const Entry* b)
        {
            const physics::VesselHull* ha = HullOf(a->vessel);
            const physics::VesselHull* hb = HullOf(b->vessel);
            return (ha ? TopSpeed(*ha) : 0.0f) > (hb ? TopSpeed(*hb) : 0.0f);
        });
        // Each bot its level: the grid's, the one asked for, or one of each -- which boat has which turns by one every race.
        const int asked = static_cast<int>(std::lround(settings.difficulty));
        int index = 0;
        int watchPlace = 0;
        for (Entry* entry : rivals)
        {
            if (slot >= 4) { break; }
            const BotDifficulty level = entry->level >= 0 ? static_cast<BotDifficulty>(entry->level)
                                      : asked >= 1 && asked <= 3 ? static_cast<BotDifficulty>(asked - 1) : static_cast<BotDifficulty>((index + raceCount_) % 3);
            entry->bot.Reset();
            entry->bot.Start();
            entry->bot.SetDifficulty(level);
            const std::size_t before = racers_.size();
            place(entry->vessel, &entry->bot, entry->name, false);
            if (racers_.size() > before)
            {
                racers_.back().difficulty = static_cast<int>(level);
                if (entry->watched) { watchPlace = static_cast<int>(racers_.size()); }
                LOG_INFO(logging::LogCategory::App, "race: {} is a {} bot", racers_.back().name, BotDifficultyName(level));
            }
            ++index;
        }
        // The grid says whose chase camera (its place on the grid, as race.watch counts), or none.
        if (startByGrid_) { settings.watch = static_cast<float>(watchPlace); }
        ++raceCount_;
        world.CollectHulls(hulls_); // where the grid has them: this frame's bots stand there
        phase_ = Phase::Countdown;
        countdown_ = std::max(settings.countdown, 0.0f);
        clock_ = 0.0f;
        sinceFinish_ = 0.0f;
        firstFinish_ = -1.0f;
        resultsOpen_ = false;
        LOG_INFO(logging::LogCategory::App, "race: {} boats on the grid ({}), {} laps, the start in {:.0f} s", racers_.size(),
                 playerRaces_ ? "the player's among them" : "bots alone", laps_, countdown_);
    }

    void Race::StopRace(physics::World& world, const char* why)
    {
        for (const Racer& racer : racers_)
        {
            if (!racer.player && racer.vessel && HullOf(racer.vessel)) { world.SetPilot(racer.vessel, nullptr); }
        }
        racers_.clear();
        DespawnBots(world);
        phase_ = Phase::Free;
        playerRaces_ = false;
        resultsOpen_ = false;
        armed_ = false;
        LOG_INFO(logging::LogCategory::App, "race: called off -- {}", why);
    }

    void Race::UpdateRacers()
    {
        for (Racer& racer : racers_)
        {
            if (!racer.bot || racer.finish >= 0.0f) { continue; }
            const RaceBot::Status& status = racer.bot->GetStatus();
            if (racer.player && status.gatesMissed > racer.missed)
            {
                missShown_ = kMissSeconds;
                missShownWidths_ = status.lastMissWidths;
            }
            racer.missed = status.gatesMissed;
            racer.missedWidths = status.missedWidths;
            const float length = racer.bot->Line().Length();
            racer.progress = static_cast<float>(status.lap) * length + (status.started ? status.station : status.station - length);
            while (static_cast<int>(racer.laps.size()) < std::min(status.lap, laps_))
            {
                racer.laps.push_back(clock_ - racer.lapStart);
                racer.lapStart = clock_;
            }
            if (status.lap >= laps_)
            {
                racer.finish = clock_;
                if (firstFinish_ < 0.0f) { firstFinish_ = clock_; }
                racer.progress = static_cast<float>(laps_) * length;
                LOG_INFO(logging::LogCategory::App, "race: {}{} finished in {:.1f} s, {} gate(s) missed (+{:.1f} s)", racer.name, racer.player ? " (the player)" : "",
                         racer.finish, racer.missed, RaceSettingsRef().missPenalty * racer.missedWidths);
            }
        }
    }

    bool Race::WindowOpen() const { return firstFinish_ < 0.0f || clock_ <= firstFinish_ + RaceSettingsRef().finishWindow; }

    RaceBot* Race::BotOf(const Racer& racer)
    {
        if (racer.bot == &helmBot_) { return &helmBot_; }
        for (Entry& entry : bots_)
        {
            if (racer.bot == &entry.bot) { return &entry.bot; }
        }
        return nullptr;
    }

    Race::DrillStats& Race::DrillOf(const std::string& vessel)
    {
        for (DrillStats& stats : drillStats_)
        {
            if (stats.vessel == vessel) { return stats; }
        }
        drillStats_.push_back(DrillStats{});
        drillStats_.back().vessel = vessel;
        return drillStats_.back();
    }

    void Race::Drill(physics::World& world, float deltaTime)
    {
        const RaceSettings& settings = RaceSettingsRef();
        const std::vector<RaceGate>& gates = track_.Gates();
        const int first = static_cast<int>(std::lround(settings.drill));
        if (first < 1 || first > static_cast<int>(gates.size()) || gates.size() < 2) { return; }
        const std::size_t count = gates.size();
        const std::size_t firstIndex = static_cast<std::size_t>(first - 1);
        const int span = std::clamp(static_cast<int>(std::lround(settings.drillGates)), 1, std::min(static_cast<int>(count) - 1, 8));
        const auto drillGate = [&](int number) {
            for (int k = 0; k < span; ++k)
            {
                if (gates[(firstIndex + static_cast<std::size_t>(k)) % count].number == number) { return k; }
            }
            return -1;
        };
        const int after = gates[(firstIndex + static_cast<std::size_t>(span)) % count].number;
        const int entryGate = gates[firstIndex].number;
        bool moved = false;
        for (Racer& racer : racers_)
        {
            if (racer.player || racer.finish >= 0.0f) { continue; }
            RaceBot* bot = BotOf(racer);
            const physics::VesselHull* hull = HullOf(racer.vessel);
            if (!bot || !hull || !bot->Line().Valid()) { continue; }
            const RaceBot::Status& s = bot->GetStatus();
            DrillStats& stats = DrillOf(racer.name);
            bool restart = racer.drillStart < 0.0f;
            if (!restart)
            {
                if (s.gatesMissed > racer.drillPrevMissed)
                {
                    const int k = drillGate(racer.drillPrevGate);
                    if (k >= 0)
                    {
                        ++racer.drillMisses;
                        ++stats.missed[k];
                    }
                }
                if (racer.drillPrevGate == entryGate && s.nextGate != entryGate) { racer.drillEntry = s.speed; }
                // A buoy contact: the speed falling 2.5 m/s under its half-second average with a mark within 7 m of the hull.
                if (racer.drillSpeedAvg - s.speed > 2.5f && clock_ - racer.drillContactAt > 3.0f)
                {
                    float nearest = 1.0e9f;
                    for (const RaceGate& gate : gates)
                    {
                        for (const Math::float2& mark : { gate.a, gate.b })
                        {
                            const Math::float2 d(mark.x - hull->position.x, mark.y - hull->position.z);
                            nearest = std::min(nearest, std::sqrt(d.Dot(d)));
                        }
                    }
                    if (nearest < 7.0f)
                    {
                        ++racer.drillContacts;
                        racer.drillContactAt = clock_;
                    }
                }
                racer.drillSpeedAvg += (s.speed - racer.drillSpeedAvg) * (1.0f - std::exp(-deltaTime / 0.5f));
                const bool done = racer.drillPrevGate != after && s.nextGate == after;
                const bool lost = clock_ - racer.drillStart > settings.drillTimeout;
                if (done || lost)
                {
                    const float seconds = clock_ - racer.drillStart;
                    const int resets = racer.resets - racer.drillResetsAt;
                    ++stats.passages;
                    stats.contacts += racer.drillContacts;
                    stats.resets += resets;
                    if (done)
                    {
                        ++stats.finished;
                        stats.seconds += seconds;
                        stats.seconds2 += static_cast<double>(seconds) * seconds;
                        stats.entry += racer.drillEntry;
                        if (racer.drillMisses == 0 && racer.drillContacts == 0 && resets == 0) { ++stats.clean; }
                    }
                    else { ++stats.timeouts; }
                    LOG_DEBUG(logging::LogCategory::App, "race: drill at gate {}: {} {} in {:.1f} s, {:.1f} m/s across it, {} missed, {} contacts", first, racer.name,
                              done ? "through" : "lost", seconds, racer.drillEntry, racer.drillMisses, racer.drillContacts);
                    restart = true;
                }
            }
            racer.drillPrevGate = s.nextGate;
            racer.drillPrevMissed = s.gatesMissed;
            if (!restart) { continue; }
            // To the drill's start: drillRun m along its line before the first gate, its gate ahead the one after that place.
            const RaceLine& line = bot->Line();
            const float length = line.Length();
            const float station = std::fmod(line.GateStation(firstIndex) - settings.drillRun + 2.0f * length, length);
            std::size_t ahead = firstIndex;
            float nearestAhead = length;
            for (std::size_t g = 0; g < count; ++g)
            {
                const float d = std::fmod(line.GateStation(g) - station + 2.0f * length, length);
                if (d > 0.5f && d < nearestAhead)
                {
                    nearestAhead = d;
                    ahead = g;
                }
            }
            const RaceLine::Sample at = line.At(station);
            const float draft = hull->settings ? hull->settings->draft * hull->scale.y : 0.4f;
            if (!world.PlaceVessel(racer.vessel, Math::float3(at.point.x, seaLevel_ - draft, at.point.y), std::atan2(at.tangent.x, at.tangent.y))) { continue; }
            bot->PutBack(station, ahead);
            racer.drillStart = clock_;
            racer.drillPrevGate = gates[ahead].number;
            racer.drillMisses = 0;
            racer.drillContacts = 0;
            racer.drillEntry = 0.0f;
            racer.drillSpeedAvg = 0.0f;
            racer.drillContactAt = -10.0f;
            racer.drillResetsAt = racer.resets;
            moved = true;
        }
        if (moved) { world.CollectHulls(hulls_); }
    }

    void Race::WatchRacers(physics::World& world, float deltaTime)
    {
        const RaceSettings& settings = RaceSettingsRef();
        if (settings.resetSeconds <= 0.0f) { return; }
        bool moved = false;
        for (Racer& racer : racers_)
        {
            // Only a boat a bot steers: the player at their own helm may go where they like.
            const bool steered = racer.player ? autopilot_ : BotSettingsRef().enabled >= 0.5f;
            RaceBot* bot = BotOf(racer);
            const physics::VesselHull* hull = HullOf(racer.vessel);
            if (racer.finish >= 0.0f || !steered || !bot || !hull || !bot->Line().Valid())
            {
                racer.lostSeconds = 0.0f;
                continue;
            }
            const RaceBot::Status& status = bot->GetStatus();
            // Lost: far off its line, or held -- aground, against a buoy or a boat, backing off something. Not in the
            // race's first seconds: everybody stands on the grid then.
            const bool lost = clock_ > 6.0f && (std::fabs(status.crossTrack) > settings.resetOff || status.state == RaceBot::State::Reversing || std::fabs(status.speed) < 1.0f);
            racer.lostSeconds = lost ? racer.lostSeconds + deltaTime : 0.0f;
            if (racer.lostSeconds < settings.resetSeconds) { continue; }
            racer.lostSeconds = 0.0f;
            const RaceLine& line = bot->Line();
            const float station = std::fmod(bot->NextGateStation() - kPutBackBefore + line.Length(), line.Length());
            const RaceLine::Sample at = line.At(station);
            const float draft = hull->settings ? hull->settings->draft * hull->scale.y : 0.4f;
            const Math::float3 was = hull->position;
            const float off = status.crossTrack;
            const unsigned char state = static_cast<unsigned char>(status.state);
            const int gate = status.nextGate;
            if (!world.PlaceVessel(racer.vessel, Math::float3(at.point.x, seaLevel_ - draft, at.point.y), std::atan2(at.tangent.x, at.tangent.y))) { continue; }
            bot->PutBack(station);
            ++racer.resets;
            moved = true;
            if (racer.player) { cameraCut_ = true; }
            LOG_INFO(logging::LogCategory::App, "race: {} was lost at {:.0f} {:.0f} ({:.0f} m off its line, {}) -- put back before gate {}, +{:.0f} s", racer.name, was.x, was.z,
                     off, RaceBotStateName(state), gate, settings.resetPenalty);
        }
        if (moved) { world.CollectHulls(hulls_); }
    }

    const Race::Racer* Race::PlayerRacer() const
    {
        for (const Racer& racer : racers_)
        {
            if (racer.player) { return &racer; }
        }
        return nullptr;
    }

    float Race::TimeOf(const Racer& racer) const
    {
        const RaceSettings& settings = RaceSettingsRef();
        return racer.finish >= 0.0f ? racer.finish + settings.missPenalty * racer.missedWidths + settings.resetPenalty * static_cast<float>(racer.resets) : -1.0f;
    }

    std::vector<const Race::Racer*> Race::Standing() const
    {
        std::vector<const Racer*> order;
        for (const Racer& racer : racers_) { order.push_back(&racer); }
        std::sort(order.begin(), order.end(), [this](const Racer* a, const Racer* b)
        {
            const float ta = TimeOf(*a);
            const float tb = TimeOf(*b);
            if ((ta >= 0.0f) != (tb >= 0.0f)) { return ta >= 0.0f; } // the finished first
            return ta >= 0.0f ? ta < tb : a->progress > b->progress;
        });
        return order;
    }

    int Race::PlaceOf(const Racer& racer) const
    {
        const std::vector<const Racer*> order = Standing();
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            if (order[i] == &racer) { return static_cast<int>(i) + 1; }
        }
        return 0;
    }

    void Race::Tick(physics::World& world, float deltaTime)
    {
        if (!active_) { return; }
        CPU_SCOPE(ProfilerScopes::kRaceTick);
        lastDelta_ = deltaTime;
        world.CollectHulls(hulls_);
        const BotSettings& settings = BotSettingsRef();
        if (settings.forget >= 0.5f)
        {
            BotSettingsRef().forget = 0.0f;
            ForgetBotMemories(level_);
        }
        pulse_ += deltaTime;
        if (!hulls_.empty())
        {
            float water = 0.0f;
            for (const physics::VesselHull& hull : hulls_) { water += hull.waterHeight; }
            water /= static_cast<float>(hulls_.size());
            seaLevel_ = seaLevelValid_ ? seaLevel_ + (water - seaLevel_) * (1.0f - std::exp(-deltaTime / 4.0f)) : water;
            seaLevelValid_ = true;
        }
        missShown_ = std::max(missShown_ - deltaTime, 0.0f);
        soundSeconds_ += deltaTime;
        const bool sound = track_.Valid() && soundSeconds_ >= 0.25f;
        if (sound) { soundSeconds_ = 0.0f; }

        // The boat the player holds: its gates and laps always, its helm on autopilot (not on the grid, and not past
        // its finish).
        helmDriven_ = false;
        Math::float2 helmAt;
        const Racer* player = PlayerRacer();
        for (const physics::VesselHull& hull : hulls_)
        {
            if (!hull.atHelm) { continue; }
            if (helmVessel_ != hull.object)
            {
                helmBot_.Reset();
                helmVessel_ = hull.object;
                autopilot_ = false;
            }
            helmDriven_ = true;
            helmAt = Math::float2(hull.position.x, hull.position.z);
            const bool drive = autopilot_ && phase_ != Phase::Countdown && !(player && player->finish >= 0.0f);
            helmBot_.SetMemory(MemoryOf(hull.object));
            if (sound && drive) { Sound(world, helmBot_, hull); }
            helmOrder_ = helmBot_.Tick(track_, hull, deltaTime, drive, &hulls_);
        }
        if (!helmDriven_)
        {
            helmVessel_ = nullptr;
            autopilot_ = false;
            helmOrder_ = physics::VesselInput{};
        }

        // The race's own boats (made by the scene, Phase::Starting), and whether the player has taken one of them.
        for (Entry& entry : bots_)
        {
            const physics::VesselHull* hull = entry.vessel ? HullOf(entry.vessel) : nullptr;
            entry.live = hull != nullptr;
            if (!hull) { continue; }
            entry.waterHeight = hull->waterHeight;
            entry.position = hull->position;
            if (hull->atHelm && !entry.yielded) // the player took it: the bot stands down until the helm is free again
            {
                world.SetPilot(entry.vessel, nullptr);
                entry.yielded = true;
            }
            else if (!hull->atHelm && entry.yielded)
            {
                entry.yielded = false;
                entry.bot.Relocate();
            }
        }

        // ---- the race ----
        switch (phase_)
        {
        case Phase::Free:
        {
            // Every course's zone waits: the one the player drives into is the race's. A script's start runs race.track.
            const std::size_t zone = helmDriven_ ? ZoneAt(helmAt) : courses_.size();
            const bool inZone = zone < courses_.size();
            if (!inZone) { armed_ = true; }
            if (inZone && armed_)
            {
                armed_ = false;
                SelectCourse(zone);
                BeginStart(nullptr);
            }
            else if (startAsked_ && !courses_.empty())
            {
                SelectCourse(std::min(static_cast<std::size_t>(std::max(std::lround(RaceSettingsRef().track), 0L)), courses_.size() - 1));
                BeginStart(nullptr);
            }
            else if (RaceGridRef().pending && !courses_.empty())
            {
                // The editor's bot race: its boats, its levels, its camera.
                RaceGrid& grid = RaceGridRef();
                std::size_t index = courses_.size();
                for (std::size_t i = 0; i < courses_.size(); ++i)
                {
                    if (courses_[i].name == grid.track) { index = i; }
                }
                if (index == courses_.size())
                {
                    LOG_WARNING(logging::LogCategory::App, "race: the editor's bot race asked for course '{}', which this level does not have", TrackDisplayName(grid.track));
                }
                else
                {
                    SelectCourse(index);
                    BeginStart(&grid);
                }
                grid.pending = false;
            }
            startAsked_ = false;
            break;
        }
        case Phase::Starting:
        {
            // The boats asked of the scene come a frame or two later (Scene::Sync makes them, their bodies the step after):
            // the grid is laid once every one is in the world, five seconds at the most.
            if (playerRaces_ && !helmDriven_)
            {
                StopRace(world, "the helm was left before the start");
                break;
            }
            startWait_ += deltaTime;
            const bool all = std::all_of(bots_.begin(), bots_.end(), [](const Entry& entry) { return entry.live; });
            if (all || startWait_ > 5.0f) { FinishStart(world); }
            break;
        }
        case Phase::Countdown:
            if (playerRaces_ && !helmDriven_)
            {
                StopRace(world, "the helm was left on the grid");
                break;
            }
            countdown_ -= deltaTime;
            if (countdown_ <= 0.0f)
            {
                phase_ = Phase::Racing;
                clock_ = 0.0f;
                LOG_INFO(logging::LogCategory::App, "race: started");
            }
            break;
        case Phase::Racing:
        {
            clock_ += deltaTime;
            UpdateRacers();
            Drill(world, deltaTime);
            if (playerRaces_ && !helmDriven_)
            {
                StopRace(world, "the helm was left");
                break;
            }
            WatchRacers(world, deltaTime);
            player = PlayerRacer();
            // Decided: the player has finished (the bots alone: all of them) -- or the first boat's finish is a finish
            // window ago, and who is still out did not finish.
            bool decided = player ? player->finish >= 0.0f : true;
            if (!player)
            {
                for (const Racer& racer : racers_) { decided = decided && racer.finish >= 0.0f; }
            }
            decided = decided || !WindowOpen() || clock_ > 3600.0f;
            if (decided)
            {
                phase_ = Phase::Finished;
                SaveBotMemories();
                sinceFinish_ = 0.0f;
                resultsOpen_ = true;
                resultsFresh_ = true;
                if (player) { LOG_INFO(logging::LogCategory::App, "race: the player is {} of {}", PlaceOf(*player), racers_.size()); }
            }
            break;
        }
        case Phase::Finished:
            clock_ += deltaTime;
            sinceFinish_ += deltaTime;
            if (WindowOpen())
            {
                UpdateRacers();
                WatchRacers(world, deltaTime);
            }
            if (!resultsOpen_)
            {
                for (const Racer& racer : racers_)
                {
                    if (!racer.player && racer.vessel && HullOf(racer.vessel)) { world.SetPilot(racer.vessel, nullptr); }
                }
                racers_.clear();
                DespawnBots(world); // the race's own boats go with it
                phase_ = Phase::Free;
                playerRaces_ = false;
                armed_ = false; // the boat lies in the zone, or near it: it waits again once it has left
            }
            break;
        }

        // ---- the bots: at the helm in a race until their own finish, the engine off otherwise ----
        const bool running = phase_ == Phase::Racing || (phase_ == Phase::Finished && WindowOpen());
        for (Entry& entry : bots_)
        {
            if (!entry.live || entry.yielded) { continue; }
            const physics::VesselHull* hull = HullOf(entry.vessel);
            if (!hull) { continue; }
            bool drive = false;
            bool finished = false;
            for (const Racer& racer : racers_)
            {
                if (racer.bot == &entry.bot)
                {
                    finished = racer.finish >= 0.0f;
                    drive = running && !finished && settings.enabled >= 0.5f;
                }
            }
            if (finished) // its engine off, and its gates counted no more: it drifts
            {
                const physics::VesselInput idle;
                world.SetPilot(entry.vessel, &idle);
                continue;
            }
            entry.bot.SetMemory(MemoryOf(entry.vessel));
            if (sound && drive) { Sound(world, entry.bot, *hull); }
            const physics::VesselInput order = entry.bot.Tick(track_, *hull, deltaTime, drive, &hulls_);
            world.SetPilot(entry.vessel, &order);
        }
    }

    std::wstring Race::HudLine() const
    {
        if (!active_ || !track_.Valid()) { return {}; }
        wchar_t line[400];
        const int gates = static_cast<int>(track_.Gates().size());
        const RaceBot::Status& mine = helmBot_.GetStatus();
        const Racer* player = PlayerRacer();
        switch (phase_)
        {
        case Phase::Free:
        {
            if (!helmDriven_) { return {}; }
            const int rivals = fleet_.empty() ? 0 : 3; // made for the race, of the fleet's kinds
            std::wstring names;
            for (const Course& course : courses_) { names += (names.empty() ? L"" : L", ") + Wide(TrackDisplayName(course.name)); }
            std::swprintf(line, sizeof(line) / sizeof(line[0]),
                          L"RACE: drive into a course's lit start zone (%ls) -- %d laps against %d boat%ls   %hs: gate %d of %d   lap %ls   best %ls   %ls",
                          names.c_str(), static_cast<int>(std::lround(RaceSettingsRef().laps)), rivals, rivals == 1 ? L"" : L"s",
                          TrackDisplayName(trackName_).c_str(), mine.nextGate, gates, Wide(LapText(mine.lapSeconds)).c_str(), Wide(LapText(mine.bestLap)).c_str(),
                          autopilot_ ? L"AUTOPILOT, B takes the helm back" : L"B autopilot");
            return line;
        }
        case Phase::Starting:
            std::swprintf(line, sizeof(line) / sizeof(line[0]), L"RACE '%hs': the boats come to the grid", TrackDisplayName(trackName_).c_str());
            return line;
        case Phase::Countdown:
            std::swprintf(line, sizeof(line) / sizeof(line[0]), L"RACE '%hs': on the grid, %d boats, %d laps", TrackDisplayName(trackName_).c_str(),
                          static_cast<int>(racers_.size()), laps_);
            return line;
        case Phase::Racing:
        case Phase::Finished:
        {
            if (player)
            {
                if (player->finish >= 0.0f)
                {
                    std::swprintf(line, sizeof(line) / sizeof(line[0]), L"RACE finished: P%d of %d, %ls (%d gate%ls missed, +%.1f s in it)   Enter closes the results",
                                  PlaceOf(*player), static_cast<int>(racers_.size()), Wide(LapText(TimeOf(*player))).c_str(), player->missed,
                                  player->missed == 1 ? L"" : L"s", RaceSettingsRef().missPenalty * player->missedWidths);
                    return line;
                }
                std::swprintf(line, sizeof(line) / sizeof(line[0]), L"RACE  P%d of %d   lap %d of %d   %ls   lap %ls   best %ls   gate %d of %d   missed %d (+%.1f s)   %ls",
                              PlaceOf(*player), static_cast<int>(racers_.size()), std::min(mine.lap + 1, laps_), laps_, Wide(LapText(clock_)).c_str(),
                              Wide(LapText(clock_ - player->lapStart)).c_str(), Wide(LapText(mine.bestLap)).c_str(), mine.nextGate, gates, player->missed,
                              RaceSettingsRef().missPenalty * player->missedWidths, autopilot_ ? L"AUTOPILOT, B takes the helm back" : L"B autopilot");
                return line;
            }
            const std::vector<const Racer*> order = Standing();
            if (order.empty()) { return {}; }
            const Racer& lead = *order.front();
            std::swprintf(line, sizeof(line) / sizeof(line[0]), L"RACE '%hs' (bots)  %hs leads   lap %d of %d   %ls   %d of %d finished",
                          TrackDisplayName(trackName_).c_str(), lead.name.c_str(), std::min(static_cast<int>(lead.laps.size()) + 1, laps_), laps_,
                          Wide(LapText(clock_)).c_str(),
                          static_cast<int>(std::count_if(racers_.begin(), racers_.end(), [](const Racer& r) { return r.finish >= 0.0f; })),
                          static_cast<int>(racers_.size()));
            std::wstring text = line;
            // race.watch: whose boat the camera is on, and where it lies.
            const RenderableObject* watched = WatchedVessel();
            if (!watched) { text += L"   (Tab: the camera on a boat)"; }
            for (const Racer& racer : racers_)
            {
                if (racer.vessel != watched || !watched) { continue; }
                std::swprintf(line, sizeof(line) / sizeof(line[0]), L"   CAMERA: %hs, P%d, missed %d   (Tab: the next boat)", racer.name.c_str(), PlaceOf(racer),
                              racer.missed);
                text += line;
            }
            return text;
        }
        }
        return {};
    }

    void Race::DrawLabels(const Camera& camera) const
    {
        if (!active_ || racers_.empty()) { return; }
        if (phase_ != Phase::Countdown && !(phase_ == Phase::Racing && clock_ < kLabelSeconds)) { return; }
        ImDrawList* list = ImGui::GetForegroundDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        ImFont* font = ImGui::GetFont();
        const float big = ImGui::GetFontSize() * 1.7f;
        const float fine = ImGui::GetFontSize();
        for (const Racer& racer : racers_)
        {
            const physics::VesselHull* hull = HullOf(racer.vessel);
            if (!hull) { continue; }
            const Math::float3 over(hull->position.x, hull->position.y + 0.3f * hull->halfLength + 2.2f, hull->position.z);
            DirectX::XMFLOAT4 clip{};
            DirectX::XMStoreFloat4(&clip, DirectX::XMVector4Transform(DirectX::XMVectorSet(over.x, over.y, over.z, 1.0f), camera.GetViewProjMatrixNoJitter().xm()));
            if (clip.w <= 0.05f) { continue; }
            const ImVec2 at((clip.x / clip.w * 0.5f + 0.5f) * io.DisplaySize.x, (0.5f - clip.y / clip.w * 0.5f) * io.DisplaySize.y);
            if (at.x < 0.0f || at.x > io.DisplaySize.x || at.y < 0.0f || at.y > io.DisplaySize.y) { continue; }
            const char* level = racer.difficulty < 0 ? "YOU" : (racer.difficulty == 0 ? "EASY" : (racer.difficulty == 1 ? "MEDIUM" : "HARD"));
            const ImU32 colour = racer.difficulty < 0 ? IM_COL32(255, 255, 255, 255)
                                 : (racer.difficulty == 0 ? IM_COL32(110, 230, 120, 255) : (racer.difficulty == 1 ? IM_COL32(250, 215, 70, 255) : IM_COL32(255, 95, 80, 255)));
            const ImVec2 levelSize = font->CalcTextSizeA(big, FLT_MAX, 0.0f, level);
            const ImVec2 nameSize = font->CalcTextSizeA(fine, FLT_MAX, 0.0f, racer.name.c_str());
            const float width = std::max(levelSize.x, nameSize.x) + 16.0f;
            const float height = levelSize.y + nameSize.y + 10.0f;
            const ImVec2 corner(at.x - 0.5f * width, at.y - height);
            list->AddRectFilled(corner, ImVec2(corner.x + width, corner.y + height), IM_COL32(20, 30, 40, 170), 4.0f);
            list->AddText(font, big, ImVec2(at.x - 0.5f * levelSize.x, corner.y + 4.0f), colour, level);
            list->AddText(font, fine, ImVec2(at.x - 0.5f * nameSize.x, corner.y + 5.0f + levelSize.y), IM_COL32(225, 230, 235, 255), racer.name.c_str());
        }
    }

    void Race::DrawBotCards(const Camera& camera) const
    {
        if (!active_ || BotSettingsRef().debug < 0.5f) { return; }
        ImDrawList* list = ImGui::GetForegroundDrawList();
        const ImGuiIO& io = ImGui::GetIO();
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const float line = size + 2.0f;
        const auto slowedText = [](char by) {
            switch (by)
            {
            case 'c': return "its cap";
            case 'p': return "its plan";
            case 'l': return "its line";
            case 'a': return "the turn it needs";
            case 'r': return "coming round";
            case 'j': return "joining";
            default: return "";
            }
        };
        const RenderableObject* watched = WatchedVessel();
        const auto card = [&](const RaceBot& bot, const physics::VesselHull& hull, const std::string& name, bool docked)
        {
            const RaceBot::Status& s = bot.GetStatus();
            const Math::float3 over(hull.position.x, hull.position.y + 0.3f * hull.halfLength + 4.0f, hull.position.z);
            DirectX::XMFLOAT4 clip{};
            DirectX::XMStoreFloat4(&clip, DirectX::XMVector4Transform(DirectX::XMVectorSet(over.x, over.y, over.z, 1.0f), camera.GetViewProjMatrixNoJitter().xm()));
            if (clip.w <= 0.05f) { return; }
            ImVec2 at((clip.x / clip.w * 0.5f + 0.5f) * io.DisplaySize.x, (0.5f - clip.y / clip.w * 0.5f) * io.DisplaySize.y);
            char text[7][128];
            int lines = 0;
            std::snprintf(text[lines++], 128, "%s  gate %d  lap %d", name.c_str(), s.nextGate, s.lap);
            std::snprintf(text[lines++], 128, "%.1f m/s -> %.1f  (%s)", s.speed, s.target, slowedText(s.slowedBy));
            std::snprintf(text[lines++], 128, "throttle %+.2f  %s", s.order.throttle, s.throttleBy);
            std::snprintf(text[lines++], 128, "rudder %+.2f  %s", s.order.rudder, s.rudderBy);
            std::snprintf(text[lines++], 128, "grip %.2f  wash to grip %.2f  off line %+.1f m", s.gripSeen, s.washThrottle, s.crossTrack);
            if (s.planned) { std::snprintf(text[lines++], 128, "plan %.1f s + %.1f s it cannot do, gate at %+.1f m", s.planSeconds, s.planPenalty, s.planOffset); }
            const char* alarm = s.state == RaceBot::State::Reversing ? "ASTERN" : (s.recovering ? "COMING ROUND" : (s.shoal ? "SHOAL AHEAD" : nullptr));
            if (alarm) { std::snprintf(text[lines++], 128, "%s", alarm); }
            float width = 0.0f;
            for (int i = 0; i < lines; ++i) { width = std::max(width, font->CalcTextSizeA(size, FLT_MAX, 0.0f, text[i]).x); }
            width += 16.0f;
            const float height = line * static_cast<float>(lines) + 30.0f;
            // The watched bot's in the bottom-left corner (the chase camera has its boat in the middle, where the card would
            // cover the water ahead); the others' over their boats, under the HUD's lines.
            if (docked) { at = ImVec2(0.5f * width + 12.0f, io.DisplaySize.y - 12.0f); }
            at.x = std::clamp(at.x, 0.5f * width + 4.0f, io.DisplaySize.x - 0.5f * width - 4.0f);
            at.y = std::clamp(at.y, height + 130.0f, io.DisplaySize.y - 4.0f);
            const ImVec2 corner(at.x - 0.5f * width, at.y - height);
            list->AddRectFilled(corner, ImVec2(corner.x + width, corner.y + height), IM_COL32(15, 22, 30, 185), 4.0f);
            for (int i = 0; i < lines; ++i)
            {
                const bool isAlarm = alarm && i == lines - 1;
                const ImU32 colour = i == 0 ? IM_COL32(255, 255, 255, 255) : (isAlarm ? IM_COL32(255, 150, 60, 255) : IM_COL32(215, 225, 235, 255));
                list->AddText(font, size, ImVec2(corner.x + 8.0f, corner.y + 4.0f + line * static_cast<float>(i)), colour, text[i]);
            }
            // Two bars: the throttle (green ahead, red astern) and the rudder (port left, starboard right), from the middle.
            const float barY = corner.y + 8.0f + line * static_cast<float>(lines);
            const float mid = corner.x + 0.5f * width;
            const float half = 0.5f * width - 12.0f;
            list->AddRectFilled(ImVec2(corner.x + 12.0f, barY), ImVec2(corner.x + width - 12.0f, barY + 6.0f), IM_COL32(60, 70, 80, 220));
            const float t = std::clamp(s.order.throttle, -1.0f, 1.0f);
            list->AddRectFilled(ImVec2(std::min(mid, mid + t * half), barY), ImVec2(std::max(mid, mid + t * half), barY + 6.0f),
                                t >= 0.0f ? IM_COL32(90, 220, 110, 255) : IM_COL32(240, 80, 70, 255));
            list->AddRectFilled(ImVec2(corner.x + 12.0f, barY + 10.0f), ImVec2(corner.x + width - 12.0f, barY + 16.0f), IM_COL32(60, 70, 80, 220));
            const float rd = std::clamp(s.order.rudder, -1.0f, 1.0f);
            list->AddRectFilled(ImVec2(std::min(mid, mid + rd * half), barY + 10.0f), ImVec2(std::max(mid, mid + rd * half), barY + 16.0f), IM_COL32(250, 210, 80, 255));
            list->AddLine(ImVec2(mid, barY - 2.0f), ImVec2(mid, barY + 18.0f), IM_COL32(255, 255, 255, 200));
        };
        // The race's own: a boat that waits at its mooring (race.solo, or not on the grid) has nothing to decide.
        for (const Racer& racer : racers_)
        {
            if (!racer.bot || racer.bot->GetStatus().state == RaceBot::State::Idle) { continue; } // a hand at its helm decides for it
            if (const physics::VesselHull* hull = HullOf(racer.vessel)) { card(*racer.bot, *hull, racer.name, racer.vessel == watched); }
        }
    }

    std::wstring Race::BigText() const
    {
        if (!active_) { return {}; }
        if (phase_ == Phase::Countdown) { return std::to_wstring(std::max(static_cast<int>(std::ceil(countdown_)), 1)); }
        if (phase_ == Phase::Racing && clock_ < kGoSeconds) { return L"GO!"; }
        if (phase_ == Phase::Racing && missShown_ > 0.0f)
        {
            wchar_t text[48];
            std::swprintf(text, sizeof(text) / sizeof(text[0]), L"MISSED +%.1f s", RaceSettingsRef().missPenalty * missShownWidths_);
            return text;
        }
        return {};
    }

    void Race::DrawResults()
    {
        if (!resultsOpen_) { return; }
        const ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (resultsFresh_)
        {
            ImGui::SetNextWindowFocus(); // Enter is the window's
            resultsFresh_ = false;
        }
        const RaceSettings& settings = RaceSettingsRef();
        if (ImGui::Begin("Race results", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove))
        {
            ImGui::Text("Course '%s': %d laps, %d gates a lap. A gate missed at its mark costs %.0f s, further off more: up to %.0f s.",
                        TrackDisplayName(trackName_).c_str(), laps_, static_cast<int>(track_.Gates().size()), settings.missPenalty,
                        settings.missPenalty * kMissWidthsMost);
            ImGui::Separator();
            const int columns = 6 + laps_;
            if (ImGui::BeginTable("standing", columns, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit))
            {
                ImGui::TableSetupColumn("#");
                ImGui::TableSetupColumn("Boat");
                ImGui::TableSetupColumn("Time");
                ImGui::TableSetupColumn("Gap");
                for (int lap = 0; lap < laps_; ++lap) { ImGui::TableSetupColumn(("Lap " + std::to_string(lap + 1)).c_str()); }
                ImGui::TableSetupColumn("Best lap");
                ImGui::TableSetupColumn("Missed");
                ImGui::TableHeadersRow();
                const std::vector<const Racer*> order = Standing();
                const float best = order.empty() ? -1.0f : TimeOf(*order.front());
                for (std::size_t i = 0; i < order.size(); ++i)
                {
                    const Racer& racer = *order[i];
                    const float time = TimeOf(racer);
                    ImGui::TableNextRow();
                    if (racer.player) { ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.25f, 0.45f, 0.25f, 0.65f))); }
                    ImGui::TableNextColumn();
                    ImGui::Text("%d", static_cast<int>(i) + 1);
                    ImGui::TableNextColumn();
                    ImGui::Text("%s%s", racer.name.c_str(), racer.player ? "  (you)" : "");
                    if (racer.difficulty >= 0)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", BotDifficultyName(static_cast<BotDifficulty>(racer.difficulty)));
                    }
                    ImGui::TableNextColumn();
                    if (time >= 0.0f) { ImGui::TextUnformatted(LapText(time).c_str()); }
                    else if (!WindowOpen()) { ImGui::TextDisabled("did not finish"); }
                    else { ImGui::TextDisabled("on lap %d", std::min(static_cast<int>(racer.laps.size()) + 1, laps_)); }
                    ImGui::TableNextColumn();
                    if (time >= 0.0f && i > 0 && best >= 0.0f) { ImGui::Text("+%.1f", time - best); }
                    float bestLap = -1.0f;
                    for (int lap = 0; lap < laps_; ++lap)
                    {
                        ImGui::TableNextColumn();
                        if (lap < static_cast<int>(racer.laps.size()))
                        {
                            ImGui::TextUnformatted(LapText(racer.laps[static_cast<std::size_t>(lap)]).c_str());
                            bestLap = bestLap < 0.0f ? racer.laps[static_cast<std::size_t>(lap)] : std::min(bestLap, racer.laps[static_cast<std::size_t>(lap)]);
                        }
                    }
                    ImGui::TableNextColumn();
                    if (bestLap >= 0.0f) { ImGui::TextUnformatted(LapText(bestLap).c_str()); }
                    ImGui::TableNextColumn();
                    if (racer.missed > 0) { ImGui::Text("%d  (+%.1f s)", racer.missed, settings.missPenalty * racer.missedWidths); }
                    else { ImGui::TextUnformatted("0"); }
                    if (racer.resets > 0)
                    {
                        ImGui::SameLine();
                        ImGui::TextDisabled("put back x%d (+%.0f s)", racer.resets, settings.resetPenalty * static_cast<float>(racer.resets));
                    }
                }
                ImGui::EndTable();
            }
            ImGui::Separator();
            const bool close = ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
            ImGui::SameLine();
            ImGui::TextDisabled("Enter. The next race: drive out of the start zone and back into it.");
            if (close) { CloseResults(); }
        }
        ImGui::End();
    }

    std::string Race::ReadoutJson() const
    {
        std::string out = ",\"race\":{";
        char item[900];
        std::snprintf(item, sizeof(item),
                      "\"active\":%s,\"gates\":%zu,\"phase\":\"%s\",\"laps\":%d,\"countdown\":%.2f,\"clock\":%.2f,\"results\":%s,\"armed\":%s,\"autopilot\":%s,\"atHelm\":%s,"
                      "\"zone\":{\"center\":[%.2f,%.2f],\"heading\":%.4f,\"halfLength\":%.1f,\"halfWidth\":%.1f}",
                      active_ ? "true" : "false", track_.Gates().size(), PhaseName(phase_), laps_, std::max(countdown_, 0.0f), clock_, resultsOpen_ ? "true" : "false",
                      armed_ ? "true" : "false", autopilot_ ? "true" : "false", helmDriven_ ? "true" : "false", start_.center.x, start_.center.y,
                      std::atan2(start_.along.x, start_.along.y), start_.halfLength, start_.halfWidth);
        out += item;
        out += ",\"track\":\"" + trackName_ + "\",\"tracks\":[";
        for (std::size_t i = 0; i < courses_.size(); ++i)
        {
            const Course& course = courses_[i];
            std::snprintf(item, sizeof(item), "%s{\"name\":\"%s\",\"gates\":%zu,\"zone\":[%.2f,%.2f]}", i > 0 ? "," : "", course.name.c_str(),
                          course.track.Gates().size(), course.start.center.x, course.start.center.y);
            out += item;
        }
        out += "],\"fleet\":[";
        for (std::size_t i = 0; i < fleet_.size(); ++i) { out += (i > 0 ? ",\"" : "\"") + fleet_[i] + "\""; }
        // The race's own boats as they stand (made for it, gone with it).
        out += "],\"spawned\":" + std::to_string(std::count_if(bots_.begin(), bots_.end(), [](const Entry& entry) { return entry.vessel != nullptr; }));
        const auto status = [&](const char* name, const RaceBot& bot, const Math::float3& position, bool comma)
        {
            const RaceBot::Status& s = bot.GetStatus();
            std::snprintf(item, sizeof(item),
                          "%s{\"vessel\":\"%s\",\"state\":\"%s\",\"lap\":%d,\"started\":%s,\"nextGate\":%d,\"passed\":%d,\"missed\":%d,\"station\":%.1f,\"length\":%.1f,\"speed\":%.2f,"
                          "\"target\":%.2f,\"slowest\":%.2f,\"lineRadius\":%.1f,\"tightestRadius\":%.1f,\"airborne\":%.3f,\"gripSeen\":%.3f,\"memoryTrusted\":%d,\"planned\":%s,\"recovering\":%s,\"throttleBy\":\"%s\",\"rudderBy\":\"%s\",\"slowedBy\":\"%c\",\"planSeconds\":%.2f,\"planPenalty\":%.2f,\"planOffset\":%.2f,\"depthAhead\":%.1f,\"shoal\":%s,\"crossTrack\":%.2f,\"lapSeconds\":%.2f,"
                          "\"lastLap\":%.2f,\"bestLap\":%.2f,\"throttle\":%.3f,\"rudder\":%.3f,\"position\":[%.2f,%.2f,%.2f]}",
                          comma ? "," : "", name, RaceBotStateName(static_cast<unsigned char>(s.state)), s.lap, s.started ? "true" : "false", s.nextGate, s.gatesPassed,
                          s.gatesMissed, s.station, bot.Line().Length(), s.speed, s.target, bot.Line().SlowestSpeed(), bot.Line().TightestRadius(), s.tightestRadius, s.airborne, s.gripSeen, s.memoryTrusted, s.planned ? "true" : "false", s.recovering ? "true" : "false", s.throttleBy, s.rudderBy, s.slowedBy, s.planSeconds, s.planPenalty, s.planOffset,
                          std::min(s.depthAhead, 999.0f), s.shoal ? "true" : "false", s.crossTrack, s.lapSeconds, s.lastLap, s.bestLap, s.order.throttle,
                          s.order.rudder, position.x, position.y, position.z);
            out += item;
            // Its plan as it stands (every 10 m, the first 400 m): where it means to go, the most it may have there and the
            // speed it expects -- what a trace needs to tell a wrong line from a late brake.
            if (const RacePlan* plan = bot.Plan(); plan && !plan->points.empty())
            {
                out.pop_back();
                out += ",\"plan\":[";
                for (std::size_t k = 0, n = 0; k < plan->points.size() && n < 40; k += 4, ++n)
                {
                    std::snprintf(item, sizeof(item), "%s[%.1f,%.1f,%.1f,%.1f]", n > 0 ? "," : "", plan->points[k].x, plan->points[k].y, plan->limit[k], plan->speed[k]);
                    out += item;
                }
                out += "]}";
            }
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
        out += "],\"standing\":[";
        const std::vector<const Racer*> order = Standing();
        for (std::size_t i = 0; i < order.size(); ++i)
        {
            const Racer& racer = *order[i];
            std::string laps;
            for (const float lap : racer.laps)
            {
                char one[24];
                std::snprintf(one, sizeof(one), "%s%.2f", laps.empty() ? "" : ",", lap);
                laps += one;
            }
            std::snprintf(item, sizeof(item), "%s{\"place\":%zu,\"vessel\":\"%s\",\"player\":%s,\"difficulty\":\"%s\",\"finished\":%s,\"finish\":%.2f,\"time\":%.2f,\"missed\":%d,\"penalty\":%.2f,\"resets\":%d,\"progress\":%.1f,\"laps\":[%s]}",
                          i > 0 ? "," : "", i + 1, racer.name.c_str(), racer.player ? "true" : "false",
                          racer.difficulty >= 0 ? BotDifficultyName(static_cast<BotDifficulty>(racer.difficulty)) : "player", racer.finish >= 0.0f ? "true" : "false", racer.finish,
                          TimeOf(racer), racer.missed, RaceSettingsRef().missPenalty * racer.missedWidths + RaceSettingsRef().resetPenalty * static_cast<float>(racer.resets),
                          racer.resets, racer.progress, laps.c_str());
            out += item;
        }
        out += "]";
        if (RaceSettingsRef().drill >= 1.0f)
        {
            std::snprintf(item, sizeof(item), ",\"drill\":{\"gate\":%d,\"run\":%.0f,\"gates\":%d,\"boats\":[", static_cast<int>(std::lround(RaceSettingsRef().drill)),
                          RaceSettingsRef().drillRun, static_cast<int>(std::lround(RaceSettingsRef().drillGates)));
            out += item;
            for (std::size_t i = 0; i < drillStats_.size(); ++i)
            {
                const DrillStats& d = drillStats_[i];
                const double n = std::max(d.finished, 1);
                const double mean = d.seconds / n;
                const double sd = std::sqrt(std::max(d.seconds2 / n - mean * mean, 0.0));
                std::snprintf(item, sizeof(item),
                              "%s{\"vessel\":\"%s\",\"passages\":%d,\"finished\":%d,\"clean\":%d,\"contacts\":%d,\"timeouts\":%d,\"resets\":%d,\"meanSeconds\":%.2f,\"sdSeconds\":%.2f,"
                              "\"meanEntry\":%.2f,\"missed\":[%d,%d,%d,%d,%d,%d,%d,%d]}",
                              i > 0 ? "," : "", d.vessel.c_str(), d.passages, d.finished, d.clean, d.contacts, d.timeouts, d.resets, mean, sd, d.entry / n, d.missed[0], d.missed[1],
                              d.missed[2], d.missed[3], d.missed[4], d.missed[5], d.missed[6], d.missed[7]);
                out += item;
            }
            out += "]}";
        }
        out += "}";
        return out;
    }

    std::vector<std::string> Race::WatchNames() const
    {
        std::vector<std::string> names;
        if (!racers_.empty())
        {
            for (const Racer& racer : racers_)
            {
                const int place = PlaceOf(racer);
                names.push_back(place > 0 && phase_ != Phase::Countdown ? racer.name + " -- P" + std::to_string(place) : racer.name);
            }
            return names;
        }
        for (const Entry& entry : bots_)
        {
            if (entry.live) { names.push_back(entry.name); }
        }
        return names;
    }

    int Race::WatchIndex() const { return WatchedVessel() ? static_cast<int>(std::lround(RaceSettingsRef().watch)) : 0; }

    void Race::SetWatch(int index)
    {
        const int count = static_cast<int>(WatchNames().size());
        RaceSettingsRef().watch = static_cast<float>(index >= 1 && index <= count ? index : 0);
    }

    std::string Race::WatchNext()
    {
        RaceSettings& settings = RaceSettingsRef();
        const int count = static_cast<int>(WatchNames().size());
        const int watch = static_cast<int>(std::lround(settings.watch));
        settings.watch = static_cast<float>(count > 0 && watch < count ? std::max(watch, 0) + 1 : 0);
        const RenderableObject* watched = WatchedVessel();
        if (!watched) { return {}; }
        for (const Racer& racer : racers_)
        {
            if (racer.vessel == watched) { return racer.name; }
        }
        for (const Entry& entry : bots_)
        {
            if (entry.vessel == watched) { return entry.name; }
        }
        return VesselName(watched);
    }

    RenderableObject* Race::WatchedVessel() const
    {
        const int watch = static_cast<int>(std::lround(RaceSettingsRef().watch));
        if (!active_ || watch < 1 || helmDriven_) { return nullptr; }
        if (!racers_.empty()) { return watch <= static_cast<int>(racers_.size()) ? racers_[static_cast<std::size_t>(watch - 1)].vessel : nullptr; }
        int index = 0;
        for (const Entry& entry : bots_)
        {
            if (entry.live && ++index == watch) { return entry.vessel; }
        }
        return nullptr;
    }

    void Race::DebugDraw(DebugDrawSystem& draw) const
    {
        if (!active_ || !track_.Valid()) { return; }
        const float y = seaLevel_ + 0.9f;

        // ---- the start zones, while they wait (every course's: the one driven into is raced), on the grid the race's ----
        // A sheet over the water with a low fence round it and the four places.
        if (phase_ == Phase::Free || phase_ == Phase::Countdown)
        {
            const float glow = 0.5f + 0.5f * std::sin(pulse_ * 2.6f);
            const bool waits = phase_ == Phase::Free && armed_;
            const Math::float3 tint = phase_ == Phase::Countdown ? Math::float3(1.0f, 0.75f, 0.1f) : waits ? Math::float3(0.15f, 1.0f, 0.45f) : Math::float3(0.6f, 0.65f, 0.7f);
            const float strength = waits ? 0.6f + 0.4f * glow : 0.6f;
            const auto zone = [&](const RaceStart& start)
            {
                const Math::quat turn = Math::quat::FromAxisAngle(Math::float3(0.0f, 1.0f, 0.0f), std::atan2(start.along.x, start.along.y));
                const auto at = [&](float along, float across, float up) {
                    const Math::float2 p = start.center + start.along * along + start.starboard * across;
                    return Math::float3(p.x, y + up, p.y);
                };
                draw.AddBox(at(0.0f, 0.0f, 0.0f), Math::float3(start.halfWidth, 0.03f, start.halfLength), turn, Math::float4(tint.x, tint.y, tint.z, 0.20f * strength), false);
                if (phase_ == Phase::Free) // on the grid the camera is inside it: the fence would tint the whole frame
                {
                    const float wall = 1.4f;
                    const Math::float4 fence(tint.x, tint.y, tint.z, 0.16f * strength);
                    draw.AddBox(at(start.halfLength, 0.0f, wall), Math::float3(start.halfWidth, wall, 0.06f), turn, fence, false);
                    draw.AddBox(at(-start.halfLength, 0.0f, wall), Math::float3(start.halfWidth, wall, 0.06f), turn, fence, false);
                    draw.AddBox(at(0.0f, start.halfWidth, wall), Math::float3(0.06f, wall, start.halfLength), turn, fence, false);
                    draw.AddBox(at(0.0f, -start.halfWidth, wall), Math::float3(0.06f, wall, start.halfLength), turn, fence, false);
                }
                for (const Math::float2& slot : start.slots)
                {
                    draw.AddBox(Math::float3(slot.x, y + 0.05f, slot.y), Math::float3(2.2f, 0.03f, 6.0f), turn, Math::float4(1.0f, 1.0f, 1.0f, 0.22f * strength), false);
                }
            };
            if (phase_ == Phase::Free)
            {
                for (const Course& course : courses_) { zone(course.start); }
            }
            else { zone(start_); }
        }

        if (BotSettingsRef().debug < 0.5f) { return; }
        const RaceBot* bot = helmDriven_ ? &helmBot_ : nullptr;
        for (const Entry& entry : bots_)
        {
            if (bot) { break; }
            if (entry.live) { bot = &entry.bot; }
        }
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
            draw.AddLine(Math::float3(a.x, y, a.y), Math::float3(b.x, y, b.y), Math::float4(0.5f + 0.3f * (1.0f - fast), 0.5f + 0.3f * fast, 0.55f, 0.35f)); // its line, dim: the plan drives
        }
        // Every bot's vision (RacePlanner): its plan coloured by the MOST it may have there (red slow, green fast, from 4 m/s to
        // its top speed), a fence of that speed every 7.5 m (0.25 m a m/s) and over it the speed it expects to have (white): where
        // the white line meets the fence tops it brakes. The marks and boats it keeps clear of (orange rings, the boats' 6 s
        // ahead), its aim (yellow, a line from the bow and a mast), its bow (white) and the way it moves (cyan): the slide.
        const auto visionOf = [&](const RaceBot& planner, const physics::VesselHull* hull)
        {
            const float topSpeed = std::max(planner.Stats().topSpeed, 5.0f);
            constexpr float kFence = 0.25f;
            if (const RacePlan* plan = planner.Plan())
            {
                for (std::size_t i = 0; i + 1 < plan->points.size(); ++i)
                {
                    const float fast = std::clamp((plan->limit[i] - 4.0f) / (topSpeed - 4.0f), 0.0f, 1.0f);
                    const Math::float4 colour(1.0f - fast, 0.25f + 0.75f * fast, 0.15f, 1.0f);
                    const Math::float2& a = plan->points[i];
                    const Math::float2& b = plan->points[i + 1];
                    draw.AddLine(Math::float3(a.x, y + 0.3f, a.y), Math::float3(b.x, y + 0.3f, b.y), colour);
                    draw.AddLine(Math::float3(a.x, y + 0.3f + kFence * plan->speed[i], a.y), Math::float3(b.x, y + 0.3f + kFence * plan->speed[i + 1], b.y),
                                 Math::float4(1.0f, 1.0f, 1.0f, 0.9f));
                    if (i % 3 == 0) { draw.AddLine(Math::float3(a.x, y + 0.3f, a.y), Math::float3(a.x, y + 0.3f + kFence * plan->limit[i], a.y), colour); }
                }
            }
            for (const PlanRival& rival : planner.PlanRivals())
            {
                const Math::float4 ring = rival.mark ? Math::float4(1.0f, 0.55f, 0.1f, 0.9f) : Math::float4(1.0f, 0.3f, 0.8f, 0.9f);
                for (int k = 0; k < 20; ++k)
                {
                    const float a0 = 6.2831853f * static_cast<float>(k) / 20.0f;
                    const float a1 = 6.2831853f * static_cast<float>(k + 1) / 20.0f;
                    draw.AddLine(Math::float3(rival.position.x + rival.clearance * std::cos(a0), y + 0.2f, rival.position.y + rival.clearance * std::sin(a0)),
                                 Math::float3(rival.position.x + rival.clearance * std::cos(a1), y + 0.2f, rival.position.y + rival.clearance * std::sin(a1)), ring);
                }
                if (!rival.mark)
                {
                    const Math::float2 there = rival.position + rival.velocity * 6.0f;
                    draw.AddLine(Math::float3(rival.position.x, y + 0.2f, rival.position.y), Math::float3(there.x, y + 0.2f, there.y), ring);
                }
            }
            const Math::float2 aim = planner.GetStatus().aim;
            draw.AddLine(Math::float3(aim.x, y, aim.y), Math::float3(aim.x, y + 3.0f, aim.y), Math::float4(1.0f, 1.0f, 0.2f, 1.0f));
            if (hull)
            {
                const Math::float3 at(hull->position.x, y + 0.5f, hull->position.z);
                draw.AddLine(at, Math::float3(aim.x, y + 0.5f, aim.y), Math::float4(1.0f, 1.0f, 0.2f, 0.7f));
                const Math::float2 bow = Math::float2(hull->forward.x, hull->forward.z).Normalized();
                draw.AddLine(at, Math::float3(at.x + bow.x * 10.0f, at.y, at.z + bow.y * 10.0f), Math::float4(1.0f, 1.0f, 1.0f, 1.0f));
                const Math::float2 way(hull->velocity.x, hull->velocity.z);
                draw.AddLine(at, Math::float3(at.x + way.x * 0.8f, at.y, at.z + way.y * 0.8f), Math::float4(0.2f, 0.9f, 1.0f, 1.0f));
            }
        };
        if (helmDriven_) { visionOf(helmBot_, HullOf(helmVessel_)); }
        for (const Entry& entry : bots_)
        {
            if (entry.live) { visionOf(entry.bot, HullOf(entry.vessel)); }
        }
    }
}
