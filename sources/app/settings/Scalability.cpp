#include "app/settings/Scalability.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include "app/settings/UserSettings.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "third_party/json/json.hpp"

namespace settings
{
namespace
{
    using json = nlohmann::json;

    // Every knob the table may name, with what it is (Scalability.h ScalabilityKnob). Content = the level's environment
    // sets it (a ceiling or "level" reads it back first); the class is what a change costs.
    const std::vector<ScalabilityKnob> kKnobs = {
        // View distance
        { "lod.pixelError", false, 'L' }, { "tessy.lodPixelError", false, 'L' }, { "lod.chunkFactor", false, 'L' },
        // Shadows
        { "shadow.mode", false, 'I' }, { "csm.atlasRes", false, 'I' }, { "csm.filterMode", false, 'L' },
        { "sdsm.partitions", false, 'L' }, { "sdsm.cacheFrames", false, 'L' },
        { "vsm.resolutionLodBiasDirectional", false, 'L' }, { "vsm.smrtRayCount", false, 'L' },
        { "vsm.smrtSamplesPerRay", false, 'L' }, { "vsm.localSmrtRayCount", false, 'L' },
        { "shadow.localMaxRes", false, 'I' }, { "contact.enabled", false, 'L' }, { "contact.steps", false, 'L' },
        { "ocean.sunShadowRays", false, 'L' }, { "vsm.windRange", false, 'L' }, { "shadow.localWindRange", false, 'L' },
        // Global illumination
        { "gi.enabled", false, 'L' }, { "gi.far", false, 'L' }, { "gi.rays", false, 'L' }, { "gi.divisor", false, 'L' },
        { "gi.farDivisor", false, 'L' }, { "gi.rayDistance", false, 'L' }, { "skyVis.enabled", false, 'L' },
        { "skyVis.rays", false, 'L' }, { "skyVis.filterRadius", false, 'L' },
        // Reflections
        { "render.reflectionSource", false, 'L' }, { "ssr.ueQuality", false, 'L' }, { "reflections.resolution", false, 'R' },
        { "rt.alphaMode", false, 'L' },
        // Water
        { "reflections.oceanResolution", false, 'R' }, { "ssr.waterNormalCascades", false, 'L' }, { "ocean.wakeHz", false, 'L' },
        // Volumetrics: fog and clouds
        { "fog.gridPixels", false, 'R' }, { "fog.gridZ", false, 'R' }, { "fog.volumetric", true, 'L' },
        { "fog.samplesPerCell", true, 'L' }, { "skyVis.fogRays", false, 'L' }, { "cloud.viewSampleCountMax", true, 'L' },
        // The march takes at least the min whatever the max says (cloud_march.hlsli): a ceiling on the max alone does
        // nothing where the level's min is as high (wind_test 64).
        { "cloud.viewSampleCountMin", true, 'L' },
        { "cloud.shadowMapSampleCount", true, 'L' }, { "cloud.msOctaves", true, 'L' }, { "cloud.captureTexels", false, 'L' },
        // Post-processing
        { "bloom.method", true, 'L' }, { "bloom.convPercent", true, 'L' }, { "bloom.ghostIntensity", true, 'L' },
        { "lightShafts.enabled", true, 'L' }, { "gtao.enabled", true, 'L' }, { "gtao.numSteps", true, 'L' },
        { "gtao.numAngles", true, 'L' },
        // Textures
        { "streaming.poolSizeMB", false, 'L' }, { "streaming.mipBias", false, 'L' },
        // Effects. The boats' spray is capped by runtime ceilings over the ocean's own rate and mist (Scene::Sync), never
        // written into its OceanRenderConfig: the editor saves that config into the level.
        { "spray.rateMax", false, 'L' }, { "spray.mistMax", false, 'L' }, { "particles.spawnScale", false, 'L' },
        { "particles.softWater", false, 'L' },
        // Render resolution
        { "dlss.mode", false, 'R' }, { "aa.renderScale", false, 'R' }, { "aa.fxaa", false, 'L' },
    };

    std::string Lower(std::string s)
    {
        for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        return s;
    }

    int LevelIndex(const ScalabilityTable& table, const std::string& name)
    {
        for (int i = 0; i < kScalabilityLevels; ++i)
        {
            if (Lower(table.levelNames[static_cast<std::size_t>(i)]) == Lower(name)) { return i; }
        }
        return -1;
    }

    bool ParseOp(const json& v, ScalabilityOp& op, std::string& why)
    {
        if (v.is_number())
        {
            op = { ScalabilityOp::Kind::Set, v.get<float>() };
            return true;
        }
        if (v.is_boolean())
        {
            op = { ScalabilityOp::Kind::Set, v.get<bool>() ? 1.0f : 0.0f };
            return true;
        }
        if (v.is_string() && v.get<std::string>() == "level")
        {
            op = { ScalabilityOp::Kind::Level, 0.0f };
            return true;
        }
        if (v.is_object() && v.size() == 1)
        {
            const auto it = v.begin();
            if (!it.value().is_number() && !it.value().is_boolean())
            {
                why = "the value of {\"" + it.key() + "\": ...} is not a number";
                return false;
            }
            const float n = it.value().is_boolean() ? (it.value().get<bool>() ? 1.0f : 0.0f) : it.value().get<float>();
            if (it.key() == "max") { op = { ScalabilityOp::Kind::Max, n }; return true; }
            if (it.key() == "min") { op = { ScalabilityOp::Kind::Min, n }; return true; }
            if (it.key() == "set") { op = { ScalabilityOp::Kind::Set, n }; return true; }
        }
        why = "a value is a number, true/false, \"level\", or {\"max\"|\"min\"|\"set\": n}";
        return false;
    }

