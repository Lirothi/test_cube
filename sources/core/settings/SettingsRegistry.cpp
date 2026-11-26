#include "core/settings/SettingsRegistry.h"

#include "core/logging/Log.h"

namespace settings
{
SettingsRegistry& Registry()
{
    static SettingsRegistry registry;
    return registry;
}

bool SettingsRegistry::Add(const SettingDesc& desc)
{
    // Rows come at static initialisation, before the log: a refusal is kept and reported by ReportRefused at boot.
    if (!desc.name || !desc.apply || Find(desc.name))
    {
        refused_.push_back(desc.name ? desc.name : "<a row without a name>");
        return false;
    }
    rows_.push_back(desc);
    return true;
}

const SettingDesc* SettingsRegistry::Find(std::string_view name) const
{
    for (const SettingDesc& d : rows_)
    {
        if (name == d.name) { return &d; }
    }
    return nullptr;
}

bool SettingsRegistry::AddFamily(const SettingFamily& family)
{
    if (!family.prefix || !family.has || !family.apply || !family.keys)
    {
        refused_.push_back(std::string("family ") + (family.prefix ? family.prefix : "<without a prefix>"));
        return false;
    }
    families_.push_back(family);
    return true;
}

const SettingFamily* SettingsRegistry::FindFamily(std::string_view name, std::string_view& key) const
{
    for (const SettingFamily& f : families_)
    {
        const std::string_view prefix(f.prefix);
        if (name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix && f.has(name.substr(prefix.size())))
        {
            key = name.substr(prefix.size());
            return &f;
        }
    }
    return nullptr;
}

bool SettingsRegistry::Apply(SettingContext& ctx, std::string_view name, float value) const
{
    if (const SettingDesc* d = Find(name)) { return d->apply(ctx, value); }
    std::string_view key;
    const SettingFamily* f = FindFamily(name, key);
    return f && f->apply(ctx, key, value);
}

bool SettingsRegistry::Read(SettingContext& ctx, std::string_view name, float& out) const
{
    if (const SettingDesc* d = Find(name)) { return d->read && d->read(ctx, out); }
    std::string_view key;
    const SettingFamily* f = FindFamily(name, key);
    return f && f->read && f->read(ctx, key, out);
}

bool SettingsRegistry::Knows(std::string_view name) const
{
    std::string_view key;
    return Find(name) || FindFamily(name, key);
}

int SettingsRegistry::ReportRefused() const
{
    for (const std::string& name : refused_)
    {
        LOG_ERROR(logging::LogCategory::App, "settings registry: {} was registered twice (or without an apply) -- the first row stays",
                  name);
    }
    return static_cast<int>(refused_.size());
}

int RegisterTable(const SettingDesc* rows, std::size_t count)
{
    int added = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        added += Registry().Add(rows[i]) ? 1 : 0;
    }
    return added;
}

int RegisterFamilies(const SettingFamily* families, std::size_t count)
{
    int added = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        added += Registry().AddFamily(families[i]) ? 1 : 0;
    }
    return added;
}
}
