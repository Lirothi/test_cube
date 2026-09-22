#include "editor/intent/LlmReaper.h"
#if WITH_EDITOR

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/logging/Log.h"
#include "editor/intent/LlmClient.h"

namespace
{
    bool ReadFlag(const char* commandLine, const char* key, std::string& out)
    {
        const char* flag = std::strstr(commandLine, key);
        if (!flag)
        {
            return false;
        }
        const char* value = flag + std::strlen(key);
        const char* end = value;
        while (*end && *end != ' ' && *end != '\t')
        {
            ++end;
        }
        out.assign(value, end);
        return !out.empty();
    }

    // Only the COUNTERS. A prometheus dump also carries gauges -- queue depth, cache usage --
    // and at least one of them can differ between two idle scrapes; comparing the whole body
    // would then read as activity forever and the server would never be retired. A `_total`
    // line moves when a request is served and at no other time.
    std::string CountersOnly(const std::string& body)
    {
        std::string counters;
        std::size_t start = 0;
        while (start < body.size())
        {
            std::size_t end = body.find('\n', start);
            if (end == std::string::npos)
            {
                end = body.size();
            }
            const std::string line = body.substr(start, end - start);
            if (!line.empty() && line[0] != '#' && line.find("_total") != std::string::npos)
            {
                counters += line;
                counters += '\n';
            }
            start = end + 1;
        }
        return counters;
    }

    bool ProcessAlive(unsigned long pid)
    {
        if (pid == 0)
        {
            return false;
        }
        HANDLE handle = ::OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!handle)
        {
            return false;
        }
        const DWORD wait = ::WaitForSingleObject(handle, 0);
        ::CloseHandle(handle);
        return wait == WAIT_TIMEOUT;
    }

    // Signalled by RequestStop; waited on instead of sleeping between polls. Manual-reset,
    // so a stop asked for before the loop reaches the wait is still there when it does --
    // the tray can be clicked at any moment, including the first second.
    HANDLE StopEvent()
    {
        static HANDLE handle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return handle;
    }

    std::atomic<bool> g_retireOnStop{false};

    bool KillProcessTree(unsigned long pid)
    {
        HANDLE handle = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!handle)
        {
            return false;
        }
        const BOOL ok = ::TerminateProcess(handle, 0);
        ::CloseHandle(handle);
        return ok != FALSE;
    }
}

namespace llmreaper
{
    bool ParseCommandLine(const char* commandLine, Options& outOptions)
    {
        if (!commandLine)
        {
            return false;
        }
        if (!ReadFlag(commandLine, "--reap-model=", outOptions.endpoint))
        {
            return false;
        }
        std::string pid;
        if (ReadFlag(commandLine, "--reap-model-pid=", pid))
        {
            outOptions.serverPid = std::strtoul(pid.c_str(), nullptr, 10);
        }
        std::string idle;
        if (ReadFlag(commandLine, "--reap-idle=", idle))
        {
            outOptions.idleSeconds = std::max(30, std::atoi(idle.c_str()));
        }
        outOptions.pollSeconds = std::max(2, std::min(outOptions.idleSeconds / 4, 30));
        return true;
    }

    EditorSessionMark::EditorSessionMark()
    {
        // Not an error if it already exists: a second editor shares the object, and the
        // countdown starts only when the LAST handle closes.
        handle_ = ::CreateMutexW(nullptr, FALSE, kEditorSessionMutex);
    }

    EditorSessionMark::~EditorSessionMark()
    {
        if (handle_)
        {
            ::CloseHandle(static_cast<HANDLE>(handle_));
        }
    }

    bool AnyEditorSessionOpen()
    {
        // SYNCHRONIZE is the cheapest right that still proves the object exists; we never
        // wait on it. Opening succeeds exactly while some editor holds a handle.
        const HANDLE handle = ::OpenMutexW(SYNCHRONIZE, FALSE, kEditorSessionMutex);
        if (!handle)
        {
            return false;
        }
        ::CloseHandle(handle);
        return true;
    }

