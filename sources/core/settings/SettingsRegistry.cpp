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

bool SettingsRegistry::Apply(SettingContext& ctx, std::string_view name, float value) const
{
    const SettingDesc* d = Find(name);
    return d && d->apply(ctx, value);
}

bool SettingsRegistry::Read(SettingContext& ctx, std::string_view name, float& out) const
{
    const SettingDesc* d = Find(name);
    return d && d->read && d->read(ctx, out);
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
}
