#pragma once

#include "LogRecord.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Project-wide event log. See docs/logging_system_plan.md for the contract.
//
//   LOG_INFO(logging::LogCategory::Scene, "Loaded level {}", path);
//   LOG_WARNING_ONCE(logging::LogCategory::RenderRt, "RT allocation failed; using SSR");
//   LOG_DEBUG_EVERY_N(120, logging::LogCategory::RenderShadow, "pages {}", count);
//   LOG_INFO_THROTTLED(std::chrono::seconds(2), logging::LogCategory::Vfx, "...");
//
// Producer path (any thread): category threshold check -> fixed-buffer format -> QPC/ids ->
// non-blocking push into a bounded ring. No heap, no locks, no file I/O. Before Initialize and
// after Shutdown accepted records go straight to DBWIN. A dedicated writer thread renders the
// session file, DBWIN mirror, console and the in-memory ring for the log viewer.
namespace logging
{
    // ---- Filtering ----------------------------------------------------------------------------

    [[nodiscard]] bool ShouldLog(LogLevel level, LogCategory category) noexcept;
    void SetCategoryThreshold(LogCategory category, LogLevel minimumLevel) noexcept;
    [[nodiscard]] LogLevel GetCategoryThreshold(LogCategory category) noexcept;
    void SetGlobalThreshold(LogLevel minimumLevel) noexcept;
    void ResetThresholds() noexcept;

    // Parses "trace|debug|info|warning|warn|error|fatal" (case-insensitive). False = unknown.
    [[nodiscard]] bool ParseLogLevel(std::string_view text, LogLevel& out) noexcept;
    // Parses a category by its text name ("render.rt", "asset", ...). False = unknown.
    [[nodiscard]] bool ParseLogCategory(std::string_view text, LogCategory& out) noexcept;

    // ---- Session lifecycle (L2/L3) ------------------------------------------------------------

    enum class ConsoleMode : std::uint8_t
    {
        Auto, // write when stdout is a console, pipe or file
        Off,
        On
    };

    struct LogConfig
    {
        std::uint32_t queueCapacity = 8192;     // records; rounded up to a power of two
        std::uint32_t memoryRingCapacity = 4096; // records kept for the viewer; 0 disables
        bool fileSink = true;
        bool debuggerSink = true;
        ConsoleMode consoleSink = ConsoleMode::Auto;
        // Every record is rendered on the calling thread under a lock (--log-sync). For chasing
        // a crash whose last lines never reach the writer; slow by design.
        bool synchronous = false;
        std::uint32_t flushIntervalMs = 250;
        std::uint32_t shutdownTimeoutMs = 5000;
        // Records below this level are not mirrored to DBWIN (the file gets everything that
        // passed the category threshold).
#if defined(NDEBUG)
        LogLevel debuggerMinimum = LogLevel::Warning;
#else
        LogLevel debuggerMinimum = LogLevel::Debug;
#endif
        // Explicit session file path; empty = logs/session_<stamp>_<pid>_<tag>.log.
        wchar_t filePath[512] = {};
        // Recorded in the session header (not owned; must outlive Initialize).
        const char* commandLine = nullptr;
    };

    // Applies the logging switches found in a command line:
    //   --log-level=<level>               global producer threshold
    //   --log-category=<name>:<level>     per-category threshold (repeatable)
    //   --log-sync                        synchronous mode
    //   --log-no-file                     no session file
    //   --log-file=<path>                 explicit session file path
    // Thresholds are applied immediately; sink choices land in `config`.
    void ApplyCommandLine(const char* commandLine, LogConfig& config) noexcept;

    // Starts the session. Returns false only when the logger is already running or the queue
    // cannot be allocated; a session file that cannot be created degrades to DBWIN and still
    // returns true. Safe to call again after Shutdown.
    bool Initialize(const LogConfig& config) noexcept;
    // Drains the queue, writes the footer, stops the writer. Never waits longer than the
    // configured shutdown timeout. Records submitted afterwards go to DBWIN.
    void Shutdown() noexcept;
    [[nodiscard]] bool IsInitialized() noexcept;

