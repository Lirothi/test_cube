#pragma once
#if WITH_EDITOR

#include <functional>
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

    // What the watch is doing right now, for anything that wants to show it -- the tray
    // icon does. Deliberately a snapshot of FACTS rather than a formatted line: the phase
    // and a deadline, so a display can count the seconds down itself and stay exact between
    // polls, which are half a minute apart.
    struct Status
    {
        enum class Phase
        {
            Reaching,     // the server has not answered yet; nothing is being counted
            EditorOpen,   // an editor is open, so the clock does not run at all
            CountingDown, // the last editor closed; `retireAtTick` is when the server goes
            Retired,      // the server is gone, by our hand or its own
        };

        Phase phase = Phase::Reaching;
        // GetTickCount64() value at which the server is retired; 0 unless CountingDown.
        unsigned long long retireAtTick = 0;
        unsigned long serverPid = 0;
    };

    // Called from the watch thread whenever the status changes, so it must not touch a
    // window directly. Passed to Run rather than registered globally: there is one watch
    // per process and tying the two together makes that visible.
    using StatusCallback = std::function<void(const Status&)>;

    // Runs the loop to completion. Returns the process exit code: 0 when the server was
    // retired or had already gone, 1 when it could not be reached at all.
    int Run(const Options& options, const StatusCallback& onStatus = {});

    // Ends a running Run() from another thread -- the tray menu does. `retireServer` says
    // whether to stop the server on the way out; false leaves it running with nobody
    // watching it, which is the user's to choose and the menu says so.
    //
    // The wait between polls is a wait on THIS signal with a timeout, not a sleep, because
    // a menu click has to act now and not in the up-to-thirty seconds until the next poll.
    void RequestStop(bool retireServer);

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
