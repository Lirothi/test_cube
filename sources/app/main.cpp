#include <crtdbg.h>
#include "core/memory/MemScope.h"
#include <windows.h>
#include "core/diagnostics/DiagPaths.h"
#include <mimalloc.h>
// operator new/delete are replaced in core/memory/MemNew.cpp (mimalloc + per-thread counters,
// memory profiling plan M0); this file no longer includes mimalloc-new-delete.h.
#include "app/App.h"
#include "app/BuildVersion.h"
#include "app/GpuRequirements.h"
#include "app/Shipping.h"
#include "app/BootParse.h"    // R3.2b: ExtractArgValue, the switches' parse
#include "app/BootSwitches.h"
#include "app/BootLogSession.h" // R3.2d: the log session WinMain opens first
#include "app/BootTools.h"      // R3.2d: the tools that run instead of the game
#include "core/settings/SettingsRegistry.h"
#include "core/diagnostics/CrashHandler.h"
#include "rendering/core/StreamlineSignature.h"

#include "app/diagnostics/RtAudit.h" // game thread plan G9: --rt-audit
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "core/logging/diagnostics/LogStress.h"
#include "core/memory/MemTrace.h"
#include "rendering/core/MemGates.h"
#include "rendering/core/MemoryReport.h"
#include "rendering/diagnostics/RetireSelfTest.h" // --retire-selftest (R16.1)
#include "core/diagnostics/ThrowSelfTest.h" // --throw-selftest, --crash-selftest=view-throw (the second review of wave 1)
#include "rendering/lighting/TimeOfDaySettings.h"  // T3: --tod-legacy-light

#include <algorithm>
#include <filesystem> // --tessy-upgrade=all walks models/
#include <vector>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

int ForceMi() { return mi_version(); }


namespace
{


// "--set=<name>:<value>[;<name>:<value>...]": hold settings at fixed values for the run, from the
// same name table --sweep uses (App::ApplyFixedSettings). EVERY occurrence, not just the first:
// this used to be a single strstr, so a command line with two `--set=` flags silently applied one
// and dropped the other -- and a measurement taken that way reports the settings you TYPED, not
// the ones that ran. Cost me an A/B. The documented `--set=a:1;b:2` form still works; the loop
// just also accepts the other one. Parsed ABOVE the scene-stress branch (occlusion plan S3a): a
// GBV gate has to be able to switch the pass under test on.
void ParseFixedSettingsArg(const char* lpCmdLine)
{
    if (!lpCmdLine) { return; }
    for (const char* flag = std::strstr(lpCmdLine, "--set="); flag != nullptr;
         flag = std::strstr(flag, "--set=")) {
        const char* p = flag + std::strlen("--set=");
        const char* end = p;
        while (*end && *end != ' ' && *end != '\t') { ++end; }
        flag = end; // resume scanning after this flag's value
        while (p < end) {
            const char* semi = std::strchr(p, ';');
            const char* itemEnd = (semi && semi < end) ? semi : end;
            const char* colon = std::strchr(p, ':');
            if (colon && colon < itemEnd) {
                Boot().frame.fixedSettings.emplace_back(std::string(p, colon),
                                             static_cast<float>(std::atof(colon + 1)));
            }
            if (itemEnd >= end) { break; }
            p = itemEnd + 1;
        }
    }
}

// Time-of-day plan T2: "--time=HH:MM", "--date=YYYY-MM-DD", "--time-speed=<x>", "--time-freeze" --
// spelled for people, carried as the ordinary `tod.*` numbers of --set (App::ApplySweepValue), so they
// apply at the same point and log the same `--set` line. --time-speed starts the clock (x times the
// level's rate); --time-freeze stops it, and wins.
void ParseTimeOfDayArgs(const char* lpCmdLine)
{
    if (!lpCmdLine) { return; }
    if (const char* flag = std::strstr(lpCmdLine, "--time=")) {
        const char* p = flag + std::strlen("--time=");
        const int hours = std::atoi(p);
        const char* colon = std::strchr(p, ':');
        const char* end = p;
        while (*end && *end != ' ' && *end != '\t') { ++end; }
        const int minutes = (colon && colon < end) ? std::atoi(colon + 1) : 0;
        Boot().frame.fixedSettings.emplace_back("tod.time", static_cast<float>(hours) + static_cast<float>(minutes) / 60.0f);
    }
    if (const char* flag = std::strstr(lpCmdLine, "--date=")) {
        int year = 0, month = 0, day = 0;
        if (sscanf_s(flag + std::strlen("--date="), "%d-%d-%d", &year, &month, &day) == 3) {
            Boot().frame.fixedSettings.emplace_back("tod.year", static_cast<float>(year));
            Boot().frame.fixedSettings.emplace_back("tod.month", static_cast<float>(month));
            Boot().frame.fixedSettings.emplace_back("tod.day", static_cast<float>(day));
        }
    }
    if (const char* flag = std::strstr(lpCmdLine, "--time-speed=")) {
        Boot().frame.fixedSettings.emplace_back("tod.speed", static_cast<float>(std::atof(flag + std::strlen("--time-speed="))));
        Boot().frame.fixedSettings.emplace_back("tod.animate", 1.0f);
    }
    if (std::strstr(lpCmdLine, "--time-freeze")) {
        Boot().frame.fixedSettings.emplace_back("tod.animate", 0.0f);
    }
    // T3 rollback: a level without `timeOfDay` lights with its `directionalLight` as authored.
    if (std::strstr(lpCmdLine, "--tod-legacy-light")) {
        g_timeOfDayConvertsDirectionalLight = false;
    }
}

void EnableDpiAwareness()
{
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

    // Try to opt-in to the most precise DPI awareness available on the host OS.
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        auto setProcessDpiAwarenessContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setProcessDpiAwarenessContext &&
            setProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        {
            return;
        }
    }

