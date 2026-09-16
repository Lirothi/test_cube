// The model watchdog's entry point. Everything it does lives in editor/intent/LlmReaper,
// which the editor shares -- this file exists only so that code can be an executable of its
// own rather than a mode of the engine binary.
//
// Windowed subsystem, not console: it is spawned by the editor and must not flash a console
// window on a machine where somebody is working.
#include <windows.h>

#include "core/logging/Log.h"
#include "editor/intent/LlmReaper.h"

int WINAPI wWinMain(
    _In_ HINSTANCE,
    _In_opt_ HINSTANCE,
    _In_ LPWSTR,
    _In_ int)
{
    // The ANSI command line, because the flags are ASCII and LlmReaper parses narrow text.
    const char* commandLine = ::GetCommandLineA();

    llmreaper::Options options;
    if (!llmreaper::ParseCommandLine(commandLine, options))
    {
        // Started with nothing to watch. Saying so costs a line in the session log and
        // saves the next person wondering why a process appeared and vanished.
        logging::LogConfig config;
        logging::Initialize(config);
        LOG_ERROR(logging::LogCategory::Editor,
            "model reaper: started without --reap-model=<host:port>; nothing to watch");
        logging::Shutdown();
        return 2;
    }

    logging::LogConfig config;
    logging::ApplyCommandLine(commandLine, config);
    logging::Initialize(config);
    logging::SetCurrentThreadName("ModelReaper");
    const int code = llmreaper::Run(options);
    logging::Shutdown();
    return code;
}