    void RequestStop(bool retireServer)
    {
        g_retireOnStop.store(retireServer, std::memory_order_relaxed);
        ::SetEvent(StopEvent());
    }

    int Run(const Options& options, const StatusCallback& onStatus)
    {
        LOG_INFO(logging::LogCategory::Editor,
            "model reaper: watching {} (pid {}); the {}s timer starts when the last editor "
            "closes, and a new session resets it. Editors and the process are checked every "
            "second, /metrics every {}s",
            options.endpoint, options.serverPid, options.idleSeconds, options.pollSeconds);

        // TWO CLOCKS. Whether an editor is open and whether the server process is alive are
        // checked EVERY SECOND -- an OpenMutex and an OpenProcess, microseconds each -- so the
        // countdown starts, and the icon changes colour, the moment the last editor closes.
        // /metrics is asked only every `pollSeconds`: it is an HTTP round trip to a server that
        // may be busy generating, and all it can add is "somebody used the web page", which
        // only ever makes the server live LONGER, so being late with it is the safe direction.
        //
        // It used to be one clock, thirty seconds, for everything. The icon then stayed green
        // for up to half a minute after the editor closed, and the five minutes started from
        // whichever poll noticed -- 300 to 330 seconds in practice, with the tray showing a
        // number the watch was not actually keeping.
        //
        // And the idle time is now a DEADLINE on the clock rather than a sum of poll
        // intervals, so the log line, the tooltip and the moment of retirement are one number.
        constexpr DWORD kTickMs = 1000;
        const unsigned long long startedAt = ::GetTickCount64();
        const unsigned long long idleMs =
            static_cast<unsigned long long>(options.idleSeconds) * 1000ull;
        const unsigned long long metricsEveryMs =
            static_cast<unsigned long long>(options.pollSeconds) * 1000ull;

        std::string lastCounters;
        bool everReached = false;
        bool sawEditor = false;
        // When the countdown last (re)started: the moment the last editor closed, or the
        // last request anybody made with no editor open. The server goes at this + idleMs.
        unsigned long long quietSince = startedAt;
        unsigned long long nextMetricsAt = startedAt;   // the first poll is immediate

        Status status;
        status.serverPid = options.serverPid;
        bool published = false;
        // On CHANGE only: the tick is once a second and most seconds nothing has moved. The
        // tray counts the seconds down itself from the deadline, so it needs no nudging.
        const auto publish = [&](Status::Phase phase, unsigned long long retireAtTick)
        {
            if (published && status.phase == phase && status.retireAtTick == retireAtTick)
            {
                return;
            }
            published = true;
            status.phase = phase;
            status.retireAtTick = retireAtTick;
            if (onStatus)
            {
                onStatus(status);
            }
        };
        publish(Status::Phase::Reaching, 0);

        // Somebody clicked the tray menu. Retiring the server is one choice and walking
        // away from it is the other; both end the watch, and the second one is worth a
        // WARNING because it leaves the memory allocated with nothing to free it.
        const auto obeyStopRequest = [&]() -> int
        {
            if (g_retireOnStop.load(std::memory_order_relaxed))
            {
                LOG_INFO(logging::LogCategory::Editor,
                    "model reaper: asked to stop llama-server pid={} now", options.serverPid);
                KillProcessTree(options.serverPid);
                publish(Status::Phase::Retired, 0);
            }
            else
            {
                LOG_WARNING(logging::LogCategory::Editor,
                    "model reaper: closing on request; llama-server pid={} keeps running and "
                    "nothing is left to retire it", options.serverPid);
            }
            return 0;
        };

        for (;;)
        {
            if (::WaitForSingleObject(StopEvent(), 0) == WAIT_OBJECT_0)
            {
                return obeyStopRequest();
            }

            // The server going away on its own -- crash, Task Manager, a second editor
            // retiring it -- ends the watch. A reaper outliving what it watches would be the
            // orphan it exists to prevent.
            if (options.serverPid != 0 && !ProcessAlive(options.serverPid))
            {
                LOG_INFO(logging::LogCategory::Editor,
                    "model reaper: server is already gone; nothing to retire");
                publish(Status::Phase::Retired, 0);
                return 0;
            }

            const unsigned long long now = ::GetTickCount64();

            // AN OPEN EDITOR STOPS THE CLOCK ENTIRELY -- it does not merely count as
            // activity. Somebody with the editor open is using the server whether or not they
            // have typed a phrase in the last five minutes, and retiring it under them would
            // charge the next phrase a reload of the whole model.
            const bool editorOpen = AnyEditorSessionOpen();
            if (editorOpen && !sawEditor)
            {
                LOG_INFO(logging::LogCategory::Editor,
                    "model reaper: an editor is open; holding the server");
                sawEditor = true;
            }
            else if (!editorOpen && sawEditor)
            {
                LOG_INFO(logging::LogCategory::Editor,
                    "model reaper: the last editor closed; retiring the server in {}s "
                    "unless one opens again", options.idleSeconds);
                sawEditor = false;
                quietSince = now;   // the countdown starts HERE, within a second of the close
            }

            if (now >= nextMetricsAt)
            {
                nextMetricsAt = now + metricsEveryMs;
                const llmclient::Response response =
                    llmclient::Get(options.endpoint, "/metrics", 5);
                if (response.ok)
                {
                    everReached = true;
                    std::string counters = CountersOnly(response.body);
                    if (counters.empty())
                    {
                        // No counters at all means the endpoint is not what we assumed.
                        // Rather than retire a server that might be busy, fall back to
                        // watching the whole body.
                        counters = response.body;
                    }
                    // With no editor open, a request can still come from the server's own web
                    // page. That counts: somebody is using the model, just not through us. The
                    // first answer counts the same way, so the clock starts when the server
                    // is up rather than while it was still loading.
                    if (counters != lastCounters)
                    {
                        lastCounters = std::move(counters);
                        if (!editorOpen)
                        {
                            quietSince = now;
                        }
                    }
                }
                else if (everReached)
                {
                    // It answered before and does not now: treat it as gone rather than as
                    // idle, because killing something unreachable is the one case where being
                    // wrong costs nothing.
                    LOG_INFO(logging::LogCategory::Editor,
                        "model reaper: server stopped answering; retiring it");
                    KillProcessTree(options.serverPid);
                    publish(Status::Phase::Retired, 0);
                    return 0;
                }
                else if (now - startedAt > 600000ull)
                {
                    // Never reached it. The model's load had ten minutes to finish.
                    LOG_ERROR(logging::LogCategory::Editor,
                        "model reaper: {} never answered in 10 minutes; giving up",
                        options.endpoint);
                    return 1;
                }
            }

            if (!everReached)
            {
                publish(Status::Phase::Reaching, 0);
            }
            else if (editorOpen)
            {
                publish(Status::Phase::EditorOpen, 0);
            }
            else
            {
                const unsigned long long retireAt = quietSince + idleMs;
                publish(Status::Phase::CountingDown, retireAt);
                if (now >= retireAt)
                {
                    LOG_INFO(logging::LogCategory::Editor,
                        "model reaper: no editor and no requests for {}s, stopping "
                        "llama-server pid={}", options.idleSeconds, options.serverPid);
                    KillProcessTree(options.serverPid);
                    publish(Status::Phase::Retired, 0);
                    return 0;
                }
            }

            // A WAIT, not a sleep: a tray click is obeyed at once rather than at the end of
            // the tick.
            if (::WaitForSingleObject(StopEvent(), kTickMs) == WAIT_OBJECT_0)
            {
                return obeyStopRequest();
            }
        }
    }
}

#endif // WITH_EDITOR
