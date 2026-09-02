#pragma once

#include "LogRecord.h"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

// Writer-thread side of the logger: wall-clock rendering, the thread-name table, the one-line
// text shape, and the sinks (session file, DBWIN, console, in-memory ring). Nothing here is
// touched by a producer except SetCurrentThreadName, which is a rare, explicitly invoked call.
namespace logging::sinks
{
    // Maps a QPC timestamp back to wall-clock text. Producers store raw QPC only; the writer
    // renders dates from the session epoch, so a producer never formats a date.
    struct SessionClock
    {
        std::int64_t qpcStart = 0;
        std::int64_t qpcFrequency = 1;
        std::uint64_t startFileTime = 0; // 100 ns since 1601, UTC, taken with the QPC start

        void Capture() noexcept;

        // Seconds since qpcStart.
        [[nodiscard]] double ElapsedSeconds(std::int64_t qpc) const noexcept;

        // "YYYY-MM-DD HH:MM:SS.mmm" in local time; returns the byte count written (0 on failure).
        std::size_t FormatWallClock(std::int64_t qpc, char* out, std::size_t capacity) const noexcept;
    };

    // Fixed table of thread names keyed by OS thread id. Bounded so a runaway naming loop can
    // never grow it; a name for an unknown thread simply renders as the bare id.
    class ThreadNameTable
    {
    public:
        static constexpr std::size_t kCapacity = 128;
        static constexpr std::size_t kNameCapacity = 32;

        // Constant-initialised so a namespace-scope table is usable from any static initializer.
        constexpr ThreadNameTable() noexcept = default;

        void Set(std::uint32_t threadId, std::string_view name) noexcept;

        // Copies the name into `out` (always terminated when capacity != 0); returns the length,
        // 0 when the thread has no name. Uses a try-lock so the emergency path can never block
        // behind a thread that died while naming itself.
        std::size_t Get(std::uint32_t threadId, char* out, std::size_t capacity) noexcept;

    private:
        struct Entry
        {
            std::uint32_t threadId = 0;
            char name[kNameCapacity] = {};
        };

        SRWLOCK lock_ = SRWLOCK_INIT;
        Entry entries_[kCapacity] = {};
        std::size_t count_ = 0;
    };

    // Renders the stable one-line shape (the date is in the session header; no thread column):
    //   01:14:22.381 WARN [render.rt] 1842 message [truncated] (Renderer.cpp:1234)\n
    // Trailing CR/LF in the message is dropped; the source suffix is added for Warning and above.
    // Returns the byte count written into `out` (terminated, never exceeds capacity - 1).
    std::size_t FormatLine(
        const LogRecord& record,
        const SessionClock& clock,
        ThreadNameTable& threadNames,
        char* out,
        std::size_t capacity) noexcept;

    inline constexpr std::size_t kLineCapacity = kLogMessageCapacity + 384;

    // Session file. All writes go through FILE_APPEND_DATA handles so the emergency path can
    // append to the same file from any thread without coordinating with the buffered writer.
    class FileSink
    {
    public:
        FileSink() noexcept = default;
        ~FileSink();

        FileSink(const FileSink&) = delete;
        FileSink& operator=(const FileSink&) = delete;

        // Creates/truncates the file. False when it cannot be opened; the sink is then inert.
        [[nodiscard]] bool Open(const wchar_t* path) noexcept;
        void Close() noexcept;
        [[nodiscard]] bool IsOpen() const noexcept { return file_ != INVALID_HANDLE_VALUE; }

        // Buffered append; writes the buffer through when it fills.
        void Append(std::string_view text) noexcept;
        // Writes buffered bytes to the OS (survives a process crash, not a kernel crash).
        void Commit() noexcept;
        // Commit + FlushFileBuffers: what an explicit Flush before an abort wants.
        void Sync() noexcept;

        // Second append-mode handle to the same file for the emergency path. Never buffered.
        [[nodiscard]] HANDLE EmergencyHandle() const noexcept { return emergency_; }

        [[nodiscard]] std::uint64_t BytesWritten() const noexcept { return bytesWritten_; }
        [[nodiscard]] bool HadWriteFailure() const noexcept { return writeFailed_; }

    private:
        static constexpr std::size_t kBufferCapacity = 64 * 1024;

        HANDLE file_ = INVALID_HANDLE_VALUE;
        HANDLE emergency_ = INVALID_HANDLE_VALUE;
        char buffer_[kBufferCapacity] = {};
        std::size_t bufferSize_ = 0;
        std::uint64_t bytesWritten_ = 0;
        bool writeFailed_ = false;
    };

    // OutputDebugStringW of one UTF-8 line. Stack buffers only, so it is also usable from the
    // emergency path.
    void WriteDebugger(std::string_view line) noexcept;

    // stdout when one is attached. A Windows-subsystem process normally has none; a redirected
    // launch (`> out.txt`, a pipe) does, and a console attached with AllocConsole does.
    class ConsoleSink
    {
    public:
        // Returns true when there is somewhere to write.
        bool Detect() noexcept;
        void Write(std::string_view line) noexcept;

    private:
        HANDLE handle_ = INVALID_HANDLE_VALUE;
        bool isConsole_ = false;
    };

    // Latest N records for the log viewer (L8). The writer thread is the only producer; readers
    // take a snapshot of records newer than their cursor. The cursor is the writer's running
    // count, so "newer than cursor" is exact regardless of producer sequence interleaving.
    class MemoryRing
    {
    public:
        constexpr MemoryRing() noexcept = default;
        ~MemoryRing();

        MemoryRing(const MemoryRing&) = delete;
        MemoryRing& operator=(const MemoryRing&) = delete;

        [[nodiscard]] bool Initialize(std::size_t capacity) noexcept;
        void Destroy() noexcept;
        [[nodiscard]] bool IsEnabled() const noexcept { return records_ != nullptr; }
        [[nodiscard]] std::size_t Capacity() const noexcept { return capacity_; }

        void Push(const LogRecord& record) noexcept;

        // Copies up to `maxCount` records with ring position >= `cursor` (oldest first). Records
        // that were overwritten since the cursor are skipped, i.e. the cursor is advanced to the
        // oldest retained one. `*newCursor` receives the cursor for the next call.
        std::size_t CopySince(
            std::uint64_t cursor,
            LogRecord* out,
            std::size_t maxCount,
            std::uint64_t* newCursor) noexcept;

        [[nodiscard]] std::uint64_t Written() const noexcept;

    private:
        SRWLOCK lock_ = SRWLOCK_INIT;
        LogRecord* records_ = nullptr;
        std::size_t capacity_ = 0;
        std::uint64_t written_ = 0;
    };

    // Session file name pieces.
    // "logs/session_YYYYMMDD_HHMMSS_<pid>_<tag>.log"; returns the length or 0 on failure.
    std::size_t BuildSessionPath(
        const SessionClock& clock, const char* buildTag, wchar_t* out, std::size_t capacity) noexcept;

    // Creates `logs` if needed. True when it exists afterwards.
    bool EnsureLogDirectory() noexcept;

    // Writes logs/latest.txt with the given path (UTF-8). Best effort; a hint, never a sink.
    void WriteLatestHint(const wchar_t* sessionPath) noexcept;
}
