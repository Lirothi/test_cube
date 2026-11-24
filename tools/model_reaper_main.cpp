// The model watchdog's entry point. Everything it does lives in editor/intent/LlmReaper,
// which the editor shares -- this file exists only so that code can be an executable of its
// own rather than a mode of the engine binary.
//
// Windowed subsystem, not console: it is spawned by the editor and must not flash a console
// window on a machine where somebody is working. Its one visible part is a notification-area
// icon, which model_reaper_tray owns.
#include <windows.h>

#include "core/logging/Log.h"
#include "core/memory/MemScope.h"
#include "editor/intent/LlmReaper.h"
#include "model_reaper_tray.h"

// The watchdog links the engine's logger and NOT its memory profiler (core/memory/MemNew.cpp replaces operator new
// and brings the tracker with it). The logger's writer thread tags what it allocates (MEM_SCOPE, memory profiling M3)
// -- and since that line went in, this project had not linked: nobody rebuilt it, the editor went on starting the exe
// an older build had left, and the first rebuild (2026-11-24) failed with two unresolved externals and took that exe
// away. Nothing here counts memory by tag, so the two calls do nothing.
namespace mem
{
void PushTag(MemTag) noexcept {}
void PopTag() noexcept {}
} // namespace mem

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
    const int code = reapertray::RunWithTray(options);
    logging::Shutdown();
    return code;
}
