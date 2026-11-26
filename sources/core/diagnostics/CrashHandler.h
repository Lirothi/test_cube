#pragma once

#include <filesystem>
#include <span>
#include <string>

// Crash reports (docs/distribution_build_plan.md §7): a minidump, a Fatal line in the session log (flushed at once) and,
// in Shipping, a message box with where both lie. Process-wide: the unhandled-exception filter (any thread's access
// violation, stack overflow, ...), SIGABRT (abort(), and std::terminate on a thread whose handler is the CRT's -- it ends in
// abort), the pure-call and the invalid-parameter handlers, and std::terminate on the threads that call InstallThisThread.
// The dump is written by a thread made at Install, so a thread out of stack still gets one. What it cannot catch:
// __fastfail and /GS (0xC0000409) leave the process without running anything in it -- the exit code and the log's last
// lines are all there is (WER or an outside watcher would be needed).
struct _EXCEPTION_POINTERS; // <Windows.h>'s, declared here so the header does not pull Windows in

namespace crash
{
    struct Options
    {
        std::filesystem::path dumpFolder = L"crashes"; // relative to the current directory (Shipping: the user's folder)
        bool messageBox = false;                        // Shipping: tell the player
        unsigned messageBoxTimeoutMs = 0;               // 0 = until closed (the self-test closes its own)
        std::wstring product = L"test_cube";
        // How many minidumps the folder keeps: at Install the older ones go. 0 = all of them (a player's are few and
        // theirs to send); a dev build names 10 -- it crashes on purpose and often, and a dump is megabytes.
        unsigned keepDumps = 0;
    };

    void Install(const Options& options);
    [[nodiscard]] bool Installed();
    // The renderer saw the device go (Renderer::ReportDeviceRemovalOnce): the report that follows tells the player it was
    // the graphics card or its driver, and to update the driver.
    void NoteDeviceRemoved(unsigned reason);
    // std::terminate's handler is per thread in the MSVC CRT: a thread of ours calls this when it starts.
    void InstallThisThread();
    // Run on the aborting thread before abort()'s report (SIGABRT): Debug's stderr pipe empties into the log there, so an
    // `assert`'s text -- written to stderr just before its abort() -- precedes the crash line (app/BootLogSession.h).
    void SetAbortHook(void (*hook)());
    // A frame task's exception, first chance (TaskSystem::RunTask's SEH filter, before the stack unwinds to its catch):
    // the throw site's frames go to the log (module+offset, named by tools/symbolize.py) and a minidump is written with
    // the exception's context. The report that follows (InvariantFailure, abort) names that dump instead of writing one
    // of the catch site, where the stack has already unwound (the second review of wave 1: R15.3 said the dump held the
    // throw site; it held RunTask's catch). The first call only.
    void NoteThrowSite(::_EXCEPTION_POINTERS* pointers);

    // An arm another subsystem owns (its own contract broken on purpose: the task system's dependency rules, ...): `run`
    // is expected not to return.
    struct SelfTestArm
    {
        const char* name;
        void (*run)();
    };

    // --crash-selftest=<arm>: av | stack | abort | terminate | throw | purecall | invalidparam | fastfail | overflow (a
    // tc::inl_vector outgrown: its Fatal line, then abort), or one of `moreArms` -- installs the handler (the message box
    // closing itself after 3 s) and crashes that way. Never returns; an unknown arm returns 2, an arm that did not crash 1.
    int SelfTest(const std::string& arm, Options options, std::span<const SelfTestArm> moreArms = {});
    // --crash-selftest=terminate-frame (main.cpp arms it instead of calling SelfTest): the game starts as always, and the
    // first frame after the renderer's BeginFrame throws out of a noexcept function on the game thread (SelfTestFrame): to
    // std::terminate, on the thread whose terminate handler the renderer sets there over this one
    // (docs/architecture_review_plan.md M2).
    void ArmSelfTestInFrame();
    void SelfTestFrame(); // throws once when armed, does nothing otherwise
}