    bool ParseCell(const json& j, ScalabilityCell& out, const std::string& where, std::vector<std::string>& errors)
    {
        if (!j.is_object())
        {
            errors.push_back(where + ": not an object of knob: value");
            return false;
        }
        bool ok = true;
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            ScalabilityOp op;
            std::string why;
            if (!ParseOp(it.value(), op, why))
            {
                errors.push_back(where + " " + it.key() + ": " + why);
                ok = false;
                continue;
            }
            out.emplace_back(it.key(), op);
        }
        return ok;
    }

    bool ParseLevels(const ScalabilityTable& table, const json& j, std::array<ScalabilityCell, kScalabilityLevels>& out,
                     const std::string& where, std::vector<std::string>& errors)
    {
        if (!j.is_object())
        {
            errors.push_back(where + ": not an object of level: {knob: value}");
            return false;
        }
        bool ok = true;
        std::array<bool, kScalabilityLevels> seen{};
        for (auto it = j.begin(); it != j.end(); ++it)
        {
            const int level = LevelIndex(table, it.key());
            if (level < 0)
            {
                errors.push_back(where + ": no level \"" + it.key() + "\"");
                ok = false;
                continue;
            }
            seen[static_cast<std::size_t>(level)] = true;
            ok = ParseCell(it.value(), out[static_cast<std::size_t>(level)], where + "." + it.key(), errors) && ok;
        }
        for (int i = 0; i < kScalabilityLevels; ++i)
        {
            if (!seen[static_cast<std::size_t>(i)])
            {
                errors.push_back(where + ": level " + table.levelNames[static_cast<std::size_t>(i)] + " is missing");
                ok = false;
            }
        }
        return ok;
    }

    void ReadLabels(const json& j, std::array<std::string, kScalabilityLevels>& out)
    {
        if (!j.is_array()) { return; }
        for (std::size_t i = 0; i < out.size() && i < j.size(); ++i)
        {
            if (j[i].is_string()) { out[i] = j[i].get<std::string>(); }
        }
    }

    std::set<std::string> CellKeys(const ScalabilityCell& cell)
    {
        std::set<std::string> keys;
        for (const auto& [name, op] : cell) { keys.insert(name); }
        return keys;
    }

    // The levels of a group name the same keys, and each key is a knob of the table: §2.5.
    void ValidateLevels(const std::array<ScalabilityCell, kScalabilityLevels>& levels, const std::string& where,
                        const ScalabilityTable& table, std::map<std::string, std::string>& owner,
                        std::vector<std::string>& errors)
    {
        const std::set<std::string> first = CellKeys(levels[0]);
        for (int i = 1; i < kScalabilityLevels; ++i)
        {
            const std::set<std::string> keys = CellKeys(levels[static_cast<std::size_t>(i)]);
            for (const std::string& k : first)
            {
                if (!keys.count(k))
                {
                    errors.push_back(where + ": " + k + " is set at " + table.levelNames[0] + " but not at " +
                                     table.levelNames[static_cast<std::size_t>(i)] +
                                     " -- a sticky key (every level of a group sets the same knobs)");
                }
            }
            for (const std::string& k : keys)
            {
                if (!first.count(k))
                {
                    errors.push_back(where + ": " + k + " is set at " + table.levelNames[static_cast<std::size_t>(i)] +
                                     " but not at " + table.levelNames[0] + " -- a sticky key");
                }
            }
        }
        for (int i = 0; i < kScalabilityLevels; ++i)
        {
            for (const auto& [name, op] : levels[static_cast<std::size_t>(i)])
            {
                const ScalabilityKnob* knob = FindScalabilityKnob(name);
                if (!knob)
                {
                    errors.push_back(where + "." + table.levelNames[static_cast<std::size_t>(i)] + ": no knob \"" + name +
                                     "\" (Scalability.cpp kKnobs: the --set names the table may use)");
                    continue;
                }
                if (op.kind != ScalabilityOp::Kind::Set && !knob->content)
                {
                    errors.push_back(where + "." + table.levelNames[static_cast<std::size_t>(i)] + ": " + name +
                                     " is not set by the level -- \"level\", max and min are for the level's own knobs");
                }
                if (op.kind != ScalabilityOp::Kind::Level && !std::isfinite(op.value))
                {
                    errors.push_back(where + "." + table.levelNames[static_cast<std::size_t>(i)] + ": " + name + " is not finite");
                }
            }
        }
        for (const std::string& k : first)
        {
            const auto [it, inserted] = owner.emplace(k, where);
            if (!inserted && it->second != where)
            {
                errors.push_back(k + " is in two groups: " + it->second + " and " + where);
            }
        }
    }

    double NowMs()
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::string FormatValue(float v)
    {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", static_cast<double>(v));
        return text;
    }
}

const std::vector<ScalabilityKnob>& ScalabilityKnobs() { return kKnobs; }

const ScalabilityKnob* FindScalabilityKnob(const std::string& name)
{
    for (const ScalabilityKnob& k : kKnobs)
    {
        if (name == k.name) { return &k; }
    }
    return nullptr;
}

namespace
{
    bool ParseScalabilityTableUnguarded(const std::string& text, ScalabilityTable& out, std::vector<std::string>& errors);
}

bool ParseScalabilityTable(const std::string& text, ScalabilityTable& out, std::vector<std::string>& errors)
{
    // A field of the wrong type ("version": "1", a string in "groups") throws out of the JSON library: an error, not a
    // crash at boot or on Reload.
    try
    {
        return ParseScalabilityTableUnguarded(text, out, errors);
    }
    catch (const std::exception& e)
    {
        errors.push_back(std::string("a field of the wrong type: ") + e.what());
        return false;
    }
}

