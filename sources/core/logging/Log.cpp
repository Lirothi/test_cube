#include "Log.h"

#include "LogQueue.h"
#include "LogSinks.h"

#include <Windows.h>
#include <process.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <string_view>

namespace logging
{
    namespace
    {
#if defined(NDEBUG)
        constexpr LogLevel kDefaultThreshold = LogLevel::Info;
#else
        constexpr LogLevel kDefaultThreshold = LogLevel::Debug;
#endif

#if !defined(NDEBUG)
        constexpr const char* kBuildTag = "debug";
#elif defined(WITH_EDITOR) && WITH_EDITOR
        constexpr const char* kBuildTag = "release_editor";
#else
        constexpr const char* kBuildTag = "release";
#endif

        constexpr std::uint8_t kDefaultThresholdValue =
            static_cast<std::uint8_t>(kDefaultThreshold);

        // Everything at namespace scope is constant-initialised: a record submitted from another
        // TU's static initializer must find a working (fallback) logger, never a half-built one.
        std::array<std::atomic<std::uint8_t>, kLogCategoryCount> g_thresholds =
        {
            kDefaultThresholdValue, kDefaultThresholdValue, kDefaultThresholdValue,
            kDefaultThresholdValue, kDefaultThresholdValue, kDefaultThresholdValue,
            kDefaultThresholdValue, kDefaultThresholdValue, kDefaultThresholdValue,
            kDefaultThresholdValue, kDefaultThresholdValue, kDefaultThresholdValue,
            kDefaultThresholdValue, kDefaultThresholdValue, kDefaultThresholdValue
        };

        std::atomic<std::uint64_t> g_sequence{ 0 };
        std::atomic<std::uint64_t> g_frame{ kInvalidLogFrame };
        std::atomic<std::uint64_t> g_submitted{ 0 };
        std::array<std::atomic<std::uint64_t>, kLogLevelCount> g_dropped = {};
        sinks::ThreadNameTable g_threadNames;

        [[nodiscard]] std::int64_t QpcFrequency() noexcept
        {
            // Function-local so the first caller initialises it, whichever TU that is; a global
            // would be zero (divide-by-zero in the throttle) for a caller in static init.
            static const std::int64_t frequency = []
            {
                LARGE_INTEGER value{};
                return QueryPerformanceFrequency(&value) != FALSE && value.QuadPart > 0 ? value.QuadPart : 1;
            }();
            return frequency;
        }

        [[nodiscard]] std::int64_t NowQpc() noexcept
        {
            LARGE_INTEGER counter{};
            QueryPerformanceCounter(&counter);
            return counter.QuadPart;
        }

        void AppendAscii(char* destination, std::size_t capacity, std::size_t& size, std::string_view text) noexcept
        {
            if (capacity == 0 || size >= capacity - 1)
            {
                return;
            }
            const std::size_t available = capacity - 1 - size;
            const std::size_t count = text.size() < available ? text.size() : available;
            std::memcpy(destination + size, text.data(), count);
            size += count;
            destination[size] = '\0';
        }

        // Pre-Initialize / post-Shutdown fallback: "[level] [category] message" straight to DBWIN.
        void EmitToDebugger(const LogRecord& record) noexcept
        {
            char line[kLogMessageCapacity + 96];
            std::size_t size = 0;
            line[0] = '\0';
            AppendAscii(line, sizeof(line), size, "[");
            AppendAscii(line, sizeof(line), size, LogLevelName(record.level));
            AppendAscii(line, sizeof(line), size, "] [");
            AppendAscii(line, sizeof(line), size, LogCategoryName(record.category));
            AppendAscii(line, sizeof(line), size, "] ");
            AppendAscii(
                line, sizeof(line), size,
                std::string_view(record.message, record.messageByteCount));
            if (size == 0 || line[size - 1] != '\n')
            {
                AppendAscii(line, sizeof(line), size, "\n");
            }
            sinks::WriteDebugger(std::string_view(line, size));
        }

        void CopyFailure(char* destination, std::size_t capacity, std::string_view message) noexcept
        {
            if (capacity == 0)
            {
                return;
            }
            const std::size_t count = message.size() < capacity - 1 ? message.size() : capacity - 1;
            std::memcpy(destination, message.data(), count);
            destination[count] = '\0';
        }

        void StampRecord(LogRecord& record) noexcept
        {
            record.qpcTimestamp = NowQpc();
            record.sequence = g_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
            record.processId = GetCurrentProcessId();
            record.threadId = GetCurrentThreadId();
            record.frame = g_frame.load(std::memory_order_relaxed);
        }

        // ---- Session state --------------------------------------------------------------------

        struct LoggerState
        {
            LogConfig config;
            LogQueue queue;
            sinks::SessionClock clock;
            sinks::FileSink file;
            sinks::ConsoleSink console;
            sinks::MemoryRing memory;
            bool consoleEnabled = false;
            bool fileRequested = false;
            wchar_t sessionPath[512] = {};

            HANDLE writerThread = nullptr;
            std::uint32_t writerThreadId = 0;
            HANDLE wakeEvent = nullptr;
            HANDLE flushEvent = nullptr;
            std::atomic<bool> stopRequested{ false };
            std::atomic<bool> writerIdle{ false };
            std::atomic<std::uint64_t> flushRequested{ 0 };
            std::atomic<std::uint64_t> flushCompleted{ 0 };
            std::atomic<std::uint64_t> written{ 0 };
            SRWLOCK syncLock = SRWLOCK_INIT;