    // Waits until everything queued so far has reached the OS. False on timeout or when the
    // writer is not running (then there is nothing to wait for).
    bool Flush(std::uint32_t timeoutMs = 2000) noexcept;

    // Published by the frame loop; kInvalidLogFrame before the first frame.
    void SetFrameNumber(std::uint64_t frame) noexcept;
    // Names the calling thread for the [tid=..../Name] column and the debugger.
    void SetCurrentThreadName(std::string_view name) noexcept;

    // UTF-8 path of the current session file; 0 when there is none.
    std::size_t GetSessionFilePath(char* out, std::size_t capacity) noexcept;

    struct LogStatistics
    {
        std::uint64_t submitted = 0;         // accepted by a producer (queued or synchronous)
        std::uint64_t written = 0;           // rendered by the writer
        std::uint64_t droppedByLevel[kLogLevelCount] = {};
        std::uint64_t fileBytes = 0;
        bool fileSinkActive = false;

        [[nodiscard]] std::uint64_t DroppedTotal() const noexcept
        {
            std::uint64_t total = 0;
            for (const std::uint64_t count : droppedByLevel)
            {
                total += count;
            }
            return total;
        }
    };

    void GetStatistics(LogStatistics& out) noexcept;

    // Snapshot for the viewer: copies records with ring position >= cursor (oldest first) and
    // returns the count; *newCursor is the position to pass next time. Cheap when nothing is
    // new. Zero when the ring is disabled or the logger is not running.
    std::size_t CopyRecentRecords(
        std::uint64_t cursor, LogRecord* out, std::size_t maxCount, std::uint64_t* newCursor) noexcept;

    // ---- Emergency path -----------------------------------------------------------------------

    // Writes one record immediately: DBWIN first, then an unbuffered append to the session file.
    // No heap, no logger locks, no dependency on the writer thread; safe from a crash handler.
    // A nested emergency write on the same thread (crash inside logging) is ignored.
    void EmergencyWrite(
        LogLevel level,
        LogCategory category,
        std::string_view message,
        std::source_location location = std::source_location::current()) noexcept;

    // ---- Raw frontend -------------------------------------------------------------------------

    // Converts into caller-owned storage, always terminates when capacity is non-zero, and returns
    // the number of UTF-8 bytes excluding the terminator.
    [[nodiscard]] std::size_t Utf16ToUtf8(
        std::wstring_view text,
        char* destination,
        std::size_t capacity,
        bool* truncated = nullptr) noexcept;

    // Already-formatted text (SDK / allocator callbacks): copied into the record, no formatting.
    void WriteRaw(
        LogLevel level,
        LogCategory category,
        std::string_view message,
        std::source_location location = std::source_location::current()) noexcept;

    void WriteRawWide(
        LogLevel level,
        LogCategory category,
        std::wstring_view message,
        std::source_location location = std::source_location::current()) noexcept;

    // Multi-line already-formatted text (shader compiler output, SDK dumps): one record per
    // line, trailing CR/space trimmed, empty lines skipped. Keeps the session file greppable.
    void WriteRawLines(
        LogLevel level,
        LogCategory category,
        std::string_view text,
        std::source_location location = std::source_location::current()) noexcept;

    // ---- Self tests ---------------------------------------------------------------------------

    // Optional allocation counter for the self tests. Supplied by the harness (mimalloc stats);
    // when absent, or when a probe allocation does not move the counter, the allocation check is
    // reported as skipped rather than passed.
    struct AllocationProbe
    {
        std::uint64_t (*count)(void* user) noexcept = nullptr;
        void* user = nullptr;
    };

    struct FrontendSelfTestResult
    {
        std::uint32_t passed = 0;
        std::uint32_t failed = 0;
        std::uint32_t skipped = 0;
        char firstFailure[192]{};

        [[nodiscard]] bool Succeeded() const noexcept { return failed == 0; }
    };

    [[nodiscard]] FrontendSelfTestResult RunFrontendSelfTests(const AllocationProbe* probe = nullptr) noexcept;

    namespace detail
    {
        class FixedBufferWriter
        {
        public:
            FixedBufferWriter(char* destination, std::size_t capacity) noexcept
                : destination_(destination), capacity_(capacity)
            {
                if (capacity_ != 0)
                {
                    destination_[0] = '\0';
                }
            }

