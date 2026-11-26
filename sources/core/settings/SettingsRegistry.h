#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The settings registry (architecture plan R2.1): one row per setting name -- what --set / --sweep / MCP set_settings
// write and what --settings-dump, the scalability table and the dev UI read -- instead of the name compared in a chain
// of `if (setting == "...")` (App.cpp ApplySweepValue / ReadSweepValue) and again in every consumer. The chain is asked
// AFTER the registry until every prefix has moved (R2.2-...); a name lives in one of the two, never both
// (tools/settings_names.py keeps the set whole).
//
// The registry is the core layer's: it knows a setting by its name and two functions, never what they reach. What they
// reach is the app's SettingContext (app/settings/SettingContext.h): the scene, the render settings.
namespace settings
{
struct SettingContext; // defined by the app

// What the value means (the dev UI's widget, the dump's reading); the registry converts nothing -- a row's apply does,
// as the chain's branch did (a Bool is value != 0, an Int is usually static_cast after a clamp: truncated, not rounded).
enum class SettingType : std::uint8_t
{
    Float,
    Bool,  // read back as 0 / 1
    Int,   // read back as a whole number
};

enum SettingFlags : std::uint32_t
{
    SettingNone = 0u,
    SettingPersistent = 1u << 0,  // saved with the player's graphics (user_settings.json)
    SettingLevelOwned = 1u << 1,  // a field of a level section: the level file sets it, a --set overrides it
    SettingTransient = 1u << 2,   // a run's A/B: never saved
    SettingScalable = 1u << 3,    // the scalability table manages it (data/config/scalability.json)
    SettingAction = 1u << 4,      // a command, not a value (race.start): no reader, never dumped
    // What a change of a Scalable setting costs (settings::ScalabilityKnob::cls, docs/scalability_plan.md): one of them.
    SettingCostNextFrame = 1u << 5,  // L: the next frame takes it
    SettingCostTargets = 1u << 6,    // R: the render targets are recreated
    SettingCostIdleGpu = 1u << 7,    // I: a reconcile at an idle GPU in Scene
    SettingCostCache = 1u << 8,      // C: a cache flushed
    SettingCostPipelines = 1u << 9,  // P: pipelines rebuilt
    SettingCostBoot = 1u << 10,      // B: only at boot
};

struct SettingDesc
{
    const char* name = nullptr;   // "bloom.intensity"
    SettingType type = SettingType::Float;
    std::uint32_t flags = SettingNone;
    // Applies a value as --set gave it: the row converts and clamps, as the chain's branch did. False = refused (the
    // chain's `return false`).
    bool (*apply)(SettingContext& ctx, float value) = nullptr;
    // Reads the value now; null = no reader (the name cannot be read back). False = cannot be read in this state.
    bool (*read)(SettingContext& ctx, float& out) = nullptr;
    const char* tab = "";         // the dev UI's tab, later (R2.n)
    const char* label = "";
    const char* hint = "";
};

// A family of names: <prefix><key> for the keys of an owner's table (game::RaceKeys, ocean::WakeKnobs, the scalability
// groups, ...). The table stays the one record per knob; the family is the registry's way to it. A row answers its name
// first; a family answers a name no row has when its `has` takes the tail (two families may share a prefix: the ocean's
// colours and its wake knobs).
struct SettingFamily
{
    const char* prefix = nullptr;  // "race."
    const char* table = nullptr;   // where the keys live, for a reader and tools/settings_names.py: "game::RaceKeys"
    SettingType type = SettingType::Float;
    std::uint32_t flags = SettingNone;
    bool (*has)(std::string_view key) = nullptr;
    // Applies <prefix><key> as the chain's prefix block did, its clamp included. False = refused.
    bool (*apply)(SettingContext& ctx, std::string_view key, float value) = nullptr;
    // Reads <prefix><key>; null = no reader. False = cannot be read in this state.
    bool (*read)(SettingContext& ctx, std::string_view key, float& out) = nullptr;
    // Every key it answers now (the dump, the dev UI); may follow what is loaded (the scalability groups).
    void (*keys)(std::vector<std::string>& out) = nullptr;
};

class SettingsRegistry
{
public:
    bool AddFamily(const SettingFamily& family);
    // The family that answers a name (its prefix starts the name and its `has` takes the tail), or null; `key` gets the
    // tail.
    const SettingFamily* FindFamily(std::string_view name, std::string_view& key) const;
    const std::vector<SettingFamily>& Families() const { return families_; }
    // A second row with a name already there (or a row without an apply) is refused; the first stays. Rows arrive at
    // static initialisation, before the log: ReportRefused logs the refusals once the log is up (main).
    bool Add(const SettingDesc& desc);
    const SettingDesc* Find(std::string_view name) const;
    // Applies / reads by name; the value goes to the row's apply as given (the row converts and clamps, as the chain's
    // branch did). Unknown names are false (the caller then asks the chain).
    bool Apply(SettingContext& ctx, std::string_view name, float value) const;
    bool Read(SettingContext& ctx, std::string_view name, float& out) const;
    bool Knows(std::string_view name) const; // a row or a family answers it
    const std::vector<SettingDesc>& All() const { return rows_; }
    int ReportRefused() const; // the number of refused rows, each an Error line

private:
    std::vector<SettingDesc> rows_;
    std::vector<SettingFamily> families_;
    std::vector<std::string> refused_;
};

// The one registry. Rows are added at static initialisation (TC_SETTINGS below), so it is a function-local static.
SettingsRegistry& Registry();

// Registers a table of rows at static initialisation: `TC_SETTINGS(kBloomSettings);` after `constexpr SettingDesc
// kBloomSettings[] = { ... }`. tools/settings_names.py finds the rows by `TC_SETTING("name"`, so write each row as
// TC_SETTING("name", ...) -- see the macro.
int RegisterTable(const SettingDesc* rows, std::size_t count);
int RegisterFamilies(const SettingFamily* families, std::size_t count);

// A row's reader: the stored value as --settings-dump prints it -- a bool 0 / 1, an enum its number, a whole number as
// a float, a string (a texture's path) 1 when set.
template <class T>
float AsFloat(const T& v)
{
    if constexpr (std::is_same_v<T, bool>) { return v ? 1.0f : 0.0f; }
    else if constexpr (std::is_enum_v<T>) { return static_cast<float>(static_cast<std::underlying_type_t<T>>(v)); }
    else if constexpr (std::is_arithmetic_v<T>) { return static_cast<float>(v); }
    else { return v.empty() ? 0.0f : 1.0f; }
}
}

