#pragma once

#include <map>
#include <string>
#include <vector>

// What a race bot has learned about its boat, kept between sessions (owner, 2026-11-12: "a bot with saved knowledge",
// "make the table"). Not a network: the TURN MAP, the curvature (1/m) of the path its boat actually makes hard over, by its
// speed. The model (game::StatsOf, CanTurn) is still water and the rudder at full wash; on wind_test's sea at ~10 m/s and
// part throttle the yacht made r 65 where the model said 38. Where the map has seen enough it stands in for the model.
// It is the BOAT's (and the sea's), not a course's: what it learned holds on any course. A course memory (a factor on the
// line's speed every 10 m, down where it went wide) was tried the same day and taken out: in five races of two laps it
// cut the misses and slowed every lap, lowering stretches where speed was not the cause; the bot plans by sight instead
// (RacePlanner). One per boat asset and level (the sea is the level's), shared by every bot at the helm of such a boat. A
// JSON file: data/bot_memory/<level>/<boat>.json, written when a race ends and when Play stops.
namespace game
{
    class BotMemory
    {
    public:
        static constexpr float kTurnBin = 1.0f;     // m/s a bin of the turn map
        static constexpr int kTurnBins = 25;        // 0 .. 24 m/s
        static constexpr float kTurnTau = 3.0f;     // s of samples a bin follows over
        static constexpr float kTurnTrusted = 3.0f; // s of samples near a speed for the map to stand in for the model whole

        // A sample of the turn map: hard over at `speed` (m/s along the hull), the path's curvature (1/m), for `seconds`.
        void LearnTurn(float speed, float curvature, float seconds);
        // The map at a speed, between its bins: the curvature and how far to trust it (0 = nothing seen, 1 = kTurnTrusted s).
        float TurnTrust(float speed, float* curvature) const;
        // Moves by one when the map's trusted values have moved past 3 % since it last did (or a bin became trusted):
        // what the bot's line is rebuilt for.
        int Version() const { return version_; }


        // The file: read (nothing there is a memory with nothing in it) and written whole (temp + rename).
        bool Load(const std::string& path);
        bool Save();
        const std::string& Path() const { return path_; }
        bool Dirty() const { return dirty_; }
        void Clear();
        // A line of what it holds, for the log and the readout.
        std::string Describe() const;
        int TrustedBins() const;

    private:
        void Touch(int bin);

        float curvature_[kTurnBins] = {};
        float seconds_[kTurnBins] = {};
        float versionCurvature_[kTurnBins] = {}; // the values the version last moved for
        bool versionTrusted_[kTurnBins] = {};
        int version_ = 0;
        std::string path_;
        bool dirty_ = false;
    };

    // The memory of a boat asset on a level (by their paths), made and read from its file the first time it is asked for;
    // `level` empty = not kept on disk. Lives for the process.
    BotMemory& BotMemoryFor(const std::string& level, const std::string& boat);
    // Every memory that learned something since it was written.
    void SaveBotMemories();
    // Every memory of a level emptied and written so (bot.forget): its bots start again from the model.
    void ForgetBotMemories(const std::string& level);
    // The file a boat's memory on a level is kept in: data/bot_memory/<level's stem>/<boat's stem>.json.
    std::string BotMemoryPath(const std::string& level, const std::string& boat);
}