            void Append(std::string_view text) noexcept
            {
                if (capacity_ == 0)
                {
                    truncated_ = truncated_ || !text.empty();
                    return;
                }

                const std::size_t available = capacity_ - 1 - size_;
                const std::size_t count = text.size() < available ? text.size() : available;
                if (count != 0)
                {
                    std::char_traits<char>::copy(destination_ + size_, text.data(), count);
                    size_ += count;
                }
                destination_[size_] = '\0';
                truncated_ = truncated_ || count != text.size();
            }

            void Append(char value) noexcept
            {
                if (capacity_ != 0 && size_ + 1 < capacity_)
                {
                    destination_[size_++] = value;
                    destination_[size_] = '\0';
                }
                else
                {
                    truncated_ = true;
                }
            }

            void Append(std::wstring_view text) noexcept
            {
                if (capacity_ == 0 || size_ + 1 >= capacity_)
                {
                    truncated_ = truncated_ || !text.empty();
                    return;
                }

                bool conversionTruncated = false;
                const std::size_t count = Utf16ToUtf8(
                    text, destination_ + size_, capacity_ - size_, &conversionTruncated);
                size_ += count;
                truncated_ = truncated_ || conversionTruncated;
            }

            void MarkFormatError() noexcept { formatError_ = true; }
            [[nodiscard]] std::size_t Size() const noexcept { return size_; }
            [[nodiscard]] bool Truncated() const noexcept { return truncated_; }
            [[nodiscard]] bool HasFormatError() const noexcept { return formatError_; }

        private:
            char* destination_ = nullptr;
            std::size_t capacity_ = 0;
            std::size_t size_ = 0;
            bool truncated_ = false;
            bool formatError_ = false;
        };

        inline void AppendPadded(
            FixedBufferWriter& writer,
            std::string_view value,
            int width,
            char fill) noexcept
        {
            for (int i = static_cast<int>(value.size()); i < width; ++i)
            {
                writer.Append(fill);
            }
            writer.Append(value);
        }

        template <typename Integer>
        void AppendInteger(FixedBufferWriter& writer, Integer value, std::string_view spec) noexcept
        {
            int base = 10;
            int width = 0;
            char fill = ' ';
            bool uppercase = false;
            std::size_t digitEnd = spec.size();

            if (!spec.empty() && (spec.back() == 'x' || spec.back() == 'X'))
            {
                base = 16;
                uppercase = spec.back() == 'X';
                --digitEnd;
            }
            if (digitEnd != 0 && spec.front() == '0')
            {
                fill = '0';
            }
            for (std::size_t i = 0; i < digitEnd; ++i)
            {
                if (spec[i] < '0' || spec[i] > '9')
                {
                    writer.MarkFormatError();
                    width = 0;
                    break;
                }
                const int digit = static_cast<int>(spec[i] - '0');
                if (width > (static_cast<int>(kLogMessageCapacity) - digit) / 10)
                {
                    writer.MarkFormatError();
                    width = static_cast<int>(kLogMessageCapacity);
                    break;
                }
                width = width * 10 + digit;
            }

            char buffer[72]{};
            const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, base);
            if (result.ec != std::errc{})
            {
                writer.MarkFormatError();
                writer.Append("<format-error>");
                return;
            }
            if (uppercase)
            {
                for (char* cursor = buffer; cursor != result.ptr; ++cursor)
                {
                    if (*cursor >= 'a' && *cursor <= 'f')
                    {
                        *cursor = static_cast<char>(*cursor - 'a' + 'A');
                    }
                }
            }
            AppendPadded(writer, std::string_view(buffer, result.ptr), width, fill);
        }

