#pragma once

#include <filesystem>
#include <string>

// Crash reports (docs/distribution_build_plan.md §7): a minidump, a Fatal line in the session log (flushed at once) and,
// in Shipping, a message box with where both lie. Process-wide: the unhandled-exception filter (any thread's access
// violation, stack overflow, ...), SIGABRT (abort(), and std::terminate on a thread whose handler is the CRT's -- it ends in
// abort), the pure-call and the invalid-parameter handlers, and std::terminate on the threads that call InstallThisThread.
// The dump is written by a thread made at Install, so a thread out of stack still gets one. What it cannot catch:
// __fastfail and /GS (0xC0000409) leave the process without running anything in it -- the exit code and the log's last
// lines are all there is (WER or an outside watcher would be needed).
namespace crash
{
    struct Options
    {
        std::filesystem::path dumpFolder = L"crashes"; // relative to the current directory (Shipping: the user's folder)
        bool messageBox = false;                        // Shipping: tell the player
        unsigned messageBoxTimeoutMs = 0;               // 0 = until closed (the self-test closes its own)
        std::wstring product = L"test_cube";
    };

    void Install(const Options& options);
    [[nodiscard]] bool Installed();
    // The renderer saw the device go (Renderer::ReportDeviceRemovalOnce): the report that follows tells the player it was
    // the graphics card or its driver, and to update the driver.
    void NoteDeviceRemoved(unsigned reason);
    // std::terminate's handler is per thread in the MSVC CRT: a thread of ours calls this when it starts.
    void InstallThisThread();

    // --crash-selftest=<arm>: av | stack | abort | terminate | purecall | invalidparam | fastfail -- installs the handler
    // (the message box closing itself after 3 s) and crashes that way. Never returns; an unknown arm returns 2.
    int SelfTest(const std::string& arm, Options options);
}
