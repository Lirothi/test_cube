#pragma once

#include <crtdbg.h>
#include <fcntl.h>
#include <io.h>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/BuildVersion.h"
#include "core/diagnostics/CrashHandler.h"
#include "core/logging/Log.h"
#include "core/memory/MemScope.h"

namespace boot
{
#if defined(_DEBUG)
// Debug, no debugger: the C runtime's stderr is a pipe whose lines this process writes into the session log -- an
// `assert`'s text among them ("Assertion failed: <expression>, file <file>, line <n>", as Fatal), which LogSession sends
// there instead of a message box. A thread empties the pipe every 50 ms; abort() empties it first (crash::SetAbortHook),
// so what was written before it -- the assert's line -- is in the log ahead of the crash report's.
namespace stderr_log
{
inline CRITICAL_SECTION g_lock;
inline HANDLE g_read = nullptr;
inline std::string g_line; // a line still being written

inline void Pump()
{
    if (g_read == nullptr) { return; }
    EnterCriticalSection(&g_lock);
    char buffer[2048];
    for (;;)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(g_read, nullptr, 0, nullptr, &available, nullptr) || available == 0) { break; }
        DWORD got = 0;
        const DWORD want = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
        if (!ReadFile(g_read, buffer, want, &got, nullptr) || got == 0) { break; } // vfs-ok: this process's pipe, not content
        for (DWORD i = 0; i < got; ++i)
        {
            const char c = buffer[i];
            if (c != '\n')
            {
                if (c != '\r') { g_line.push_back(c); }
                continue;
            }
            if (g_line.rfind("Assertion failed", 0) == 0)
            {
                LOG_FATAL(logging::LogCategory::Core, "CRT assert: {}", g_line);
            }
            else if (!g_line.empty())
            {
                LOG_ERROR(logging::LogCategory::Core, "stderr: {}", g_line);
            }
            g_line.clear();
        }
    }
    LeaveCriticalSection(&g_lock);
}

inline DWORD WINAPI Reader(void*)
{
    for (;;)
    {
        Pump();
        Sleep(50);
    }
}

inline void Install()
{
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, nullptr, 64 * 1024)) { return; }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(writeEnd), _O_WRONLY | _O_TEXT);
    if (fd < 0)
    {
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return;
    }
    // A GUI program's stderr stream starts with no descriptor at all (_NO_CONSOLE_FILENO), and a _dup2 onto fd 2 does not
    // give it one: the stream is reopened first (on NUL, a descriptor of its own), then that descriptor becomes the pipe.
    FILE* reopened = nullptr;
    const bool haveStream = freopen_s(&reopened, "NUL", "w", stderr) == 0; // vfs-ok: the null device, a stream's descriptor
    const int target = haveStream ? _fileno(stderr) : -1;
    if (target < 0 || _dup2(fd, target) != 0)
    {
        _close(fd); // closes writeEnd
        CloseHandle(readEnd);
        return;
    }
    _close(fd); // the stream's descriptor holds its own duplicate of the write end
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(target)));
    InitializeCriticalSection(&g_lock);
    g_read = readEnd;
    crash::SetAbortHook(&Pump);
    // thread-ok: nothing it calls throws but std::string's growth (bad_alloc ends in std::terminate, the crash handler's)
    if (const HANDLE reader = CreateThread(nullptr, 64 * 1024, Reader, nullptr, 0, nullptr)) { CloseHandle(reader); }
}
} // namespace stderr_log
#endif

// The log session is the first thing that exists and the last thing that goes away: every
// early-return harness below runs inside it, and the RAII object flushes the session file on
// each of those returns. --log-level / --log-category / --log-sync / --log-no-file / --log-file
// are read here so they reach the harnesses too.
struct LogSession {
    explicit LogSession(const char* commandLine) {
        MEM_SCOPE(MemTag::Log); // the logger's rings and tables (memory profiling M3)
        logging::LogConfig config;
        logging::ApplyCommandLine(commandLine, config);
        config.commandLine = commandLine;
        config.version = build::VersionString();
        logging::Initialize(config);
        logging::SetCurrentThreadName("Main");
#if defined(_DEBUG)
        // A failed CRT assert used to reach the session log as NOTHING: the report went to a
        // message box that waited for a click, and the process then died with abort()'s exit
        // code 3 or STATUS_BREAKPOINT -- a Debug-only failure read as "the log stops" (volumetric
        // fog plan A: an assert in the first fog frame cost an hour of guessing and a click from
        // the user). Diagnostics are log events: the report text lands as [FATAL], and with no
        // debugger attached the process ends itself with exit code 3 -- no window, nothing to
        // click, the headless gates read the log. Under a debugger the default break still
        // happens (the hook returns FALSE), so interactive debugging is unchanged.
        //
        // The hook below takes _ASSERT / _ASSERTE / _RPT (the _CrtDbgReport road). `assert` does NOT
        // come here: the UCRT's _wassert reports on its own road, a message box in a GUI program
        // (2026-11-26: the R5.3 slot asserts' controls waited on "Assertion failed!" on a hidden
        // desktop until killed). Without a debugger it goes to stderr instead -- its text, then
        // abort(), which crash::Install's SIGABRT reports (a Fatal line, a minidump, exit 3) -- and
        // stderr is a pipe whose lines reach the log (stderr_log). With a debugger: the box, Retry
        // breaks there.
        if (!IsDebuggerPresent())
        {
            stderr_log::Install();
            _set_error_mode(_OUT_TO_STDERR);
        }
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // abort(): no dialog, no WER
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        // _ASSERTE's wide reports (_CrtDbgReportW) never reach the narrow _CrtSetReportHook; the W2
        // hook sees both.
        _CrtSetReportHookW2(_CRT_RPTHOOK_INSTALL, [](int reportType, wchar_t* message, int* returnValue) -> int {
            if (reportType == _CRT_ASSERT || reportType == _CRT_ERROR)
            {
                char narrow[2048] = {};
                if (message) { WideCharToMultiByte(CP_UTF8, 0, message, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr); }
                LOG_FATAL(logging::LogCategory::Core, "CRT {}: {}",
                          reportType == _CRT_ASSERT ? "assert" : "error", narrow);
                logging::Shutdown(); // flush the session file; the process is about to die
                if (!IsDebuggerPresent()) { TerminateProcess(GetCurrentProcess(), 3u); }
            }
            if (returnValue) { *returnValue = 0; }
            return FALSE; // default handling continues (debugger break)
        });
#endif
    }
    ~LogSession() { logging::Shutdown(); }
};
}