            // Writer-thread (or synchronous-caller) scratch.
            std::uint64_t droppedReported[kLogLevelCount] = {};
            char line[sinks::kLineCapacity] = {};
            LogRecord scratch;
        };

        std::atomic<LoggerState*> g_state{ nullptr };
        // Producers register here around every touch of g_state's target so Shutdown can wait
        // for them before freeing it. Two uncontended seq_cst RMWs per accepted record; the
        // alternative (never freeing) makes repeated Initialize/Shutdown leak the ring.
        alignas(64) std::atomic<std::int32_t> g_activeProducers{ 0 };

        struct StateGuard
        {
            LoggerState* state = nullptr;

            StateGuard() noexcept
            {
                if (g_state.load(std::memory_order_seq_cst) == nullptr)
                {
                    return;
                }
                g_activeProducers.fetch_add(1, std::memory_order_seq_cst);
                state = g_state.load(std::memory_order_seq_cst);
                if (state == nullptr)
                {
                    g_activeProducers.fetch_sub(1, std::memory_order_seq_cst);
                }
            }

            ~StateGuard()
            {
                if (state != nullptr)
                {
                    g_activeProducers.fetch_sub(1, std::memory_order_seq_cst);
                }
            }

            StateGuard(const StateGuard&) = delete;
            StateGuard& operator=(const StateGuard&) = delete;
        };

        // ---- Writer side ----------------------------------------------------------------------

        void ProcessRecord(LoggerState& state, const LogRecord& record) noexcept
        {
            const std::size_t length = sinks::FormatLine(
                record, state.clock, g_threadNames, state.line, sizeof(state.line));
            const std::string_view line(state.line, length);

            if (state.file.IsOpen())
            {
                state.file.Append(line);
                if (record.level >= LogLevel::Error)
                {
                    state.file.Commit();
                }
            }
            if (state.config.debuggerSink && record.level >= state.config.debuggerMinimum)
            {
                sinks::WriteDebugger(line);
            }
            if (state.consoleEnabled)
            {
                state.console.Write(line);
            }
            state.memory.Push(record);
            state.written.fetch_add(1, std::memory_order_relaxed);
        }

        template <typename... Args>
        void ProcessSynthetic(
            LoggerState& state, LogLevel level, std::string_view format, Args&&... args) noexcept
        {
            LogRecord record;
            record.level = level;
            record.category = LogCategory::Core;
            const std::source_location location = std::source_location::current();
            record.sourceFile = location.file_name();
            record.sourceFunction = location.function_name();
            record.sourceLine = location.line();
            detail::FormatRecord(record, format, std::forward<Args>(args)...);
            StampRecord(record);
            ProcessRecord(state, record);
        }

        void ReportDrops(LoggerState& state) noexcept
        {
            std::uint64_t now[kLogLevelCount];
            std::uint64_t total = 0;
            for (std::size_t i = 0; i < kLogLevelCount; ++i)
            {
                now[i] = g_dropped[i].load(std::memory_order_relaxed);
                total += now[i] - state.droppedReported[i];
            }
            if (total == 0)
            {
                return;
            }
            ProcessSynthetic(
                state, LogLevel::Warning,
                "logging: dropped {} records since the last report (trace={} debug={} info={} warning={} error={} fatal={}); ring capacity {}",
                total,
                now[0] - state.droppedReported[0], now[1] - state.droppedReported[1],
                now[2] - state.droppedReported[2], now[3] - state.droppedReported[3],
                now[4] - state.droppedReported[4], now[5] - state.droppedReported[5],
                state.queue.Capacity());
            for (std::size_t i = 0; i < kLogLevelCount; ++i)
            {
                state.droppedReported[i] = now[i];
            }
        }

        std::size_t DrainQueue(LoggerState& state) noexcept
        {
            std::size_t processed = 0;
            while (state.queue.TryPop(state.scratch))
            {
                ProcessRecord(state, state.scratch);
                ++processed;
                if ((processed & 255) == 0)
                {
                    state.file.Commit();
                }
            }
            ReportDrops(state);
            state.file.Commit();
            return processed;
        }

        void ServiceFlushRequests(LoggerState& state) noexcept
        {
            const std::uint64_t requested = state.flushRequested.load(std::memory_order_acquire);
            if (requested != state.flushCompleted.load(std::memory_order_relaxed))
            {
                state.file.Sync();
                state.flushCompleted.store(requested, std::memory_order_release);
                SetEvent(state.flushEvent);
            }
        }

        unsigned __stdcall WriterMain(void* argument)
        {
            LoggerState& state = *static_cast<LoggerState*>(argument);
            SetCurrentThreadName("LogWriter");

            for (;;)
            {
                DrainQueue(state);
                ServiceFlushRequests(state);
                if (state.stopRequested.load(std::memory_order_acquire))
                {
                    break;
                }

                // Idle handshake (see LogQueue::TryPush): publish "idle", then re-check. A
                // producer that pushed before seeing the flag is caught by the re-check; one that
                // pushes after it will see the flag and signal the event.
                state.writerIdle.store(true, std::memory_order_seq_cst);
                const bool pending = state.queue.ApproximateSize() != 0 ||
                    state.flushRequested.load(std::memory_order_seq_cst) !=
                        state.flushCompleted.load(std::memory_order_relaxed) ||
                    state.stopRequested.load(std::memory_order_seq_cst);
                if (!pending)
                {
                    WaitForSingleObject(state.wakeEvent, state.config.flushIntervalMs);
                }
                state.writerIdle.store(false, std::memory_order_seq_cst);
            }

            // Final drain. Shutdown has already waited for in-flight producers, so this sees
            // every record that was accepted.
            DrainQueue(state);
            ServiceFlushRequests(state);
            return 0;
        }

