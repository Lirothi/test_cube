#pragma once

#include <filesystem>
#include <string>
#include <vector>

// What a shipped build does differently at its start (docs/distribution_build_plan.md §6, §7). Compiled in every
// configuration (the pieces are testable in a dev build); main.cpp calls them under TC_PACKAGED (Shipping and Development),
// the switch filter under TC_SHIPPING alone.
namespace shipping
{
    // The switches a shipped build keeps (§7, §11 item 6): --window, --level, --log-level, --user-dir, and for the gates
    // --shot, --shot-delay, --cam-pos, --cam-rot, --wind-freeze, --time, --play, --crash-selftest. The rest is dropped
    // (named in `dropped`): no harness, no --set, no tool that rewrites content. Split as Windows splits it
    // (CommandLineToArgvW: "--user-dir=C:\a b" arrives quoted whole) and rejoined as --name="value" where the value has a
    // space -- the form main.cpp's readers take.
    [[nodiscard]] std::string FilterCommandLine(const wchar_t* fullCommandLine, std::vector<std::string>& dropped);

    // The exe's folder (the content root of a shipped build).
    [[nodiscard]] std::filesystem::path ExeFolder();
    // %LOCALAPPDATA%\<product> -- by the known-folder API, wide: a Cyrillic user name survives.
    [[nodiscard]] std::filesystem::path DefaultUserFolder(const wchar_t* product);
    // Makes the user's folder and its parts (settings, cache, logs, crashes, bot_memory) and makes it the current
    // directory, so what is written relative (logs/, imgui.ini, --shot) lands there. False (and why) when it cannot.
    bool PrepareUserFolder(const std::filesystem::path& folder, std::wstring& error);

    // A message for the player (no window of ours exists yet, or none any more). The environment variable
    // TEST_CUBE_MESSAGE_TIMEOUT_MS closes it after that long -- the gates' way past a box nobody clicks.
    void ShowMessage(const std::wstring& title, const std::wstring& text, bool error = true);
    // The same box with a timeout of its own (0 = the variable's, else until closed); MessageBoxTimeoutW, user32's.
    void ShowMessageTimed(const std::wstring& title, const std::wstring& text, bool error, unsigned timeoutMs);

    // config/boot.json in the packages (the cook writes it from the profile): what a shipped build starts.
    struct BootConfig
    {
        std::string product;
        std::string version;
        std::string bootLevel;
        std::string startMode;    // "play" or "free"
    };
    bool ReadBootConfig(BootConfig& out, std::string& error);
}
