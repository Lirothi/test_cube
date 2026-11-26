#pragma once

#include <crtdbg.h>
#include <windows.h>

#include "app/BuildVersion.h"
#include "core/logging/Log.h"
#include "core/memory/MemScope.h"

namespace boot
{
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
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT); // abort(): no dialog, no WER
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        // `assert` reports through the WIDE path (_wassert -> _CrtDbgReportW), which the narrow
        // _CrtSetReportHook never sees; the W2 hook is the one that fires for it.
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
