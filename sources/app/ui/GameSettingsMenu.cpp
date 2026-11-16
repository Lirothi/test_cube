#include "app/ui/GameSettingsMenu.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>

#include "app/camera/Camera.h"
#include "app/settings/Scalability.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "imgui.h"
#include "input/InputManager.h"
#include "rendering/core/Renderer.h"
#include "third_party/json/json.hpp"

namespace
{
    using json = nlohmann::json;

    constexpr int kFpsChoices[] = { 0, 30, 60, 90, 120, 144, 165, 240 };
    constexpr int kBackgroundFpsChoices[] = { 0, 10, 15, 30, 60 };
    constexpr double kConfirmSeconds = 15.0;

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

    // A labelled row of the two-column table every tab draws: the label left, the control filling the right.
    bool BeginRows(const char* id)
    {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) { return false; }
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.36f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.64f);
        return true;
    }
    void Row(const char* label, const char* hint = nullptr)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (hint && ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", hint); }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
    }
    void Hint(const char* hint)
    {
        if (hint && ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", hint); }
    }

    std::string FpsName(int fps) { return fps <= 0 ? std::string(menu_text::kOff) : std::to_string(fps); }

    // The groups' settings a player may set on their own (Advanced, §6.2 item 2): a short safe list -- the cost knobs whose
    // every value is a valid picture -- not the ~60 knobs of the table.
    struct AdvancedKnob
    {
        const char* name;
        const char* label;
        float min;
        float max;
        bool integer;
        bool toggle;
    };
    constexpr AdvancedKnob kAdvanced[] = {
        { "vsm.smrtRayCount", "Sun shadow rays", 1.0f, 8.0f, true, false },
        { "contact.enabled", "Contact shadows", 0.0f, 1.0f, true, true },
        { "gi.rays", "Bounce light rays per probe", 16.0f, 64.0f, true, false },
        { "skyVis.rays", "Sky light rays", 2.0f, 16.0f, true, false },
        { "reflections.resolution", "Reflection resolution", 0.25f, 1.0f, false, false },
        { "skyVis.fogRays", "Fog sky rays", 0.0f, 4.0f, true, false },
        { "lod.pixelError", "Detail distance (pixel error)", 0.5f, 16.0f, false, false },
        { "streaming.poolSizeMB", "Texture memory (MB)", 256.0f, 8192.0f, true, false },
        { "particles.spawnScale", "Particle amount", 0.1f, 1.0f, false, false },
    };
}

// --- GameMenuStyle ---------------------------------------------------------------------------------------------------