        // ---- Producer helpers -----------------------------------------------------------------

        bool FlushState(LoggerState& state, std::uint32_t timeoutMs) noexcept
        {
            if (state.config.synchronous)
            {
                // Try-lock: a crash handler may call Flush on the very thread that died inside
                // the synchronous render path while holding this lock (SRW locks are not
                // recursive). Better to skip the sync than to hang the handler.
                if (!TryAcquireSRWLockExclusive(&state.syncLock))
                {
                    return false;
                }
                state.file.Sync();
                ReleaseSRWLockExclusive(&state.syncLock);
                return true;
            }
            if (GetCurrentThreadId() == state.writerThreadId)
            {
                DrainQueue(state);
                state.file.Sync();
                return true;
            }

            const std::uint64_t mine = state.flushRequested.fetch_add(1, std::memory_order_seq_cst) + 1;
            SetEvent(state.wakeEvent);
            const std::uint64_t deadline = GetTickCount64() + timeoutMs;
            while (state.flushCompleted.load(std::memory_order_acquire) < mine)
            {
                const std::uint64_t now = GetTickCount64();
                if (now >= deadline)
                {
                    return false;
                }
                const std::uint64_t remaining = deadline - now;
                WaitForSingleObject(state.flushEvent, remaining < 10 ? static_cast<DWORD>(remaining) : 10);
            }
            return true;
        }

        // Unbuffered append of one already-formatted line via the emergency handle.
        void EmergencyAppend(LoggerState& state, std::string_view line) noexcept
        {
            const HANDLE handle = state.file.EmergencyHandle();
            if (handle != INVALID_HANDLE_VALUE && handle != nullptr)
            {
                DWORD written = 0;
                WriteFile(handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            }
        }

        void EmergencyRecord(LoggerState* state, LogRecord& record) noexcept
        {
            record.flags = static_cast<std::uint8_t>(record.flags | LogRecordFlagEmergency);
            if (state == nullptr)
            {
                EmitToDebugger(record);
                return;
            }
            char line[sinks::kLineCapacity];
            const std::size_t length = sinks::FormatLine(record, state->clock, g_threadNames, line, sizeof(line));
            sinks::WriteDebugger(std::string_view(line, length));
            EmergencyAppend(*state, std::string_view(line, length));
        }

        void SubmitToState(LoggerState& state, LogRecord& record) noexcept
        {
            g_submitted.fetch_add(1, std::memory_order_relaxed);

            if (state.config.synchronous)
            {
                AcquireSRWLockExclusive(&state.syncLock);
                ProcessRecord(state, record);
                state.file.Commit();
                ReleaseSRWLockExclusive(&state.syncLock);
                return;
            }

            if (record.level == LogLevel::Fatal)
            {
                // Ordered through the queue so it lands after everything before it, then flushed
                // so the caller may abort. If either step fails the line is appended directly.
                if (state.queue.TryPush(record) && FlushState(state, 2000))
                {
                    return;
                }
                EmergencyRecord(&state, record);
                return;
            }

            if (!state.queue.TryPush(record))
            {
                g_dropped[static_cast<std::size_t>(record.level)].fetch_add(1, std::memory_order_relaxed);
                // Narrowed from the plan's Warning+: a warning storm on a full ring must not
                // serialise render workers on the global OutputDebugString mutex.
                if (record.level >= LogLevel::Error)
                {
                    EmitToDebugger(record);
                }
                return;
            }
            if (state.writerIdle.load(std::memory_order_seq_cst))
            {
                SetEvent(state.wakeEvent);
            }
        }

        // ---- Command line -----------------------------------------------------------------------

        [[nodiscard]] bool IsTokenEnd(char c) noexcept
        {
            return c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        // Finds "--name" as a whole token (followed by '=', whitespace or end). Returns the pointer
        // past the name, or nullptr.
        [[nodiscard]] const char* FindFlag(const char* commandLine, const char* start, std::string_view name) noexcept
        {
            for (const char* cursor = start; *cursor != '\0'; ++cursor)
            {
                if (cursor != commandLine && !IsTokenEnd(cursor[-1]))
                {
                    continue;
                }
                if (std::strncmp(cursor, name.data(), name.size()) != 0)
                {
                    continue;
                }
                const char next = cursor[name.size()];
                if (next == '=' || IsTokenEnd(next))
                {
                    return cursor + name.size();
                }
            }
            return nullptr;
        }

        // Reads the value after '=' (quoted or bare) into out; returns the end of the token.
        const char* ReadValue(const char* cursor, char* out, std::size_t capacity) noexcept
        {
            std::size_t size = 0;
            if (capacity != 0)
            {
                out[0] = '\0';
            }
            if (*cursor != '=')
            {
                return cursor;
            }
            ++cursor;
            const bool quoted = *cursor == '"';
            if (quoted)
            {
                ++cursor;
            }
            while (*cursor != '\0' && (quoted ? *cursor != '"' : !IsTokenEnd(*cursor)))
            {
                if (size + 1 < capacity)
                {
                    out[size++] = *cursor;
                    out[size] = '\0';
                }
                ++cursor;
            }
            if (quoted && *cursor == '"')
            {
                ++cursor;
            }
            return cursor;
        }

        [[nodiscard]] bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < a.size(); ++i)
            {
                char x = a[i];
                char y = b[i];
                if (x >= 'A' && x <= 'Z') { x = static_cast<char>(x - 'A' + 'a'); }
                if (y >= 'A' && y <= 'Z') { y = static_cast<char>(y - 'A' + 'a'); }
                if (x != y)
                {
                    return false;
                }
            }
            return true;
        }