namespace
{
bool ParseScalabilityTableUnguarded(const std::string& text, ScalabilityTable& out, std::vector<std::string>& errors)
{
    const std::size_t before = errors.size();
    const json root = json::parse(text, nullptr, false, /*ignore_comments=*/true);
    if (!root.is_object())
    {
        errors.push_back("not a JSON object");
        return false;
    }
    ScalabilityTable t;
    t.version = root.value("version", 0);
    if (root.contains("levels"))
    {
        const json& names = root["levels"];
        if (!names.is_array() || names.size() != kScalabilityLevels)
        {
            errors.push_back("\"levels\" is an array of " + std::to_string(kScalabilityLevels) + " names");
        }
        else
        {
            for (std::size_t i = 0; i < names.size(); ++i)
            {
                if (names[i].is_string()) { t.levelNames[i] = names[i].get<std::string>(); }
            }
        }
    }
    if (root.contains("defaultLevel"))
    {
        const int level = root["defaultLevel"].is_string() ? LevelIndex(t, root["defaultLevel"].get<std::string>()) : -1;
        if (level < 0) { errors.push_back("\"defaultLevel\" is not one of the levels"); }
        else { t.defaultLevel = level; }
    }
    if (root.contains("resolution"))
    {
        const json& r = root["resolution"];
        t.resolutionLabel = r.value("label", t.resolutionLabel);
        t.resolutionHint = r.value("hint", std::string());
        if (r.contains("levelLabels")) { ReadLabels(r["levelLabels"], t.resolutionLevelLabels); }
        if (!r.contains("dlss") || !r.contains("native"))
        {
            errors.push_back("resolution: needs \"dlss\" and \"native\"");
        }
        else
        {
            ParseLevels(t, r["dlss"], t.resolutionDlss, "resolution.dlss", errors);
            ParseLevels(t, r["native"], t.resolutionNative, "resolution.native", errors);
        }
    }
    if (!root.contains("groups") || !root["groups"].is_array())
    {
        errors.push_back("\"groups\" is an array of groups");
    }
    else
    {
        for (const json& g : root["groups"])
        {
            ScalabilityGroup group;
            group.key = g.value("key", std::string());
            if (group.key.empty())
            {
                errors.push_back("a group without a \"key\"");
                continue;
            }
            group.label = g.value("label", group.key);
            group.hint = g.value("hint", std::string());
            if (g.contains("levelLabels")) { ReadLabels(g["levelLabels"], group.levelLabels); }
            if (!g.contains("levels"))
            {
                errors.push_back("group " + group.key + ": no \"levels\"");
                continue;
            }
            ParseLevels(t, g["levels"], group.levels, "group " + group.key, errors);
            t.groups.push_back(std::move(group));
        }
    }
    if (errors.size() != before) { return false; }
    if (!ValidateScalabilityTable(t, errors)) { return false; }
    out = std::move(t);
    return true;
}
}

bool ValidateScalabilityTable(const ScalabilityTable& table, std::vector<std::string>& errors)
{
    const std::size_t before = errors.size();
    std::map<std::string, std::string> owner;
    std::set<std::string> groupKeys;
    for (const ScalabilityGroup& g : table.groups)
    {
        if (!groupKeys.insert(g.key).second) { errors.push_back("group " + g.key + " twice"); }
        if (g.key == "all" || g.key == "resolution" || g.key == "unpin" || g.key == "reload")
        {
            errors.push_back("group " + g.key + ": the name is sg." + g.key + "'s own");
        }
        ValidateLevels(g.levels, "group " + g.key, table, owner, errors);
    }
    ValidateLevels(table.resolutionDlss, "resolution.dlss", table, owner, errors);
    // The two resolution roads are one choice: a knob of the native one may not be another group's either.
    ValidateLevels(table.resolutionNative, "resolution.native", table, owner, errors);
    return errors.size() == before;
}

bool Scalability::LoadTable()
{
    tableErrors_.clear();
    loaded_ = false;
    std::string text;
    if (!vfs::ReadText(kTablePath, text))
    {
        tableErrors_.push_back(std::string("no ") + kTablePath);
        LOG_WARNING(logging::LogCategory::App, "scalability: no {} -- the graphics levels are off", kTablePath);
        return false;
    }
    ScalabilityTable table;
    if (!ParseScalabilityTable(text, table, tableErrors_))
    {
        for (const std::string& e : tableErrors_)
        {
            LOG_ERROR(logging::LogCategory::App, "scalability: {}: {}", kTablePath, e);
        }
        return false;
    }
    table_ = std::move(table);
    loaded_ = true;
    groupLevels_.assign(table_.groups.size(), -1);
    resolutionLevel_ = -1;
    std::size_t keys = 0;
    for (const ScalabilityGroup& g : table_.groups) { keys += g.levels[0].size(); }
    LOG_INFO(logging::LogCategory::App, "scalability: {} -- {} groups, {} knobs, levels {} / {} / {} / {}", kTablePath,
             table_.groups.size(), keys, table_.levelNames[0], table_.levelNames[1], table_.levelNames[2], table_.levelNames[3]);
    return true;
}