GameMenuStyle::GameMenuStyle()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& base = ImGui::GetStyle();
    // 20 pixels on a 1080 line, in proportion to the window's height (720: 13, 1440: 27, 2160: 40).
    em_ = std::clamp(20.0f * viewport->Size.y / 1080.0f, 12.0f, 64.0f);
    const float scale = std::max(base.FontScaleMain * base.FontScaleDpi, 0.01f);
    unscaled_ = em_ / scale; // PushFont's size is before the style's FontScaleMain x FontScaleDpi
    ImGui::PushFont(nullptr, unscaled_);
    const auto var = [this](ImGuiStyleVar v, ImVec2 value) { ImGui::PushStyleVar(v, value); ++vars_; };
    const auto varf = [this](ImGuiStyleVar v, float value) { ImGui::PushStyleVar(v, value); ++vars_; };
    var(ImGuiStyleVar_WindowPadding, ImVec2(Em(1.2f), Em(1.0f)));
    var(ImGuiStyleVar_FramePadding, ImVec2(Em(0.6f), Em(0.32f)));
    var(ImGuiStyleVar_ItemSpacing, ImVec2(Em(0.6f), Em(0.45f)));
    var(ImGuiStyleVar_ItemInnerSpacing, ImVec2(Em(0.4f), Em(0.3f)));
    var(ImGuiStyleVar_CellPadding, ImVec2(Em(0.3f), Em(0.2f)));
    varf(ImGuiStyleVar_WindowRounding, Em(0.35f));
    varf(ImGuiStyleVar_FrameRounding, Em(0.2f));
    varf(ImGuiStyleVar_PopupRounding, Em(0.2f));
    varf(ImGuiStyleVar_GrabRounding, Em(0.2f));
    varf(ImGuiStyleVar_TabRounding, Em(0.2f));
    varf(ImGuiStyleVar_GrabMinSize, Em(0.7f));
    varf(ImGuiStyleVar_ScrollbarSize, Em(0.7f));
    varf(ImGuiStyleVar_WindowBorderSize, 1.0f);
    // The colours as they are meant to look: the UI is drawn into an sRGB target, which takes these as linear.
    const auto color = [this](ImGuiCol c, float r, float g, float b, float a) {
        const auto linear = [](float v) { return std::pow(v, 2.2f); };
        ImGui::PushStyleColor(c, ImVec4(linear(r), linear(g), linear(b), a));
        ++colors_;
    };
    color(ImGuiCol_WindowBg, 0.055f, 0.075f, 0.095f, 0.96f);
    color(ImGuiCol_PopupBg, 0.07f, 0.09f, 0.115f, 0.98f);
    color(ImGuiCol_Border, 0.25f, 0.35f, 0.45f, 0.6f);
    color(ImGuiCol_Text, 0.92f, 0.95f, 0.97f, 1.0f);
    color(ImGuiCol_TextDisabled, 0.55f, 0.62f, 0.68f, 1.0f);
    color(ImGuiCol_FrameBg, 0.12f, 0.16f, 0.20f, 1.0f);
    color(ImGuiCol_FrameBgHovered, 0.17f, 0.24f, 0.30f, 1.0f);
    color(ImGuiCol_FrameBgActive, 0.20f, 0.30f, 0.38f, 1.0f);
    color(ImGuiCol_Button, 0.13f, 0.30f, 0.40f, 1.0f);
    color(ImGuiCol_ButtonHovered, 0.18f, 0.42f, 0.55f, 1.0f);
    color(ImGuiCol_ButtonActive, 0.22f, 0.50f, 0.64f, 1.0f);
    color(ImGuiCol_Header, 0.15f, 0.34f, 0.45f, 1.0f);
    color(ImGuiCol_HeaderHovered, 0.18f, 0.42f, 0.55f, 1.0f);
    color(ImGuiCol_HeaderActive, 0.22f, 0.50f, 0.64f, 1.0f);
    color(ImGuiCol_Tab, 0.10f, 0.15f, 0.19f, 1.0f);
    color(ImGuiCol_TabHovered, 0.18f, 0.42f, 0.55f, 1.0f);
    color(ImGuiCol_TabSelected, 0.15f, 0.34f, 0.45f, 1.0f);
    color(ImGuiCol_TabSelectedOverline, 0.36f, 0.78f, 0.92f, 1.0f);
    color(ImGuiCol_SliderGrab, 0.36f, 0.78f, 0.92f, 1.0f);
    color(ImGuiCol_SliderGrabActive, 0.55f, 0.88f, 0.98f, 1.0f);
    color(ImGuiCol_CheckMark, 0.45f, 0.86f, 0.98f, 1.0f);
    color(ImGuiCol_Separator, 0.25f, 0.35f, 0.45f, 0.7f);
    color(ImGuiCol_ModalWindowDimBg, 0.0f, 0.0f, 0.0f, 0.45f);
}

GameMenuStyle::~GameMenuStyle()
{
    ImGui::PopStyleColor(colors_);
    ImGui::PopStyleVar(vars_);
    ImGui::PopFont();
}

void GameMenuStyle::Backdrop() const
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos, ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
                                                  IM_COL32(0, 0, 0, 150));
}