        template <typename Float>
        void AppendFloat(FixedBufferWriter& writer, Float value, std::string_view spec) noexcept
        {
            char buffer[128]{};
            std::to_chars_result result{};
            if (spec.size() >= 3 && spec.front() == '.' && spec.back() == 'f')
            {
                int precision = 0;
                for (std::size_t i = 1; i + 1 < spec.size(); ++i)
                {
                    if (spec[i] < '0' || spec[i] > '9')
                    {
                        writer.MarkFormatError();
                        writer.Append("<format-error>");
                        return;
                    }
                    precision = precision * 10 + static_cast<int>(spec[i] - '0');
                }
                if (precision > 32)
                {
                    writer.MarkFormatError();
                    writer.Append("<format-error>");
                    return;
                }
                result = std::to_chars(
                    buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, precision);
            }
            else
            {
                if (!spec.empty())
                {
                    writer.MarkFormatError();
                }
                result = std::to_chars(buffer, buffer + sizeof(buffer), value);
            }

            if (result.ec != std::errc{})
            {
                writer.MarkFormatError();
                writer.Append("<format-error>");
                return;
            }
            writer.Append(std::string_view(buffer, result.ptr));
        }

        template <typename Value>
        void AppendArgument(FixedBufferWriter& writer, Value&& value, std::string_view spec) noexcept
        {
            using Raw = std::remove_reference_t<Value>;
            using Decayed = std::decay_t<Value>;

            // nullptr_t is tested BEFORE the string_view conversions: in C++20
            // is_convertible_v<nullptr_t, string_view> is true, and taking that branch would
            // construct string_view(nullptr), i.e. strlen(nullptr).
            if constexpr (std::is_same_v<Decayed, std::nullptr_t>)
            {
                writer.Append("<null>");
            }
            else if constexpr (std::is_same_v<Decayed, char>)
            {
                writer.Append(value);
            }
            else if constexpr (std::is_same_v<Decayed, bool>)
            {
                writer.Append(value ? "true" : "false");
            }
            else if constexpr (std::is_same_v<Decayed, const char*> || std::is_same_v<Decayed, char*>)
            {
                writer.Append(value != nullptr ? std::string_view(value) : std::string_view("<null>"));
            }
            else if constexpr (
                std::is_same_v<Decayed, const wchar_t*> || std::is_same_v<Decayed, wchar_t*>)
            {
                writer.Append(value != nullptr ? std::wstring_view(value) : std::wstring_view(L"<null>"));
            }
            else if constexpr (std::is_convertible_v<Raw, std::string_view>)
            {
                writer.Append(std::string_view(value));
            }
            else if constexpr (std::is_convertible_v<Raw, std::wstring_view>)
            {
                writer.Append(std::wstring_view(value));
            }
            else if constexpr (std::is_enum_v<Decayed>)
            {
                AppendInteger(writer, static_cast<std::underlying_type_t<Decayed>>(value), spec);
            }
            else if constexpr (std::is_integral_v<Decayed>)
            {
                AppendInteger(writer, value, spec);
            }
            else if constexpr (std::is_floating_point_v<Decayed>)
            {
                AppendFloat(writer, value, spec);
            }
            else if constexpr (std::is_pointer_v<Decayed>)
            {
                writer.Append("0x");
                AppendInteger(writer, reinterpret_cast<std::uintptr_t>(value), "x");
            }
            else
            {
                static_assert(!sizeof(Value), "Unsupported logging argument type");
            }

            if (!spec.empty() &&
                !(std::is_integral_v<Decayed> || std::is_floating_point_v<Decayed> ||
                  std::is_enum_v<Decayed>))
            {
                writer.MarkFormatError();
            }
        }

        inline void AppendFormatTail(FixedBufferWriter& writer, std::string_view format) noexcept
        {
            for (std::size_t i = 0; i < format.size(); ++i)
            {
                if (format[i] == '{' && i + 1 < format.size() && format[i + 1] == '{')
                {
                    writer.Append('{');
                    ++i;
                }
                else if (format[i] == '}' && i + 1 < format.size() && format[i + 1] == '}')
                {
                    writer.Append('}');
                    ++i;
                }
                else
                {
                    if (format[i] == '{' || format[i] == '}')
                    {
                        writer.MarkFormatError();
                    }
                    writer.Append(format[i]);
                }
            }
        }

        inline void FormatArguments(FixedBufferWriter& writer, std::string_view format) noexcept
        {
            AppendFormatTail(writer, format);
        }