void Scalability::LoadUser()
{
    if (!loaded_) { return; }
    const std::filesystem::path file = UserSettingsPath();
    if (UserSettingsDamaged()) { return; } // a Warning in the log: not managed
    const std::string text = settings::ReadUserSection("scalability");
    const std::string overridesText = settings::ReadUserSection("overrides");
    if (text.empty() && overridesText.empty()) { return; }
    const json parsed = json::parse(text.empty() ? std::string("{}") : text, nullptr, false);
    const json s = parsed.is_object() ? parsed : json::object();
    const json o = json::parse(overridesText.empty() ? std::string("{}") : overridesText, nullptr, false);
    overrides_.clear();
    if (o.is_object())
    {
        for (auto it = o.begin(); it != o.end(); ++it)
        {
            const ScalabilityKnob* knob = FindScalabilityKnob(it.key());
            if (!knob || knob->content || !it.value().is_number())
            {
                LOG_WARNING(logging::LogCategory::App, "scalability: override {} ignored (not a project knob of the levels)", it.key());
                continue;
            }
            overrides_[it.key()] = it.value().get<float>();
        }
    }
    if (s.contains("groups") && s["groups"].is_object())
    {
        for (auto it = s["groups"].begin(); it != s["groups"].end(); ++it)
        {
            const int g = GroupIndex(it.key());
            if (g >= 0 && it.value().is_number_integer())
            {
                groupLevels_[static_cast<std::size_t>(g)] = std::clamp(it.value().get<int>(), -1, kScalabilityLevels - 1);
            }
        }
    }
    if (s.contains("resolution") && s["resolution"].is_number_integer())
    {
        resolutionLevel_ = std::clamp(s["resolution"].get<int>(), -1, kScalabilityLevels - 1);
    }
    LOG_INFO(logging::LogCategory::App, "scalability: {} -- preset {}, resolution {}, {} override(s) by hand", vfs::Narrow(file.wstring()),
             Preset() >= 0 ? table_.levelNames[static_cast<std::size_t>(Preset())] : Preset() == -1 ? "Custom" : "not managed",
             resolutionLevel_ >= 0 ? table_.levelNames[static_cast<std::size_t>(resolutionLevel_)] : "not managed",
             overrides_.size());
}

bool Scalability::SaveUser(std::string& why) const
{
    if (!loaded_)
    {
        why = "the table did not load: the saved levels are kept as they are";
        return false;
    }
    json groups = json::object();
    for (std::size_t g = 0; g < table_.groups.size(); ++g)
    {
        if (groupLevels_[g] >= 0) { groups[table_.groups[g].key] = groupLevels_[g]; }
    }
    const json levels = { { "groups", groups }, { "resolution", resolutionLevel_ } };
    json overrides = json::object();
    for (const auto& [name, value] : overrides_) { overrides[name] = value; }
    return settings::WriteUserSections({ { "scalability", levels.dump() },
                                         { "overrides", overrides.empty() ? std::string() : overrides.dump() } }, why);
}

std::string Scalability::ReadUserSection(const std::string& key) { return settings::ReadUserSection(key); }

bool Scalability::WriteUserSection(const std::string& key, const std::string& jsonText, std::string& why)
{
    return settings::WriteUserSection(key, jsonText, why);
}

bool Scalability::ManagedProjectKnob(const std::string& name) const
{
    const ScalabilityKnob* knob = FindScalabilityKnob(name);
    if (!knob || knob->content) { return false; }
    for (std::size_t g = 0; g < table_.groups.size(); ++g)
    {
        if (groupLevels_[g] < 0) { continue; }
        for (const auto& [n, op] : table_.groups[g].levels[static_cast<std::size_t>(groupLevels_[g])])
        {
            if (n == name) { return true; }
        }
    }
    if (const ScalabilityCell* cell = ResolutionCell(resolutionLevel_))
    {
        for (const auto& [n, op] : *cell)
        {
            if (n == name) { return true; }
        }
    }
    return false;
}

std::map<std::string, float> Scalability::ManagedValues() const
{
    std::map<std::string, float> out;
    for (const auto& [name, value] : applied_)
    {
        float current = 0.0f;
        if (ManagedProjectKnob(name) && Read(name, current)) { out[name] = current; }
    }
    return out;
}

bool Scalability::NoteManualValues()
{
    bool changed = false;
    for (auto& [name, applied] : applied_)
    {
        if (pinned_.count(name) || !ManagedProjectKnob(name)) { continue; }
        float current = 0.0f;
        if (!Read(name, current) || current == applied) { continue; }
        const auto it = overrides_.find(name);
        if (it == overrides_.end() || it->second != current)
        {
            overrides_[name] = current;
            changed = true;
            LOG_INFO(logging::LogCategory::App, "scalability: {} set by hand to {} over its level ({}) -- an override",
                     name, FormatValue(current), FormatValue(applied));
        }
        applied = current;
    }
    return changed;
}

void Scalability::AfterReset(const std::map<std::string, float>& before)
{
    for (const auto& [name, was] : before)
    {
        float current = 0.0f;
        if (pinned_.count(name) || !Read(name, current) || current == was) { continue; }
        // The reset put the build's default in: that is now the knob's own value, and the level's is written next.
        original_[name] = current;
        overrides_.erase(name);
        applied_.erase(name);
    }
}

std::map<std::string, float> Scalability::ActiveOverrides() const
{
    std::map<std::string, float> out;
    for (const auto& [name, value] : overrides_)
    {
        if (ManagedProjectKnob(name)) { out[name] = value; }
    }
    return out;
}

void Scalability::ClearOverride(const std::string& name)
{
    if (name.empty()) { overrides_.clear(); }
    else { overrides_.erase(name); }
}

int Scalability::GroupLevel(int group) const
{
    return group >= 0 && group < static_cast<int>(groupLevels_.size()) ? groupLevels_[static_cast<std::size_t>(group)] : -1;
}

int Scalability::GroupIndex(const std::string& key) const
{
    for (std::size_t g = 0; g < table_.groups.size(); ++g)
    {
        if (table_.groups[g].key == key) { return static_cast<int>(g); }
    }
    return -1;
}

void Scalability::SetGroupLevel(int group, int level)
{
    if (group < 0 || group >= static_cast<int>(groupLevels_.size())) { return; }
    groupLevels_[static_cast<std::size_t>(group)] = std::clamp(level, -1, kScalabilityLevels - 1);
}

