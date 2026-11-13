#include "editor/dist/DistBuildJob.h"

#if WITH_EDITOR

#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "third_party/json/json.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace dist
{
namespace
{
    std::wstring Quote(const std::wstring& arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) { return arg; }
        std::wstring out = L"\"";
        for (const wchar_t c : arg)
        {
            if (c == L'"') { out += L'\\'; }
            out += c;
        }
        return out + L"\"";
    }

    std::wstring FindPython()
    {
        for (const wchar_t* name : { L"python.exe", L"py.exe" })
        {
            wchar_t found[MAX_PATH] = {};
            if (SearchPathW(nullptr, name, nullptr, MAX_PATH, found, nullptr) > 0) { return found; }
        }
        return {};
    }
}

std::wstring BuildCommandLine(const std::wstring& python, const BuildRequest& request)
{
    std::wstring cmd = Quote(python) + L" -u tools\\build_dist.py --progress=json";
    cmd += L" " + Quote(L"--profile=" + vfs::Widen(request.profile.empty() ? std::string("test") : request.profile));
    cmd += L" --mode=" + vfs::Widen(request.mode);
    if (!request.levels.empty())
    {
        std::string levels;
        for (const std::string& level : request.levels) { levels += (levels.empty() ? "" : ",") + level; }
        cmd += L" " + Quote(L"--levels=" + vfs::Widen(levels));
    }
    if (!request.bootLevel.empty()) { cmd += L" " + Quote(L"--boot-level=" + vfs::Widen(request.bootLevel)); }
    if (!request.version.empty()) { cmd += L" " + Quote(L"--version=" + vfs::Widen(request.version)); }
    if (request.noEncrypt) { cmd += L" --no-encrypt"; }
    if (request.skipBuild) { cmd += L" --skip-build"; }
    return cmd;
}

struct BuildJob::State
{
    mutable std::mutex mutex;
    BuildStatus status;
    int nextId = 1;
    HANDLE job = nullptr;
    HANDLE process = nullptr;
    HANDLE readPipe = nullptr;
    std::thread reader;
    std::chrono::steady_clock::time_point started;

    void Close()
    {
        if (reader.joinable()) { reader.join(); }
        if (readPipe) { CloseHandle(readPipe); readPipe = nullptr; }
        if (process) { CloseHandle(process); process = nullptr; }
        if (job) { CloseHandle(job); job = nullptr; } // KILL_ON_JOB_CLOSE: nothing of it outlives the handle
    }

    void OnLine(const std::string& line)
    {
        BuildEvent event;
        const nlohmann::json json = nlohmann::json::parse(line, nullptr, false);
        if (json.is_object())
        {
            event.stage = json.value("stage", std::string());
            event.message = json.value("message", std::string());
            event.level = json.value("level", std::string("info"));
            event.event = json.value("event", std::string());
            event.fraction = json.value("fraction", 0.0);
            event.seconds = json.value("seconds", 0.0);
        }
        else
        {
            event.message = line;
            event.level = line.find("Error") != std::string::npos || line.find("Traceback") != std::string::npos ? "error" : "info";
            event.event = "raw";
        }
        std::lock_guard<std::mutex> lock(mutex);
        BuildStatus& s = status;
        if (!event.stage.empty()) { s.stage = event.stage; }
        s.fraction = std::max(s.fraction, event.fraction);
        s.seconds = event.seconds;
        if (event.event == "stage")
        {
            if (!s.stages.empty() && s.stages.back().seconds < 0.0) { s.stages.back().seconds = 0.0; }
            s.stages.push_back({ event.stage, -1.0 });
        }
        else if (event.event == "stage_end" && !s.stages.empty() && s.stages.back().seconds < 0.0)
        {
            // "done in 2.4 s"
            const std::size_t at = event.message.find("in ");
            s.stages.back().seconds = at == std::string::npos ? 0.0 : std::atof(event.message.c_str() + at + 3);
        }
        if (event.level == "error") { s.errors.push_back(event.message); }
        if (event.level == "warning") { ++s.warningCount; }
        if (event.event == "done" && json.is_object())
        {
            const auto text = [&](const char* key)
            {
                const auto it = json.find(key);
                return it != json.end() && it->is_string() ? it->get<std::string>() : std::string();
            };
            s.folder = text("folder");
            s.zip = text("zip");
            s.report = text("report");
            s.version = text("version");
            if (json.contains("bytes") && json["bytes"].is_number()) { s.bytes = json["bytes"].get<std::uint64_t>(); }
            s.fraction = 1.0;
        }
        if (s.events.size() < 4000) { s.events.push_back(std::move(event)); }
    }

