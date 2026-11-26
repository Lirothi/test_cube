#include "app/BootParse.h"

#include "app/BootOptions.h"
#include "core/logging/Log.h"
#include "rendering/core/DlssHandler.h" // sl::DLSSMode: --dlss

#include <windows.h> // the wide command line: --intent, --chat

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

std::string ExtractArgValue(const char* cmd, const char* key)
{
    const char* p = std::strstr(cmd, key);
    if (!p) { return {}; }
    const bool wholeQuoted = p > cmd && p[-1] == '"'; // "--shot=C:\My Shots\a.png", quoted as one argument
    p += std::strlen(key);
    while (*p == '=' || std::isspace(static_cast<unsigned char>(*p))) { ++p; }
    std::string v;
    if (*p == '"' || wholeQuoted) {
        if (*p == '"') { ++p; }
        while (*p && *p != '"') { v.push_back(*p); ++p; }
    } else {
        while (*p && !std::isspace(static_cast<unsigned char>(*p))) { v.push_back(*p); ++p; }
    }
    return v;
}

namespace boot::parse
{
// --- editor ----------------------------------------------------------------------------------------------------------

// "--log-window": open the session-log viewer at boot (see App.h).
void LogWindow(const char* cmd)
{
    if (std::strstr(cmd, "--log-window")) {
        Boot().editor.logWindow = true;
    }
}

// "--mem-window": open Developer Controls on the Memory tab at boot (see App.h).
void MemWindow(const char* cmd)
{
    if (std::strstr(cmd, "--mem-window")) {
        Boot().editor.memoryWindow = true;
    }
}

// "--dev-tab=<label>": open Developer Controls on that tab at boot (see App.h).
void DevTab(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--dev-tab=")) {
        const char* value = flag + std::strlen("--dev-tab=");
        const char* end = value;
        while (*end && *end != ' ' && *end != '\t') { ++end; }
        Boot().editor.devTab.assign(value, end);
    }
}

// "--settings-menu[=display|graphics|image|controls|menu]": the player's settings open at boot (see App.h).
void SettingsMenu(const char* cmd)
{
    if (std::strstr(cmd, "--settings-menu")) {
        const std::string tab = ExtractArgValue(cmd, "--settings-menu=");
        Boot().editor.settingsMenu = tab.empty() || tab.rfind("--", 0) == 0 ? "graphics" : tab;
    }
}

// "--mcp-port=<n>": serve the editor's MCP on that port (a script's editor beside the owner's, see App.h).
void McpPort(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--mcp-port=")) {
        Boot().editor.mcpPort = std::atoi(flag + std::strlen("--mcp-port="));
    }
}

// "--editor": open the Level Editor at boot, so a --shot can photograph its panels.
void Editor(const char* cmd)
{
    if (std::strstr(cmd, "--editor")) {
        Boot().editor.openEditor = true;
    }
}

// "--play": boot into the play session (docs/play_mode_plan.md). With the editor in the build the world stands until then; without it the world runs and the camera is free until then.
void Play(const char* cmd)
{
    if (std::strstr(cmd, "--play")) {
        Boot().editor.play = true;
    }
}

// "--edit-mesh=<path>": open the Mesh Editor on that asset at boot (implies --editor).
void EditMesh(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--edit-mesh=")) {
        const char* value = flag + std::strlen("--edit-mesh=");
        const char* end = value;
        while (*end && *end != ' ' && *end != '\t') { ++end; }
        Boot().editor.editMesh.assign(value, end);
        Boot().editor.openEditor = Boot().editor.openEditor || !Boot().editor.editMesh.empty();
    }
}

// "--build-dist": the Build Distribution window open at boot (implies --editor), for a --shot of it.
void BuildDist(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--build-dist")) {
        Boot().dist.buildDist = true;
        Boot().editor.openEditor = true;
        // "--build-dist=<profile>[,override-licences]": the profile it opens on.
        if (flag[std::strlen("--build-dist")] == '=') {
            const char* value = flag + std::strlen("--build-dist=");
            const char* end = value;
            while (*end && *end != ' ' && *end != '\t') { ++end; }
            Boot().dist.buildDistProfile.assign(value, end);
        }
    }
}

