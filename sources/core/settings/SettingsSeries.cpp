#include "core/settings/SettingsSeries.h"

#include <algorithm>

namespace settings
{
namespace
{
bool Contains(const std::vector<std::string>& list, const std::string& name)
{
    return std::find(list.begin(), list.end(), name) != list.end();
}
} // namespace

bool SettingsSeries::Set(const Host& host, const std::string& name, float value)
{
    if (!started_)
    {
        started_ = true;
        pinsBefore_ = host.pins();
    }
    const bool action = host.isAction(name);
    const auto kept = [&](const std::string& n) {
        return std::any_of(before_.begin(), before_.end(), [&](const Noted& b) { return b.name == n; });
    };
    if (host.rewritesKnobs(name) && !knobsKept_)
    {
        // Before the level change, so Restore (newest first) puts the group's level back first and these over it.
        knobsKept_ = true;
        for (const std::string& knob : host.knobs())
        {
            float v = 0.0f;
            if (host.read(knob, v)) { before_.push_back({ knob, v }); }
        }
    }
    float was = 0.0f;
    const bool readable = !action && !kept(name) && host.read(name, was);
    if (!host.apply(name, value))
    {
        return false;
    }
    host.pin(name, true);
    if (readable)
    {
        // A name set while nothing could read it (the ocean's colours on a level without one) and readable now: its
        // value now is the one this series has not changed yet.
        before_.push_back({ name, was });
        unreadable_.erase(std::remove(unreadable_.begin(), unreadable_.end(), name), unreadable_.end());
    }
    else if (!action && !kept(name) && !Contains(unreadable_, name))
    {
        unreadable_.push_back(name);
    }
    return true;
}

std::string SettingsSeries::Restore(const Host& host)
{
    // The count is of names, each once, whose old value was taken back (a refusal is named instead).
    std::vector<std::string> putBack;
    std::vector<std::string> refused;
    for (auto it = before_.rbegin(); it != before_.rend(); ++it)
    {
        std::vector<std::string>& list = host.apply(it->name, it->value) ? putBack : refused;
        if (!Contains(list, it->name)) { list.push_back(it->name); }
    }
    if (started_)
    {
        const std::set<std::string> now = host.pins();
        for (const std::string& n : now)
        {
            if (pinsBefore_.count(n) == 0) { host.pin(n, false); }
        }
        for (const std::string& n : pinsBefore_)
        {
            if (now.count(n) == 0) { host.pin(n, true); }
        }
    }
    std::string text = "Put back " + std::to_string(putBack.size()) + " name(s) set_settings changed.";
    if (!refused.empty())
    {
        text += " Refused their old value:";
        for (const std::string& n : refused) { text += " " + n; }
        text += ".";
    }
    if (!unreadable_.empty())
    {
        // No reader, or nothing to read it from while it was set: there is no old value.
        text += " Not readable when set, they stay as set:";
        for (const std::string& n : unreadable_) { text += " " + n; }
        text += ".";
    }
    before_.clear();
    unreadable_.clear();
    pinsBefore_.clear();
    started_ = false;
    knobsKept_ = false;
    return text;
}
} // namespace settings
