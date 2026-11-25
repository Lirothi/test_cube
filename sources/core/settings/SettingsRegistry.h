#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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

class SettingsRegistry
{
public:
    // A second row with a name already there (or a row without an apply) is refused; the first stays. Rows arrive at
    // static initialisation, before the log: ReportRefused logs the refusals once the log is up (main).
    bool Add(const SettingDesc& desc);
    const SettingDesc* Find(std::string_view name) const;
    // Applies / reads by name; the value goes to the row's apply as given (the row converts and clamps, as the chain's
    // branch did). Unknown names are false (the caller then asks the chain).
    bool Apply(SettingContext& ctx, std::string_view name, float value) const;
    bool Read(SettingContext& ctx, std::string_view name, float& out) const;
    const std::vector<SettingDesc>& All() const { return rows_; }
    int ReportRefused() const; // the number of refused rows, each an Error line

private:
    std::vector<SettingDesc> rows_;
    std::vector<std::string> refused_;
};

// The one registry. Rows are added at static initialisation (TC_SETTINGS below), so it is a function-local static.
SettingsRegistry& Registry();

// Registers a table of rows at static initialisation: `TC_SETTINGS(kBloomSettings);` after `constexpr SettingDesc
// kBloomSettings[] = { ... }`. tools/settings_names.py finds the rows by `TC_SETTING("name"`, so write each row as
// TC_SETTING("name", ...) -- see the macro.
int RegisterTable(const SettingDesc* rows, std::size_t count);
}

// One row: TC_SETTING("bloom.intensity", Float, SettingLevelOwned, apply, read) -- a designated SettingDesc.
#define TC_SETTING(NAME, TYPE, FLAGS, APPLY, READ) \
    ::settings::SettingDesc{ NAME, ::settings::SettingType::TYPE, (FLAGS), (APPLY), (READ) }
#define TC_SETTINGS_PASTE2(a, b) a##b
#define TC_SETTINGS_PASTE(a, b) TC_SETTINGS_PASTE2(a, b)
#define TC_SETTINGS(TABLE) \
    static const int TC_SETTINGS_PASTE(tcSettingsRegistered_, __LINE__) = \
        ::settings::RegisterTable((TABLE), sizeof(TABLE) / sizeof((TABLE)[0]))
