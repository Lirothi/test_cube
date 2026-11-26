#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Graphics scalability as UE has it (docs/scalability_plan.md §1-§5): the frame's cost is split into GROUPS (sg.shadows,
// sg.gi, ...), each with four LEVELS (Low, Medium, High, Ultra); a cell group x level is a set of assignments to knobs by
// their `--set` names (App.cpp ApplySweepValue -- one name table for everything). A PRESET puts every group on one
// level; groups on different levels are Custom. The render resolution (sg.resolution: the DLSS mode, or the render scale
// and FXAA without DLSS) is a choice of its own: a preset does not move it (§10 item 6).
//
// The table is data, data/config/scalability.json (in the packages, from the tree in dev): a level is tuned without a
// rebuild. Ultra is today's picture: the package's defaults and the level's own values. Below it a level only takes cost
// away; a knob the LEVEL sets (the clouds' samples, the fog's volume, GTAO, the bloom...) is never raised above what the
// level asks for -- {"max": n} is a ceiling, "level" its own value.
//
// The layers, weakest first (§3.3): compiled values, the build's graphics defaults (config/graphics_defaults.json; a dev
// build's tools/dist/graphics_defaults.json) with the person's own graphics section of user_settings.json, the level,
// scalability, manual overrides over a group's level, `--set` / MCP. A knob that `--set` or MCP wrote is PINNED for the
// process: scalability leaves it as it was told (UE's SetByScalability, the weakest priority there is).
namespace settings
{
    constexpr int kScalabilityLevels = 4;

    // How the value of a knob is chosen at a level.
    struct ScalabilityOp
    {
        enum class Kind { Set, Max, Min, Level };
        Kind kind = Kind::Set;
        float value = 0.0f;
    };
    using ScalabilityCell = std::vector<std::pair<std::string, ScalabilityOp>>; // in the file's order

    struct ScalabilityGroup
    {
        std::string key;    // "shadows" -> sg.shadows
        std::string label;  // what the menu says
        std::string hint;
        std::array<std::string, kScalabilityLevels> levelLabels; // empty = the table's level names
        std::array<ScalabilityCell, kScalabilityLevels> levels;
    };

    struct ScalabilityTable
    {
        int version = 0;
        std::array<std::string, kScalabilityLevels> levelNames{ "Low", "Medium", "High", "Ultra" };
        int defaultLevel = kScalabilityLevels - 1;
        std::vector<ScalabilityGroup> groups;
        std::string resolutionLabel = "Render resolution";
        std::string resolutionHint;
        std::array<std::string, kScalabilityLevels> resolutionLevelLabels;
        std::array<ScalabilityCell, kScalabilityLevels> resolutionDlss;   // DLSS available
        std::array<ScalabilityCell, kScalabilityLevels> resolutionNative; // no DLSS: a render scale and FXAA
    };

    // What a knob of the table is (the "class table" beside the name table, §4.1): content = the LEVEL sets it (it is
    // read back as the level's own value before a ceiling is applied); the class says what a change costs:
    // L the next frame, R the render targets recreated, I a reconcile at an idle GPU in Scene, C a cache flushed,
    // P pipelines rebuilt, B only at boot.
    struct ScalabilityKnob
    {
        const char* name;
        bool content;
        char cls;
    };
    // Every knob the table may name; the validator refuses any other.
    const std::vector<ScalabilityKnob>& ScalabilityKnobs();
    const ScalabilityKnob* FindScalabilityKnob(const std::string& name);

    // The knobs, as the app reaches them (App.cpp): the `--set` writer and its reader. Both on the game thread.
    struct ScalabilityAccess
    {
        std::function<bool(const std::string& name, float value)> write;
        std::function<bool(const std::string& name, float& value)> read;
        std::function<bool()> dlssAvailable;
        // Around one apply: the renderer's target batch (Renderer::BeginTargetBatch), so the knobs that recreate the
        // render targets recreate them once; End returns how many recreations the apply cost.
        std::function<void()> beginBatch;
        std::function<int()> endBatch;
    };

