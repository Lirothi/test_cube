#pragma once

// Every word the game's menu says (docs/scalability_plan.md §6.1, §10 item 8): one table, so a translation is a
// mechanical replacement of these strings and nothing else. English, as the rest of the game's UI.
namespace menu_text
{
    // The Esc menu
    inline constexpr const char* kMenuTitle = "Menu";
    inline constexpr const char* kContinue = "Continue";
    inline constexpr const char* kSettings = "Settings";
    inline constexpr const char* kLevels = "Level";
    inline constexpr const char* kLevelHere = "  (open)";
    inline constexpr const char* kLoadLevel = "Load";
    inline constexpr const char* kRestartLevel = "Restart";
    inline constexpr const char* kNoLevels = "no levels";
    inline constexpr const char* kQuit = "Quit";

    // The settings screen
    inline constexpr const char* kSettingsTitle = "Settings";
    inline constexpr const char* kTabDisplay = "Display";
    inline constexpr const char* kTabGraphics = "Graphics";
    inline constexpr const char* kTabImage = "Image";
    inline constexpr const char* kTabControls = "Controls";
    inline constexpr const char* kApply = "Apply";
    inline constexpr const char* kCancel = "Cancel";
    inline constexpr const char* kBack = "Back";
    inline constexpr const char* kNotSaved = "Not saved: ";
    inline constexpr const char* kSaved = "Saved.";

    // Display
    inline constexpr const char* kWindowMode = "Window mode";
    inline constexpr const char* kWindowModeHint = "Borderless covers the whole monitor at its own resolution; windowed keeps a frame.";
    inline constexpr const char* kWindowed = "Windowed";
    inline constexpr const char* kBorderless = "Borderless fullscreen";
    inline constexpr const char* kMonitor = "Monitor";
    inline constexpr const char* kWindowSize = "Window size";
    inline constexpr const char* kWindowSizeBorderless = "the monitor's own";
    inline constexpr const char* kWindowSizeHint = "Windowed only: a borderless window is always the monitor's native resolution.";
    inline constexpr const char* kVsync = "VSync";
    inline constexpr const char* kVsyncHint = "Wait for the monitor: no tearing, the frame rate at most the refresh rate.";
    inline constexpr const char* kFpsLimit = "Frame-rate limit";
    inline constexpr const char* kBackgroundFps = "Limit in the background";
    inline constexpr const char* kBackgroundFpsHint = "While another window has the focus: the game draws less and keeps the machine cool.";
    inline constexpr const char* kOff = "Off";
    inline constexpr const char* kConfirmTitle = "Keep these display settings?";
    inline constexpr const char* kConfirmCountdown = "Reverting in %d s.";
    inline constexpr const char* kKeep = "Keep";
    inline constexpr const char* kRevert = "Revert";

    // Graphics
    inline constexpr const char* kAutodetect = "Auto-detect";
    inline constexpr const char* kAutodetectHint = "Measures this machine's frame for a few seconds and picks the levels for 60 fps.";
    inline constexpr const char* kQuality = "Quality";
    inline constexpr const char* kQualityHint = "Every group below at one level. Ultra is the full picture; each step down takes cost away.\n"
                                                "Custom shows while the groups are on different levels.";
    inline constexpr const char* kCustom = "Custom";
    inline constexpr const char* kUltraDefault = "Ultra (default)";
    inline constexpr const char* kDefault = "Default";
    inline constexpr const char* kGroups = "Groups";
    inline constexpr const char* kPauseNote = "* a change pauses the game for a moment (render targets rebuilt, shadows reconciled).";
    inline constexpr const char* kManualChanges = "Changed by hand: %d setting(s), kept over the levels.";
    inline constexpr const char* kBackToLevels = "Back to the levels";
    inline constexpr const char* kAdvanced = "Advanced";
    inline constexpr const char* kAdvancedHint = "A few settings of the groups on their own. A value set here stays over any quality level "
                                                 "until it is taken back.";
    inline constexpr const char* kAdvancedNeedsLevel = "Choose a quality level first: these settings are set over it.";
    inline constexpr const char* kLevelValue = "level: %g";
    inline constexpr const char* kApplied = "Applied: %d setting(s) changed.";

    // Image
    inline constexpr const char* kBrightness = "Brightness";
    inline constexpr const char* kBrightnessHint = "Brighter or darker than the scene's own exposure, in stops.";
    inline constexpr const char* kFieldOfView = "Field of view";
    inline constexpr const char* kFieldOfViewHint = "Horizontal at 16:9; a wider screen sees wider, not less high.";
    inline constexpr const char* kLevelFov = "Use the scene's own";

    // Controls
    inline constexpr const char* kSensitivity = "Mouse sensitivity";
    inline constexpr const char* kInvertY = "Invert vertical look";
    inline constexpr const char* kSmoothing = "Mouse smoothing";
    inline constexpr const char* kSmoothingHint = "Eases the look in over this time: smoother turns, a little lag. 0 = off.";
    inline constexpr const char* kKeys = "Keys";
    inline constexpr const char* kKeysNote = "The keys cannot be changed here yet.";
}
