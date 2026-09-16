#include "editor/intent/LlmClient.h"
#if WITH_EDITOR

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// winsock2 BEFORE windows.h: windows.h pulls in the original winsock, and the two declare
// the same names differently. iphlpapi needs the newer one for AF_INET and htons.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

#pragma comment(lib, "winhttp.lib")

#include "core/logging/Log.h"

namespace
{
    std::wstring Widen(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
            wide.data(), needed);
        return wide;
    }

    // "127.0.0.1:8127" -> host + port.
    void SplitHostPort(const std::string& hostPort, std::wstring& outHost, int& outPort)
    {
        const std::size_t colon = hostPort.rfind(':');
        if (colon == std::string::npos)
        {
            outHost = Widen(hostPort);
            outPort = 8080;
            return;
        }
        outHost = Widen(hostPort.substr(0, colon));
        outPort = std::atoi(hostPort.c_str() + colon + 1);
        if (outPort <= 0)
        {
            outPort = 8080;
        }
    }

    // One RAII owner for the three WinHTTP handles, because five early returns and three
    // manual closes is how a handle leak gets written.
    struct WinHttpSession
    {
        HINTERNET session = nullptr;
        HINTERNET connection = nullptr;
        HINTERNET request = nullptr;

        ~WinHttpSession()
        {
            if (request)    { ::WinHttpCloseHandle(request); }
            if (connection) { ::WinHttpCloseHandle(connection); }
            if (session)    { ::WinHttpCloseHandle(session); }
        }
    };

    llmclient::Response Send(const std::string& hostPort,
        const std::string& path,
        const wchar_t* verb,
        const std::string* jsonBody,
        int timeoutSeconds,
        const llmclient::StreamCallback* onChunk = nullptr)
    {
        llmclient::Response response;

        std::wstring host;
        int port = 8080;
        SplitHostPort(hostPort, host, port);

        WinHttpSession http;
        http.session = ::WinHttpOpen(L"test_cube-editor/1",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!http.session)
        {
            response.error = "WinHttpOpen failed";
            return response;
        }

        const int milliseconds = timeoutSeconds > 0 ? timeoutSeconds * 1000 : 30000;
        ::WinHttpSetTimeouts(http.session, 5000, 5000, milliseconds, milliseconds);

        http.connection = ::WinHttpConnect(http.session, host.c_str(),
            static_cast<INTERNET_PORT>(port), 0);
        if (!http.connection)
        {
            response.error = "cannot connect to " + hostPort;
            return response;
        }

        http.request = ::WinHttpOpenRequest(http.connection, verb, Widen(path).c_str(),
            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (!http.request)
        {
            response.error = "WinHttpOpenRequest failed";
            return response;
        }

        const wchar_t* headers = L"Content-Type: application/json\r\n";
        const DWORD bodySize = jsonBody ? static_cast<DWORD>(jsonBody->size()) : 0;
        void* bodyData = jsonBody && !jsonBody->empty()
            ? const_cast<char*>(jsonBody->data()) : WINHTTP_NO_REQUEST_DATA;

        if (!::WinHttpSendRequest(http.request, jsonBody ? headers : WINHTTP_NO_ADDITIONAL_HEADERS,
                jsonBody ? static_cast<DWORD>(-1) : 0, bodyData, bodySize, bodySize, 0))
        {
            response.error = "request to " + hostPort + " failed (server not up?)";
            return response;
        }
        if (!::WinHttpReceiveResponse(http.request, nullptr))
        {
            response.error = "no response from " + hostPort;
            return response;
        }

        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        ::WinHttpQueryHeaders(http.request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
        response.statusCode = static_cast<long>(status);

        std::string body;
        for (;;)
        {
            DWORD available = 0;
            if (!::WinHttpQueryDataAvailable(http.request, &available) || available == 0)
            {
                break;
            }
            const std::size_t offset = body.size();
            body.resize(offset + available);
            DWORD read = 0;
            if (!::WinHttpReadData(http.request, body.data() + offset, available, &read))
            {
                break;
            }
            body.resize(offset + read);
            if (onChunk && *onChunk && read > 0)
            {
                // Handed over as it lands. The callback runs on this worker thread, so
                // whatever it touches has to be safe to touch from here.
                (*onChunk)(std::string_view(body).substr(offset, read));
            }
        }

        response.body = std::move(body);
        response.ok = status >= 200 && status < 300;
        if (!response.ok && response.error.empty())
        {
            response.error = "server returned HTTP " + std::to_string(status);
        }
        return response;
    }
}

namespace llmclient
{
    Response PostJson(const std::string& hostPort,
        const std::string& path,
        const std::string& jsonBody,
        int timeoutSeconds)
    {
        return Send(hostPort, path, L"POST", &jsonBody, timeoutSeconds);
    }

    Response PostJsonStreaming(const std::string& hostPort,
        const std::string& path,
        const std::string& jsonBody,
        int timeoutSeconds,
        const StreamCallback& onChunk)
    {
        return Send(hostPort, path, L"POST", &jsonBody, timeoutSeconds, &onChunk);
    }

    Response Get(const std::string& hostPort, const std::string& path, int timeoutSeconds)
    {
        return Send(hostPort, path, L"GET", nullptr, timeoutSeconds);
    }

    bool ServerProcess::Running() const
    {
        if (!handle)
        {
            return false;
        }
        DWORD code = 0;
        return ::GetExitCodeProcess(static_cast<HANDLE>(handle), &code) && code == STILL_ACTIVE;
    }

    void ServerProcess::Terminate()
    {
        if (handle)
        {
            ::TerminateProcess(static_cast<HANDLE>(handle), 0);
            ::CloseHandle(static_cast<HANDLE>(handle));
            handle = nullptr;
        }
        // Closing the job is what makes this stick even if TerminateProcess was raced; it
        // is also the only thing that runs when the editor dies without warning.
        if (job)
        {
            ::CloseHandle(static_cast<HANDLE>(job));
            job = nullptr;
        }
        processId = 0;
    }

    unsigned long FindListenerPid(const std::string& hostPort)
    {
        std::wstring host;
        int port = 0;
        SplitHostPort(hostPort, host, port);
        if (port <= 0)
        {
            return 0;
        }
        // Network byte order: the table stores the port as it goes on the wire, and
        // comparing it against a host-order int is the classic way to match nothing on a
        // little-endian machine and then blame the API.
        const DWORD wanted = static_cast<DWORD>(::htons(static_cast<u_short>(port)));

        DWORD size = 0;
        if (::GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET,
                TCP_TABLE_OWNER_PID_LISTENER, 0) != ERROR_INSUFFICIENT_BUFFER)
        {
            return 0;
        }
        std::vector<char> buffer(size);
        if (::GetExtendedTcpTable(buffer.data(), &size, FALSE, AF_INET,
                TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR)
        {
            return 0;
        }
        const MIB_TCPTABLE_OWNER_PID* table =
            reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i)
        {
            const MIB_TCPROW_OWNER_PID& row = table->table[i];
            if (row.dwState == MIB_TCP_STATE_LISTEN && row.dwLocalPort == wanted)
            {
                return row.dwOwningPid;
            }
        }
        return 0;
    }

    bool StopListener(const std::string& hostPort, std::string& outError)
    {
        const unsigned long pid = FindListenerPid(hostPort);
        if (pid == 0)
        {
            outError = "nothing is listening on " + hostPort;
            return false;
        }
        HANDLE handle = ::OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!handle)
        {
            outError = "cannot stop pid " + std::to_string(pid) + " (error " +
                std::to_string(::GetLastError()) + ")";
            return false;
        }
        const BOOL ok = ::TerminateProcess(handle, 0);
        ::CloseHandle(handle);
        if (!ok)
        {
            outError = "could not terminate pid " + std::to_string(pid) + " (error " +
                std::to_string(::GetLastError()) + ")";
            return false;
        }
        LOG_INFO(logging::LogCategory::Editor,
            "intent model: stopped whatever was listening on {} (pid {})", hostPort, pid);
        return true;
    }

    bool StartReaper(const std::string& hostPort,
        unsigned long serverPid,
        int idleSeconds,
        std::string& outError)
    {
        // This executable's own path, used only to locate model_reaper.exe beside it -- see
        // the note below for why the watchdog is a binary of its own and not a mode of this
        // one, which is what it was first.
        wchar_t modulePath[MAX_PATH] = {};
        if (::GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
        {
            outError = "could not find this executable to start the model reaper";
            return false;
        }

        // model_reaper.exe, beside this one. It began as a MODE of the engine binary and
        // that was wrong twice: a live watchdog held test_cube.exe open, so the next build
        // of Release_Editor failed to link; and copying the exe to %TEMP% to dodge that lock
        // produced a process without the DLLs this binary links against, which died before
        // its first log line and left the server running with nobody watching. Its own
        // binary has neither problem -- five source files, no graphics, no DLLs.
        std::wstring reaperPath = modulePath;
        const std::size_t lastSlash = reaperPath.find_last_of(L"\\/");
        reaperPath = (lastSlash == std::wstring::npos ? std::wstring{}
                                                     : reaperPath.substr(0, lastSlash + 1)) +
            L"model_reaper.exe";
        if (::GetFileAttributesW(reaperPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            outError = "model_reaper.exe is missing beside the editor; build "
                "tools/model_reaper.vcxproj";
            return false;
        }

        std::wstring command = L"\"" + reaperPath + L"\"" +
            L" --reap-model=" + Widen(hostPort) +
            L" --reap-model-pid=" + std::to_wstring(serverPid) +
            L" --reap-idle=" + std::to_wstring(idleSeconds);
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        // DETACHED and in its own group: it has to outlive this process, which is the whole
        // point, so it must not be in our job and must not take our Ctrl+C.
        if (!::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB,
                nullptr, nullptr, &startup, &info))
        {
            outError = "could not start the model reaper (error " +
                std::to_string(::GetLastError()) + ")";
            return false;
        }
        LOG_INFO(logging::LogCategory::Editor,
            "intent model: reaper pid={} will retire the server after {}s idle",
            info.dwProcessId, idleSeconds);
        ::CloseHandle(info.hThread);
        ::CloseHandle(info.hProcess);
        return true;
    }

    ServerProcess StartServer(const std::string& serverExe,
        const std::string& modelPath,
        const std::string& hostPort,
        int gpuLayers,
        int contextTokens,
        int threads,
        bool webUi,
        bool keepAliveAfterExit,
        std::string& outError)
    {
        ServerProcess process;

        std::wstring host;
        int port = 8080;
        SplitHostPort(hostPort, host, port);

        // By default --no-webui keeps it a service rather than a website. The chat page is
        // worth having on demand though: this is a capable general model, and the command
        // bar deliberately only ever asks it one constrained question. Someone who wants to
        // actually talk to it should not have to relaunch the server by hand.
        //
        // The chat template is applied on OUR side either way: the prefix has to stay
        // byte-identical between requests for the prefill cache to hit (E4).
        std::string command = "\"" + serverExe + "\"" +
            " -m \"" + modelPath + "\"" +
            " --host " + hostPort.substr(0, hostPort.rfind(':')) +
            " --port " + std::to_string(port) +
            " -c " + std::to_string(contextTokens > 0 ? contextTokens : 8192) +
            " -ngl " + std::to_string(gpuLayers) +
            // A 35B mixture-of-experts does not fit on a 24 GB card, and it does not need
            // to: --cpu-moe leaves the expert weights (nearly all of the file) in RAM and
            // puts only the dense and attention tensors on the GPU. Measured on this
            // machine, atoll level, 2.5k-token system prompt: 6.0 s -> 3.5 s per phrase
            // once the prefill is cached, 21.4 s -> 17.1 s for the first one. Without it,
            // `-ngl 99` would simply fail to allocate.
            //
            // It only makes sense with layers on the GPU at all, so it rides the one knob
            // the user actually sets rather than becoming a second one to get wrong.
            (gpuLayers > 0 ? " --cpu-moe" : "") +
            // MEMORY-MAPPED, EXPLICITLY. A 38 GB model left running would be a third of a
            // workstation's RAM if it were a real allocation -- mmap makes it file-backed
            // page cache instead, which Windows reclaims the moment something else wants
            // the memory and re-reads on the next request. `auto` already picks this, but
            // spelling it out is what keeps a future default change from quietly turning a
            // background server into a resident one.
            //
            // --mlock is deliberately NOT here: that is the flag that pins all 38 GB for
            // real, and it is the opposite of what a server sitting idle beside an editor
            // should do.
            (threads > 0 ? " -t " + std::to_string(threads) : "") +
            " --load-mode mmap" +
            // The counters the reaper watches. Without them "idle" could only mean "the
            // editor made no request", which is blind to the server's own chat page -- and
            // that blindness is how a conversation used to get its model retired underneath
            // it. With them, activity means activity, whoever caused it.
            " --metrics" +
            (webUi ? "" : " --no-webui");

        std::wstring wideCommand = Widen(command);
        std::vector<wchar_t> mutableCommand(wideCommand.begin(), wideCommand.end());
        mutableCommand.push_back(L'\0');

        // The job object dies with this process, and JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        // means the server dies with it -- on a clean exit, on a crash, on a kill from Task
        // Manager, on a debugger stop. No shutdown path has to be trusted.
        HANDLE job = keepAliveAfterExit ? nullptr : ::CreateJobObjectW(nullptr, nullptr);
        if (job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                    &limits, sizeof(limits)))
            {
                ::CloseHandle(job);
                job = nullptr;
            }
        }

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};

        // CREATE_NO_WINDOW: the editor should not sprout a console. The server's own log
        // is its business; ours is the session log line below.
        const std::wstring workingDirectory = [&]() -> std::wstring
        {
            const std::size_t slash = serverExe.find_last_of("/\\");
            return slash == std::string::npos ? std::wstring{} : Widen(serverExe.substr(0, slash));
        }();

        // CREATE_SUSPENDED so the child cannot outlive the assignment: started running, it
        // could in principle spawn something of its own before it joins the job.
        if (!::CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
                &startup, &info))
        {
            outError = "could not start " + serverExe + " (error " +
                std::to_string(::GetLastError()) + ")";
            if (job)
            {
                ::CloseHandle(job);
            }
            return process;
        }

        if (!keepAliveAfterExit && !job)
        {
            // Asked for the guarantee and could not have it: better to fail loudly than to
            // leave a 38 GB process with no owner.
            ::TerminateProcess(info.hProcess, 0);
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
            outError = "could not create a job object for llama-server (error " +
                std::to_string(::GetLastError()) + "); refusing to start an unowned server";
            return process;
        }
        if (job && !::AssignProcessToJobObject(job, info.hProcess))
        {
            // Without the job there is no orphan guarantee, and that guarantee is the point.
            // Better to fail loudly here than to leave a 38 GB process with no owner.
            ::TerminateProcess(info.hProcess, 0);
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
            ::CloseHandle(job);
            outError = "could not put llama-server in a job object (error " +
                std::to_string(::GetLastError()) + "); refusing to start an unowned server";
            return process;
        }

        ::ResumeThread(info.hThread);
        ::CloseHandle(info.hThread);
        process.handle = info.hProcess;
        process.job = job;
        process.processId = info.dwProcessId;
        // WHICH LIFETIME IT ACTUALLY GOT. The line said "job-owned: dies with the editor"
        // unconditionally, including for a kept-alive server that has no job at all -- so
        // the one log anybody reads to find out whether the server will outlive the session
        // answered the opposite of the truth, right above the line announcing its watchdog.
        LOG_INFO(logging::LogCategory::Editor,
            "intent model: started llama-server pid={} on {} ({})",
            info.dwProcessId, hostPort,
            job ? "job-owned: dies with the editor"
                : "kept alive after the editor exits; a watchdog retires it");
        return process;
    }
}

#endif // WITH_EDITOR
