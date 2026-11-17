#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// The one file of a person's own settings (docs/scalability_plan.md §6.4): user_settings.json at
// vfs::WritablePath(Settings) -- the repo root in a dev build, <user>\settings\ in a packaged one. Its sections:
//   graphics     what differs from the build's graphics defaults: the Developer Controls' knobs, frame pacing, mouse
//                smoothing (GraphicsSettings.cpp; graphics_settings.json until 2026-11-16, read once to move it here)
//   scalability  the graphics levels (Scalability.h)
//   overrides    values set by hand over a managed group's level, by --set name (Scalability.h)
//   autodetect   the last measurement (App.cpp)
//   display      the window: mode, monitor, size, the frame-rate limit in the background (DisplayMode.h)
//   image        brightness, field of view
//   controls     mouse sensitivity, invert Y
// Every writer replaces its own sections and keeps the others: the file is read, changed, written whole to a temp file
// and renamed over, so a crash or a full disk leaves the old file, and two writers of different sections do not undo
// each other.
namespace settings
{
    inline constexpr const char* kUserSettingsFile = "user_settings.json";
    // 1: scalability, autodetect, display (2026-11-13). 2: graphics, overrides, image, controls (2026-11-16).
    inline constexpr int kUserSettingsVersion = 2;

    std::filesystem::path UserSettingsPath();
    // A section as JSON text; "" when the file or the section is missing, or the file is not JSON.
    std::string ReadUserSection(const std::string& key);
    // The file exists and is not a JSON object (a Warning once; its sections read as missing).
    bool UserSettingsDamaged();
    // Replaces these sections (each a JSON text; "" removes the section) and keeps the rest, atomically.
    bool WriteUserSections(const std::vector<std::pair<std::string, std::string>>& sections, std::string& why);
    inline bool WriteUserSection(const std::string& key, const std::string& jsonText, std::string& why)
    {
        return WriteUserSections({ { key, jsonText } }, why);
    }

    // The menu's Image and Controls (§6.2 items 3-4). Applied to the camera and the exposure (Camera.h
    // g_cameraLookSensitivity / g_cameraInvertY / g_cameraPlayerFovDeg, render::g_playerExposureEv).
    struct ImageSettings
    {
        float brightnessEv = 0.0f; // the player's exposure offset, -2 .. +2 EV, over the level's
        float fovDeg = 0.0f;       // horizontal at 16:9, 70 .. 110; 0 = the level's own field of view
    };
    struct ControlsSettings
    {
        float sensitivity = 1.0f;  // x the bindings' look speed, 0.25 .. 4
        bool invertY = false;      // the look's vertical turned the other way: the mouse pushed away looks DOWN (flight style)
    };
    inline constexpr float kBrightnessMinEv = -2.0f, kBrightnessMaxEv = 2.0f;
    inline constexpr float kFovMinDeg = 70.0f, kFovMaxDeg = 110.0f;
    inline constexpr float kSensitivityMin = 0.25f, kSensitivityMax = 4.0f;

    ImageSettings LoadImageSettings();
    ControlsSettings LoadControlsSettings();
    // What the knobs hold now (the menu's Cancel puts these back).
    ImageSettings CurrentImageSettings();
    ControlsSettings CurrentControlsSettings();
    void ApplyImageSettings(const ImageSettings& s);
    void ApplyControlsSettings(const ControlsSettings& s);
    bool SaveImageAndControls(const ImageSettings& image, const ControlsSettings& controls, std::string& why);
    // The controls section alone (Developer Controls > Frame: a build with the editor has no game menu).
    bool SaveControlsSettings(const ControlsSettings& controls, std::string& why);
    // At boot, after the graphics settings: the saved image and controls (nothing saved = the knobs stay as they are).
    void ApplySavedImageAndControls();
}
