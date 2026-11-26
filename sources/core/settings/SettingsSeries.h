// The bookkeeping of MCP's set_settings / reset_settings (architecture plan R2.n): the value each name had before a
// series first changed it, put back newest first by Restore, and the scalability pins as they were before the series.
// Pure logic: the host reads, writes and pins -- App.cpp's McpRuntimeHost on the scene, a fake in
// tools/intent_regression.cpp (TestSettingsSeriesRestores).
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace settings
{
class SettingsSeries
{
public:
    struct Host
    {
        std::function<bool(const std::string&, float&)> read;  // its value now; false = nothing reads it now
        std::function<bool(const std::string&, float)> apply;  // false = an unknown name, a value refused
        std::function<bool(const std::string&)> isAction;      // race.start: no value to keep
        // sg.*: a level change (or sg.unpin, sg.reload) rewrites the scalability knobs -- their values are kept, once a
        // series, before the first such change: putting the group's level back gives an unpinned knob the level's
        // value, not the one it had.
        std::function<bool(const std::string&)> rewritesKnobs;
        std::function<std::vector<std::string>()> knobs;
        std::function<std::set<std::string>()> pins;           // the scalability pins now
        std::function<void(const std::string&, bool)> pin;     // pin (true) or unpin a name
    };

    // set_settings, one name: keeps what it had before the series' first change of it, applies, pins it (what MCP set,
    // the graphics levels leave alone). False = not applied (an unknown name, a value refused).
    bool Set(const Host& host, const std::string& name, float value);
    // reset_settings: every kept value back, newest first (a name set twice ends at its first value); the pins as they
    // were before the series (the ones it set go, the ones sg.unpin took come back). The answer's text.
    std::string Restore(const Host& host);

private:
    struct Noted
    {
        std::string name;
        float value = 0.0f;
    };
    std::vector<Noted> before_;
    std::vector<std::string> unreadable_; // set while nothing could read it: no value to put back
    std::set<std::string> pinsBefore_;
    bool started_ = false;
    bool knobsKept_ = false;
};
} // namespace settings