    HMODULE shcore = GetModuleHandleW(L"shcore.dll");
    if (!shcore)
    {
        shcore = LoadLibraryW(L"shcore.dll");
    }
    if (shcore)
    {
        using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
        auto setProcessDpiAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (setProcessDpiAwareness &&
            SUCCEEDED(setProcessDpiAwareness(2 /*PROCESS_PER_MONITOR_DPI_AWARE*/)))
        {
            return;
        }
    }

    if (user32)
    {
        using SetProcessDpiAwareFn = BOOL(WINAPI*)();
        if (auto setProcessDpiAware = reinterpret_cast<SetProcessDpiAwareFn>(
                GetProcAddress(user32, "SetProcessDPIAware")))
        {
            setProcessDpiAware();
        }
    }
}
} // namespace

// Entry point
int WINAPI WinMain(
    _In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPSTR lpCmdLine,
    _In_ int nShowCmd
)
{
#if TC_PACKAGED
    // A packaged build -- Shipping, and Development, the same with every developer feature kept (docs/scalability_plan.md
    // §7) -- before anything else: DLLs only from the exe's folder and System32; the content at the exe's folder and
    // everything written in the user's (%LOCALAPPDATA%\test_cube, Development's test_cube_dev, --user-dir), which becomes
    // the current directory -- the log, imgui.ini, the caches and the crash dumps land there. Shipping also keeps only the
    // switches a player's build has (Shipping.cpp).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    std::vector<std::string> droppedSwitches; // a shipped build's switches it ignores (none in Development)
#if TC_SHIPPING
    static std::string shippingCommandLine = shipping::FilterCommandLine(GetCommandLineW(), droppedSwitches);
    lpCmdLine = shippingCommandLine.data();
#endif
    std::filesystem::path shippingUserFolder;
    {
        const std::string userDir = ExtractArgValue(lpCmdLine, "--user-dir");
        std::error_code ec;
        shippingUserFolder = userDir.empty() ? shipping::DefaultUserFolder(build::Shipping() ? L"test_cube" : L"test_cube_dev")
                                             : std::filesystem::absolute(std::filesystem::path(vfs::Widen(userDir)), ec);
        vfs::SetContentRoot(shipping::ExeFolder());
        std::wstring why;
        if (!shipping::PrepareUserFolder(shippingUserFolder, why)) {
            shipping::ShowMessage(L"test_cube", L"test_cube cannot start: " + why);
            return 1;
        }
        vfs::SetUserDirectory(shippingUserFolder);
    }
#endif

    // "--log-stress" runs the logging stress harness instead of the app; exit code is the number
    // of failed checks. It owns the logger's lifetime itself (repeated Initialize/Shutdown is one
    // of the things it tests), so it is dispatched BEFORE the normal session below.
    if (lpCmdLine && std::strstr(lpCmdLine, "--log-stress") != nullptr) {
        return RunLogStress(lpCmdLine);
    }

    // The log session (app/BootLogSession.h): the first thing that exists and the last that goes away -- every
    // early-return tool below runs inside it.
    boot::LogSession logSession(lpCmdLine);
    settings::Registry().ReportRefused(); // R2.1: rows refused at static initialisation (a name twice), now that the log is up

    // --help, --crash-selftest (app/BootTools.cpp): a tool that answers before anything else exists.
    if (const int code = boot::RunEarlyTools(lpCmdLine); code >= 0) {
        return code;
    }
#if !TC_PACKAGED
    // A dev build's crash is reported too (2026-11-18): a Fatal line saying what happened and a minidump in logs/crashes.
    // Until now only a packaged build installed the handler (below), and a dev run that died read as "the log just
    // stops": an access violation on a worker, a throw nobody caught, the debug layer's break with no debugger, abort()
    // out of an invariant. No message box -- the gates read the log and the exit code -- and every harness and tool below
    // runs under it. Not under a debugger: it takes the fault first-hand, and abort() must still break into it. A CRT
    // assert keeps its own road (the report hook above ends the process before abort is reached). The newest 10 dumps
    // are kept.
    if (!IsDebuggerPresent()) {
        crash::Options options;
        options.dumpFolder = L"logs/crashes";
        options.keepDumps = 10;
        crash::Install(options);
    }
#endif
    // --gpu-check (any build but Shipping, which runs it anyway): what the card has against what a shipped build needs.
    if (!build::Shipping() && lpCmdLine && std::strstr(lpCmdLine, "--gpu-check")) {
        const gpu::RequirementCheck check = gpu::CheckRequirements();
        LOG_INFO(logging::LogCategory::Render, "gpu check: {} -- {} (driver {}){}", check.ok ? "PASS" : "FAIL", check.adapter,
                 check.driver, check.ok ? std::string() : ": " + vfs::Narrow(gpu::RequirementMessage(check, L"test_cube")));
        return check.ok ? 0 : 1;
    }

#if TC_PACKAGED
    // The packages, the D3D12 runtime, the card and Streamline's signature (app/BootTools.cpp).
    if (const int code = boot::PreparePackagedContent(lpCmdLine, droppedSwitches, shippingUserFolder); code >= 0) {
        return code;
    }
#else
    // The loose tree's content tools: --vfs / --pak-*, --shader-model / -record / -cook, --cook (app/BootTools.cpp).
    if (const int code = boot::RunContentTools(lpCmdLine); code >= 0) {
        return code;
    }
#endif

    // The harnesses that run instead of the game -- self-tests, stress, benchmarks, --import (app/BootTools.cpp).
    if (const int code = boot::RunHarnessTools(lpCmdLine); code >= 0) {
        return code;
    }

    EnableDpiAwareness();

    // "--set=..." and the time of day's switches: recorded here, applied once the level is in (App::ApplyFixedSettings).
    ParseFixedSettingsArg(lpCmdLine);
    ParseTimeOfDayArgs(lpCmdLine);
    // "--memtrace[=all|cpu|gpu][,stacks=big|all|off|1/N][,start=<frame>]": every allocation and
    // free into traces/memtrace_*.mtrace (memory profiling plan M4, core/memory/MemTrace.h);
    // tools/memtrace.py reads it. Starts here unless start= delays it -- above the scene-stress
    // branch, whose level churn is the trace's hardest case.
    {
        memtrace::Options memtraceOptions;
        if (memtrace::ParseSwitch(lpCmdLine, memtraceOptions)) {
            memtrace::Configure(memtraceOptions);
        }
    }
    // "--mem-selftest=leak@<tag>[,kb=<n>][,cpu|gpu|both]" / "--mem-readout=<s>[,warmup=<s>][,limit=<MB/s>]":
    // the memory gates (plan M6, rendering/core/MemGates.h). Above the scene-stress branch: its
    // revisit check is proven with the self-test's leak.
    memgate::ParseAndConfigure(lpCmdLine);

    // "--throw-selftest=texstream|texdecode|fsprobe": that detached task throws once; its owner must see it land and start
    // again (core/diagnostics/ThrowSelfTest.h; the second review of wave 1, item 2). The verdict is the log's and the
    // exit code's.
    if (lpCmdLine) {
        if (const std::string site = ExtractArgValue(lpCmdLine, "--throw-selftest"); !site.empty()) {
            if (!diag::ArmThrowSelfTest(site)) {
                return 2;
            }
        }
    }

    // The switches of a run: each a row of the switch table whose parse function reads it -- BootOptions' groups
    // (R3.2b, app/BootParse.cpp) and the debug switches (R3.2c, app/BootParseDev.cpp) -- in the table's order.
    if (lpCmdLine) {
        boot::ParseSwitches(lpCmdLine);
        // The stress's level over --level, as when its branch returned before --level was read (R3.3e): roughness and sky
        // run on wind_test, --stress-level boots into its level.
        if (Boot().harness.sceneStressRoughness || Boot().harness.sceneStressSky) {
            Boot().frame.levelPath = "data/levels/wind_test.json";
        } else if (Boot().harness.sceneStress && !Boot().harness.sceneStressExtraLevel.empty()) {
            Boot().frame.levelPath = Boot().harness.sceneStressExtraLevel;
        }
    }
    // The asset tools that rewrite or read the loose tree: --uv-density, --seam-ratio, --tessy-upgrade,
    // --water-mask-lid, --lod-report, --reimport (app/BootTools.cpp).
    if (const int code = boot::RunAssetTools(lpCmdLine); code >= 0) {
        return code;
    }

#if TC_PACKAGED
    // What the build starts (config/boot.json): its level, Play by its rule (app/BootTools.cpp).
    if (const int code = boot::ReadPackagedBootLevel(); code >= 0) {
        return code;
    }
#endif

    // "--boot-dump=<file>" (R3.2b): the parse is over -- every field, and no App.
    if (!Boot().harness.bootDumpPath.empty()) {
        const bool written = WriteBootDump(Boot().harness.bootDumpPath);
        LOG_INFO(logging::LogCategory::App, "boot dump: {} {}", written ? "written to" : "could NOT write", Boot().harness.bootDumpPath);
        return written ? 0 : 2;
    }

    App app;
    app.Run(hInstance, nShowCmd);
    memtrace::Stop(); // App::Run stops it after the main loop; this catches a run that ended another way
    // A --mem-readout's failing series, else an --rt-audit's moved fields; 0 otherwise.
    const int exitCode = []() {
        if (const int memExit = memgate::ExitCode(); memExit != 0) { return memExit; }
        if (const int retireExit = render::RetireSelfTestExitCode(); retireExit >= 0) { return retireExit; } // --retire-selftest
        if (const int throwExit = diag::ThrowSelfTestExitCode(); throwExit >= 0) { return throwExit; } // --throw-selftest
        if (Boot().harness.scalabilityCheckExit != 0) { return Boot().harness.scalabilityCheckExit; } // --scalability-check's failed reads
        if (Boot().harness.sceneStressExit != 0) { return Boot().harness.sceneStressExit; } // --scene-stress ended before its verdict
        return rtaudit::ExitCode();
    }();
    // --scene-stress ends the process here, its teardown done, as its own loop always did: past this point Streamline's
    // exit handlers (sl.interposer, execute_onexit_table) read an unloaded module after a churn -- an access violation in
    // two stress runs of three, never in an ordinary run (R3.3e; the old driver never got this far). The log's footer
    // first: the session object in WinMain does not unwind.
    if (Boot().harness.sceneStress) {
        logging::Shutdown();
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(exitCode));
    }
    return exitCode;
}