void GameMenuStyle::Title(const char* text) const
{
    ImGui::PushFont(nullptr, unscaled_ * 1.4f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Spacing();
}

// --- GameSettingsMenu ------------------------------------------------------------------------------------------------

void GameSettingsMenu::Open()
{
    open_ = true;
    atOpen_ = Take();
    status_.clear();
    monitors_ = display::Monitors();
    confirming_ = false;
}

void GameSettingsMenu::Cancel()
{
    if (!open_) { return; }
    settings::TheScalability().AbortAutodetect(); // a measurement in progress applies nothing more
    Restore(atOpen_);
    confirming_ = false;
    open_ = false;
}

void GameSettingsMenu::OpenTab(const std::string& tab)
{
    Open();
    requestedTab_ = tab == "display" ? 0 : tab == "image" ? 2 : tab == "controls" ? 3 : 1;
}

GameSettingsMenu::Snapshot GameSettingsMenu::Take()
{
    const settings::Scalability& s = settings::TheScalability();
    Snapshot snap;
    for (std::size_t g = 0; g < s.Table().groups.size(); ++g) { snap.groups.push_back(s.GroupLevel(static_cast<int>(g))); }
    snap.resolution = s.ResolutionLevel();
    snap.overrides = s.Overrides();
    snap.vsync = render::g_vsync_game;
    snap.maxFps = render::g_maxFps_game;
    snap.mouseSmoothingMs = g_cameraMouseSmoothingMs;
    snap.window = display::Current(display::MainWindow());
    snap.image = settings::CurrentImageSettings();
    snap.controls = settings::CurrentControlsSettings();
    return snap;
}

bool GameSettingsMenu::Same(const Snapshot& a, const Snapshot& b)
{
    return a.groups == b.groups && a.resolution == b.resolution && a.overrides == b.overrides && a.vsync == b.vsync &&
           a.maxFps == b.maxFps && a.mouseSmoothingMs == b.mouseSmoothingMs && display::SameWindow(a.window, b.window) &&
           a.window.backgroundFps == b.window.backgroundFps && a.image.brightnessEv == b.image.brightnessEv &&
           a.image.fovDeg == b.image.fovDeg && a.controls.sensitivity == b.controls.sensitivity &&
           a.controls.invertY == b.controls.invertY;
}

void GameSettingsMenu::Restore(const Snapshot& snap)
{
    settings::Scalability& s = settings::TheScalability();
    for (std::size_t g = 0; g < snap.groups.size(); ++g) { s.SetGroupLevel(static_cast<int>(g), snap.groups[g]); }
    s.SetResolutionLevel(snap.resolution);
    s.ClearOverride("");
    for (const auto& [name, value] : snap.overrides) { s.SetOverride(name, value); }
    s.Apply("settings menu: cancelled");
    render::g_vsync_game = snap.vsync;
    render::g_maxFps_game = snap.maxFps;
    g_cameraMouseSmoothingMs = snap.mouseSmoothingMs;
    display::SetBackgroundFps(snap.window.backgroundFps);
    settings::ApplyImageSettings(snap.image);
    settings::ApplyControlsSettings(snap.controls);
    if (!display::SameWindow(display::Current(display::MainWindow()), snap.window)) { display::Request(snap.window); }
}

bool GameSettingsMenu::SaveAll(std::string& why)
{
    settings::Scalability& s = settings::TheScalability();
    if (s.Loaded() && !s.SaveUser(why)) { return false; }
    display::DisplaySettings window = display::Current(display::MainWindow());
    if (window.mode == display::WindowMode::Borderless)
    {
        // A borderless window is the monitor's size: the windowed size saved before is kept for the way back.
        const display::DisplaySettings saved = display::Load();
        window.width = saved.width;
        window.height = saved.height;
    }
    if (!display::Save(window, why)) { return false; }
    if (!settings::SaveImageAndControls(settings::CurrentImageSettings(), settings::CurrentControlsSettings(), why)) { return false; }
    if (saveGraphics_ && !saveGraphics_())
    {
        why = "the graphics settings";
        return false;
    }
    LOG_INFO(logging::LogCategory::App, "settings: saved ({})", vfs::Narrow(settings::UserSettingsPath().wstring()));
    return true;
}

void GameSettingsMenu::ApplySavedDisplay()
{
    // The display section of 2026-11-13 also carried VSync and the frame-rate limit; they are the graphics section's
    // since (one place for each knob) -- a file that still has them is read once more and rewritten without them by the
    // next Apply.
    const json d = json::parse(settings::ReadUserSection("display"), nullptr, false);
    if (!d.is_object() || (!d.contains("vsync") && !d.contains("maxFps"))) { return; }
    if (d.contains("vsync") && d["vsync"].is_boolean()) { render::g_vsync_game = d["vsync"].get<bool>(); }
    if (d.contains("maxFps") && d["maxFps"].is_number_integer()) { render::g_maxFps_game = render::ClampMaxFps(d["maxFps"].get<int>()); }
    LOG_INFO(logging::LogCategory::App, "settings: the player's display -- vsync {}, frame-rate limit {}", render::g_vsync_game,
             render::g_maxFps_game);
}

void GameSettingsMenu::ChangeWindow(const display::DisplaySettings& to)
{
    const display::DisplaySettings now = display::Current(display::MainWindow());
    if (display::SameWindow(now, to)) { return; }
    // Asked once more while one waits for Keep: the way back stays the first one.
    if (!confirming_) { confirmPrevious_ = now; }
    display::Request(to);
    confirming_ = true;
    confirmDeadline_ = ImGui::GetTime() + kConfirmSeconds;
}

void GameSettingsMenu::DrawDisplayConfirm()
{
    if (!confirming_) { return; }
    if (!ImGui::IsPopupOpen(menu_text::kConfirmTitle)) { ImGui::OpenPopup(menu_text::kConfirmTitle); }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(menu_text::kConfirmTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
    {
        const double left = confirmDeadline_ - ImGui::GetTime();
        ImGui::Text(menu_text::kConfirmCountdown, static_cast<int>(std::ceil(std::max(left, 0.0))));
        const float w = ImGui::GetFontSize() * 8.0f;
        const bool keep = ImGui::Button(menu_text::kKeep, ImVec2(w, 0.0f));
        ImGui::SameLine();
        const bool revert = ImGui::Button(menu_text::kRevert, ImVec2(w, 0.0f)) || left <= 0.0;
        if (keep || revert)
        {
            if (revert)
            {
                display::Request(confirmPrevious_);
                LOG_INFO(logging::LogCategory::App, "settings: the display change was {}", left <= 0.0 ? "not confirmed: put back" : "reverted");
            }
            confirming_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GameSettingsMenu::DrawDisplay()
{
    if (monitors_.empty()) { monitors_ = display::Monitors(); }
    const display::DisplaySettings now = display::Current(display::MainWindow());
    display::DisplaySettings want = now;
    bool windowChanged = false;
    if (!BeginRows("display_rows")) { return; }

    Row(menu_text::kWindowMode, menu_text::kWindowModeHint);
    const char* modes[] = { menu_text::kWindowed, menu_text::kBorderless };
    int mode = now.mode == display::WindowMode::Borderless ? 1 : 0;
    if (ImGui::Combo("##mode", &mode, modes, 2))
    {
        want.mode = mode == 1 ? display::WindowMode::Borderless : display::WindowMode::Windowed;
        windowChanged = true;
    }

    Row(menu_text::kMonitor);
    const int monitor = display::FindMonitor(monitors_, now.monitor);
    if (ImGui::BeginCombo("##monitor", display::MonitorLabel(monitors_, monitor).c_str()))
    {
        for (int i = 0; i < static_cast<int>(monitors_.size()); ++i)
        {
            if (ImGui::Selectable(display::MonitorLabel(monitors_, i).c_str(), i == monitor) && i != monitor)
            {
                want.monitor = monitors_[static_cast<std::size_t>(i)].device;
                windowChanged = true;
            }
        }
        ImGui::EndCombo();
    }

    Row(menu_text::kWindowSize, menu_text::kWindowSizeHint);
    const bool borderless = now.mode == display::WindowMode::Borderless;
    char current[64];
    if (borderless && monitor >= 0)
    {
        // The monitor's own size, not the window's (a --window= borderless window of another size is a script's).
        const RECT& r = monitors_[static_cast<std::size_t>(monitor)].rect;
        std::snprintf(current, sizeof(current), "%dx%d (%s)", static_cast<int>(r.right - r.left), static_cast<int>(r.bottom - r.top),
                      menu_text::kWindowSizeBorderless);
    }
    else { std::snprintf(current, sizeof(current), "%dx%d", now.width, now.height); }
    ImGui::BeginDisabled(borderless || monitor < 0);
    if (ImGui::BeginCombo("##size", current))
    {
        const auto sizes = display::WindowedSizes(monitors_[static_cast<std::size_t>(std::max(monitor, 0))], now.width, now.height);
        for (const auto& [w, h] : sizes)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "%dx%d", w, h);
            if (ImGui::Selectable(name, w == now.width && h == now.height) && (w != now.width || h != now.height))
            {
                want.width = w;
                want.height = h;
                windowChanged = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    Row(menu_text::kVsync, menu_text::kVsyncHint);
    bool vsync = render::g_vsync_game;
    if (ImGui::Checkbox("##vsync", &vsync)) { render::g_vsync_game = vsync; }

    Row(menu_text::kFpsLimit);
    const int fps = render::g_maxFps_game;
    if (ImGui::BeginCombo("##fps", FpsName(fps).c_str()))
    {
        for (const int choice : kFpsChoices)
        {
            if (ImGui::Selectable(FpsName(choice).c_str(), fps == choice)) { render::g_maxFps_game = render::ClampMaxFps(choice); }
        }
        ImGui::EndCombo();
    }

    Row(menu_text::kBackgroundFps, menu_text::kBackgroundFpsHint);
    const int background = display::BackgroundFps();
    if (ImGui::BeginCombo("##bgfps", FpsName(background).c_str()))
    {
        for (const int choice : kBackgroundFpsChoices)
        {
            if (ImGui::Selectable(FpsName(choice).c_str(), background == choice)) { display::SetBackgroundFps(choice); }
        }
        ImGui::EndCombo();
    }
    ImGui::EndTable();

    if (windowChanged)
    {
        // From borderless back to a window: the windowed size it had last (saved), else the build's default there.
        if (want.mode == display::WindowMode::Windowed && now.mode == display::WindowMode::Borderless)
        {
            const display::DisplaySettings saved = display::Load();
            const bool wasWindowed = atOpen_.window.mode == display::WindowMode::Windowed;
            want.width = wasWindowed ? atOpen_.window.width : saved.width;
            want.height = wasWindowed ? atOpen_.window.height : saved.height;
        }
        ChangeWindow(want);
    }
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
    if (ImGui::Button(menu_text::kAutodetect)) { s.RequestAutodetect(false); } // Apply keeps it, Cancel drops it
    Hint(menu_text::kAutodetectHint);
    ImGui::EndDisabled();
    if (!s.AutodetectStatus().empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", s.AutodetectStatus().c_str());
    }
    ImGui::BeginDisabled(measuring);

    if (BeginRows("quality_rows"))
    {
        int preset = s.Preset();
        Row(menu_text::kQuality, menu_text::kQualityHint);
        if (LevelCombo("##preset", preset, table.levelNames, preset == -1 ? menu_text::kCustom : menu_text::kUltraDefault))
        {
            s.SetPreset(preset);
            changed = true;
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
        Row(table.resolutionLabel.c_str(), table.resolutionHint.empty() ? nullptr : table.resolutionHint.c_str());
        if (LevelCombo("##resolution", resolution, resolutionNames, menu_text::kDefault))
        {
            s.SetResolutionLevel(resolution);
            changed = true;
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText(menu_text::kGroups);
    if (BeginRows("group_rows"))
    {
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
            const std::string label = group.label + (GroupPauses(group) ? " *" : "");
            Row(label.c_str(), group.hint.empty() ? nullptr : group.hint.c_str());
            if (LevelCombo("##level", level, names, menu_text::kDefault))
            {
                s.SetGroupLevel(static_cast<int>(g), level);
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", menu_text::kPauseNote);

    // What was changed by hand over the levels (Developer Controls, Advanced below): kept over any preset.
    const std::map<std::string, float> manual = s.ActiveOverrides();
    if (!manual.empty())
    {
        ImGui::Text(menu_text::kManualChanges, static_cast<int>(manual.size()));
        ImGui::SameLine();
        if (ImGui::SmallButton(menu_text::kBackToLevels))
        {
            s.ClearOverride("");
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader(menu_text::kAdvanced))
    {
        ImGui::TextDisabled("%s", menu_text::kAdvancedHint);
        if (BeginRows("advanced_rows"))
        {
            for (const AdvancedKnob& k : kAdvanced)
            {
                if (!s.ManagedProjectKnob(k.name)) { continue; }
                float value = 0.0f;
                if (!s.Read(k.name, value)) { continue; }
                ImGui::PushID(k.name);
                const bool overridden = s.Overrides().count(k.name) != 0;
                Row(k.label);
                bool edited = false;
                if (k.toggle)
                {
                    bool on = value != 0.0f;
                    if (ImGui::Checkbox("##v", &on)) { value = on ? 1.0f : 0.0f; edited = true; }
                }
                else if (k.integer)
                {
                    int v = static_cast<int>(std::lround(value));
                    const float width = ImGui::GetContentRegionAvail().x - (overridden ? ImGui::GetFontSize() * 5.0f : 0.0f);
                    ImGui::SetNextItemWidth(std::max(width, ImGui::GetFontSize() * 4.0f));
                    if (ImGui::SliderInt("##v", &v, static_cast<int>(k.min), static_cast<int>(k.max))) { value = static_cast<float>(v); edited = true; }
                }
                else
                {
                    const float width = ImGui::GetContentRegionAvail().x - (overridden ? ImGui::GetFontSize() * 5.0f : 0.0f);
                    ImGui::SetNextItemWidth(std::max(width, ImGui::GetFontSize() * 4.0f));
                    if (ImGui::SliderFloat("##v", &value, k.min, k.max, "%.2f")) { edited = true; }
                }
                if (overridden)
                {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(menu_text::kBackToLevels))
                    {
                        s.ClearOverride(k.name);
                        changed = true;
                    }
                }
                if (edited)
                {
                    s.SetOverride(k.name, std::clamp(value, k.min, k.max));
                    changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (s.Preset() == -2) { ImGui::TextDisabled("%s", menu_text::kAdvancedNeedsLevel); }
    }
    ImGui::EndDisabled();
    if (changed)
    {
        const settings::ScalabilityReport r = s.Apply("settings menu");
        char text[96];
        std::snprintf(text, sizeof(text), menu_text::kApplied, r.keysWritten);
        status_ = text;
    }
}

void GameSettingsMenu::DrawImage()
{
    settings::ImageSettings image = settings::CurrentImageSettings();
    bool changed = false;
    if (!BeginRows("image_rows")) { return; }
    Row(menu_text::kBrightness, menu_text::kBrightnessHint);
    if (ImGui::SliderFloat("##brightness", &image.brightnessEv, settings::kBrightnessMinEv, settings::kBrightnessMaxEv, "%+.1f EV"))
    {
        image.brightnessEv = std::round(image.brightnessEv * 10.0f) / 10.0f;
        changed = true;
    }
    Hint(menu_text::kBrightnessHint);
    Row(menu_text::kFieldOfView, menu_text::kFieldOfViewHint);
    bool levelFov = image.fovDeg <= 0.0f;
    if (ImGui::Checkbox(menu_text::kLevelFov, &levelFov))
    {
        image.fovDeg = levelFov ? 0.0f : 90.0f;
        changed = true;
    }
    if (!levelFov)
    {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderFloat("##fov", &image.fovDeg, settings::kFovMinDeg, settings::kFovMaxDeg, "%.0f deg"))
        {
            image.fovDeg = std::round(image.fovDeg);
            changed = true;
        }
        Hint(menu_text::kFieldOfViewHint);
    }
    ImGui::EndTable();
    if (changed) { settings::ApplyImageSettings(image); }
}

void GameSettingsMenu::DrawControls(InputManager& input)
{
    settings::ControlsSettings controls = settings::CurrentControlsSettings();
    bool changed = false;
    if (BeginRows("controls_rows"))
    {
        Row(menu_text::kSensitivity);
        if (ImGui::SliderFloat("##sensitivity", &controls.sensitivity, settings::kSensitivityMin, settings::kSensitivityMax, "x%.2f",
                               ImGuiSliderFlags_Logarithmic))
        {
            changed = true;
        }
        Row(menu_text::kInvertY);
        if (ImGui::Checkbox("##invert", &controls.invertY)) { changed = true; }
        Row(menu_text::kSmoothing, menu_text::kSmoothingHint);
        float smoothing = g_cameraMouseSmoothingMs;
        if (ImGui::SliderFloat("##smoothing", &smoothing, 0.0f, 100.0f, "%.0f ms"))
        {
            g_cameraMouseSmoothingMs = std::round(smoothing);
        }
        Hint(menu_text::kSmoothingHint);
        ImGui::EndTable();
    }
    if (changed) { settings::ApplyControlsSettings(controls); }

    ImGui::SeparatorText(menu_text::kKeys);
    // The game's keys, not the developer's (F2..F11, Ctrl+...): the bindings file's own descriptions of these actions.
    static const std::pair<const char*, const char*> kGameKeys[] = {
        { "MoveZ", "Forward / back" }, { "MoveX", "Left / right" }, { "Sprint", "Sprint" }, { "Jump", "Jump" },
        { "Crouch", "Crouch" }, { "LookToggle", "Look (free camera: hold)" }, { "PossessEject", "Walk from here / fly" },
        { "Interact", "Take the helm / leave it" }, { "ToggleView", "Camera view" }, { "Anchor", "Anchor" },
        { "Autopilot", "Autopilot" }, { "WatchNext", "Watch the next boat" }, { "FreeMouse", "Free the mouse" },
        { "StopPlay", "Menu" },
    };
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
    {
        for (const auto& [action, label] : kGameKeys)
        {
            for (const ActionMap::BindingDesc& d : input.GetBindingDescs())
            {
                if (d.action != action) { continue; }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(label);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", d.keys.c_str());
                break;
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("%s", menu_text::kKeysNote);
}

bool GameSettingsMenu::Draw(InputManager& input)
{
    if (!open_) { return false; }
    GameMenuStyle style;
    style.Backdrop();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(style.Em(40.0f), viewport->Size.x * 0.95f), std::min(style.Em(34.0f), viewport->Size.y * 0.9f)),
                             ImGuiCond_Always);
    bool closed = false;
    ImGui::Begin(menu_text::kSettingsTitle, nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
                                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar);
    style.Title(menu_text::kSettingsTitle);
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    if (ImGui::BeginTabBar("settings_tabs"))
    {
        const char* tabs[] = { menu_text::kTabDisplay, menu_text::kTabGraphics, menu_text::kTabImage, menu_text::kTabControls };
        for (int t = 0; t < 4; ++t)
        {
            const ImGuiTabItemFlags flags = requestedTab_ == t ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(tabs[t], nullptr, flags)) { continue; }
            ImGui::BeginChild("tab", ImVec2(0.0f, -footer), ImGuiChildFlags_None);
            switch (t)
            {
            case 0: DrawDisplay(); break;
            case 1: DrawGraphics(); break;
            case 2: DrawImage(); break;
            default: DrawControls(input); break;
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        requestedTab_ = -1;
        ImGui::EndTabBar();
    }
    DrawDisplayConfirm();

    ImGui::Separator();
    // While auto-detect measures, Apply would keep a level it is only trying: wait for it (Esc still cancels it).
    const bool measuring = settings::TheScalability().AutodetectRunning();
    const bool dirty = !Same(Take(), atOpen_);
    const float w = style.Em(7.0f);
    ImGui::BeginDisabled(measuring || confirming_);
    if (dirty)
    {
        if (ImGui::Button(menu_text::kApply, ImVec2(w, 0.0f)))
        {
            std::string why;
            if (SaveAll(why))
            {
                atOpen_ = Take();
                status_ = menu_text::kSaved;
            }
            else
            {
                status_ = std::string(menu_text::kNotSaved) + why;
                LOG_WARNING(logging::LogCategory::App, "settings: not saved: {}", why);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(menu_text::kCancel, ImVec2(w, 0.0f)))
        {
            settings::TheScalability().AbortAutodetect();
            Restore(atOpen_);
            status_.clear();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(menu_text::kBack, ImVec2(w, 0.0f)))
    {
        // Back keeps what was changed (Lyra's): saved, and closed.
        std::string why;
        if (dirty && !SaveAll(why)) { LOG_WARNING(logging::LogCategory::App, "settings: not saved: {}", why); }
        open_ = false;
        closed = true;
    }
    ImGui::EndDisabled();
    if (!status_.empty())
    {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", status_.c_str());
    }
    ImGui::End();
    return closed;
}
