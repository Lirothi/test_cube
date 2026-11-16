#include "app/ui/GameSettingsMenu.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>

#include "app/settings/Scalability.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "imgui.h"
#include "rendering/core/Renderer.h"
#include "third_party/json/json.hpp"

namespace
{
    using json = nlohmann::json;

    constexpr int kFpsChoices[] = { 0, 30, 60, 90, 120, 144, 165, 240 };

    std::filesystem::path UserFile()
    {
        return vfs::WritablePath(vfs::Writable::Settings, settings::Scalability::kUserFile);
    }

    json ReadUserFile()
    {
        std::ifstream in(UserFile(), std::ios::binary); // vfs-ok: the user's settings file
        if (!in) { return json::object(); }
        json root = json::parse(in, nullptr, false, true);
        return root.is_object() ? root : json::object();
    }

    // A group whose knobs recreate the render targets or wait for an idle GPU: its change costs a short pause.
    bool GroupPauses(const settings::ScalabilityGroup& g)
    {
        for (const auto& [name, op] : g.levels[0])
        {
            const settings::ScalabilityKnob* k = settings::FindScalabilityKnob(name);
            if (k && (k->cls == 'R' || k->cls == 'I')) { return true; }
        }
        return false;
    }

    // A combo over the levels: -1 = not set by the player (the build's defaults: Ultra's picture).
    bool LevelCombo(const char* label, int& level, const std::array<std::string, settings::kScalabilityLevels>& names,
                    const char* unsetName)
    {
        const std::string preview = level >= 0 ? names[static_cast<std::size_t>(level)] : unsetName;
        bool changed = false;
        if (ImGui::BeginCombo(label, preview.c_str()))
        {
            for (int i = 0; i < settings::kScalabilityLevels; ++i)
            {
                if (ImGui::Selectable(names[static_cast<std::size_t>(i)].c_str(), level == i))
                {
                    changed = level != i;
                    level = i;
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
}

void GameSettingsMenu::Open()
{
    open_ = true;
    atOpen_ = Take();
    status_.clear();
}

void GameSettingsMenu::Cancel()
{
    if (!open_) { return; }
    settings::TheScalability().AbortAutodetect(); // a measurement in progress applies nothing more
    Restore(atOpen_);
    open_ = false;
}

void GameSettingsMenu::OpenTab(const std::string& tab)
{
    Open();
    requestedTab_ = tab == "display" ? 1 : 0;
}

GameSettingsMenu::Snapshot GameSettingsMenu::Take()
{
    const settings::Scalability& s = settings::TheScalability();
    Snapshot snap;
    for (std::size_t g = 0; g < s.Table().groups.size(); ++g) { snap.groups.push_back(s.GroupLevel(static_cast<int>(g))); }
    snap.resolution = s.ResolutionLevel();
    snap.vsync = render::g_vsync_game;
    snap.maxFps = render::g_maxFps_game;
    return snap;
}

void GameSettingsMenu::Restore(const Snapshot& snap)
{
    settings::Scalability& s = settings::TheScalability();
    for (std::size_t g = 0; g < snap.groups.size(); ++g) { s.SetGroupLevel(static_cast<int>(g), snap.groups[g]); }
    s.SetResolutionLevel(snap.resolution);
    s.Apply("settings menu: cancelled");
    render::g_vsync_game = snap.vsync;
    render::g_maxFps_game = snap.maxFps;
}

bool GameSettingsMenu::SaveDisplay(std::string& why)
{
    json root = ReadUserFile();
    root["version"] = 1;
    root["display"] = { { "vsync", render::g_vsync_game }, { "maxFps", render::g_maxFps_game } };
    const std::filesystem::path file = UserFile();
    std::error_code ec;
    if (file.has_parent_path()) { std::filesystem::create_directories(file.parent_path(), ec); }
    std::filesystem::path temp = file;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc); // vfs-ok: the user's settings file
        if (!out)
        {
            why = "cannot write " + vfs::Narrow(temp.wstring());
            return false;
        }
        out << root.dump(2) << "\n";
        out.flush();
        if (!out.good())
        {
            why = "write failed";
            out.close();
            std::filesystem::remove(temp, ec);
            return false; // the good file stays
        }
    }
    std::filesystem::rename(temp, file, ec);
    if (ec)
    {
        why = ec.message();
        return false;
    }
    return true;
}

void GameSettingsMenu::ApplySavedDisplay()
{
    const json root = ReadUserFile();
    if (!root.contains("display") || !root["display"].is_object()) { return; }
    const json& d = root["display"];
    if (d.contains("vsync") && d["vsync"].is_boolean()) { render::g_vsync_game = d["vsync"].get<bool>(); }
    if (d.contains("maxFps") && d["maxFps"].is_number_integer()) { render::g_maxFps_game = render::ClampMaxFps(d["maxFps"].get<int>()); }
    LOG_INFO(logging::LogCategory::App, "settings: the player's display -- vsync {}, frame-rate limit {}", render::g_vsync_game,
             render::g_maxFps_game);
}

void GameSettingsMenu::DrawGraphics()
{
    settings::Scalability& s = settings::TheScalability();
    if (!s.Loaded())
    {
        ImGui::TextWrapped("The graphics levels are not available (%s did not load).", settings::Scalability::kTablePath);
        return;
    }
    const settings::ScalabilityTable& table = s.Table();
    bool changed = false;

    const bool measuring = s.AutodetectRunning();
    ImGui::BeginDisabled(measuring);
    if (ImGui::Button("Auto-detect", ImVec2(200.0f, 0.0f))) { s.RequestAutodetect(false); } // Done keeps it, Cancel drops it
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Measures this machine's frame for a few seconds and picks the levels for 60 fps."); }
    if (!s.AutodetectStatus().empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", s.AutodetectStatus().c_str());
    }
    ImGui::BeginDisabled(measuring);

    int preset = s.Preset();
    const char* presetName = preset == -1 ? "Custom" : "Ultra (default)";
    ImGui::SetNextItemWidth(360.0f);
    if (LevelCombo("Quality", preset, table.levelNames, presetName))
    {
        s.SetPreset(preset);
        changed = true;
    }
    if (ImGui::IsItemHovered())
    {
        ImGui::SetTooltip("Every group below at one level. Ultra is the full picture; each step down takes cost away.\n"
                          "Custom shows while the groups are on different levels.");
    }

    std::array<std::string, settings::kScalabilityLevels> resolutionNames = table.levelNames;
    for (int i = 0; i < settings::kScalabilityLevels; ++i)
    {
        const std::string& label = table.resolutionLevelLabels[static_cast<std::size_t>(i)];
        if (!label.empty() && s.DlssAvailable())
        {
            resolutionNames[static_cast<std::size_t>(i)] = table.levelNames[static_cast<std::size_t>(i)] + " -- DLSS " + label;
        }
    }
    int resolution = s.ResolutionLevel();
    ImGui::SetNextItemWidth(360.0f);
    if (LevelCombo(table.resolutionLabel.c_str(), resolution, resolutionNames, "Default"))
    {
        s.SetResolutionLevel(resolution);
        changed = true;
    }
    if (ImGui::IsItemHovered() && !table.resolutionHint.empty()) { ImGui::SetTooltip("%s", table.resolutionHint.c_str()); }

    ImGui::SeparatorText("Groups");
    for (std::size_t g = 0; g < table.groups.size(); ++g)
    {
        const settings::ScalabilityGroup& group = table.groups[g];
        std::array<std::string, settings::kScalabilityLevels> names = table.levelNames;
        for (int i = 0; i < settings::kScalabilityLevels; ++i)
        {
            const std::string& label = group.levelLabels[static_cast<std::size_t>(i)];
            if (!label.empty()) { names[static_cast<std::size_t>(i)] = label; }
        }
        int level = s.GroupLevel(static_cast<int>(g));
        ImGui::PushID(static_cast<int>(g));
        ImGui::SetNextItemWidth(360.0f);
        const std::string label = group.label + (GroupPauses(group) ? " *" : "");
        if (LevelCombo(label.c_str(), level, names, "Default"))
        {
            s.SetGroupLevel(static_cast<int>(g), level);
            changed = true;
        }
        if (ImGui::IsItemHovered() && !group.hint.empty()) { ImGui::SetTooltip("%s", group.hint.c_str()); }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("* a change pauses the game for a moment (render targets rebuilt, shadows reconciled).");
    if (changed)
    {
        const settings::ScalabilityReport r = s.Apply("settings menu");
        status_ = "Applied: " + std::to_string(r.keysWritten) + " setting(s) changed.";
    }
}

void GameSettingsMenu::DrawDisplay()
{
    bool vsync = render::g_vsync_game;
    if (ImGui::Checkbox("VSync", &vsync)) { render::g_vsync_game = vsync; }
    if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Wait for the monitor: no tearing, the frame rate at most the refresh rate."); }
    int current = render::g_maxFps_game;
    const std::string preview = current == 0 ? std::string("Off") : std::to_string(current);
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::BeginCombo("Frame-rate limit", preview.c_str()))
    {
        for (const int fps : kFpsChoices)
        {
            const std::string name = fps == 0 ? std::string("Off") : std::to_string(fps);
            if (ImGui::Selectable(name.c_str(), current == fps)) { render::g_maxFps_game = render::ClampMaxFps(fps); }
        }
        ImGui::EndCombo();
    }
}

bool GameSettingsMenu::Draw()
{
    if (!open_) { return false; }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    bool closed = false;
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_AlwaysAutoResize);
    if (ImGui::BeginTabBar("settings_tabs"))
    {
        const ImGuiTabItemFlags graphicsFlags = requestedTab_ == 0 ? ImGuiTabItemFlags_SetSelected : 0;
        const ImGuiTabItemFlags displayFlags = requestedTab_ == 1 ? ImGuiTabItemFlags_SetSelected : 0;
        requestedTab_ = -1;
        if (ImGui::BeginTabItem("Graphics", nullptr, graphicsFlags))
        {
            tab_ = 0;
            DrawGraphics();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display", nullptr, displayFlags))
        {
            tab_ = 1;
            DrawDisplay();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    // While auto-detect measures, Done would keep a level it is only trying: wait for it (Esc still cancels it).
    ImGui::BeginDisabled(settings::TheScalability().AutodetectRunning());
    if (ImGui::Button("Done", ImVec2(200.0f, 0.0f)))
    {
        std::string why;
        const bool savedLevels = settings::TheScalability().SaveUser(why);
        const bool savedDisplay = savedLevels && SaveDisplay(why);
        if (!savedLevels || !savedDisplay) { LOG_WARNING(logging::LogCategory::App, "settings: not saved: {}", why); }
        else { LOG_INFO(logging::LogCategory::App, "settings: saved ({})", vfs::Narrow(UserFile().wstring())); }
        open_ = false;
        closed = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(200.0f, 0.0f)))
    {
        Cancel();
        closed = true;
    }
    if (!status_.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_.c_str());
    }
    ImGui::End();
    return closed;
}