    void Read()
    {
        std::string pending;
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(readPipe, buffer, sizeof(buffer), &got, nullptr) && got > 0)
        {
            pending.append(buffer, got);
            for (std::size_t nl = pending.find('\n'); nl != std::string::npos; nl = pending.find('\n'))
            {
                std::string line = pending.substr(0, nl);
                pending.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) { line.pop_back(); }
                if (!line.empty()) { OnLine(line); }
            }
        }
        if (!pending.empty()) { OnLine(pending); }
        WaitForSingleObject(process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        std::lock_guard<std::mutex> lock(mutex);
        status.exitCode = static_cast<int>(code);
        status.running = false;
        status.finished = true;
        if (!status.stages.empty() && status.stages.back().seconds < 0.0) { status.stages.back().seconds = 0.0; }
        LOG_INFO(logging::LogCategory::Editor, "build distribution #{}: {} {} -- exit {}{}", status.id, status.request.mode,
                 status.request.profile, status.exitCode, status.cancelled ? " (cancelled)" : "");
    }
};

BuildJob::BuildJob() : state_(std::make_unique<State>()) {}

BuildJob::~BuildJob()
{
    if (state_->job) { TerminateJobObject(state_->job, 2); }
    state_->Close();
}

bool BuildJob::Start(const BuildRequest& request, std::string& why)
{
    State& s = *state_;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.status.running)
        {
            why = "a build is running (#" + std::to_string(s.status.id) + ")";
            return false;
        }
    }
    s.Close();
    const std::wstring python = FindPython();
    if (python.empty())
    {
        why = "python.exe is not on the PATH (the build runs tools/build_dist.py)";
        return false;
    }
    SECURITY_ATTRIBUTES inherit{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &inherit, 0))
    {
        why = "cannot create a pipe";
        return false;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) { SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)); }

    std::wstring cmd = BuildCommandLine(python, request);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = writePipe;
    startup.hStdError = writePipe;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION info{};
    const std::wstring cwd = vfs::ContentRoot().wstring();
    // PYTHONIOENCODING: the events carry Cyrillic paths; a console code page would turn them into '?'.
    std::wstring env;
    {
        wchar_t* block = GetEnvironmentStringsW();
        for (const wchar_t* p = block; *p; p += wcslen(p) + 1)
        {
            if (_wcsnicmp(p, L"PYTHONIOENCODING=", 17) != 0) { env.append(p).push_back(L'\0'); }
        }
        FreeEnvironmentStringsW(block);
        env.append(L"PYTHONIOENCODING=utf-8").push_back(L'\0');
        env.push_back(L'\0');
    }
    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, env.data(), cwd.c_str(),
                                        &startup, &info);
    CloseHandle(writePipe);
    if (!created)
    {
        CloseHandle(readPipe);
        if (job) { CloseHandle(job); }
        why = "cannot start " + vfs::Narrow(cmd);
        return false;
    }
    if (job) { AssignProcessToJobObject(job, info.hProcess); }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.status = BuildStatus{};
        s.status.id = s.nextId++;
        s.status.running = true;
        s.status.request = request;
        s.status.stage = "prepare";
    }
    s.job = job;
    s.process = info.hProcess;
    s.readPipe = readPipe;
    s.started = std::chrono::steady_clock::now();
    s.reader = std::thread([&s] { s.Read(); });
    LOG_INFO(logging::LogCategory::Editor, "build distribution #{}: {}", s.status.id, vfs::Narrow(cmd));
    return true;
}

void BuildJob::Cancel()
{
    State& s = *state_;
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.status.running || !s.job) { return; }
    s.status.cancelled = true;
    TerminateJobObject(s.job, 2);
}

bool BuildJob::Running() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status.running;
}

BuildStatus BuildJob::Status() const
{
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->status;
}

std::string BuildJob::StatusJson(int id) const
{
    const BuildStatus s = Status();
    nlohmann::json out;
    if (s.id == 0 || (id != 0 && id != s.id))
    {
        out["error"] = s.id == 0 ? "no build has run in this editor" : "no build #" + std::to_string(id) + " (the last is #" +
                                                                           std::to_string(s.id) + ")";
        return out.dump();
    }
    out["id"] = s.id;
    out["running"] = s.running;
    out["finished"] = s.finished;
    out["cancelled"] = s.cancelled;
    out["exitCode"] = s.exitCode;
    out["mode"] = s.request.mode;
    out["profile"] = s.request.profile;
    out["stage"] = s.stage;
    out["fraction"] = s.fraction;
    out["seconds"] = s.seconds;
    out["errors"] = s.errors;
    out["warnings"] = s.warningCount;
    nlohmann::json stages = nlohmann::json::array();
    for (const StageTime& t : s.stages) { stages.push_back({ { "stage", t.stage }, { "seconds", t.seconds } }); }
    out["stages"] = stages;
    nlohmann::json tail = nlohmann::json::array();
    const std::size_t from = s.events.size() > 12 ? s.events.size() - 12 : 0;
    for (std::size_t i = from; i < s.events.size(); ++i)
    {
        tail.push_back({ { "stage", s.events[i].stage }, { "level", s.events[i].level }, { "message", s.events[i].message } });
    }
    out["lastEvents"] = tail;
    if (s.finished && s.exitCode == 0)
    {
        out["result"] = { { "folder", s.folder }, { "zip", s.zip }, { "report", s.report }, { "version", s.version },
                          { "bytes", s.bytes } };
    }
    return out.dump();
}

BuildJob& TheBuildJob()
{
    static BuildJob job;
    return job;
}
}

#endif // WITH_EDITOR
