#include "editor/intent/LlmReaper.h"
#if WITH_EDITOR

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

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

    int Run(const Options& options)
    {
        LOG_INFO(logging::LogCategory::Editor,
            "model reaper: watching {} (pid {}), retiring it after {}s with no requests",
            options.endpoint, options.serverPid, options.idleSeconds);

        std::string lastCounters;
        double idleFor = 0.0;
        bool everReached = false;

        for (;;)
        {
            // The server going away on its own -- crash, Task Manager, a second editor
            // retiring it -- ends the watch. A reaper outliving what it watches would be the
            // orphan it exists to prevent.
            if (options.serverPid != 0 && !ProcessAlive(options.serverPid))
            {
                LOG_INFO(logging::LogCategory::Editor,
                    "model reaper: server is already gone; nothing to retire");
                return 0;
            }

            const llmclient::Response response =
                llmclient::Get(options.endpoint, "/metrics", 5);
            if (response.ok)
            {
                everReached = true;
                std::string counters = CountersOnly(response.body);
                if (counters.empty())
                {
                    // No counters at all means the endpoint is not what we assumed. Rather
                    // than retire a server that might be busy, fall back to watching the
                    // whole body and say so once.
                    counters = response.body;
                }
                if (counters != lastCounters)
                {
                    lastCounters = std::move(counters);
                    idleFor = 0.0;
                }
                else
                {
                    idleFor += static_cast<double>(options.pollSeconds);
                }
            }
            else if (everReached)
            {
                // It answered before and does not now: treat it as gone rather than as idle,
                // because killing something unreachable is the one case where being wrong
                // costs nothing.
                LOG_INFO(logging::LogCategory::Editor,
                    "model reaper: server stopped answering; retiring it");
                KillProcessTree(options.serverPid);
                return 0;
            }
            else
            {
                // Never reached it. Give the 38 GB load its time before deciding.
                idleFor += static_cast<double>(options.pollSeconds);
                if (idleFor > 600.0)
                {
                    LOG_ERROR(logging::LogCategory::Editor,
                        "model reaper: {} never answered in 10 minutes; giving up",
                        options.endpoint);
                    return 1;
                }
            }

            if (idleFor >= static_cast<double>(options.idleSeconds))
            {
                LOG_INFO(logging::LogCategory::Editor,
                    "model reaper: no requests for {}s, stopping llama-server pid={}",
                    options.idleSeconds, options.serverPid);
                KillProcessTree(options.serverPid);
                return 0;
            }

            std::this_thread::sleep_for(std::chrono::seconds(options.pollSeconds));
        }
    }
}

#endif // WITH_EDITOR
