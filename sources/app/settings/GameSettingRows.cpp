#include "app/Systems.h"
#include "app/game/PlayWorld.h" // R6.2a
#include "app/game/PlayerStart.h"
#include "app/game/Race.h"
#include "app/game/RaceBot.h"
#include "app/game/VesselCamera.h"
#include "app/scene/Scene.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"
#include "input/InputActions.h"
#include "input/InputManager.h"
#include "physics/CollisionJson.h"
#include "physics/PhysicsWorld.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

// --set / --sweep / MCP set_settings names race.*, bot.*, player.*, vessel.* (the game's numbers, docs/play_mode_plan.md),
// architecture plan R2.2. Each owner's key table is the one record of its knobs (game::RaceKeys, game::BotKeys,
// game::PlayerKeys, physics::VesselKeys); a family is the registry's way to it, as App.cpp's prefix blocks were. The
// keys that are not knobs -- a script's race start, what a script says in place of the player's keys -- are rows.
namespace settings
{
namespace
{
const SettingDesc kGameRows[] = {
    // The race (game::Race): a script's start and the results away.
    TC_SETTING("race.start", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) {
                   game::PlayWorld* play = game::ActivePlayWorld(); // R6.2a: the session's race
                   if (value != 0.0f && play) { play->race.RequestStart(); }
                   return true;
               },
               nullptr),
    TC_SETTING("race.close", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) {
                   game::PlayWorld* play = game::ActivePlayWorld();
                   if (value != 0.0f && play) { play->race.CloseResults(); }
                   return true;
               },
               nullptr),
    // What a script says in place of the keys goes into the keys' own channel (architecture plan R12.3): a press is the
    // next frame's press, held / axis values stay until 0. testMoveX / Z are the WORLD's axes (actions::kMoveWorldX / Z);
    // testThrottle / testRudder / testSprint have actions of their own that only the helm and the player read
    // (kHelmThrottle / kHelmRudder / kScriptSprint): the free camera reads W S / A D, Shift.
    TC_SETTING("player.testMoveX", Float, SettingTransient,
               [](SettingContext&, float value) { Systems::GetInput().InjectAxis(actions::kMoveWorldX, std::clamp(value, -1.0f, 1.0f)); return true; },
               nullptr),
    TC_SETTING("player.testMoveZ", Float, SettingTransient,
               [](SettingContext&, float value) { Systems::GetInput().InjectAxis(actions::kMoveWorldZ, std::clamp(value, -1.0f, 1.0f)); return true; },
               nullptr),
    TC_SETTING("player.testThrottle", Float, SettingTransient,
               [](SettingContext&, float value) { Systems::GetInput().InjectAxis(actions::kHelmThrottle, std::clamp(value, -1.0f, 1.0f)); return true; },
               nullptr),
    TC_SETTING("player.testRudder", Float, SettingTransient,
               [](SettingContext&, float value) { Systems::GetInput().InjectAxis(actions::kHelmRudder, std::clamp(value, -1.0f, 1.0f)); return true; },
               nullptr),
    TC_SETTING("player.testCrouch", Bool, SettingTransient,
               [](SettingContext&, float value) { Systems::GetInput().InjectHeld(actions::kCrouch, value != 0.0f); return true; },
               nullptr),
    TC_SETTING("player.testSprint", Bool, SettingTransient,
               [](SettingContext&, float value) { Systems::GetInput().InjectHeld(actions::kScriptSprint, value != 0.0f); return true; },
               nullptr),
    TC_SETTING("player.testJump", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) { if (value != 0.0f) { Systems::GetInput().InjectPress(actions::kJump); } return true; },
               nullptr),
    TC_SETTING("player.testInteract", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) { if (value != 0.0f) { Systems::GetInput().InjectPress(actions::kInteract); } return true; },
               nullptr),
    TC_SETTING("player.testView", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) { if (value != 0.0f) { Systems::GetInput().InjectPress(actions::kToggleView); } return true; },
               nullptr),
    TC_SETTING("player.testAnchor", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) { if (value != 0.0f) { Systems::GetInput().InjectPress(actions::kAnchor); } return true; },
               nullptr),
    TC_SETTING("player.testAutopilot", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) { if (value != 0.0f) { Systems::GetInput().InjectPress(actions::kAutopilot); } return true; },
               nullptr),
    // Not keys: where a script holds the boat's camera (game::ScriptCamera).
    TC_SETTING("player.testOrbit", Float, SettingTransient,
               [](SettingContext&, float value) { game::ScriptCamera().orbit = value; return true; },
               nullptr),
    TC_SETTING("player.testOrbitPitch", Float, SettingTransient,
               [](SettingContext&, float value) { game::ScriptCamera().orbitPitch = value; return true; },
               nullptr),
    TC_SETTING("player.testFreeCamera", Bool, SettingTransient,
               [](SettingContext&, float value) { game::ScriptCamera().free = value != 0.0f; return true; },
               nullptr),
};
TC_SETTINGS(kGameRows);