    // The table from JSON text; false (and every reason) when it does not parse or does not validate.
    bool ParseScalabilityTable(const std::string& text, ScalabilityTable& out, std::vector<std::string>& errors);
    // The rules (§2.5, §4.1): every name is a knob of ScalabilityKnobs(); every level of a group (and of the resolution)
    // names the SAME keys -- no "sticky" key a level leaves as the previous one set it; a key in one group only; "level",
    // max and min only on a knob the level sets. Appends the reasons; true = clean.
    bool ValidateScalabilityTable(const ScalabilityTable& table, std::vector<std::string>& errors);

    struct ScalabilityReport
    {
        int keysWritten = 0;     // knobs whose value changed
        int keysPinned = 0;      // left alone: --set / MCP wrote them
        int keysOverridden = 0;  // set to a value changed by hand (overrides) instead of the level's
        int keysFailed = 0;      // the writer did not know the name (a bug: the gate catches it)
        int targetChanges = 0;   // R knobs that moved: the render targets are recreated
        int recreations = 0;     // how many times they were (one batch: at most 1, the fog's grid at the next frame aside)
        int idleChanges = 0;     // I knobs that moved: a reconcile at an idle GPU
        double ms = 0.0;
        std::string reason;
        std::vector<std::string> changed; // "name old -> new"
    };

    class Scalability
    {
    public:
        static constexpr const char* kTablePath = "data/config/scalability.json";
        static constexpr const char* kUserFile = "user_settings.json"; // vfs::WritablePath(Settings, ...)

        void SetAccess(ScalabilityAccess access) { access_ = std::move(access); }

        // The table (kTablePath through the VFS). False when it is missing or invalid: scalability then does nothing.
        bool LoadTable();
        bool Loaded() const { return loaded_; }
        const ScalabilityTable& Table() const { return table_; }
        const std::vector<std::string>& TableErrors() const { return tableErrors_; }

        // The player's choice (user_settings.json "scalability") and the values set by hand over it ("overrides").
        // Nothing saved = not managed: no knob is touched and the picture is the build's defaults with the person's own
        // graphics section (a dev build: the developer's). A choice is saved by SaveUser, with the overrides.
        void LoadUser();
        bool SaveUser(std::string& why) const;
        // Another section of user_settings.json as JSON text ("" = none), and its replacement (the others kept) --
        // UserSettings.h's, kept here for the callers that had them.
        static std::string ReadUserSection(const std::string& key);
        static bool WriteUserSection(const std::string& key, const std::string& jsonText, std::string& why);

        // MANUAL OVERRIDES (§3.3, layer 5): a project knob a managed group sets, changed by hand (Developer Controls, a
        // menu's Advanced) -- stronger than the group's level, weaker than --set / MCP, saved with the levels by --set
        // name. A preset chosen over them keeps them; the menu and the Developer Controls tab show them and take them
        // back. A group not managed leaves its overrides unused (its knobs are plain ones then).
        //
        // Before a save: a managed project knob that holds other than what the last apply made it hold was changed by
        // hand -- it becomes an override (a pinned knob, --set's, never does). True when the overrides changed.
        bool NoteManualValues();
        // After a Reset put knobs back to the build's defaults: a managed knob the reset moved loses its override, and
        // the default becomes its own value (what "Not managed" gives back); the next Apply writes the level's value.
        // `before`: each managed knob's value before the reset (ManagedValues()).
        void AfterReset(const std::map<std::string, float>& before);
        std::map<std::string, float> ManagedValues() const;
        // The overrides in use: the knob's group is managed.
        std::map<std::string, float> ActiveOverrides() const;
        const std::map<std::string, float>& Overrides() const { return overrides_; }
        // Take one (or every one, "") back to the group's level; Apply writes it.
        void ClearOverride(const std::string& name);
        // A value set over the group's level (the menu's Advanced; Cancel's restore). Apply writes it.
        void SetOverride(const std::string& name, float value) { overrides_[name] = value; }
        // The project knob a managed group sets at its level (no content knob, no pin): the cell's knobs.
        bool ManagedProjectKnob(const std::string& name) const;
        // Who sets a project knob now: "Shadows: Low", "Render resolution: Balanced" -- "" when no managed group or
        // resolution sets it, or a script pinned it (Developer Controls' mark beside the control).
        std::string ManagerLabel(const std::string& name) const;