        std::size_t WideToUtf8(const wchar_t* text, char* out, std::size_t capacity) noexcept
        {
            if (capacity == 0)
            {
                return 0;
            }
            out[0] = '\0';
            if (text == nullptr || text[0] == L'\0')
            {
                return 0;
            }
            const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(capacity), nullptr, nullptr);
            if (length <= 0)
            {
                out[0] = '\0';
                return 0;
            }
            return static_cast<std::size_t>(length - 1);
        }
    }

    // ---- Filtering ----------------------------------------------------------------------------

    bool ShouldLog(LogLevel level, LogCategory category) noexcept
    {
        if (!IsValid(level) || !IsValid(category))
        {
            return false;
        }
        const std::uint8_t minimum =
            g_thresholds[static_cast<std::size_t>(category)].load(std::memory_order_relaxed);
        return static_cast<std::uint8_t>(level) >= minimum;
    }

    void SetCategoryThreshold(LogCategory category, LogLevel minimumLevel) noexcept
    {
        if (!IsValid(category) || !IsValid(minimumLevel))
        {
            return;
        }
        g_thresholds[static_cast<std::size_t>(category)].store(
            static_cast<std::uint8_t>(minimumLevel), std::memory_order_relaxed);
    }

    LogLevel GetCategoryThreshold(LogCategory category) noexcept
    {
        if (!IsValid(category))
        {
            return kDefaultThreshold;
        }
        return static_cast<LogLevel>(
            g_thresholds[static_cast<std::size_t>(category)].load(std::memory_order_relaxed));
    }

    void SetGlobalThreshold(LogLevel minimumLevel) noexcept
    {
        if (!IsValid(minimumLevel))
        {
            return;
        }
        for (auto& threshold : g_thresholds)
        {
            threshold.store(static_cast<std::uint8_t>(minimumLevel), std::memory_order_relaxed);
        }
    }

    void ResetThresholds() noexcept
    {
        SetGlobalThreshold(kDefaultThreshold);
    }

    bool ParseLogLevel(std::string_view text, LogLevel& out) noexcept
    {
        for (std::size_t i = 0; i < kLogLevelCount; ++i)
        {
            if (EqualsIgnoreCase(text, LogLevelName(static_cast<LogLevel>(i))))
            {
                out = static_cast<LogLevel>(i);
                return true;
            }
        }
        if (EqualsIgnoreCase(text, "warn"))
        {
            out = LogLevel::Warning;
            return true;
        }
        return false;
    }

    bool ParseLogCategory(std::string_view text, LogCategory& out) noexcept
    {
        for (std::size_t i = 0; i < kLogCategoryCount; ++i)
        {
            if (EqualsIgnoreCase(text, LogCategoryName(static_cast<LogCategory>(i))))
            {
                out = static_cast<LogCategory>(i);
                return true;
            }
        }
        return false;
    }

    void ApplyCommandLine(const char* commandLine, LogConfig& config) noexcept
    {
        config.commandLine = commandLine;
        if (commandLine == nullptr)
        {
            return;
        }

        char value[512];
        if (const char* cursor = FindFlag(commandLine, commandLine, "--log-level"))
        {
            ReadValue(cursor, value, sizeof(value));
            LogLevel level{};
            if (ParseLogLevel(value, level))
            {
                SetGlobalThreshold(level);
            }
        }
        for (const char* cursor = FindFlag(commandLine, commandLine, "--log-category"); cursor != nullptr;
             cursor = FindFlag(commandLine, cursor, "--log-category"))
        {
            cursor = ReadValue(cursor, value, sizeof(value));
            const std::string_view pair(value);
            const std::size_t colon = pair.find(':');
            if (colon == std::string_view::npos)
            {
                continue;
            }
            LogCategory category{};
            LogLevel level{};
            if (ParseLogCategory(pair.substr(0, colon), category) && ParseLogLevel(pair.substr(colon + 1), level))
            {
                SetCategoryThreshold(category, level);
            }
        }
        if (FindFlag(commandLine, commandLine, "--log-sync") != nullptr)
        {
            config.synchronous = true;
        }
        if (FindFlag(commandLine, commandLine, "--log-no-file") != nullptr)
        {
            config.fileSink = false;
        }
        if (const char* cursor = FindFlag(commandLine, commandLine, "--log-file"))
        {
            ReadValue(cursor, value, sizeof(value));
            if (value[0] != '\0')
            {
                // lpCmdLine is ANSI; the session path is kept wide.
                const int length = MultiByteToWideChar(
                    CP_ACP, 0, value, -1, config.filePath,
                    static_cast<int>(std::size(config.filePath)));
                if (length <= 0)
                {
                    config.filePath[0] = L'\0';
                }
            }
        }
    }

    // ---- Lifecycle ----------------------------------------------------------------------------

    bool Initialize(const LogConfig& config) noexcept
    {
        if (g_state.load(std::memory_order_seq_cst) != nullptr)
        {
            return false;
        }

        LoggerState* state = new (std::nothrow) LoggerState;
        if (state == nullptr)
        {
            return false;
        }
        state->config = config;
        state->clock.Capture();

        if (!config.synchronous && !state->queue.Initialize(config.queueCapacity))
        {
            delete state;
            return false;
        }
        (void)state->memory.Initialize(config.memoryRingCapacity); // failure = no viewer ring

        // Per-session counters.
        g_submitted.store(0, std::memory_order_relaxed);
        for (auto& count : g_dropped)
        {
            count.store(0, std::memory_order_relaxed);
        }

        bool fileFailed = false;
        bool autoPath = false;
        if (config.fileSink)
        {
            state->fileRequested = true;
            if (config.filePath[0] != L'\0')
            {
                wcsncpy_s(state->sessionPath, config.filePath, _TRUNCATE);
            }
            else
            {
                sinks::EnsureLogDirectory();
                autoPath = sinks::BuildSessionPath(state->clock, kBuildTag, state->sessionPath, std::size(state->sessionPath)) != 0;
            }
            if (!state->file.Open(state->sessionPath))
            {
                fileFailed = true;
            }
            else if (autoPath)
            {
                sinks::WriteLatestHint(state->sessionPath);
            }
        }
        state->consoleEnabled = config.consoleSink != ConsoleMode::Off && state->console.Detect();

        state->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        state->flushEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!config.synchronous)
        {
            unsigned threadId = 0;
            const uintptr_t handle = _beginthreadex(nullptr, 0, &WriterMain, state, 0, &threadId);
            if (handle == 0)
            {
                if (state->wakeEvent != nullptr) { CloseHandle(state->wakeEvent); }
                if (state->flushEvent != nullptr) { CloseHandle(state->flushEvent); }
                delete state;
                return false;
            }
            state->writerThread = reinterpret_cast<HANDLE>(handle);
            state->writerThreadId = threadId;
        }

        g_state.store(state, std::memory_order_seq_cst);

        // Session header, through the normal path so it is ordered like every other record.
        {
            wchar_t executable[512] = {};
            GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
            wchar_t directory[512] = {};
            GetCurrentDirectoryW(static_cast<DWORD>(std::size(directory)), directory);
            LOG_INFO(LogCategory::Core, "session start: build={} pid={} exe={}", kBuildTag, GetCurrentProcessId(), executable);
            LOG_INFO(LogCategory::Core, "session cwd={}", directory);
            LOG_INFO(LogCategory::Core, "session cmdline={}", config.commandLine != nullptr ? config.commandLine : GetCommandLineA());
            if (state->file.IsOpen())
            {
                LOG_INFO(LogCategory::Core, "session file={} queue={} ring={} sync={}", state->sessionPath,
                    state->queue.Capacity(), state->memory.Capacity(), config.synchronous);
            }
            else if (fileFailed)
            {
                LOG_WARNING(LogCategory::Core, "logging: cannot create session file {} (error {}); continuing with DBWIN only",
                    state->sessionPath, GetLastError());
            }
            else
            {
                LOG_INFO(LogCategory::Core, "session file=<none> (disabled) queue={} sync={}",
                    state->queue.Capacity(), config.synchronous);
            }
        }
        return true;
    }

    void Shutdown() noexcept
    {
        LoggerState* state = g_state.exchange(nullptr, std::memory_order_seq_cst);
        if (state == nullptr)
        {
            return;
        }

        // 1. Producers now fall back to DBWIN. Wait for the ones already inside; the writer is
        //    still running so a Flush waiter among them completes normally.
        const std::uint64_t producerDeadline = GetTickCount64() + state->config.shutdownTimeoutMs;
        while (g_activeProducers.load(std::memory_order_seq_cst) != 0 && GetTickCount64() < producerDeadline)
        {
            Sleep(0);
        }
        const bool producersClear = g_activeProducers.load(std::memory_order_seq_cst) == 0;

        // 2. Stop the writer; it drains everything that was accepted.
        bool writerStopped = state->config.synchronous;
        if (!writerStopped)
        {
            state->stopRequested.store(true, std::memory_order_seq_cst);
            SetEvent(state->wakeEvent);
            writerStopped = WaitForSingleObject(state->writerThread, state->config.shutdownTimeoutMs) == WAIT_OBJECT_0;
            if (writerStopped)
            {
                CloseHandle(state->writerThread);
                state->writerThread = nullptr;
            }
        }

        if (!writerStopped || !producersClear)
        {
            // Something is still inside the state; freeing it would be a use-after-free. Leave
            // it (and its handles) to the OS and say so where a human can see it.
            LogRecord record;
            record.level = LogLevel::Error;
            record.category = LogCategory::Core;
            detail::FormatRecord(record, "logging: shutdown timed out (writerStopped={} producersClear={}); session state abandoned",
                writerStopped, producersClear);
            StampRecord(record);
            EmergencyRecord(state, record);
            return;
        }

        // 3. Footer, then close. The writer is gone, so this thread owns the sinks.
        {
            const std::uint64_t submitted = g_submitted.load(std::memory_order_relaxed);
            std::uint64_t dropped = 0;
            for (const auto& count : g_dropped)
            {
                dropped += count.load(std::memory_order_relaxed);
            }
            ReportDrops(*state);
            ProcessSynthetic(*state, LogLevel::Info,
                "session end: clean shutdown; submitted={} written={} dropped={} fileBytes={} elapsed={:.3f}s",
                submitted, state->written.load(std::memory_order_relaxed), dropped,
                state->file.BytesWritten(), state->clock.ElapsedSeconds(NowQpc()));
        }
        state->file.Close();
        if (state->wakeEvent != nullptr) { CloseHandle(state->wakeEvent); }
        if (state->flushEvent != nullptr) { CloseHandle(state->flushEvent); }
        delete state;
    }

    bool IsInitialized() noexcept
    {
        return g_state.load(std::memory_order_seq_cst) != nullptr;
    }

    bool Flush(std::uint32_t timeoutMs) noexcept
    {
        StateGuard guard;
        if (guard.state == nullptr)
        {
            return false;
        }
        return FlushState(*guard.state, timeoutMs);
    }

    void SetFrameNumber(std::uint64_t frame) noexcept
    {
        g_frame.store(frame, std::memory_order_relaxed);
    }

    void SetCurrentThreadName(std::string_view name) noexcept
    {
        g_threadNames.Set(GetCurrentThreadId(), name);

        // Also visible in the debugger / ETW. Resolved dynamically: SetThreadDescription is a
        // Windows 10 1607 export and the project does not otherwise pin that SDK level.
        using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        static const SetThreadDescriptionFn setDescription = []() -> SetThreadDescriptionFn
        {
            const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
            return kernel32 != nullptr
                ? reinterpret_cast<SetThreadDescriptionFn>(GetProcAddress(kernel32, "SetThreadDescription"))
                : nullptr;
        }();
        if (setDescription != nullptr && !name.empty())
        {
            wchar_t wide[sinks::ThreadNameTable::kNameCapacity];
            const std::size_t limit = std::size(wide) - 1;
            const std::size_t count = name.size() < limit ? name.size() : limit;
            const int length = MultiByteToWideChar(
                CP_UTF8, 0, name.data(), static_cast<int>(count), wide, static_cast<int>(limit));
            wide[length > 0 ? length : 0] = L'\0';
            setDescription(GetCurrentThread(), wide);
        }
    }

    std::size_t GetSessionFilePath(char* out, std::size_t capacity) noexcept
    {
        StateGuard guard;
        if (guard.state == nullptr || !guard.state->file.IsOpen())
        {
            if (capacity != 0)
            {
                out[0] = '\0';
            }
            return 0;
        }
        return WideToUtf8(guard.state->sessionPath, out, capacity);
    }

    void GetStatistics(LogStatistics& out) noexcept
    {
        out = LogStatistics{};
        out.submitted = g_submitted.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < kLogLevelCount; ++i)
        {
            out.droppedByLevel[i] = g_dropped[i].load(std::memory_order_relaxed);
        }
        StateGuard guard;
        if (guard.state != nullptr)
        {
            out.written = guard.state->written.load(std::memory_order_relaxed);
            out.fileBytes = guard.state->file.BytesWritten();
            out.fileSinkActive = guard.state->file.IsOpen();
        }
    }

    bool GetSessionEpoch(std::int64_t* qpcStart, std::int64_t* qpcFrequency) noexcept
    {
        StateGuard guard;
        if (guard.state == nullptr)
        {
            if (qpcStart != nullptr) { *qpcStart = 0; }
            if (qpcFrequency != nullptr) { *qpcFrequency = 0; }
            return false;
        }
        if (qpcStart != nullptr) { *qpcStart = guard.state->clock.qpcStart; }
        if (qpcFrequency != nullptr) { *qpcFrequency = guard.state->clock.qpcFrequency; }
        return true;
    }

    std::size_t GetThreadName(std::uint32_t threadId, char* out, std::size_t capacity) noexcept
    {
        return g_threadNames.Get(threadId, out, capacity);
    }

    std::size_t CopyRecentRecords(
        std::uint64_t cursor, LogRecord* out, std::size_t maxCount, std::uint64_t* newCursor) noexcept
    {
        StateGuard guard;
        if (guard.state == nullptr)
        {
            if (newCursor != nullptr)
            {
                *newCursor = cursor;
            }
            return 0;
        }
        return guard.state->memory.CopySince(cursor, out, maxCount, newCursor);
    }

    // ---- Emergency ----------------------------------------------------------------------------

    void EmergencyWrite(
        LogLevel level,
        LogCategory category,
        std::string_view message,
        std::source_location location) noexcept
    {
        static thread_local int depth = 0;
        if (depth != 0)
        {
            return; // a crash inside logging gets exactly one attempt
        }
        ++depth;

        LogRecord record;
        record.level = level;
        record.category = category;
        record.sourceFile = location.file_name();
        record.sourceFunction = location.function_name();
        record.sourceLine = location.line();
        detail::FixedBufferWriter writer(record.message, sizeof(record.message));
        writer.Append(message);
        record.messageByteCount = static_cast<std::uint16_t>(writer.Size());
        record.flags = writer.Truncated() ? LogRecordFlagTruncated : LogRecordFlagNone;
        StampRecord(record);

        StateGuard guard;
        EmergencyRecord(guard.state, record);
        --depth;
    }

    // ---- Raw frontend -------------------------------------------------------------------------

    std::size_t Utf16ToUtf8(
        std::wstring_view text,
        char* destination,
        std::size_t capacity,
        bool* truncated) noexcept
    {
        bool wasTruncated = false;
        std::size_t size = 0;
        if (capacity != 0)
        {
            destination[0] = '\0';
        }

        auto appendByte = [&](std::uint8_t byte)
        {
            if (capacity != 0 && size + 1 < capacity)
            {
                destination[size++] = static_cast<char>(byte);
                destination[size] = '\0';
            }
            else
            {
                wasTruncated = true;
            }
        };

        for (std::size_t i = 0; i < text.size(); ++i)
        {
            std::uint32_t codePoint = static_cast<std::uint16_t>(text[i]);
            if (codePoint >= 0xD800u && codePoint <= 0xDBFFu)
            {
                if (i + 1 < text.size())
                {
                    const std::uint32_t low = static_cast<std::uint16_t>(text[i + 1]);
                    if (low >= 0xDC00u && low <= 0xDFFFu)
                    {
                        codePoint = 0x10000u + ((codePoint - 0xD800u) << 10u) + (low - 0xDC00u);
                        ++i;
                    }
                    else
                    {
                        codePoint = 0xFFFDu;
                    }
                }
                else
                {
                    codePoint = 0xFFFDu;
                }
            }
            else if (codePoint >= 0xDC00u && codePoint <= 0xDFFFu)
            {
                codePoint = 0xFFFDu;
            }

            std::uint8_t encoded[4]{};
            std::size_t encodedSize = 0;
            if (codePoint <= 0x7Fu)
            {
                encoded[0] = static_cast<std::uint8_t>(codePoint);
                encodedSize = 1;
            }
            else if (codePoint <= 0x7FFu)
            {
                encoded[0] = static_cast<std::uint8_t>(0xC0u | (codePoint >> 6u));
                encoded[1] = static_cast<std::uint8_t>(0x80u | (codePoint & 0x3Fu));
                encodedSize = 2;
            }
            else if (codePoint <= 0xFFFFu)
            {
                encoded[0] = static_cast<std::uint8_t>(0xE0u | (codePoint >> 12u));
                encoded[1] = static_cast<std::uint8_t>(0x80u | ((codePoint >> 6u) & 0x3Fu));
                encoded[2] = static_cast<std::uint8_t>(0x80u | (codePoint & 0x3Fu));
                encodedSize = 3;
            }
            else
            {
                encoded[0] = static_cast<std::uint8_t>(0xF0u | (codePoint >> 18u));
                encoded[1] = static_cast<std::uint8_t>(0x80u | ((codePoint >> 12u) & 0x3Fu));
                encoded[2] = static_cast<std::uint8_t>(0x80u | ((codePoint >> 6u) & 0x3Fu));
                encoded[3] = static_cast<std::uint8_t>(0x80u | (codePoint & 0x3Fu));
                encodedSize = 4;
            }

            if (capacity == 0 || size + encodedSize >= capacity)
            {
                wasTruncated = true;
                break;
            }
            for (std::size_t byte = 0; byte < encodedSize; ++byte)
            {
                appendByte(encoded[byte]);
            }
        }

        if (truncated != nullptr)
        {
            *truncated = wasTruncated;
        }
        return size;
    }

    void WriteRaw(
        LogLevel level,
        LogCategory category,
        std::string_view message,
        std::source_location location) noexcept
    {
        if (!ShouldLog(level, category))
        {
            return;
        }

        LogRecord record;
        record.level = level;
        record.category = category;
        record.sourceFile = location.file_name();
        record.sourceFunction = location.function_name();
        record.sourceLine = location.line();
        detail::FixedBufferWriter writer(record.message, sizeof(record.message));
        writer.Append(message);
        record.messageByteCount = static_cast<std::uint16_t>(writer.Size());
        if (writer.Truncated())
        {
            record.flags = static_cast<std::uint8_t>(record.flags | LogRecordFlagTruncated);
        }
        detail::SubmitRecord(record);
    }

    void WriteRawLines(
        LogLevel level,
        LogCategory category,
        std::string_view text,
        std::source_location location) noexcept
    {
        if (!ShouldLog(level, category))
        {
            return;
        }
        while (!text.empty())
        {
            const std::size_t newline = text.find('\n');
            std::string_view line = text.substr(0, newline);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            {
                line.remove_suffix(1);
            }
            if (!line.empty())
            {
                WriteRaw(level, category, line, location);
            }
            if (newline == std::string_view::npos)
            {
                break;
            }
            text.remove_prefix(newline + 1);
        }
    }

    void WriteRawWide(
        LogLevel level,
        LogCategory category,
        std::wstring_view message,
        std::source_location location) noexcept
    {
        if (!ShouldLog(level, category))
        {
            return;
        }

        LogRecord record;
        record.level = level;
        record.category = category;
        record.sourceFile = location.file_name();
        record.sourceFunction = location.function_name();
        record.sourceLine = location.line();
        detail::FixedBufferWriter writer(record.message, sizeof(record.message));
        writer.Append(message);
        record.messageByteCount = static_cast<std::uint16_t>(writer.Size());
        if (writer.Truncated())
        {
            record.flags = static_cast<std::uint8_t>(record.flags | LogRecordFlagTruncated);
        }
        detail::SubmitRecord(record);
    }

    namespace detail
    {
        void SubmitRecord(LogRecord& record) noexcept
        {
            StampRecord(record);
            StateGuard guard;
            if (guard.state == nullptr)
            {
                EmitToDebugger(record);
                return;
            }
            SubmitToState(*guard.state, record);
        }

        bool ThrottleAllows(
            std::atomic<std::int64_t>& nextAllowedNanoseconds,
            std::int64_t intervalNanoseconds) noexcept
        {
            const std::int64_t frequency = QpcFrequency();
            const std::int64_t counter = NowQpc();
            const std::int64_t wholeSeconds = counter / frequency;
            const std::int64_t remainder = counter % frequency;
            const std::int64_t now = wholeSeconds * 1'000'000'000ll +
                remainder * 1'000'000'000ll / frequency;
            std::int64_t expected = nextAllowedNanoseconds.load(std::memory_order_relaxed);
            while (now >= expected)
            {
                const std::int64_t maximum = (std::numeric_limits<std::int64_t>::max)();
                const std::int64_t desired = intervalNanoseconds > maximum - now
                    ? maximum
                    : now + intervalNanoseconds;
                if (nextAllowedNanoseconds.compare_exchange_weak(
                        expected, desired, std::memory_order_relaxed, std::memory_order_relaxed))
                {
                    return true;
                }
            }
            return false;
        }
    }

    // ---- Self tests ---------------------------------------------------------------------------

    FrontendSelfTestResult RunFrontendSelfTests(const AllocationProbe* probe) noexcept
    {
        FrontendSelfTestResult result{};
        auto check = [&](bool condition, std::string_view failure)
        {
            if (condition)
            {
                ++result.passed;
                return;
            }
            if (result.failed++ == 0)
            {
                CopyFailure(result.firstFailure, sizeof(result.firstFailure), failure);
            }
        };

        const LogLevel oldCoreThreshold = GetCategoryThreshold(LogCategory::Core);
        SetCategoryThreshold(LogCategory::Core, LogLevel::Fatal);
        int evaluated = 0;
        LOG_DEBUG(LogCategory::Core, "filtered {}", ++evaluated);
        check(evaluated == 0, "runtime filtering evaluated a format argument");
        SetCategoryThreshold(LogCategory::Core, oldCoreThreshold);

#if defined(NDEBUG)
        int traceEvaluated = 0;
        LOG_TRACE(LogCategory::Core, "compiled out {}", ++traceEvaluated);
        check(traceEvaluated == 0, "Release Trace evaluated a format argument");
#endif

        {
            LogRecord formatted{};
            detail::FormatRecord(formatted, "value={} hex={:04X} float={:.2f}", 42, 255u, 1.5);
            check(
                std::string_view(formatted.message, formatted.messageByteCount) ==
                    "value=42 hex=00FF float=1.50" &&
                    formatted.flags == LogRecordFlagNone,
                "fixed-buffer scalar formatting produced an unexpected result");
        }
        {
            // Regression: a bare nullptr literal used to take the string_view branch.
            LogRecord nullFormatted{};
            detail::FormatRecord(nullFormatted, "{}", nullptr);
            check(
                std::string_view(nullFormatted.message, nullFormatted.messageByteCount) == "<null>",
                "nullptr literal did not render as <null>");
        }
        {
            LogRecord mismatched{};
            detail::FormatRecord(mismatched, "{} and {}", 1);
            check((mismatched.flags & LogRecordFlagFormatError) != 0, "missing argument was not flagged");
            LogRecord surplus{};
            detail::FormatRecord(surplus, "{}", 1, 2);
            check((surplus.flags & LogRecordFlagFormatError) != 0, "surplus argument was not flagged");
        }

        {
            char oversized[kLogMessageCapacity + 128]{};
            for (std::size_t i = 0; i + 1 < std::size(oversized); ++i)
            {
                oversized[i] = 'x';
            }
            LogRecord truncated{};
            detail::FormatRecord(truncated, "{}", std::string_view(oversized));
            check(
                truncated.messageByteCount == kLogMessageCapacity - 1 &&
                    truncated.message[kLogMessageCapacity - 1] == '\0' &&
                    (truncated.flags & LogRecordFlagTruncated) != 0,
                "oversized message did not truncate and terminate correctly");
        }

        {
            char utf8[16]{};
            bool utf8Truncated = false;
            const std::size_t utf8Size = Utf16ToUtf8(L"A\u20AC", utf8, sizeof(utf8), &utf8Truncated);
            check(
                utf8Size == 4 && !utf8Truncated &&
                    std::string_view(utf8, utf8Size) == std::string_view("A\xE2\x82\xAC", 4),
                "UTF-16 to UTF-8 conversion failed");
        }

        {
            int onceHits = 0;
            for (int i = 0; i < 16; ++i)
            {
                LOG_WARNING_ONCE(LogCategory::Core, "once {}", ++onceHits);
            }
            check(onceHits == 1, "ONCE emitted more than once");
            int everyHits = 0;
            for (int i = 0; i < 40; ++i)
            {
                LOG_INFO_EVERY_N(10, LogCategory::Core, "every {}", ++everyHits);
            }
            check(everyHits == 4, "EVERY_N(10) did not emit 4 times out of 40");
        }

        // Allocation check: only meaningful when the probe can actually see allocations.
        if (probe == nullptr || probe->count == nullptr)
        {
            ++result.skipped;
        }
        else
        {
            const std::uint64_t before = probe->count(probe->user);
            {
                // Through a volatile sink so the new/delete pair cannot be elided.
                static void* volatile sink = nullptr;
                sink = new (std::nothrow) char[64];
                delete[] static_cast<char*>(sink);
                sink = nullptr;
            }
            const std::uint64_t calibrated = probe->count(probe->user);
            if (calibrated == before)
            {
                ++result.skipped; // the allocator does not count in this build
            }
            else
            {
                const std::wstring_view wide = L"wide\u20AC";
                const std::string_view narrow = "narrow";
                LogRecord warm{};
                detail::FormatRecord(warm, "{} {} {}", narrow, wide, 1);
                const std::uint64_t start = probe->count(probe->user);
                for (int i = 0; i < 256; ++i)
                {
                    LogRecord scratch;
                    detail::FormatRecord(scratch, "i={} hex={:08x} f={:.3f} s={} w={} b={} p={}",
                        i, 0xBEEFu + i, 0.25 * i, narrow, wide, true, static_cast<const void*>(&scratch));
                    LOG_INFO(LogCategory::Core, "self-test {} {} {}", i, narrow, wide);
                }
                const std::uint64_t end = probe->count(probe->user);
                check(end == start, "formatting/submit path allocated");
            }
        }

        return result;
    }
}