// "--show-import[=<name>]": the Import Assets window open at boot, <name> selected (implies
// --editor). Not "--import...": that prefix is the offline converter's, matched by strstr.
void ShowImport(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--show-import")) {
        const char* value = flag + std::strlen("--show-import");
        if (*value == '=') {
            ++value;
            const char* end = value;
            while (*end && *end != ' ' && *end != '\t') { ++end; }
            Boot().editor.importItem.assign(value, end);
        }
        Boot().editor.showImport = true;
        Boot().editor.openEditor = true;
    }
}

// "--inspect=<type>[#<group>]": the Inspector on that environment object, scrolled to the group (implies --editor);
// or on the level object of that NAME ("--inspect="small_yacht 001#end"", quoted when it holds a space; any group
// of an object scrolls to the Inspector's end, where its type's own rows -- Physics, Buoyancy -- are drawn).
void Inspect(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--inspect=")) {
        const char* value = flag + std::strlen("--inspect=");
        const char* end = value;
        if (*value == '"')
        {
            ++value;
            end = value;
            while (*end && *end != '"') { ++end; }
        }
        else
        {
            while (*end && *end != ' ' && *end != '\t') { ++end; }
        }
        Boot().editor.inspect.assign(value, end);
        Boot().editor.openEditor = Boot().editor.openEditor || !Boot().editor.inspect.empty();
    }
}

// --- frame -----------------------------------------------------------------------------------------------------------

void Level(const char* cmd)
{
    if (std::strstr(cmd, "--level=")) {
        Boot().frame.levelPath = ExtractArgValue(cmd, "--level="); // quoted or not
    }
}

// "--shot=<path>" grabs the backbuffer to a PNG after "--shot-delay=<sec>" (default 7)
// and exits — reliable headless verification on the flip-model swapchain.
void Shot(const char* cmd)
{
    if (std::strstr(cmd, "--shot=")) {
        Boot().frame.shotPath = ExtractArgValue(cmd, "--shot="); // a path with a space comes quoted
    }
}

void ShotDelay(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--shot-delay=")) {
        Boot().frame.shotDelaySec = std::atof(flag + std::strlen("--shot-delay="));
    }
}

// Phase series from one process (see App.h): "--shot-count=<n> --shot-step=<sec>" with
// --wind-freeze steps the frozen clock between frames instead of relaunching per phase.
void ShotCount(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--shot-count=")) {
        Boot().frame.shotCount = std::max(std::atoi(flag + std::strlen("--shot-count=")), 1);
    }
}

void ShotStep(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--shot-step=")) {
        Boot().frame.shotStepSec = std::atof(flag + std::strlen("--shot-step="));
    }
}

void ShotInterval(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--shot-interval=")) {
        // 0 = CONSECUTIVE frames (the save is synchronous, so the next loop iteration shoots the
        // next frame) -- what a temporal flicker needs; with --wind-freeze + --shot-step each frame
        // still advances the clock by exactly the step.
        Boot().frame.shotIntervalSec = std::max(std::atof(flag + std::strlen("--shot-interval=")), 0.0);
    }
}

// "--cam-pos=x,y,z --cam-rot=x,y,z,w": reproduce the exact view a screenshot was taken
// from. Both are printed verbatim by the on-screen HUD, so a shot round-trips into a
// headless repro instead of being re-guessed by hand (see AGENTS.md).
void CamPose(const char* cmd)
{
    const auto readFloats = [cmd](const char* key, float* out, int n) -> bool {
        const char* flag = std::strstr(cmd, key);
        if (!flag) { return false; }
        const char* p = flag + std::strlen(key);
        for (int i = 0; i < n; ++i) {
            out[i] = static_cast<float>(std::atof(p));
            const char* comma = std::strchr(p, ',');
            if (!comma) { return i == n - 1; }
            p = comma + 1;
        }
        return true;
    };
    const bool hasPos = readFloats("--cam-pos=", Boot().frame.camPos, 3);
    const bool hasRot = readFloats("--cam-rot=", Boot().frame.camRot, 4);
    Boot().frame.camOverride = hasPos || hasRot;
    // "--cam-fly=x,z": constant camera drift (m/s) — the headless stand-in for flying.
    readFloats("--cam-fly=", Boot().frame.camFly, 2);
    // "--cam-fly-delay=<sec>": hold still this long first (motion-onset transient capture).
    readFloats("--cam-fly-delay=", &Boot().frame.camFlyDelay, 1);
    // "--cam-fly-yaw=<deg/s>": turn while drifting (headless mouse look; moves the cascade boxes).
    readFloats("--cam-fly-yaw=", &Boot().frame.camFlyYaw, 1);
    // "--cam-orbit=<radius>,<deg/s>[,<sec per frame>]": ring around the start position (grove
    // fly-through); the third value steps the orbit by a fixed time per frame (App.h).
    readFloats("--cam-orbit=", Boot().frame.camOrbit, 3);
}

