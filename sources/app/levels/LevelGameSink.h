#pragma once
// Architecture plan R6.2b (docs/architecture_review_plan.md, section 11.2, wave 3): what the level loader hands the game.
// The loader reads the level's file and makes the world; the game's part of a level -- the Player Start a session starts
// from, the race course its marks make -- is the game's to read (game::PlayWorld implements this), so the loader knows no
// game type. Every load: BeginLevel, then every object of the file in order, then EndLevel once the file was read to its
// end (a malformed file stops before it, and the course read before stays).
#include <string>

#include "third_party/json/json.hpp"

class LevelGameSink
{
public:
    virtual ~LevelGameSink() = default;
    virtual void BeginLevel(const std::string& path) = 0;
    // `enabled`: the object's own switch (a build without the editor never hands a disabled one).
    virtual void LevelObject(const nlohmann::json& object, bool enabled) = 0;
    virtual void EndLevel() = 0;
};