void Scalability::SetPreset(int level)
{
    for (int& l : groupLevels_) { l = std::clamp(level, -1, kScalabilityLevels - 1); }
}

int Scalability::Preset() const
{
    if (groupLevels_.empty()) { return -2; }
    const int first = groupLevels_.front();
    for (int l : groupLevels_)
    {
        if (l != first) { return -1; }
    }
    return first < 0 ? -2 : first;
}

void Scalability::SetResolutionLevel(int level) { resolutionLevel_ = std::clamp(level, -1, kScalabilityLevels - 1); }

bool Scalability::Managed() const
{
    return resolutionLevel_ >= 0 || std::any_of(groupLevels_.begin(), groupLevels_.end(), [](int l) { return l >= 0; });
}

const ScalabilityCell* Scalability::ResolutionCell(int level) const
{
    if (level < 0 || level >= kScalabilityLevels) { return nullptr; }
    const bool dlss = access_.dlssAvailable && access_.dlssAvailable();
    return dlss ? &table_.resolutionDlss[static_cast<std::size_t>(level)] : &table_.resolutionNative[static_cast<std::size_t>(level)];
}

void Scalability::Pin(const std::string& name)
{
    if (FindScalabilityKnob(name)) { pinned_.insert(name); }
}

bool Scalability::Target(const std::string& name, const ScalabilityOp& op, float& out)
{
    if (op.kind == ScalabilityOp::Kind::Set)
    {
        out = op.value;
        return true;
    }
    float current = 0.0f;
    if (!access_.read || !access_.read(name, current)) { return false; }
    // A value other than the last one scalability wrote is the level's own (a load, an Inspector edit): the new base.
    const auto written = lastWritten_.find(name);
    if (written == lastWritten_.end() || written->second != current || !contentBase_.count(name))
    {
        contentBase_[name] = current;
    }
    const float base = contentBase_[name];
    switch (op.kind)
    {
    case ScalabilityOp::Kind::Level: out = base; break;
    case ScalabilityOp::Kind::Max: out = std::min(base, op.value); break;
    case ScalabilityOp::Kind::Min: out = std::max(base, op.value); break;
    default: out = op.value; break;
    }
    return true;
}

bool Scalability::WriteKnob(const std::string& name, float target, float current, bool known, ScalabilityReport& report)
{
    if (!access_.write || !access_.write(name, target))
    {
        ++report.keysFailed;
        LOG_ERROR_ONCE_PER_MESSAGE(logging::LogCategory::App, "scalability: {} is not a --set name", name);
        return false;
    }
    ++report.keysWritten;
    const ScalabilityKnob* knob = FindScalabilityKnob(name);
    if (knob && knob->cls == 'R') { ++report.targetChanges; }
    if (knob && knob->cls == 'I') { ++report.idleChanges; }
    report.changed.push_back(name + " " + (known ? FormatValue(current) : std::string("?")) + " -> " + FormatValue(target));
    return true;
}

bool Scalability::ApplyCell(const ScalabilityCell& cell, ScalabilityReport& report)
{
    bool ok = true;
    for (const auto& [name, op] : cell)
    {
        if (pinned_.count(name))
        {
            ++report.keysPinned;
            continue;
        }
        const ScalabilityKnob* knob = FindScalabilityKnob(name);
        const bool content = knob && knob->content;
        float current = 0.0f;
        const bool known = access_.read && access_.read(name, current);
        // A project knob's value before scalability first managed it: what "Not managed" puts back.
        if (!content && known && !original_.count(name)) { original_[name] = current; }
        float target = 0.0f;
        if (!Target(name, op, target))
        {
            ++report.keysFailed;
            ok = false;
            LOG_ERROR_ONCE_PER_MESSAGE(logging::LogCategory::App, "scalability: {} cannot be read", name);
            continue;
        }
        // A value set by hand over the level's (a project knob only: the level's own knobs are the level's).
        if (const auto manual = overrides_.find(name); !content && manual != overrides_.end())
        {
            target = manual->second;
            ++report.keysOverridden;
        }
        if (known && current == target)
        {
            if (content) { lastWritten_[name] = target; }
            else { applied_[name] = target; }
            continue;
        }
        if (!WriteKnob(name, target, current, known, report))
        {
            ok = false;
            continue;
        }
        // Every write, a set value too: what the knob holds after its own clamp, so the next apply does not read it as
        // the level's new value, nor the next save as a change by hand.
        float stored = target;
        const float held = access_.read && access_.read(name, stored) ? stored : target;
        if (content) { lastWritten_[name] = held; }
        else { applied_[name] = held; }
    }
    return ok;
}

bool Scalability::HasRestore(const ScalabilityCell& cell) const
{
    for (const auto& [name, op] : cell)
    {
        if (original_.count(name) || lastWritten_.count(name)) { return true; }
    }
    return false;
}

void Scalability::RestoreCell(const ScalabilityCell& cell, ScalabilityReport& report)
{
    // A group (or the resolution) no longer managed: its knobs back to what they were before scalability -- a project
    // knob's own value, a level's knob the level's own value -- and forgotten.
    for (const auto& [name, op] : cell)
    {
        if (pinned_.count(name)) { continue; }
        float back = 0.0f;
        bool restore = false;
        applied_.erase(name);
        if (const auto it = original_.find(name); it != original_.end())
        {
            back = it->second;
            restore = true;
            original_.erase(it);
        }
        else if (const auto written = lastWritten_.find(name); written != lastWritten_.end())
        {
            const auto base = contentBase_.find(name);
            float current = 0.0f;
            // Only while the knob still holds what scalability wrote: anything else is the level's newer value.
            if (base != contentBase_.end() && access_.read && access_.read(name, current) && current == written->second)
            {
                back = base->second;
                restore = true;
            }
            lastWritten_.erase(written);
            if (base != contentBase_.end()) { contentBase_.erase(base); }
        }
        if (!restore) { continue; }
        float current = 0.0f;
        const bool known = access_.read && access_.read(name, current);
        if (known && current == back) { continue; }
        WriteKnob(name, back, current, known, report);
    }
}