        template <typename First, typename... Rest>
        void FormatArguments(
            FixedBufferWriter& writer,
            std::string_view format,
            First&& first,
            Rest&&... rest) noexcept
        {
            for (std::size_t i = 0; i < format.size(); ++i)
            {
                if (format[i] == '{')
                {
                    if (i + 1 < format.size() && format[i + 1] == '{')
                    {
                        writer.Append(format.substr(0, i));
                        writer.Append('{');
                        FormatArguments(
                            writer, format.substr(i + 2), std::forward<First>(first),
                            std::forward<Rest>(rest)...);
                        return;
                    }

                    const std::size_t close = format.find('}', i + 1);
                    if (close == std::string_view::npos)
                    {
                        writer.Append(format);
                        writer.MarkFormatError();
                        return;
                    }

                    writer.Append(format.substr(0, i));
                    std::string_view spec = format.substr(i + 1, close - i - 1);
                    if (!spec.empty())
                    {
                        if (spec.front() == ':')
                        {
                            spec.remove_prefix(1);
                        }
                        else
                        {
                            writer.MarkFormatError();
                        }
                    }
                    AppendArgument(writer, std::forward<First>(first), spec);
                    FormatArguments(
                        writer, format.substr(close + 1), std::forward<Rest>(rest)...);
                    return;
                }
                if (format[i] == '}' && i + 1 < format.size() && format[i + 1] == '}')
                {
                    writer.Append(format.substr(0, i));
                    writer.Append('}');
                    FormatArguments(
                        writer, format.substr(i + 2), std::forward<First>(first),
                        std::forward<Rest>(rest)...);
                    return;
                }
                if (format[i] == '}')
                {
                    writer.MarkFormatError();
                }
            }

            writer.Append(format);
            writer.MarkFormatError();
        }

        template <typename... Args>
        void FormatRecord(LogRecord& record, std::string_view format, Args&&... args) noexcept
        {
            FixedBufferWriter writer(record.message, sizeof(record.message));
            FormatArguments(writer, format, std::forward<Args>(args)...);
            record.messageByteCount = static_cast<std::uint16_t>(writer.Size());
            if (writer.Truncated())
            {
                record.flags = static_cast<std::uint8_t>(record.flags | LogRecordFlagTruncated);
            }
            if (writer.HasFormatError())
            {
                record.flags = static_cast<std::uint8_t>(record.flags | LogRecordFlagFormatError);
            }
        }

        // Stamps QPC/sequence/ids/frame and hands the record to the queue (or the fallback).
        void SubmitRecord(LogRecord& record) noexcept;

        [[nodiscard]] bool ThrottleAllows(
            std::atomic<std::int64_t>& nextAllowedNanoseconds,
            std::int64_t intervalNanoseconds) noexcept;

        inline bool EveryNAllows(std::atomic<std::uint64_t>& counter, std::uint64_t interval) noexcept
        {
            return interval != 0 && counter.fetch_add(1, std::memory_order_relaxed) % interval == 0;
        }

        template <typename Rep, typename Period>
        [[nodiscard]] std::int64_t ToNanoseconds(std::chrono::duration<Rep, Period> duration) noexcept
        {
            const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
            return nanoseconds.count() > 0 ? nanoseconds.count() : 0;
        }

        template <typename... Args>
        void WriteFormatted(
            LogLevel level,
            LogCategory category,
            std::source_location location,
            std::string_view format,
            Args&&... args) noexcept
        {
            // Default-initialised on purpose: the message buffer is written by the formatter and
            // only messageByteCount bytes of it are ever read, so zeroing 1 KiB per call would be
            // pure waste on the producer path.
            LogRecord record;
            record.level = level;
            record.category = category;
            record.sourceFile = location.file_name();
            record.sourceFunction = location.function_name();
            record.sourceLine = location.line();
            FormatRecord(record, format, std::forward<Args>(args)...);
            SubmitRecord(record);
        }
    }
}

#define TC_LOG_IMPL(levelValue, categoryValue, ...)                                              \
    do                                                                                           \
    {                                                                                            \
        const ::logging::LogCategory tcLogCategory = (categoryValue);                           \
        if (::logging::ShouldLog((levelValue), tcLogCategory))                                  \
        {                                                                                        \
            ::logging::detail::WriteFormatted(                                                  \
                (levelValue), tcLogCategory, std::source_location::current(), __VA_ARGS__);     \
        }                                                                                        \
    } while (false)

