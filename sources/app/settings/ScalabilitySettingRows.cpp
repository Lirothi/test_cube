#include "app/settings/Scalability.h"
#include "app/settings/SettingContext.h"
#include "core/settings/SettingsRegistry.h"

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

// --set / --sweep / MCP set_settings names sg.* (the graphics levels, docs/scalability_plan.md), architecture plan R2.2:
// sg.all, sg.resolution, sg.unpin and sg.reload are rows; sg.<group> is a family whose keys are the scalability table's
// groups (data/config/scalability.json) -- the table is their one record. Every name runs App.cpp's old sg. branch as it
// was (ApplyLevels).
namespace settings
{
namespace
{
// The graphics levels: sg.<group>:<0..3> one group, sg.all a preset, sg.resolution the render resolution's level; -1
// leaves it alone again. sg.unpin forgets what --set / MCP pinned, sg.reload reads the table again. Applied at once and
// TRANSIENT, as every --set: the player's choice is saved by the settings menu and the Developer Controls tab
// (Scalability::SaveUser), never by a script.
bool ApplyLevels(std::string_view setting, float value)
{
    settings::Scalability& s = settings::TheScalability();
    const std::string group(setting.substr(3));
    const int level = static_cast<int>(std::lround(value));
    if (group == "reload")
    {
        s.LoadTable();
        s.LoadUser();
    }
    else if (group == "unpin") { s.ClearPins(); }
    else if (!s.Loaded()) { return false; }
    else if (group == "all") { s.SetPreset(level); }
    else if (group == "resolution") { s.SetResolutionLevel(level); }
    else if (const int g = s.GroupIndex(group); g >= 0) { s.SetGroupLevel(g, level); }
    else { return false; }
    s.Apply(std::string(setting));
    return true;
}

const SettingDesc kScalabilityRows[] = {
    TC_SETTING("sg.all", Int, SettingTransient,
               [](SettingContext&, float value) { return ApplyLevels("sg.all", value); },
               [](SettingContext&, float& out) { out = static_cast<float>(TheScalability().Preset()); return true; }),
    TC_SETTING("sg.resolution", Int, SettingTransient,
               [](SettingContext&, float value) { return ApplyLevels("sg.resolution", value); },
               [](SettingContext&, float& out) { out = static_cast<float>(TheScalability().ResolutionLevel()); return true; }),
    TC_SETTING("sg.unpin", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) { return ApplyLevels("sg.unpin", value); },
               nullptr),
    TC_SETTING("sg.reload", Bool, SettingTransient | SettingAction,
               [](SettingContext&, float value) { return ApplyLevels("sg.reload", value); },
               nullptr),
};
TC_SETTINGS(kScalabilityRows);

const SettingFamily kScalabilityFamilies[] = {
    TC_SETTING_FAMILY("sg.", "scalability groups", Int, SettingTransient,
                      [](std::string_view key) { return TheScalability().GroupIndex(std::string(key)) >= 0; },
                      [](SettingContext&, std::string_view key, float value) { return ApplyLevels("sg." + std::string(key), value); },
                      [](SettingContext&, std::string_view key, float& out)
                      {
                          const Scalability& s = TheScalability();
                          const int g = s.GroupIndex(std::string(key));
                          if (g < 0) { return false; }
                          out = static_cast<float>(s.GroupLevel(g));
                          return true;
                      },
                      [](std::vector<std::string>& out)
                      {
                          for (const ScalabilityGroup& g : TheScalability().Table().groups) { out.push_back(g.key); }
                      }),
};
TC_SETTING_FAMILIES(kScalabilityFamilies);
}
}
