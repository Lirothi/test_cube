#pragma once
#if WITH_EDITOR

#include <string>

// The watchdog that lets a llama-server outlive the editor without becoming an orphan.
//
// The server used to die WITH the editor, by construction: it was launched inside a job
// object with KILL_ON_JOB_CLOSE, so no shutdown path had to be trusted. Keeping it alive
// across a restart means giving that up, and the guarantee has to be rebuilt somewhere --
// because 38 GB left running by a program the user has already closed is exactly what that
// design existed to prevent.
//
// It is rebuilt here. The editor spawns this process alongside the server; it holds no
// model, no device and no window -- a loop, a socket and a named handle.
//
// WHAT IT COUNTS IS EDITOR SESSIONS, NOT REQUESTS, and the difference is the whole point of
// keeping the server alive. The first version retired the server after the request counters
// had been still for the idle window, which counted down while somebody was sitting in the
// editor thinking -- the server could be retired out from under an open session that simply
// had not typed anything for five minutes, and the next phrase then paid the 38 GB reload
// the arrangement exists to avoid.
//
// So the rule is the one a person would state: while ANY editor is open the timer does not
// run at all; it starts when the last one closes; and a new session starting resets it.
// That is read from a named mutex every editor holds for its lifetime (kEditorSessionMutex),
// which needs no PID, no IPC and no cooperation on the way out -- Windows releases the
// handle whether the editor exited cleanly or was killed.
namespace llmreaper
{
    // Held by every editor session, for its whole life. The reaper opens it by name: if the
    // open succeeds, somebody is editing. `Local\` rather than `Global\` because both
    // processes are the same user in the same session, and Global would need privileges for
    // no benefit.
    inline constexpr const wchar_t* kEditorSessionMutex = L"Local\\test_cube_editor_session";

    struct Options
    {
        std::string endpoint;         // "127.0.0.1:8127"
        unsigned long serverPid = 0;
        int idleSeconds = 300;
        int pollSeconds = 15;
    };

    // Parses "--reap-model=<host:port> --reap-model-pid=<pid> --reap-idle=<seconds>".
    // False when the command line is not asking for the reaper at all.
    bool ParseCommandLine(const char* commandLine, Options& outOptions);

    // Runs the loop to completion. Returns the process exit code: 0 when the server was
    // retired or had already gone, 1 when it could not be reached at all.
    int Run(const Options& options);

    // Held by an editor for as long as it lives, so the reaper can tell whether anybody is
    // still editing. Created once at editor startup and never released early: a second
    // editor opening the same name simply shares the object, and the last handle closing is
    // what starts the countdown.
    class EditorSessionMark
    {
    public:
        EditorSessionMark();
        ~EditorSessionMark();
        EditorSessionMark(const EditorSessionMark&) = delete;
        EditorSessionMark& operator=(const EditorSessionMark&) = delete;

    private:
        void* handle_ = nullptr;
    };

    // True when at least one editor session is open.
    bool AnyEditorSessionOpen();
}

#endif // WITH_EDITOR