// One row: TC_SETTING("bloom.intensity", Float, SettingLevelOwned, apply, read) -- a designated SettingDesc.
#define TC_SETTING(NAME, TYPE, FLAGS, APPLY, READ) \
    ::settings::SettingDesc{ NAME, ::settings::SettingType::TYPE, (FLAGS), (APPLY), (READ) }
#define TC_SETTINGS_PASTE2(a, b) a##b
#define TC_SETTINGS_PASTE(a, b) TC_SETTINGS_PASTE2(a, b)
#define TC_SETTINGS(TABLE) \
    static const int TC_SETTINGS_PASTE(tcSettingsRegistered_, __LINE__) = \
        ::settings::RegisterTable((TABLE), sizeof(TABLE) / sizeof((TABLE)[0]))
// One family: TC_SETTING_FAMILY("race.", "game::RaceKeys", Float, SettingTransient, has, apply, read, keys).
#define TC_SETTING_FAMILY(PREFIX, TABLE, TYPE, FLAGS, HAS, APPLY, READ, KEYS) \
    ::settings::SettingFamily{ PREFIX, TABLE, ::settings::SettingType::TYPE, (FLAGS), (HAS), (APPLY), (READ), (KEYS) }
#define TC_SETTING_FAMILIES(TABLE) \
    static const int TC_SETTINGS_PASTE(tcSettingFamiliesRegistered_, __LINE__) = \
        ::settings::RegisterFamilies((TABLE), sizeof(TABLE) / sizeof((TABLE)[0]))
