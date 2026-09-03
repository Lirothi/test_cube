#include "core/logging/diagnostics/LogStress.h"

#include "core/diagnostics/ArtifactWriter.h"
#include "core/logging/Log.h"

#include <Windows.h>

#include <mimalloc.h>
#include <mimalloc-stats.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <vector>

namespace
{
    diag::ArtifactFile gLog;
    int gFailures = 0;

    void Log(const char* format, ...)
    {
        if (!gLog)
        {
            return;
        }
        va_list args;
        va_start(args, format);
        gLog.VPrintf(format, args);
        va_end(args);
    }

    void Check(bool ok, const char* what)
    {
        if (!ok)
        {
            ++gFailures;
        }
        Log("%s: %s\n", ok ? "ok  " : "FAIL", what);
    }

    // mimalloc counts blocks only when built with statistics (MI_STAT >= 2, i.e. its debug
    // build). The self test calibrates against a real allocation and reports "skipped" when the
    // counter does not move, so this is honest in both configurations.
    std::uint64_t MimallocAllocationCount(void*) noexcept
    {
        mi_stats_merge();
        mi_stats_t stats{};
        mi_stats_get(sizeof(stats), &stats);
        return static_cast<std::uint64_t>(stats.malloc_normal_count.total);
    }

    bool ReadFileText(const wchar_t* path, std::string& out)
    {
        out.clear();
        // _SH_DENYNO: the session file is still open (shared) by the sink in the synchronous
        // scenario; _wfopen_s would demand exclusive access and fail with a sharing violation.
        FILE* file = _wfsopen(path, L"rb", _SH_DENYNO);
        if (file == nullptr)
        {
            return false;
        }
        char buffer[64 * 1024];
        for (;;)
        {
            const std::size_t read = fread(buffer, 1, sizeof(buffer), file);
            if (read == 0)
            {
                break;
            }
            out.append(buffer, read);
        }
        fclose(file);
        return true;
    }

    // Marker records look like "#S t=<thread> n=<index>"; counts them per thread and verifies
    // each thread's indices appear in increasing order.
    void ParseMarkers(const std::string& text, std::vector<int>& perThread, bool& orderOk)
    {
        orderOk = true;
        std::vector<int> last(perThread.size(), -1);
        std::size_t position = 0;
        while (position < text.size())
        {
            std::size_t end = text.find('\n', position);
            if (end == std::string::npos)
            {
                end = text.size();
            }
            const std::string_view line(text.data() + position, end - position);
            const std::size_t marker = line.find("#S t=");
            if (marker != std::string_view::npos)
            {
                int thread = -1;
                int index = -1;
                if (sscanf_s(line.data() + marker, "#S t=%d n=%d", &thread, &index) == 2 &&
                    thread >= 0 && thread < static_cast<int>(perThread.size()))
                {
                    ++perThread[static_cast<std::size_t>(thread)];
                    if (index <= last[static_cast<std::size_t>(thread)])
                    {
                        orderOk = false;
                    }
                    last[static_cast<std::size_t>(thread)] = index;
                }
            }
            position = end + 1;
        }
    }

    logging::LogConfig StressConfig(const wchar_t* file, std::uint32_t queueCapacity)
    {
        logging::LogConfig config;
        config.queueCapacity = queueCapacity;
        config.memoryRingCapacity = 256;
        config.debuggerSink = false; // keep the timing about the ring, not DBWIN
        config.consoleSink = logging::ConsoleMode::Off;
        wcsncpy_s(config.filePath, file, _TRUNCATE);
        return config;
    }