// Temporary VSM perf harness (see App.h). "--profdump=<path>" dumps profiler rows + exits.
// "--vsm-refdist=<f>" pre-sets the runtime VSM tunable for a sweep;
// "--vsm-resident" flips g_residentIterOnly on. All read the globals in VirtualShadowMap.h.
void ProfDump(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--profdump=")) {
        const char* p = flag + std::strlen("--profdump=");
        std::string path;
        while (*p && !std::isspace(static_cast<unsigned char>(*p))) { path.push_back(*p); ++p; }
        Boot().frame.profDumpPath = path;
    }
}

// "--trace=<frames>": headless equivalent of the CaptureTrace key.
void Trace(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--trace=")) {
        Boot().frame.traceFrames = static_cast<uint32_t>(std::atoi(flag + std::strlen("--trace=")));
    }
}

// "--window=<w>x<h>": borderless window with exactly that client size (see App.h).
void Window(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--window=")) {
        const char* p = flag + std::strlen("--window=");
        Boot().frame.windowSize[0] = std::atoi(p);
        if (const char* x = std::strchr(p, 'x')) { Boot().frame.windowSize[1] = std::atoi(x + 1); }
        if (Boot().frame.windowSize[0] < 64 || Boot().frame.windowSize[1] < 64) { Boot().frame.windowSize[0] = Boot().frame.windowSize[1] = 0; }
    }
}

// "--dlss=<off|perf|balanced|quality|ultraperf|ultraquality|dlaa>": boot upscaler mode, so a
// native-resolution capture no longer needs an F-key by hand (see App.h). An unrecognised
// value leaves the compiled default in place rather than failing — check the capture's
// render scale, not just the exit code, when a shot comes back at the wrong resolution.
void Dlss(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--dlss=")) {
        const char* p = flag + std::strlen("--dlss=");
        const auto is = [p](const char* name) { return std::strncmp(p, name, std::strlen(name)) == 0; };
        if      (is("ultraperf"))    { Boot().frame.dlssMode = static_cast<int>(sl::DLSSMode::eUltraPerformance); }
        else if (is("ultraquality")) { Boot().frame.dlssMode = static_cast<int>(sl::DLSSMode::eUltraQuality); }
        else if (is("off"))          { Boot().frame.dlssMode = static_cast<int>(sl::DLSSMode::eOff); }
        else if (is("perf"))         { Boot().frame.dlssMode = static_cast<int>(sl::DLSSMode::eMaxPerformance); }
        else if (is("balanced"))     { Boot().frame.dlssMode = static_cast<int>(sl::DLSSMode::eBalanced); }
        else if (is("quality"))      { Boot().frame.dlssMode = static_cast<int>(sl::DLSSMode::eMaxQuality); }
        else if (is("dlaa"))         { Boot().frame.dlssMode = static_cast<int>(sl::DLSSMode::eDLAA); }
    }
}

// "--scalability=low|medium|high|ultra|off": the graphics preset at boot, over what the player saved
// (docs/scalability_plan.md §4.5). Kept by Shipping's switch filter.
void Scalability(const char* cmd)
{
    if (const std::string level = ExtractArgValue(cmd, "--scalability="); !level.empty()) {
        std::string l = level;
        for (char& c : l) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        Boot().frame.scalability = l == "low" ? 0 : l == "medium" ? 1 : l == "high" ? 2 : l == "ultra" ? 3 : l == "off" ? -1 :
                            l == "auto" ? -3 : -2;
        if (Boot().frame.scalability == -2) {
            LOG_WARNING(logging::LogCategory::App, "--scalability={}: low, medium, high, ultra, auto or off", level);
        }
    }
}

// "--no-hud": empty HUD text buffer, so a --shot carries no frame-varying FPS readout.
void NoHud(const char* cmd)
{
    if (std::strstr(cmd, "--no-hud")) {
        Boot().frame.hudHidden = true;
    }
}