        // -1 = the group is not managed (scalability leaves its knobs alone).
        int GroupLevel(int group) const;
        int GroupIndex(const std::string& key) const;
        void SetGroupLevel(int group, int level);
        // Every group on one level (a preset); -1 = none managed any more.
        void SetPreset(int level);
        // The single level every group shares, -1 = Custom (groups differ), -2 = not managed at all.
        int Preset() const;
        int ResolutionLevel() const { return resolutionLevel_; }
        void SetResolutionLevel(int level);
        bool Managed() const;

        // The knobs to the chosen levels (one batch: R and I knobs are counted; the renderer recreates what moved once).
        // A group or the resolution at -1 has its knobs put back to what they were before scalability managed them (a
        // project knob's own value, a level's knob the level's value). `quiet`: a throttled Debug line, and only when a
        // knob moved (an Inspector drag re-applies every frame).
        // `noteManual`: a knob changed since the last apply is first kept as an override (NoteManualValues) -- not after a
        // level load, which puts project knobs back itself (the CSM's project config): that is no change by hand.
        ScalabilityReport Apply(const std::string& reason, bool quiet = false, bool noteManual = true);
        // An editor environment edit re-applied these knobs from the document (by name prefix): their current values are
        // the level's own, whatever scalability last wrote (an Inspector that sets a knob to the ceiling's value).
        void ForgetContent(const std::vector<std::string>& prefixes);
        // For the self-test: a table without the file.
        void SetTableForTest(ScalabilityTable table)
        {
            table_ = std::move(table);
            loaded_ = true;
            groupLevels_.assign(table_.groups.size(), -1);
            resolutionLevel_ = -1;
        }
        // A level was loaded or its environment re-applied: the level's own values are new -- forget the ceilings' bases
        // and apply again.
        ScalabilityReport OnContentReloaded(const std::string& reason);
        // `--set` / MCP / --sweep wrote `name`: scalability leaves it alone for the rest of the process.
        void Pin(const std::string& name);
        void ClearPins() { pinned_.clear(); }
        void Unpin(const std::string& name) { pinned_.erase(name); } // MCP reset_settings put the name back
        const std::set<std::string>& Pins() const { return pinned_; }

        // What the cell of a group at a level asks a knob to be, with the knob's current value (for a ceiling, the
        // level's own value) -- the Developer Controls tab and the gate's read-back.
        bool Target(const std::string& name, const ScalabilityOp& op, float& out);
        // A project knob scalability manages: its value before scalability first wrote it (what graphics_settings.json
        // keeps -- a Developer Controls autosave must not record a preset's values as the developer's own). False when
        // the knob is not managed.
        bool OriginalValue(const std::string& name, float& out) const
        {
            const auto it = original_.find(name);
            if (it == original_.end()) { return false; }
            out = it->second;
            return true;
        }
        // A knob's current value through the app's reader.
        bool Read(const std::string& name, float& out) const { return access_.read && access_.read(name, out); }
        bool DlssAvailable() const { return access_.dlssAvailable && access_.dlssAvailable(); }
        const ScalabilityReport& LastReport() const { return lastReport_; }
        std::string StatusJson() const;