void Scalability::ForgetContent(const std::vector<std::string>& prefixes)
{
    const auto matches = [&](const std::string& name) {
        return std::any_of(prefixes.begin(), prefixes.end(), [&](const std::string& p) { return name.rfind(p, 0) == 0; });
    };
    for (auto it = lastWritten_.begin(); it != lastWritten_.end();) { it = matches(it->first) ? lastWritten_.erase(it) : std::next(it); }
    for (auto it = contentBase_.begin(); it != contentBase_.end();) { it = matches(it->first) ? contentBase_.erase(it) : std::next(it); }
}

ScalabilityReport Scalability::Apply(const std::string& reason, bool quiet, bool noteManual)
{
    ScalabilityReport report;
    report.reason = reason;
    if (!loaded_)
    {
        lastReport_ = report;
        return report;
    }
    // A knob changed by hand since the last apply (a Developer Controls edit not saved yet) is kept as an override, not
    // overwritten by the level being applied now.
    if (noteManual) { NoteManualValues(); }
    // Anything to do: a managed group, or one let go whose knobs are still scalability's.
    bool work = Managed();
    for (std::size_t g = 0; g < table_.groups.size() && !work; ++g)
    {
        work = groupLevels_[g] < 0 && HasRestore(table_.groups[g].levels[0]);
    }
    if (!work && resolutionLevel_ < 0) { work = HasRestore(table_.resolutionDlss[0]) || HasRestore(table_.resolutionNative[0]); }
    if (!work)
    {
        lastReport_ = report;
        return report;
    }
    const double start = NowMs();
    if (access_.beginBatch) { access_.beginBatch(); }
    for (std::size_t g = 0; g < table_.groups.size(); ++g)
    {
        const int level = groupLevels_[g];
        if (level < 0) { RestoreCell(table_.groups[g].levels[0], report); }
        else { ApplyCell(table_.groups[g].levels[static_cast<std::size_t>(level)], report); }
    }
    if (const ScalabilityCell* cell = ResolutionCell(resolutionLevel_)) { ApplyCell(*cell, report); }
    else
    {
        RestoreCell(table_.resolutionDlss[0], report);
        RestoreCell(table_.resolutionNative[0], report);
    }
    if (access_.endBatch) { report.recreations = access_.endBatch(); }
    report.ms = NowMs() - start;
    const int preset = Preset();
    const std::string presetName = preset >= 0 ? table_.levelNames[static_cast<std::size_t>(preset)]
                                 : preset == -1 ? std::string("Custom") : std::string("groups not managed");
    const std::string resolutionName = resolutionLevel_ >= 0 ? table_.levelNames[static_cast<std::size_t>(resolutionLevel_)]
                                                             : std::string("not managed");
    if (quiet && report.keysFailed == 0)
    {
        // An Inspector drag re-applies every frame: a line only when a knob moved.
        if (report.keysWritten > 0)
        {
            LOG_DEBUG_THROTTLED(std::chrono::seconds(1), logging::LogCategory::App, "scalability: applied ({}): {} knob(s) changed",
                                reason, report.keysWritten);
        }
    }
    else
    {
        LOG_INFO(logging::LogCategory::App,
                 "scalability: applied ({}) -- {}, resolution {}: {} knob(s) changed, {} pinned by --set/MCP, {} set by "
                 "hand, {} failed; {} target knob(s) -> {} recreation(s), idle reconciles {}; {:.2f} ms",
                 reason, presetName, resolutionName, report.keysWritten, report.keysPinned, report.keysOverridden,
                 report.keysFailed, report.targetChanges, report.recreations, report.idleChanges, report.ms);
        for (const std::string& c : report.changed)
        {
            LOG_DEBUG(logging::LogCategory::App, "scalability:   {}", c);
        }
    }
    lastReport_ = report;
    return report;
}

ScalabilityReport Scalability::OnContentReloaded(const std::string& reason)
{
    contentBase_.clear();
    lastWritten_.clear();
    return Apply(reason, /*quiet=*/false, /*noteManual=*/false);
}

std::string Scalability::StatusJson() const
{
    json groups = json::object();
    for (std::size_t g = 0; g < table_.groups.size(); ++g) { groups[table_.groups[g].key] = groupLevels_.empty() ? -1 : groupLevels_[g]; }
    const int preset = Preset();
    json out = {
        { "loaded", loaded_ },
        { "table", kTablePath },
        { "errors", tableErrors_ },
        { "levels", json::array({ table_.levelNames[0], table_.levelNames[1], table_.levelNames[2], table_.levelNames[3] }) },
        { "preset", preset >= 0 ? table_.levelNames[static_cast<std::size_t>(preset)] : preset == -1 ? "Custom" : "none" },
        { "groups", groups },
        { "resolution", resolutionLevel_ },
        { "pinned", json(std::vector<std::string>(pinned_.begin(), pinned_.end())) },
        { "overrides", json(overrides_) },
        { "overridesInUse", json(ActiveOverrides()) },
        { "autodetect", { { "running", autodetectRunning_ }, { "status", autodetectStatus_ } } },
        { "lastApply", { { "reason", lastReport_.reason }, { "written", lastReport_.keysWritten },
                         { "pinned", lastReport_.keysPinned }, { "overridden", lastReport_.keysOverridden },
                         { "failed", lastReport_.keysFailed },
                         { "renderTargets", lastReport_.targetChanges }, { "recreations", lastReport_.recreations },
                         { "idleReconciles", lastReport_.idleChanges },
                         { "ms", lastReport_.ms }, { "changed", lastReport_.changed } } },
    };
    return out.dump();
}