// "--sweep=<setting>:<v0>,<v1>,...[;<setting>:<v0>,...]": one process, one shot per value
// (see App.h). The first value count drives --shot-count, so the caller does not have to
// keep the two in sync; the groups after a ';' advance in step with the first.
void Sweep(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--sweep=")) {
        const char* p = flag + std::strlen("--sweep=");
        const char* end = p;
        while (*end && *end != ' ' && *end != '\t') { ++end; }
        bool first = true;
        while (p < end) {
            const char* semi = std::strchr(p, ';');
            const char* groupEnd = (semi && semi < end) ? semi : end;
            const char* colon = std::strchr(p, ':');
            if (colon && colon < groupEnd) {
                std::vector<float> values;
                const char* v = colon + 1;
                while (v < groupEnd) {
                    values.push_back(static_cast<float>(std::atof(v)));
                    const char* comma = std::strchr(v, ',');
                    if (!comma || comma >= groupEnd) { break; }
                    v = comma + 1;
                }
                if (first) {
                    Boot().frame.sweepSetting.assign(p, colon);
                    Boot().frame.sweepValues = std::move(values);
                } else {
                    Boot().frame.sweepExtra.emplace_back(std::string(p, colon), std::move(values));
                }
                first = false;
            }
            p = groupEnd + 1;
        }
        if (!Boot().frame.sweepValues.empty()) {
            Boot().frame.shotCount = static_cast<int>(Boot().frame.sweepValues.size());
        }
    }
}

// --- harness ---------------------------------------------------------------------------------------------------------

// "--settings-dump=<file>" (architecture plan R2.0): every readable setting after the warmup, then quit.
void GraphicsResetAtDump(const char* cmd)
{
    Boot().harness.graphicsResetAtDump = std::strstr(cmd, "--graphics-reset-at-dump") != nullptr;
    if (std::strstr(cmd, "--graphics-reset-at-dump=")) {
        Boot().harness.graphicsResetSetting = ExtractArgValue(cmd, "--graphics-reset-at-dump=");
    }
}

void SettingsDump(const char* cmd)
{
    if (std::strstr(cmd, "--settings-dump=")) {
        Boot().harness.settingsDumpPath = ExtractArgValue(cmd, "--settings-dump="); // a path with a space comes quoted
    }
}

// "--boot-dump=<file>" (architecture plan R3.2b): what the command line set, then out before the App starts.
void BootDump(const char* cmd)
{
    if (std::strstr(cmd, "--boot-dump=")) {
        Boot().harness.bootDumpPath = ExtractArgValue(cmd, "--boot-dump=");
    }
}

// "--scalability-check": the graphics levels' gate in this process (see App.cpp Boot().harness.scalabilityCheck).
void ScalabilityCheck(const char* cmd)
{
    if (std::strstr(cmd, "--scalability-check")) {
        Boot().harness.scalabilityCheck = true;
    }
}

// "--intent=<phrase>": one phrase through the editor's real pipeline, then quit.
// Implies --editor, because the pipeline lives behind EditorController::Draw.
void Intent(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--intent=")) {
        const char* value = flag + std::strlen("--intent=");
        // A PHRASE HAS SPACES IN IT, and stopping at the first one is how the first
        // version of this turned "bury all coconut_palm" into "bury". Quotes do not
        // survive the shell reliably either, so the rule is: take everything up to the
        // next flag (" --") or the end of the line, and strip surrounding quotes if any
        // did survive.
        // WinMain's cmd is ANSI, so a Russian phrase arrives as question marks --
        // which is exactly what happened the first time this was pointed at the model.
        // Take the phrase from the WIDE command line and convert it to UTF-8, which is
        // what the prompt and the session log want anyway.
        if (const wchar_t* wide = ::GetCommandLineW()) {
            if (const wchar_t* wflag = std::wcsstr(wide, L"--intent=")) {
                const wchar_t* wvalue = wflag + 9;   // wcslen(L"--intent=")
                std::wstring wphrase;
                if (*wvalue == L'"') {
                    ++wvalue;
                    while (*wvalue && *wvalue != L'"') { wphrase.push_back(*wvalue++); }
                } else {
                    const wchar_t* wnext = std::wcsstr(wvalue, L" --");
                    wphrase.assign(wvalue, wnext ? wnext : wvalue + std::wcslen(wvalue));
                }
                const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wphrase.c_str(),
                    static_cast<int>(wphrase.size()), nullptr, 0, nullptr, nullptr);
                std::string utf8(static_cast<std::size_t>(needed), char{});
                ::WideCharToMultiByte(CP_UTF8, 0, wphrase.c_str(),
                    static_cast<int>(wphrase.size()), utf8.data(), needed, nullptr, nullptr);
                value = nullptr;   // the wide path won
                Boot().harness.intentPhrase = utf8;
            }
        }

        std::string phrase;
        if (value && *value == '"') {
            ++value;
            while (*value && *value != '"') { phrase.push_back(*value++); }
        } else if (value) {
            const char* nextFlag = std::strstr(value, " --");
            phrase.assign(value, nextFlag ? nextFlag : value + std::strlen(value));
        }
        // Shells hand over quotes half-eaten -- PowerShell delivered the closing one and
        // not the opening one -- so strip whitespace and quotes from BOTH ends rather
        // than trusting either to arrive.
        const auto trimmable = [](char c) { return c == ' ' || c == '	' || c == '"'; };
        if (!phrase.empty()) {
            Boot().harness.intentPhrase = phrase;
        }
        std::string& out = Boot().harness.intentPhrase;
        while (!out.empty() && trimmable(out.back())) { out.pop_back(); }
        std::size_t front = 0;
        while (front < out.size() && trimmable(out[front])) { ++front; }
        out = out.substr(front);
        Boot().editor.openEditor = true;
    }
}