        // Auto-detect (§5): the app measures the real frame at High and steps up or down (App.cpp). Asked by the menu's
        // button, --scalability=auto, or a packaged build's first start; the status is what the menu shows.
        // `save`: the result is written to user_settings.json -- only a packaged build's own first start does; the menu
        // keeps it for Done, a script (--scalability=auto in dev, MCP) never writes the developer's file.
        void RequestAutodetect(bool save) { autodetectRequested_ = true; autodetectSave_ = save; autodetectAbort_ = false; }
        bool TakeAutodetectRequest() { const bool r = autodetectRequested_; autodetectRequested_ = false; return r; }
        bool AutodetectSaves() const { return autodetectSave_; }
        // The menu's Cancel / Esc while it measures: it stops and applies nothing more.
        // A run that saves -- a packaged build's first start on a system, `--scalability=auto` there -- is FORCED (owner,
        // 2026-11-17): the game's until it is done (AppController holds the input, the menu and F8 back and shows that it
        // measures), and nothing cancels it. A quit before the end saves nothing, so the next start measures again.
        bool AutodetectForced() const { return autodetectSave_ && (autodetectRequested_ || autodetectRunning_); }
        void AbortAutodetect()
        {
            if (AutodetectForced()) { return; }
            autodetectAbort_ = autodetectRunning_;
            autodetectRequested_ = false;
        }
        bool TakeAutodetectAbort() { const bool r = autodetectAbort_; autodetectAbort_ = false; return r; }
        void SetAutodetectStatus(std::string status) { autodetectStatus_ = std::move(status); }
        const std::string& AutodetectStatus() const { return autodetectStatus_; }
        bool AutodetectRunning() const { return autodetectRunning_; }
        void SetAutodetectRunning(bool running) { autodetectRunning_ = running; }
        // `--set scalability.gpuTimeScale:<x>` (UE's sg.Test.GPUPerfIndexOverride): the measured frame times x, so a
        // fast machine can play a slow one -- the gate's control (§8.4).
        float autodetectGpuScale = 1.0f;

    private:
        const ScalabilityCell* ResolutionCell(int level) const;
        bool ApplyCell(const ScalabilityCell& cell, ScalabilityReport& report);
        bool WriteKnob(const std::string& name, float target, float current, bool known, ScalabilityReport& report);
        bool HasRestore(const ScalabilityCell& cell) const;
        void RestoreCell(const ScalabilityCell& cell, ScalabilityReport& report);

        ScalabilityAccess access_;
        ScalabilityTable table_;
        bool loaded_ = false;
        std::vector<std::string> tableErrors_;
        std::vector<int> groupLevels_;    // per group, -1 = not managed
        int resolutionLevel_ = -1;
        std::set<std::string> pinned_;
        // A content knob's own value, and what scalability last wrote into it: a value that differs from the last write
        // is the level's (a load, an Inspector edit) and becomes the new base.
        std::map<std::string, float> contentBase_;
        std::map<std::string, float> lastWritten_;
        // A project knob's value before scalability first wrote it: "Not managed" puts it back.
        std::map<std::string, float> original_;
        // A project knob's value as the last apply left it (the level's target or an override): anything else at a save
        // was changed by hand (NoteManualValues).
        std::map<std::string, float> applied_;
        // Values set by hand over a managed group's level (user_settings.json "overrides").
        std::map<std::string, float> overrides_;
        ScalabilityReport lastReport_;
        bool autodetectRequested_ = false;
        bool autodetectRunning_ = false;
        bool autodetectSave_ = false;
        bool autodetectAbort_ = false;
        std::string autodetectStatus_;
    };

    // The app's one instance (the game thread's).
    Scalability& TheScalability();

    // "--scalability-selftest" (docs/scalability_plan.md §8.1), no window: the shipped table parses and validates; the
    // control arms -- a misspelt knob, a sticky key, a knob in two groups, a ceiling on a knob the level does not set,
    // a level missing -- each must FAIL; a ceiling over the level's own value takes the smaller one, and a level's new
    // value is the ceiling's new base. Per-case lines and the verdict in the session log; returns the failed checks.
    int RunScalabilitySelfTest();
}