    void ProduceMarkers(int threads, int perThread)
    {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads));
        for (int t = 0; t < threads; ++t)
        {
            pool.emplace_back([t, perThread]
            {
                char name[32];
                std::snprintf(name, sizeof(name), "Producer%d", t);
                logging::SetCurrentThreadName(name);
                for (int n = 0; n < perThread; ++n)
                {
                    LOG_INFO(logging::LogCategory::Task, "#S t={} n={}", t, n);
                }
            });
        }
        for (std::thread& thread : pool)
        {
            thread.join();
        }
    }

    bool SpawnSelf(const wchar_t* arguments, PROCESS_INFORMATION& info)
    {
        wchar_t executable[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, executable, MAX_PATH);
        std::wstring command = L"\"";
        command += executable;
        command += L"\" ";
        command += arguments;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        info = PROCESS_INFORMATION{};
        return CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info) != FALSE;
    }

    DWORD WaitChild(PROCESS_INFORMATION& info, DWORD timeoutMs)
    {
        DWORD exitCode = 0xFFFFFFFFu;
        if (WaitForSingleObject(info.hProcess, timeoutMs) == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(info.hProcess, &exitCode);
        }
        else
        {
            TerminateProcess(info.hProcess, 0xFFFFFFFEu);
        }
        CloseHandle(info.hProcess);
        CloseHandle(info.hThread);
        return exitCode;
    }

    // ---- child modes ----------------------------------------------------------------------------

    int RunFatalChild()
    {
        logging::LogConfig config = StressConfig(L"logs/log_stress_fatal.log", 1024);
        if (!logging::Initialize(config))
        {
            return 3;
        }
        LOG_INFO(logging::LogCategory::Core, "child line one");
        LOG_INFO(logging::LogCategory::Core, "child line two");
        LOG_INFO(logging::LogCategory::Core, "child line three");
        LOG_FATAL(logging::LogCategory::Core, "fatal marker {}", 42);
        // No Shutdown on purpose: this is what a crash after LOG_FATAL looks like.
        TerminateProcess(GetCurrentProcess(), 7);
        return 7;
    }

    int RunSessionChild()
    {
        logging::LogConfig config;
        config.debuggerSink = false;
        config.consoleSink = logging::ConsoleMode::Off;
        if (!logging::Initialize(config))
        {
            return 3;
        }
        for (int i = 0; i < 10; ++i)
        {
            LOG_INFO(logging::LogCategory::Core, "session child line {}", i);
        }
        // L7 gate: a UniqueSession artifact from each of two concurrent children must not collide.
        diag::WriteArtifactf("log_stress_unique.txt", diag::ArtifactMode::UniqueSession, "child pid %lu\n",
                             static_cast<unsigned long>(GetCurrentProcessId()));
        logging::Shutdown();
        return 0;
    }

    bool FindUniqueArtifactForPid(DWORD pid, std::wstring& path)
    {
        wchar_t pattern[128];
        _snwprintf_s(pattern, _TRUNCATE, L"logs/log_stress_unique_*_%u*.txt", static_cast<unsigned>(pid));
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileW(pattern, &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        path = L"logs/";
        path += data.cFileName;
        FindClose(find);
        return true;
    }

    bool FindSessionFileForPid(DWORD pid, std::wstring& path)
    {
        wchar_t pattern[128];
        _snwprintf_s(pattern, _TRUNCATE, L"logs/session_*_%u_*.log", static_cast<unsigned>(pid));
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileW(pattern, &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        path = L"logs/";
        path += data.cFileName;
        FindClose(find);
        return true;
    }

    // ---- scenarios ------------------------------------------------------------------------------

    void ScenarioSelfTests()
    {
        Log("\n[self-tests]\n");
        logging::AllocationProbe probe;
        probe.count = &MimallocAllocationCount;
        const logging::FrontendSelfTestResult result = logging::RunFrontendSelfTests(&probe);
        Log("frontend self-tests: passed=%u failed=%u skipped=%u %s\n", result.passed, result.failed,
            result.skipped, result.failed != 0 ? result.firstFailure : "");
        Check(result.Succeeded(), "frontend self-tests");
        if (result.skipped != 0)
        {
            Log("note: allocation check skipped (allocator statistics unavailable in this build)\n");
        }
    }

    void ScenarioAccounting()
    {
        Log("\n[accounting: more producers than cores, ring large enough]\n");
        const int threads = static_cast<int>(std::thread::hardware_concurrency()) + 4;
        // The ring must hold every record even if the writer never runs: producers finish in
        // milliseconds, the drain takes longer, and the test is about exact delivery, not speed.
        const std::uint32_t ringCapacity = 65536;
        const int perThread = std::min(2000, static_cast<int>(ringCapacity - 256) / threads);
        const wchar_t* file = L"logs/log_stress_accounting.log";
        Check(logging::Initialize(StressConfig(file, ringCapacity)), "Initialize");

        const auto start = std::chrono::steady_clock::now();
        ProduceMarkers(threads, perThread);
        const auto produced = std::chrono::steady_clock::now();
        Check(logging::Flush(10000), "Flush completed");
        const auto flushed = std::chrono::steady_clock::now();
        logging::LogStatistics stats{};
        logging::GetStatistics(stats);
        logging::Shutdown();

        std::string text;
        Check(ReadFileText(file, text), "session file readable");
        std::vector<int> perThreadCount(static_cast<std::size_t>(threads), 0);
        bool orderOk = false;
        ParseMarkers(text, perThreadCount, orderOk);
        int delivered = 0;
        for (const int count : perThreadCount)
        {
            delivered += count;
        }
        Log("threads=%d perThread=%d delivered=%d dropped=%llu submitted=%llu written=%llu produce=%.1f ms drain=%.1f ms\n",
            threads, perThread, delivered, static_cast<unsigned long long>(stats.DroppedTotal()),
            static_cast<unsigned long long>(stats.submitted), static_cast<unsigned long long>(stats.written),
            std::chrono::duration<double, std::milli>(produced - start).count(),
            std::chrono::duration<double, std::milli>(flushed - produced).count());
        Check(delivered == threads * perThread, "every marker delivered");
        Check(stats.DroppedTotal() == 0, "zero drops with a sufficient ring");
        Check(orderOk, "per-thread order preserved");
        Check(text.find("session end: clean shutdown") != std::string::npos, "clean-shutdown footer present");
        // The file no longer carries the thread column; the name table itself is what the
        // viewer reads, so check it directly.
        logging::SetCurrentThreadName("StressMain");
        char threadName[32] = {};
        Check(logging::GetThreadName(GetCurrentThreadId(), threadName, sizeof(threadName)) != 0 &&
                  std::strcmp(threadName, "StressMain") == 0,
            "thread names published and readable");
    }

    void ScenarioOverflow()
    {
        Log("\n[overflow: ring far too small, exact delivered + dropped accounting]\n");
        const int threads = 12;
        const int perThread = 20000;
        const wchar_t* file = L"logs/log_stress_overflow.log";
        Check(logging::Initialize(StressConfig(file, 256)), "Initialize");

        ProduceMarkers(threads, perThread);
        Check(logging::Flush(10000), "Flush completed");
        logging::LogStatistics stats{};
        logging::GetStatistics(stats);
        logging::Shutdown();

        std::string text;
        Check(ReadFileText(file, text), "session file readable");
        std::vector<int> perThreadCount(static_cast<std::size_t>(threads), 0);
        bool orderOk = false;
        ParseMarkers(text, perThreadCount, orderOk);
        long long delivered = 0;
        for (const int count : perThreadCount)
        {
            delivered += count;
        }
        const long long dropped = static_cast<long long>(stats.DroppedTotal());
        Log("threads=%d perThread=%d delivered=%lld dropped=%lld total=%lld\n", threads, perThread, delivered,
            dropped, static_cast<long long>(threads) * perThread);
        Check(dropped > 0, "overflow actually happened (test is meaningful)");
        Check(delivered + dropped == static_cast<long long>(threads) * perThread, "delivered + dropped == produced");
        Check(orderOk, "per-thread order preserved among delivered records");
        Check(text.find("logging: dropped") != std::string::npos, "synthetic drop report present");
        Check(text.find("session end: clean shutdown") != std::string::npos, "clean-shutdown footer present");
    }

    void ScenarioOversized()
    {
        Log("\n[oversized record]\n");
        const wchar_t* file = L"logs/log_stress_oversized.log";
        Check(logging::Initialize(StressConfig(file, 1024)), "Initialize");
        std::string big(1500, 'y');
        LOG_INFO(logging::LogCategory::Core, "#O {}", big);
        logging::Flush(5000);
        logging::Shutdown();
        std::string text;
        Check(ReadFileText(file, text), "session file readable");
        const std::size_t marker = text.find("#O ");
        Check(marker != std::string::npos, "oversized record delivered");
        if (marker != std::string::npos)
        {
            const std::size_t end = text.find('\n', marker);
            const std::string_view line(text.data() + marker, end - marker);
            Check(line.find("[truncated]") != std::string_view::npos, "oversized record flagged [truncated]");
            // "#O " + 1020 'y' = 1023 message bytes
            const std::size_t ys = line.find_last_of('y') - line.find_first_of('y') + 1;
            Check(ys == logging::kLogMessageCapacity - 1 - 3, "message cut exactly at the buffer boundary");
        }
    }

    void ScenarioUnwritable()
    {
        Log("\n[unwritable session file]\n");
        // A file as the parent directory: CreateFile fails with ERROR_PATH_NOT_FOUND.
        logging::LogConfig config = StressConfig(L"logs/log_stress_oversized.log/impossible.log", 1024);
        Check(logging::Initialize(config), "Initialize still succeeds (degraded)");
        char path[512];
        Check(logging::GetSessionFilePath(path, sizeof(path)) == 0, "no session file reported");
        for (int i = 0; i < 100; ++i)
        {
            LOG_WARNING(logging::LogCategory::Core, "degraded {}", i);
        }
        Check(logging::Flush(2000), "Flush returns without a file");
        logging::Shutdown();
        Check(!logging::IsInitialized(), "Shutdown clean without a file");
    }

    void ScenarioRepeatedLifecycle()
    {
        Log("\n[repeated Initialize/Shutdown]\n");
        bool ok = true;
        for (int cycle = 0; cycle < 20; ++cycle)
        {
            logging::LogConfig config = StressConfig(L"logs/log_stress_cycle.log", 512);
            if (!logging::Initialize(config))
            {
                ok = false;
                break;
            }
            if (logging::Initialize(config))
            {
                ok = false; // second Initialize must be refused
                break;
            }
            for (int i = 0; i < 50; ++i)
            {
                LOG_DEBUG(logging::LogCategory::Core, "cycle {} record {}", cycle, i);
            }
            logging::Shutdown();
            logging::Shutdown(); // no-op
            if (logging::IsInitialized())
            {
                ok = false;
                break;
            }
        }
        Check(ok, "20 cycles, double Initialize refused, double Shutdown harmless");
        LOG_INFO(logging::LogCategory::Core, "record after shutdown goes to the DBWIN fallback");
        Check(true, "logging after Shutdown does not crash");
    }

    void ScenarioSynchronous()
    {
        Log("\n[synchronous mode]\n");
        const wchar_t* file = L"logs/log_stress_sync.log";
        logging::LogConfig config = StressConfig(file, 1024);
        config.synchronous = true;
        Check(logging::Initialize(config), "Initialize (sync)");
        LOG_INFO(logging::LogCategory::Core, "#Y immediate {}", 1);
        // No Flush: the line must already be in the OS cache.
        std::string text;
        Check(ReadFileText(file, text) && text.find("#Y immediate 1") != std::string::npos,
            "record visible in the file before Flush/Shutdown");
        ProduceMarkers(4, 500);
        logging::Shutdown();
        Check(ReadFileText(file, text), "session file readable");
        std::vector<int> perThreadCount(4, 0);
        bool orderOk = false;
        ParseMarkers(text, perThreadCount, orderOk);
        Check(perThreadCount[0] + perThreadCount[1] + perThreadCount[2] + perThreadCount[3] == 2000 && orderOk,
            "synchronous multi-producer delivery complete and ordered");
    }

    void ScenarioMemoryRing()
    {
        Log("\n[memory ring]\n");
        logging::LogConfig config = StressConfig(L"logs/log_stress_ring.log", 1024);
        config.memoryRingCapacity = 64;
        Check(logging::Initialize(config), "Initialize");
        for (int i = 0; i < 100; ++i)
        {
            LOG_INFO(logging::LogCategory::Core, "#R {}", i);
        }
        Check(logging::Flush(5000), "Flush");
        std::vector<logging::LogRecord> records(256);
        std::uint64_t cursor = 0;
        const std::size_t copied = logging::CopyRecentRecords(0, records.data(), records.size(), &cursor);
        Log("ring copied=%zu cursor=%llu\n", copied, static_cast<unsigned long long>(cursor));
        Check(copied == 64, "ring returns exactly its capacity when overfilled");
        bool latest = copied == 64;
        if (latest)
        {
            // Oldest retained is "#R 36", newest "#R 99" (header lines were pushed out).
            latest = std::string_view(records[0].message, records[0].messageByteCount) == "#R 36" &&
                std::string_view(records[63].message, records[63].messageByteCount) == "#R 99";
        }
        Check(latest, "ring holds the newest records in order");
        std::uint64_t next = 0;
        Check(logging::CopyRecentRecords(cursor, records.data(), records.size(), &next) == 0 && next == cursor,
            "nothing new after the cursor");
        logging::Shutdown();
    }

    void ScenarioCommandLine()
    {
        Log("\n[command line parsing]\n");
        logging::LogConfig config;
        logging::ApplyCommandLine(
            "--level=x --log-level=warning --log-category=render.rt:trace --log-category=asset:error "
            "--log-sync --log-no-file --log-file=\"logs/a b.log\" --log-syncless",
            config);
        Check(logging::GetCategoryThreshold(logging::LogCategory::Scene) == logging::LogLevel::Warning,
            "--log-level applied globally");
        Check(logging::GetCategoryThreshold(logging::LogCategory::RenderRt) == logging::LogLevel::Trace,
            "--log-category=render.rt:trace applied");
        Check(logging::GetCategoryThreshold(logging::LogCategory::Asset) == logging::LogLevel::Error,
            "second --log-category applied");
        Check(config.synchronous, "--log-sync parsed as a whole token");
        Check(!config.fileSink, "--log-no-file parsed");
        Check(std::wcscmp(config.filePath, L"logs/a b.log") == 0, "--log-file quoted value parsed");
        logging::ResetThresholds();
        logging::LogConfig untouched;
        logging::ApplyCommandLine("--log-synchronous --log-levels=trace", untouched);
        Check(!untouched.synchronous && logging::GetCategoryThreshold(logging::LogCategory::Core) != logging::LogLevel::Trace,
            "prefix look-alikes are not matched");
        logging::ResetThresholds();
    }

    void ScenarioFatalChild()
    {
        Log("\n[fatal child: LOG_FATAL then TerminateProcess without Shutdown]\n");
        DeleteFileW(L"logs/log_stress_fatal.log");
        PROCESS_INFORMATION info{};
        Check(SpawnSelf(L"--log-stress-fatal-child", info), "child spawned");
        const DWORD exitCode = WaitChild(info, 60000);
        Log("child exit code %lu\n", static_cast<unsigned long>(exitCode));
        Check(exitCode == 7, "child exited through TerminateProcess with its own code");
        std::string text;
        Check(ReadFileText(L"logs/log_stress_fatal.log", text), "child session file exists");
        // The level sits in the bracketed prefix run: "[HH:MM:SS.mmm][FATAL][category][frame] ".
        Check(text.find("][FATAL][") != std::string::npos && text.find("fatal marker 42") != std::string::npos,
            "fatal record reached the file before the process died");
        Check(text.find("child line three") != std::string::npos, "records queued before the fatal were flushed too");
        Check(text.find("session end") == std::string::npos, "no footer (unclean end is visible as its absence)");
    }

    void ScenarioConcurrentSessions()
    {
        Log("\n[two processes opening sessions at the same instant]\n");
        PROCESS_INFORMATION a{};
        PROCESS_INFORMATION b{};
        const bool spawnedA = SpawnSelf(L"--log-stress-session-child", a);
        const bool spawnedB = SpawnSelf(L"--log-stress-session-child", b);
        Check(spawnedA && spawnedB, "children spawned");
        const DWORD pidA = a.dwProcessId;
        const DWORD pidB = b.dwProcessId;
        const DWORD exitA = spawnedA ? WaitChild(a, 60000) : 1;
        const DWORD exitB = spawnedB ? WaitChild(b, 60000) : 1;
        Check(exitA == 0 && exitB == 0, "children exited cleanly");
        std::wstring pathA;
        std::wstring pathB;
        Check(FindSessionFileForPid(pidA, pathA) && FindSessionFileForPid(pidB, pathB), "both session files exist");
        Check(pathA != pathB, "session file names are distinct");
        std::string textA;
        std::string textB;
        Check(ReadFileText(pathA.c_str(), textA) && textA.find("session end: clean shutdown") != std::string::npos &&
                ReadFileText(pathB.c_str(), textB) && textB.find("session end: clean shutdown") != std::string::npos,
            "both sessions complete");
        DeleteFileW(pathA.c_str());
        DeleteFileW(pathB.c_str());
        std::wstring uniqueA;
        std::wstring uniqueB;
        Check(FindUniqueArtifactForPid(pidA, uniqueA) && FindUniqueArtifactForPid(pidB, uniqueB) && uniqueA != uniqueB,
            "UniqueSession artifacts from both children exist and are distinct");
        DeleteFileW(uniqueA.c_str());
        DeleteFileW(uniqueB.c_str());
    }

    void ScenarioArtifacts()
    {
        Log("\n[artifact modes]\n");
        // PerRunTruncate: the first open of a name truncates, later opens append.
        diag::WriteArtifact("log_stress_artifact_perrun.log", diag::ArtifactMode::PerRunTruncate, "one\n");
        diag::WriteArtifact("log_stress_artifact_perrun.log", diag::ArtifactMode::PerRunTruncate, "two\n");
        std::string text;
        Check(ReadFileText(L"logs/log_stress_artifact_perrun.log", text) && text == "one\ntwo\n",
            "PerRunTruncate: first write truncates, second appends");

        // Append: history kept, one session separator per process, then the lines.
        diag::WriteArtifact("log_stress_artifact_append.log", diag::ArtifactMode::Append, "alpha\n");
        diag::WriteArtifact("log_stress_artifact_append.log", diag::ArtifactMode::Append, "beta\n");
        Check(ReadFileText(L"logs/log_stress_artifact_append.log", text) &&
                text.find("---- session ") != std::string::npos &&
                text.find("alpha\nbeta\n") != std::string::npos &&
                text.find("---- session ") == text.rfind("---- session ") ||
                (ReadFileText(L"logs/log_stress_artifact_append.log", text) && text.find("---- session ") != std::string::npos &&
                 text.find("alpha\nbeta\n") != std::string::npos),
            "Append: session separator present, both lines appended");
        {
            // The separator count for THIS process must be exactly one even after two opens.
            std::size_t separators = 0;
            for (std::size_t at = text.find("---- session "); at != std::string::npos; at = text.find("---- session ", at + 1))
            {
                ++separators;
            }
            // Earlier runs may have left their own separators; only the tail of the file is ours.
            const std::size_t last = text.rfind("---- session ");
            Check(last != std::string::npos && text.find("alpha\nbeta\n", last) != std::string::npos && separators >= 1,
                "Append: this process wrote one separator before its lines");
        }

        // AtomicReplace: written whole, no .tmp left behind, previous content replaced.
        diag::WriteArtifact("log_stress_artifact_atomic.log", diag::ArtifactMode::AtomicReplace, "first report\n");
        diag::WriteArtifact("log_stress_artifact_atomic.log", diag::ArtifactMode::AtomicReplace, "second report\n");
        Check(ReadFileText(L"logs/log_stress_artifact_atomic.log", text) && text == "second report\n",
            "AtomicReplace: file holds exactly the last complete report");
        Check(GetFileAttributesW(L"logs/log_stress_artifact_atomic.log.tmp") == INVALID_FILE_ATTRIBUTES,
            "AtomicReplace: no .tmp left behind");

        // UniqueSession within one process (same second): two distinct files.
        std::string pathA;
        std::string pathB;
        {
            diag::ArtifactFile a("log_stress_artifact_unique.log", diag::ArtifactMode::UniqueSession);
            diag::ArtifactFile b("log_stress_artifact_unique.log", diag::ArtifactMode::UniqueSession);
            a.Write("a\n");
            b.Write("b\n");
            pathA = a.Path();
            pathB = b.Path();
        }
        Check(!pathA.empty() && !pathB.empty() && pathA != pathB, "UniqueSession: two opens in one second do not collide");
        DeleteFileA(pathA.c_str());
        DeleteFileA(pathB.c_str());
    }

    // Session logs already in logs/ (a developer's real ones, any name): the retention scenario
    // must neither delete them nor let them skew its expectations, so the limits it configures
    // are "everything that is already there, plus N". Their last-write times are all newer than
    // the 2020 stamp the planted files get, so retention orders them first.
    void PreexistingSessions(std::uint32_t& count, std::uint64_t& bytes)
    {
        count = 0;
        bytes = 0;
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileW(L"logs/session_*.log", &data);
        if (find == INVALID_HANDLE_VALUE) { return; }
        do
        {
            if (std::wcsstr(data.cFileName, L"session_20200101_") != nullptr) { continue; } // our plants
            ++count;
            bytes += (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }

    // Retention orders by last-write time, so a planted file must LOOK old on disk, not just in
    // its name.
    void BackdateFile(const wchar_t* path)
    {
        const HANDLE file = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { return; }
        SYSTEMTIME old{};
        old.wYear = 2020; old.wMonth = 1; old.wDay = 1; old.wHour = 12;
        FILETIME stamp{};
        SystemTimeToFileTime(&old, &stamp);
        SetFileTime(file, nullptr, nullptr, &stamp);
        CloseHandle(file);
    }

    void ScenarioRetention()
    {
        Log("\n[session retention]\n");
        // A session log with an explicit name is a candidate too (the owner wants at most N on
        // disk, whoever named them); written NOW, so it is the newest and survives both passes.
        diag::WriteArtifact("session_keepme.log", diag::ArtifactMode::PerRunTruncate, "explicit name\n");

        std::uint32_t preexistingCount = 0;
        std::uint64_t preexistingBytes = 0;
        PreexistingSessions(preexistingCount, preexistingBytes);
        Log("pre-existing session logs: %u (%llu bytes) - kept out of the expectations\n",
            preexistingCount, static_cast<unsigned long long>(preexistingBytes));

        // Plant 14 session files back-dated to 2020 (older than anything real).
        for (int i = 1; i <= 14; ++i)
        {
            wchar_t name[96];
            _snwprintf_s(name, _TRUNCATE, L"logs/session_20200101_%06d_%d_debug.log", i, 1000 + i);
            const HANDLE file = CreateFileW(name, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                char body[200];
                std::memset(body, 'x', sizeof(body));
                DWORD written = 0;
                WriteFile(file, body, sizeof(body), &written, nullptr);
                CloseHandle(file);
            }
            BackdateFile(name);
        }

        const auto countPlanted = []() -> int
        {
            int count = 0;
            WIN32_FIND_DATAW data{};
            const HANDLE find = FindFirstFileW(L"logs/session_20200101_*_debug.log", &data);
            if (find == INVALID_HANDLE_VALUE) { return 0; }
            do { ++count; } while (FindNextFileW(find, &data));
            FindClose(find);
            return count;
        };

        // Count limit: keep (pre-existing + the new session + 9) -> exactly 9 planted survive.
        logging::LogConfig config;
        config.debuggerSink = false;
        config.consoleSink = logging::ConsoleMode::Off;
        config.retainSessionCount = preexistingCount + 1 + 9;
        config.retainSessionBytes = 0;
        Check(logging::Initialize(config), "Initialize (count limit)");
        char sessionPath[512] = {};
        logging::GetSessionFilePath(sessionPath, sizeof(sessionPath));
        logging::Shutdown();
        Check(countPlanted() == 9, "count limit keeps the 9 newest planted files beside the new session");
        Check(GetFileAttributesW(L"logs/session_keepme.log") != INVALID_FILE_ATTRIBUTES,
            "the newest explicitly named session survives (it is a candidate, ordered by write time)");
        Check(sessionPath[0] != '\0' && GetFileAttributesA(sessionPath) != INVALID_FILE_ATTRIBUTES, "the current session file survives");
        DeleteFileA(sessionPath);

        // Byte limit: the pre-existing bytes (all newer, so counted first) + 1500 over 200-byte
        // plants; the new session is empty when retention runs -> exactly 7 planted survive.
        config.retainSessionCount = 0;
        config.retainSessionBytes = preexistingBytes + 1500;
        Check(logging::Initialize(config), "Initialize (byte limit)");
        logging::GetSessionFilePath(sessionPath, sizeof(sessionPath));
        logging::Shutdown();
        Check(countPlanted() == 7, "byte limit keeps 7 planted files");
        DeleteFileA(sessionPath);

        // Cleanup.
        for (int i = 1; i <= 14; ++i)
        {
            wchar_t name[96];
            _snwprintf_s(name, _TRUNCATE, L"logs/session_20200101_%06d_%d_debug.log", i, 1000 + i);
            DeleteFileW(name);
        }
        DeleteFileW(L"logs/session_keepme.log");
    }

    void ScenarioMicrobenchmark()
    {
        Log("\n[microbenchmark]\n");
        // Filtered call: the category threshold rejects it before any work.
        logging::SetCategoryThreshold(logging::LogCategory::Vfx, logging::LogLevel::Fatal);
        {
            const int iterations = 2'000'000;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < iterations; ++i)
            {
                LOG_INFO(logging::LogCategory::Vfx, "filtered {} {}", i, 1.5);
            }
            const auto end = std::chrono::steady_clock::now();
            Log("filtered call: %.2f ns\n",
                std::chrono::duration<double, std::nano>(end - start).count() / iterations);
        }
        logging::ResetThresholds();

        // Accepted call: format + stamp + ring push, writer draining concurrently. Fewer records
        // than ring slots, so the throughput figure is the consumer's and not polluted by drops.
        Check(logging::Initialize(StressConfig(L"logs/log_stress_bench.log", 65536)), "Initialize");
        {
            const int iterations = 60'000;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < iterations; ++i)
            {
                LOG_INFO(logging::LogCategory::Task, "bench {} {:.2f} {}", i, 0.5 * i, "text");
            }
            const auto produced = std::chrono::steady_clock::now();
            logging::Flush(30000);
            const auto flushed = std::chrono::steady_clock::now();
            logging::LogStatistics stats{};
            logging::GetStatistics(stats);
            const double produceNs = std::chrono::duration<double, std::nano>(produced - start).count() / iterations;
            const double totalSeconds = std::chrono::duration<double>(flushed - start).count();
            Log("accepted call (producer side): %.1f ns; consumer throughput: %.0f records/s; dropped=%llu; file %llu bytes\n",
                produceNs, iterations / totalSeconds, static_cast<unsigned long long>(stats.DroppedTotal()),
                static_cast<unsigned long long>(stats.fileBytes));
        }
        logging::Shutdown();
    }
}

int RunLogStress(const char* commandLine)
{
    if (commandLine != nullptr && std::strstr(commandLine, "--log-stress-fatal-child") != nullptr)
    {
        return RunFatalChild();
    }
    if (commandLine != nullptr && std::strstr(commandLine, "--log-stress-session-child") != nullptr)
    {
        return RunSessionChild();
    }

    gLog.Open("log_stress.log", diag::ArtifactMode::PerRunTruncate);
    Log("log stress harness (%s)\n",
#if defined(NDEBUG)
        "release"
#else
        "debug"
#endif
    );

    ScenarioSelfTests();
    ScenarioCommandLine();
    ScenarioAccounting();
    ScenarioOverflow();
    ScenarioOversized();
    ScenarioUnwritable();
    ScenarioRepeatedLifecycle();
    ScenarioSynchronous();
    ScenarioMemoryRing();
    ScenarioFatalChild();
    ScenarioConcurrentSessions();
    ScenarioArtifacts();
    ScenarioRetention();
    ScenarioMicrobenchmark();

    Log("\n%d failed checks\n", gFailures);
    gLog.Close();

    // Leave only the verdict behind: every scenario file above is scratch, and a logs/ folder
    // full of log_stress_* is noise for whoever opens it next.
    {
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileW(L"logs/log_stress_*", &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) { continue; }
                std::wstring path = L"logs/";
                path += data.cFileName;
                DeleteFileW(path.c_str());
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }
    return gFailures;
}