#define TC_LOG_ONCE_IMPL(levelValue, categoryValue, ...)                                         \
    do                                                                                           \
    {                                                                                            \
        const ::logging::LogCategory tcLogCategory = (categoryValue);                           \
        if (::logging::ShouldLog((levelValue), tcLogCategory))                                  \
        {                                                                                        \
            static std::atomic_flag tcLogOnceFlag;                                              \
            if (!tcLogOnceFlag.test_and_set(std::memory_order_relaxed))                         \
            {                                                                                    \
                ::logging::detail::WriteFormatted(                                              \
                    (levelValue), tcLogCategory, std::source_location::current(), __VA_ARGS__); \
            }                                                                                    \
        }                                                                                        \
    } while (false)

#define TC_LOG_EVERY_N_IMPL(levelValue, intervalValue, categoryValue, ...)                        \
    do                                                                                           \
    {                                                                                            \
        const ::logging::LogCategory tcLogCategory = (categoryValue);                           \
        if (::logging::ShouldLog((levelValue), tcLogCategory))                                  \
        {                                                                                        \
            static std::atomic<std::uint64_t> tcLogCounter{ 0 };                                \
            const std::uint64_t tcLogInterval = static_cast<std::uint64_t>(intervalValue);       \
            if (::logging::detail::EveryNAllows(tcLogCounter, tcLogInterval))                    \
            {                                                                                    \
                ::logging::detail::WriteFormatted(                                              \
                    (levelValue), tcLogCategory, std::source_location::current(), __VA_ARGS__); \
            }                                                                                    \
        }                                                                                        \
    } while (false)

#define TC_LOG_THROTTLED_IMPL(levelValue, durationValue, categoryValue, ...)                      \
    do                                                                                           \
    {                                                                                            \
        const ::logging::LogCategory tcLogCategory = (categoryValue);                           \
        if (::logging::ShouldLog((levelValue), tcLogCategory))                                  \
        {                                                                                        \
            static std::atomic<std::int64_t> tcLogNextAllowed{ 0 };                             \
            const std::int64_t tcLogInterval =                                                   \
                ::logging::detail::ToNanoseconds((durationValue));                               \
            if (::logging::detail::ThrottleAllows(tcLogNextAllowed, tcLogInterval))             \
            {                                                                                    \
                ::logging::detail::WriteFormatted(                                              \
                    (levelValue), tcLogCategory, std::source_location::current(), __VA_ARGS__); \
            }                                                                                    \
        }                                                                                        \
    } while (false)

#if defined(NDEBUG)
#define LOG_TRACE(categoryValue, ...) do { } while (false)
#define LOG_TRACE_ONCE(categoryValue, ...) do { } while (false)
#define LOG_TRACE_EVERY_N(intervalValue, categoryValue, ...) do { } while (false)
#define LOG_TRACE_THROTTLED(durationValue, categoryValue, ...) do { } while (false)
#else
#define LOG_TRACE(categoryValue, ...)                                                            \
    TC_LOG_IMPL(::logging::LogLevel::Trace, categoryValue, __VA_ARGS__)
#define LOG_TRACE_ONCE(categoryValue, ...)                                                       \
    TC_LOG_ONCE_IMPL(::logging::LogLevel::Trace, categoryValue, __VA_ARGS__)
#define LOG_TRACE_EVERY_N(intervalValue, categoryValue, ...)                                     \
    TC_LOG_EVERY_N_IMPL(                                                                         \
        ::logging::LogLevel::Trace, intervalValue, categoryValue, __VA_ARGS__)
#define LOG_TRACE_THROTTLED(durationValue, categoryValue, ...)                                   \
    TC_LOG_THROTTLED_IMPL(                                                                       \
        ::logging::LogLevel::Trace, durationValue, categoryValue, __VA_ARGS__)
#endif

#define LOG_DEBUG(categoryValue, ...)                                                            \
    TC_LOG_IMPL(::logging::LogLevel::Debug, categoryValue, __VA_ARGS__)
#define LOG_INFO(categoryValue, ...)                                                             \
    TC_LOG_IMPL(::logging::LogLevel::Info, categoryValue, __VA_ARGS__)