const SettingFamily kGameFamilies[] = {
    // The race's numbers by one table (game::RaceKeys).
    TC_SETTING_FAMILY("race.", "game::RaceKeys", Float, SettingTransient,
                      [](std::string_view key) { return game::FindRaceKey(key) != nullptr; },
                      [](SettingContext&, std::string_view key, float value)
                      {
                          const game::RaceKey* entry = game::FindRaceKey(key);
                          game::RaceSettingsRef().*(entry->member) = std::clamp(value, entry->lo, entry->hi);
                          return true;
                      },
                      [](SettingContext&, std::string_view key, float& out)
                      {
                          out = AsFloat(game::RaceSettingsRef().*(game::FindRaceKey(key)->member));
                          return true;
                      },
                      [](std::vector<std::string>& out) { for (const game::RaceKey& k : game::RaceKeys()) { out.emplace_back(k.key); } }),
    // The race's bots (game::RaceBot): one table.
    TC_SETTING_FAMILY("bot.", "game::BotKeys", Float, SettingTransient,
                      [](std::string_view key) { return game::FindBotKey(key) != nullptr; },
                      [](SettingContext&, std::string_view key, float value)
                      {
                          const game::BotKey* entry = game::FindBotKey(key);
                          game::BotSettingsRef().*(entry->member) = std::clamp(value, entry->lo, entry->hi);
                          return true;
                      },
                      [](SettingContext&, std::string_view key, float& out)
                      {
                          out = AsFloat(game::BotSettingsRef().*(game::FindBotKey(key)->member));
                          return true;
                      },
                      [](std::vector<std::string>& out) { for (const game::BotKey& k : game::BotKeys()) { out.emplace_back(k.key); } }),
    // The player (docs/play_mode_plan.md): its numbers by one table.
    TC_SETTING_FAMILY("player.", "game::PlayerKeys", Float, SettingTransient,
                      [](std::string_view key) { return game::FindPlayerKey(key) != nullptr; },
                      [](SettingContext&, std::string_view key, float value)
                      {
                          const game::PlayerKey* entry = game::FindPlayerKey(key);
                          physics::GameSettings().player.*(entry->member) = std::clamp(value, entry->lo, entry->hi);
                          return true;
                      },
                      [](SettingContext&, std::string_view key, float& out)
                      {
                          out = AsFloat(physics::GameSettings().player.*(game::FindPlayerKey(key)->member));
                          return true;
                      },
                      [](std::vector<std::string>& out) { for (const game::PlayerKey& k : game::PlayerKeys()) { out.emplace_back(k.key); } }),
    // The boats (docs/play_mode_plan.md): every vessel in the world, transient. An asset's own numbers are its mesh.json
    // "vessel". No reader: each boat holds its own.
    TC_SETTING_FAMILY("vessel.", "physics::VesselKeys", Float, SettingTransient,
                      [](std::string_view key)
                      {
                          for (const physics::VesselKey& k : physics::VesselKeys())
                          {
                              if (key == k.key) { return true; }
                          }
                          return false;
                      },
                      [](SettingContext& c, std::string_view key, float value) { return c.scene.Physics().TuneVessels(std::string(key), value); },
                      nullptr,
                      [](std::vector<std::string>& out) { for (const physics::VesselKey& k : physics::VesselKeys()) { out.emplace_back(k.key); } }),
};
TC_SETTING_FAMILIES(kGameFamilies);
}
}
