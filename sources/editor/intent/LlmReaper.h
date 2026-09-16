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
// It is rebuilt here. The editor spawns this process alongside the server; it polls the
// server's /metrics endpoint, and when the REQUEST COUNTERS have not moved for the idle
// window it kills the server and exits. It holds no model, no device and no window -- it is
// a loop and a socket.
//
// Counting activity from /metrics rather than from the editor is also strictly more honest
// than what came before: the old idle timer could only see requests the editor itself made,
// so chatting with the server's own web page did not count as use and the model could be
// retired out from under a conversation. Every request counts now, whoever made it.
namespace llmreaper
{
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
}

#endif // WITH_EDITOR