#define LOG_WARNING(categoryValue, ...)                                                          \
    TC_LOG_IMPL(::logging::LogLevel::Warning, categoryValue, __VA_ARGS__)
#define LOG_ERROR(categoryValue, ...)                                                            \
    TC_LOG_IMPL(::logging::LogLevel::Error, categoryValue, __VA_ARGS__)
#define LOG_FATAL(categoryValue, ...)                                                            \
    TC_LOG_IMPL(::logging::LogLevel::Fatal, categoryValue, __VA_ARGS__)

#define LOG_DEBUG_ONCE(categoryValue, ...)                                                       \
    TC_LOG_ONCE_IMPL(::logging::LogLevel::Debug, categoryValue, __VA_ARGS__)
#define LOG_INFO_ONCE(categoryValue, ...)                                                        \
    TC_LOG_ONCE_IMPL(::logging::LogLevel::Info, categoryValue, __VA_ARGS__)
#define LOG_WARNING_ONCE(categoryValue, ...)                                                     \
    TC_LOG_ONCE_IMPL(::logging::LogLevel::Warning, categoryValue, __VA_ARGS__)
#define LOG_ERROR_ONCE(categoryValue, ...)                                                       \
    TC_LOG_ONCE_IMPL(::logging::LogLevel::Error, categoryValue, __VA_ARGS__)
#define LOG_FATAL_ONCE(categoryValue, ...)                                                       \
    TC_LOG_ONCE_IMPL(::logging::LogLevel::Fatal, categoryValue, __VA_ARGS__)

#define LOG_DEBUG_EVERY_N(intervalValue, categoryValue, ...)                                     \
    TC_LOG_EVERY_N_IMPL(                                                                         \
        ::logging::LogLevel::Debug, intervalValue, categoryValue, __VA_ARGS__)
#define LOG_INFO_EVERY_N(intervalValue, categoryValue, ...)                                      \
    TC_LOG_EVERY_N_IMPL(                                                                         \
        ::logging::LogLevel::Info, intervalValue, categoryValue, __VA_ARGS__)
#define LOG_WARNING_EVERY_N(intervalValue, categoryValue, ...)                                   \
    TC_LOG_EVERY_N_IMPL(                                                                         \
        ::logging::LogLevel::Warning, intervalValue, categoryValue, __VA_ARGS__)
#define LOG_ERROR_EVERY_N(intervalValue, categoryValue, ...)                                     \
    TC_LOG_EVERY_N_IMPL(                                                                         \
        ::logging::LogLevel::Error, intervalValue, categoryValue, __VA_ARGS__)
#define LOG_FATAL_EVERY_N(intervalValue, categoryValue, ...)                                     \
    TC_LOG_EVERY_N_IMPL(                                                                         \
        ::logging::LogLevel::Fatal, intervalValue, categoryValue, __VA_ARGS__)

#define LOG_DEBUG_THROTTLED(durationValue, categoryValue, ...)                                   \
    TC_LOG_THROTTLED_IMPL(                                                                       \
        ::logging::LogLevel::Debug, durationValue, categoryValue, __VA_ARGS__)
#define LOG_INFO_THROTTLED(durationValue, categoryValue, ...)                                    \
    TC_LOG_THROTTLED_IMPL(                                                                       \
        ::logging::LogLevel::Info, durationValue, categoryValue, __VA_ARGS__)
#define LOG_WARNING_THROTTLED(durationValue, categoryValue, ...)                                 \
    TC_LOG_THROTTLED_IMPL(                                                                       \
        ::logging::LogLevel::Warning, durationValue, categoryValue, __VA_ARGS__)
#define LOG_ERROR_THROTTLED(durationValue, categoryValue, ...)                                   \
    TC_LOG_THROTTLED_IMPL(                                                                       \
        ::logging::LogLevel::Error, durationValue, categoryValue, __VA_ARGS__)
#define LOG_FATAL_THROTTLED(durationValue, categoryValue, ...)                                   \
    TC_LOG_THROTTLED_IMPL(                                                                       \
        ::logging::LogLevel::Fatal, durationValue, categoryValue, __VA_ARGS__)