void IntentRun(const char* cmd)
{
    if (std::strstr(cmd, "--intent-run")) {
        Boot().harness.intentRun = true;
    }
}

void IntentRepeat(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--intent-repeat=")) {
        Boot().harness.intentRepeat = std::max(1, std::atoi(flag + std::strlen("--intent-repeat=")));
    }
}

void IntentTimeout(const char* cmd)
{
    if (const char* flag = std::strstr(cmd, "--intent-timeout=")) {
        Boot().harness.intentTimeoutSec = std::max(5.0, std::atof(flag + std::strlen("--intent-timeout=")));
    }
}

// "--chat=<phrase>": one message through the Model Chat panel. Wide command line for
// the same reason --intent uses it -- lpCmdLine is ANSI and turns Russian into
// question marks before anything else gets a look at it.
void Chat(const char* cmd)
{
    if (const wchar_t* wide = ::GetCommandLineW()) {
        // One parse for both flags. `--then=<phrase>` sends a SECOND phrase down the
        // same box once the first has landed, in ONE editor session -- the only way to
        // test what the model remembers, since a second process is a second session and
        // remembers nothing by construction. The harness presses Run for the first one,
        // so the question has something real to be about.
        const auto phraseAfter = [wide](const wchar_t* flagName) -> std::string {
            const wchar_t* wflag = std::wcsstr(wide, flagName);
            if (!wflag) { return {}; }
            const wchar_t* wvalue = wflag + std::wcslen(flagName);
            std::wstring wphrase;
            if (*wvalue == L'"') {
                ++wvalue;
                while (*wvalue && *wvalue != L'"') { wphrase.push_back(*wvalue++); }
            } else {
                const wchar_t* wnext = std::wcsstr(wvalue, L" --");
                wphrase.assign(wvalue, wnext ? wnext : wvalue + std::wcslen(wvalue));
            }
            const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wphrase.c_str(),
                static_cast<int>(wphrase.size()), nullptr, 0, nullptr, nullptr);
            std::string utf8(static_cast<std::size_t>(needed), char{});
            ::WideCharToMultiByte(CP_UTF8, 0, wphrase.c_str(),
                static_cast<int>(wphrase.size()), utf8.data(), needed, nullptr, nullptr);
            // Shells hand quotes over half-eaten, exactly as --intent found: strip them
            // from BOTH ends rather than trusting either to arrive.
            const auto trimmable = [](char c) { return c == ' ' || c == '\t' || c == '"'; };
            while (!utf8.empty() && trimmable(utf8.back())) { utf8.pop_back(); }
            std::size_t front = 0;
            while (front < utf8.size() && trimmable(utf8[front])) { ++front; }
            return utf8.substr(front);
        };
        Boot().harness.chatPhrase = phraseAfter(L"--chat=");
        Boot().harness.chatThenPhrase = phraseAfter(L"--then=");
        if (!Boot().harness.chatPhrase.empty()) {
            Boot().editor.openEditor = true;
        }
    }
}

} // namespace boot::parse
