#include "app/harness/SettingsDumpHarness.h"

#include "app/BootOptions.h"
#include "core/Helpers.h" // GetTimeSeconds
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace harness
{
bool SettingsDumpHarness::Wanted() const { return !Boot().harness.settingsDumpPath.empty(); }

void SettingsDumpHarness::OnFrame(FrameContext& ctx)
{
    if (!started_) { started_ = true; start_ = GetTimeSeconds(); }
    if (GetTimeSeconds() - start_ < Boot().frame.shotDelaySec) { return; }

    const std::string path = Boot().harness.settingsDumpPath;
    Boot().harness.settingsDumpPath.clear();
    ctx.running = false;
    // The names: R2.0's set (tools/settings_names.txt, written from the sources). A dev checkout's file.
    std::string names;
    if (!vfs::ReadText("tools/settings_names.txt", names))
    {
        LOG_ERROR(logging::LogCategory::App, "settings dump: cannot read tools/settings_names.txt (a dev checkout's file)");
        failed_ = true;
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        LOG_ERROR(logging::LogCategory::App, "settings dump: cannot write {}", path);
        failed_ = true;
        return;
    }
    std::istringstream lines(names);
    std::string line;
    int written = 0;
    int unreadable = 0;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); } // a checkout with core.autocrlf writes the list CRLF
        if (line.empty() || line[0] == '#') { continue; }
        const std::string name = line.substr(0, line.find(' '));
        float value = 0.0f;
        if (!read_(ctx.scene, ctx.settings, name, value))
        {
            ++unreadable;
            continue;
        }
        char text[64];
        std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(value));
        out << name << ' ' << text << '\n';
        ++written;
    }
    if (graphics_)
    {
        std::vector<std::string> extra;
        graphics_(ctx, false, extra);
        if (Boot().harness.graphicsResetAtDump) { graphics_(ctx, true, extra); }
        for (const std::string& l : extra) { out << l << '\n'; }
    }
    out.close();
    LOG_INFO(logging::LogCategory::App, "settings dump: {} values ({} names without a reader) -> {}", written, unreadable, path);
}
}