Scalability& TheScalability()
{
    static Scalability instance;
    return instance;
}

int RunScalabilitySelfTest()
{
    int failed = 0;
    const auto check = [&](bool ok, const std::string& what)
    {
        if (!ok) { ++failed; }
        if (ok) { LOG_INFO(logging::LogCategory::App, "scalability self-test: ok   {}", what); }
        else { LOG_ERROR(logging::LogCategory::App, "scalability self-test: FAIL {}", what); }
    };
    std::string text;
    if (!vfs::ReadText(Scalability::kTablePath, text))
    {
        LOG_ERROR(logging::LogCategory::App, "scalability self-test: FAIL no {}", Scalability::kTablePath);
        return 1;
    }
    {
        ScalabilityTable table;
        std::vector<std::string> errors;
        const bool ok = ParseScalabilityTable(text, table, errors);
        for (const std::string& e : errors) { LOG_ERROR(logging::LogCategory::App, "scalability self-test:   {}", e); }
        check(ok, std::string("the shipped table parses and validates (") + std::to_string(table.groups.size()) + " groups)");
        if (ok)
        {
            // Every knob kKnobs names is used by the table, and every level of every group sets something.
            std::set<std::string> used;
            for (const ScalabilityGroup& g : table.groups)
            {
                for (const auto& [name, op] : g.levels[0]) { used.insert(name); }
                check(!g.levels[0].empty(), "group " + g.key + " sets knobs");
            }
            for (const auto& [name, op] : table.resolutionDlss[0]) { used.insert(name); }
            for (const auto& [name, op] : table.resolutionNative[0]) { used.insert(name); }
            std::string unused;
            for (const ScalabilityKnob& k : ScalabilityKnobs())
            {
                if (!used.count(k.name)) { unused += std::string(unused.empty() ? "" : ", ") + k.name; }
            }
            if (!unused.empty())
            {
                LOG_INFO(logging::LogCategory::App, "scalability self-test: knobs the table may name but does not: {}", unused);
            }
        }
    }

    // The control arms: each mutation of the shipped table must be refused, naming why.
    const json original = json::parse(text, nullptr, false, true);
    const auto mustFail = [&](const std::string& what, const std::function<void(json&)>& mutate, const std::string& expect)
    {
        json j = original;
        mutate(j);
        ScalabilityTable table;
        std::vector<std::string> errors;
        const bool ok = ParseScalabilityTable(j.dump(), table, errors);
        std::string all;
        for (const std::string& e : errors) { all += e + " | "; }
        check(!ok && all.find(expect) != std::string::npos, "control: " + what + " is refused (" + (all.empty() ? "no error" : all.substr(0, 200)) + ")");
    };
    const auto firstGroup = [](json& j, const char* key) -> json& {
        for (json& g : j["groups"]) { if (g.value("key", std::string()) == key) { return g; } }
        return j["groups"][0];
    };
    mustFail("a misspelt knob", [&](json& j) {
        for (const char* l : { "Low", "Medium", "High", "Ultra" }) { firstGroup(j, "shadows")["levels"][l]["shadow.mdoe"] = 1; }
    }, "no knob \"shadow.mdoe\"");
    mustFail("a sticky key", [&](json& j) { firstGroup(j, "gi")["levels"]["Medium"].erase("gi.rays"); }, "sticky");
    mustFail("a knob in two groups", [&](json& j) {
        for (const char* l : { "Low", "Medium", "High", "Ultra" }) { firstGroup(j, "water")["levels"][l]["gi.rays"] = 64; }
    }, "in two groups");
    mustFail("a ceiling on a knob the level does not set", [&](json& j) {
        firstGroup(j, "textures")["levels"]["Low"]["streaming.mipBias"] = json{ { "max", 1 } };
    }, "not set by the level");
    mustFail("a level missing", [&](json& j) { firstGroup(j, "effects")["levels"].erase("High"); }, "level High is missing");

    // A ceiling over the level's own value, and the level's new value as its new base.
    {
        Scalability s;
        std::map<std::string, float> knobs = { { "cloud.viewSampleCountMax", 128.0f } };
        s.SetAccess({ [&](const std::string& n, float v) { knobs[n] = v; return true; },
                      [&](const std::string& n, float& v) { const auto it = knobs.find(n); if (it == knobs.end()) { return false; } v = it->second; return true; },
                      []() { return true; } });
        float out = 0.0f;
        s.Target("cloud.viewSampleCountMax", { ScalabilityOp::Kind::Max, 64.0f }, out);
        check(out == 64.0f, "a ceiling of 64 over the level's 128 is 64 (got " + FormatValue(out) + ")");
        knobs["cloud.viewSampleCountMax"] = 32.0f;
        s.Target("cloud.viewSampleCountMax", { ScalabilityOp::Kind::Max, 64.0f }, out);
        check(out == 32.0f, "a ceiling of 64 over the level's 32 is 32 (got " + FormatValue(out) + ")");
        s.Target("cloud.viewSampleCountMax", { ScalabilityOp::Kind::Level, 0.0f }, out);
        check(out == 32.0f, "\"level\" is the level's own value, 32 (got " + FormatValue(out) + ")");
    }
    // The real road (Apply): a ceiling, a level raised, the level's own value back; a level that sets its knob to the
    // ceiling's value; "Not managed" putting a project knob back.
    {
        Scalability s;
        std::map<std::string, float> knobs = { { "cloud.viewSampleCountMax", 128.0f }, { "gi.rays", 64.0f } };
        s.SetAccess({ [&](const std::string& n, float v) { knobs[n] = v; return true; },
                      [&](const std::string& n, float& v) { const auto it = knobs.find(n); if (it == knobs.end()) { return false; } v = it->second; return true; },
                      []() { return true; } });
        ScalabilityTable t;
        ScalabilityGroup g;
        g.key = "test";
        for (int l = 0; l < kScalabilityLevels; ++l)
        {
            const bool top = l == kScalabilityLevels - 1;
            g.levels[static_cast<std::size_t>(l)] = {
                { "cloud.viewSampleCountMax", top ? ScalabilityOp{ ScalabilityOp::Kind::Level, 0.0f } : ScalabilityOp{ ScalabilityOp::Kind::Max, 48.0f } },
                { "gi.rays", ScalabilityOp{ ScalabilityOp::Kind::Set, top ? 64.0f : 32.0f } } };
        }
        t.groups.push_back(g);
        s.SetTableForTest(t);
        s.SetPreset(0);
        s.Apply("self-test Low");
        check(knobs["cloud.viewSampleCountMax"] == 48.0f && knobs["gi.rays"] == 32.0f, "Low: the ceiling 48 and gi.rays 32 are written");
        s.SetPreset(3);
        s.Apply("self-test Ultra");
        check(knobs["cloud.viewSampleCountMax"] == 128.0f, "Ultra after Low: the level's own 128 is back (got " +
              FormatValue(knobs["cloud.viewSampleCountMax"]) + ")");
        s.SetPreset(0);
        s.Apply("self-test Low again");
        knobs["cloud.viewSampleCountMax"] = 48.0f; // the level is edited to the ceiling's own value (an Inspector)
        s.ForgetContent({ "cloud." });
        s.Apply("self-test environment edit", true);
        s.SetPreset(3);
        s.Apply("self-test Ultra after the edit");
        check(knobs["cloud.viewSampleCountMax"] == 48.0f, "a level edited to the ceiling's value keeps it at Ultra (got " +
              FormatValue(knobs["cloud.viewSampleCountMax"]) + ")");
        knobs["gi.rays"] = 64.0f;
        Scalability fresh;
        fresh.SetAccess({ [&](const std::string& n, float v) { knobs[n] = v; return true; },
                          [&](const std::string& n, float& v) { const auto it = knobs.find(n); if (it == knobs.end()) { return false; } v = it->second; return true; },
                          []() { return true; } });
        fresh.SetTableForTest(t);
        knobs["cloud.viewSampleCountMax"] = 128.0f;
        fresh.SetPreset(0);
        fresh.Apply("self-test Low");
        fresh.SetPreset(-1);
        fresh.Apply("self-test not managed");
        check(knobs["gi.rays"] == 64.0f && knobs["cloud.viewSampleCountMax"] == 128.0f,
              "Not managed puts back gi.rays 64 and the level's 128 (got " + FormatValue(knobs["gi.rays"]) + ", " +
              FormatValue(knobs["cloud.viewSampleCountMax"]) + ")");

        // Manual overrides (§3.3, layer 5): a knob changed by hand over its level is noted at a save, kept over another
        // preset, taken away by a reset, unused while its group is not managed; a --set (a pin) never becomes one.
        knobs = { { "cloud.viewSampleCountMax", 128.0f }, { "gi.rays", 64.0f } };
        Scalability o;
        o.SetAccess({ [&](const std::string& n, float v) { knobs[n] = v; return true; },
                      [&](const std::string& n, float& v) { const auto it = knobs.find(n); if (it == knobs.end()) { return false; } v = it->second; return true; },
                      []() { return true; } });
        o.SetTableForTest(t);
        o.SetPreset(0);
        o.Apply("self-test Low");
        check(!o.NoteManualValues() && o.Overrides().empty(), "nothing changed by hand: no override");
        knobs["gi.rays"] = 48.0f; // a Developer Controls edit while Low is chosen
        const bool noted = o.NoteManualValues();
        check(noted && o.Overrides().count("gi.rays") && o.Overrides().at("gi.rays") == 48.0f,
              "a knob changed by hand over Low becomes an override (48)");
        o.SetPreset(3);
        o.Apply("self-test Ultra over the override");
        check(knobs["gi.rays"] == 48.0f, "a preset keeps the override (got " + FormatValue(knobs["gi.rays"]) + ")");
        const std::map<std::string, float> before = o.ManagedValues();
        knobs["gi.rays"] = 64.0f; // a Reset puts the build's default in
        o.AfterReset(before);
        o.SetPreset(0);
        o.Apply("self-test after the reset");
        check(knobs["gi.rays"] == 32.0f && o.Overrides().empty(),
              "a reset takes the override away and the level's 32 is written (got " + FormatValue(knobs["gi.rays"]) + ")");
        o.SetOverride("gi.rays", 40.0f);
        o.SetPreset(-1);
        o.Apply("self-test not managed with an override");
        check(knobs["gi.rays"] == 64.0f && o.ActiveOverrides().empty() && o.Overrides().size() == 1,
              "a group not managed: the knob's own 64 back, the override kept and unused (got " + FormatValue(knobs["gi.rays"]) + ")");
        o.ClearOverride("");
        o.SetPreset(0);
        o.Apply("self-test Low, pinned");
        o.Pin("gi.rays");
        knobs["gi.rays"] = 50.0f; // a --set
        check(!o.NoteManualValues() && o.Overrides().empty(), "a pinned knob (--set) never becomes an override");
    }
    LOG_INFO(logging::LogCategory::App, "scalability self-test: {} ({} failed)", failed == 0 ? "PASS" : "FAIL", failed);
    return failed;
}
}
