#include "LogSinks.h"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <new>

namespace logging::sinks
{
    namespace
    {
        // Must match diag::kLogDir (DiagPaths.h). Spelled here so this TU does not pull in
        // <filesystem>; L7 replaces DiagPaths with the artifact API and unifies the two.
        constexpr const wchar_t* kLogDirectory = L"logs";

        constexpr std::string_view kLevelTags[kLogLevelCount] =
        {
            "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"
        };

        // Minimal bounded appender for the writer side (same contract as the producer-side
        // FixedBufferWriter: always terminated, never overruns, silently truncates).
        struct LineWriter
        {
            char* out;
            std::size_t capacity;
            std::size_t size = 0;

            LineWriter(char* destination, std::size_t cap) noexcept : out(destination), capacity(cap)
            {
                if (capacity != 0)
                {
                    out[0] = '\0';
                }
            }

            void Append(std::string_view text) noexcept
            {
                if (capacity == 0 || size >= capacity - 1)
                {
                    return;
                }
                const std::size_t available = capacity - 1 - size;
                const std::size_t count = text.size() < available ? text.size() : available;
                std::memcpy(out + size, text.data(), count);
                size += count;
                out[size] = '\0';
            }

            void Append(char value) noexcept
            {
                Append(std::string_view(&value, 1));
            }

            void AppendUnsigned(std::uint64_t value, int width = 0, char fill = ' ') noexcept
            {
                char digits[24];
                const auto result = std::to_chars(digits, digits + sizeof(digits), value);
                const int length = static_cast<int>(result.ptr - digits);
                for (int i = length; i < width; ++i)
                {
                    Append(fill);
                }
                Append(std::string_view(digits, static_cast<std::size_t>(length)));
            }
        };

        [[nodiscard]] std::string_view BaseName(const char* path) noexcept
        {
            if (path == nullptr)
            {
                return {};
            }
            const std::string_view full(path);
            const std::size_t slash = full.find_last_of("/\\");
            return slash == std::string_view::npos ? full : full.substr(slash + 1);
        }

        [[nodiscard]] bool LocalTimeAt(const SessionClock& clock, std::int64_t qpc, SYSTEMTIME& local) noexcept
        {
            const std::int64_t frequency = clock.qpcFrequency > 0 ? clock.qpcFrequency : 1;
            const std::int64_t delta = qpc >= clock.qpcStart ? qpc - clock.qpcStart : 0;
            // Split to keep the multiply inside 64 bits: a 10 MHz counter overflows the naive
            // form after ~10 days of uptime.
            const std::int64_t wholeSeconds = delta / frequency;
            const std::int64_t remainder = delta % frequency;
            const std::uint64_t hundredNanoseconds =
                static_cast<std::uint64_t>(wholeSeconds) * 10'000'000ull +
                static_cast<std::uint64_t>(remainder) * 10'000'000ull / static_cast<std::uint64_t>(frequency);
            const std::uint64_t fileTimeValue = clock.startFileTime + hundredNanoseconds;

            FILETIME fileTime{};
            fileTime.dwLowDateTime = static_cast<DWORD>(fileTimeValue & 0xFFFFFFFFull);
            fileTime.dwHighDateTime = static_cast<DWORD>(fileTimeValue >> 32);
            SYSTEMTIME utc{};
            if (!FileTimeToSystemTime(&fileTime, &utc))
            {
                return false;
            }
            if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
            {
                local = utc;
            }
            return true;
        }

        std::size_t Utf8ToWideStack(std::string_view text, wchar_t* out, std::size_t capacity) noexcept
        {
            if (capacity == 0)
            {
                return 0;
            }
            if (text.empty())
            {
                out[0] = L'\0';
                return 0;
            }
            int converted = MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out,
                static_cast<int>(capacity - 1));
            if (converted <= 0)
            {
                converted = MultiByteToWideChar(
                    CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out,
                    static_cast<int>(capacity - 1));
            }
            if (converted <= 0)
            {
                out[0] = L'\0';
                return 0;
            }
            out[converted] = L'\0';
            return static_cast<std::size_t>(converted);
        }
    }

    // ---- SessionClock -----------------------------------------------------------------------

    void SessionClock::Capture() noexcept
    {
        LARGE_INTEGER frequency{};
        qpcFrequency = QueryPerformanceFrequency(&frequency) != FALSE && frequency.QuadPart > 0
            ? frequency.QuadPart
            : 1;
        FILETIME fileTime{};
        GetSystemTimePreciseAsFileTime(&fileTime);
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        qpcStart = counter.QuadPart;
        startFileTime = (static_cast<std::uint64_t>(fileTime.dwHighDateTime) << 32) | fileTime.dwLowDateTime;
    }

    double SessionClock::ElapsedSeconds(std::int64_t qpc) const noexcept
    {
        const std::int64_t frequency = qpcFrequency > 0 ? qpcFrequency : 1;
        return static_cast<double>(qpc - qpcStart) / static_cast<double>(frequency);
    }

    std::size_t SessionClock::FormatWallClock(std::int64_t qpc, char* out, std::size_t capacity) const noexcept
    {
        SYSTEMTIME local{};
        if (capacity < 24 || !LocalTimeAt(*this, qpc, local))
        {
            if (capacity != 0)
            {
                out[0] = '\0';
            }
            return 0;
        }
        const int written = std::snprintf(
            out, capacity, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
            static_cast<unsigned>(local.wYear), static_cast<unsigned>(local.wMonth),
            static_cast<unsigned>(local.wDay), static_cast<unsigned>(local.wHour),
            static_cast<unsigned>(local.wMinute), static_cast<unsigned>(local.wSecond),
            static_cast<unsigned>(local.wMilliseconds));
        return written > 0 ? static_cast<std::size_t>(written) : 0;
    }

    // ---- ThreadNameTable --------------------------------------------------------------------

    void ThreadNameTable::Set(std::uint32_t threadId, std::string_view name) noexcept
    {
        AcquireSRWLockExclusive(&lock_);
        Entry* target = nullptr;
        for (std::size_t i = 0; i < count_; ++i)
        {
            if (entries_[i].threadId == threadId)
            {
                target = &entries_[i];
                break;
            }
        }
        if (target == nullptr && count_ < kCapacity)
        {
            target = &entries_[count_++];
            target->threadId = threadId;
        }
        if (target != nullptr)
        {
            const std::size_t count = name.size() < kNameCapacity - 1 ? name.size() : kNameCapacity - 1;
            std::memcpy(target->name, name.data(), count);
            target->name[count] = '\0';
        }
        ReleaseSRWLockExclusive(&lock_);
    }

    std::size_t ThreadNameTable::Get(std::uint32_t threadId, char* out, std::size_t capacity) noexcept
    {
        if (capacity == 0)
        {
            return 0;
        }
        out[0] = '\0';
        std::size_t length = 0;
        if (!TryAcquireSRWLockShared(&lock_))
        {
            return 0;
        }
        for (std::size_t i = 0; i < count_; ++i)
        {
            if (entries_[i].threadId == threadId)
            {
                length = std::strlen(entries_[i].name);
                if (length > capacity - 1)
                {
                    length = capacity - 1;
                }
                std::memcpy(out, entries_[i].name, length);
                out[length] = '\0';
                break;
            }
        }
        ReleaseSRWLockShared(&lock_);
        return length;
    }

    // ---- FormatLine ---------------------------------------------------------------------------

    std::size_t FormatLine(
        const LogRecord& record,
        const SessionClock& clock,
        ThreadNameTable& threadNames,
        char* out,
        std::size_t capacity) noexcept
    {
        LineWriter line(out, capacity);

        line.AppendUnsigned(record.sequence, 8, '0');
        line.Append(' ');

        char wallClock[32];
        const std::size_t wallClockLength = clock.FormatWallClock(record.qpcTimestamp, wallClock, sizeof(wallClock));
        line.Append(wallClockLength != 0 ? std::string_view(wallClock, wallClockLength) : std::string_view("----------- --:--:--.---"));
        line.Append(' ');

        {
            const double elapsed = clock.ElapsedSeconds(record.qpcTimestamp);
            const std::uint64_t milliseconds =
                elapsed > 0.0 ? static_cast<std::uint64_t>(elapsed * 1000.0 + 0.5) : 0;
            line.Append('+');
            line.AppendUnsigned(milliseconds / 1000);
            line.Append('.');
            line.AppendUnsigned(milliseconds % 1000, 3, '0');
            line.Append("s [");
        }

        line.Append(IsValid(record.level) ? kLevelTags[static_cast<std::size_t>(record.level)] : std::string_view("?????"));
        line.Append("] [");
        line.Append(LogCategoryName(record.category));
        line.Append("] [frame=");
        if (record.frame == kInvalidLogFrame)
        {
            line.Append('-');
        }
        else
        {
            line.AppendUnsigned(record.frame);
        }
        line.Append("] [tid=");
        line.AppendUnsigned(record.threadId);
        {
            char name[ThreadNameTable::kNameCapacity];
            const std::size_t nameLength = threadNames.Get(record.threadId, name, sizeof(name));
            if (nameLength != 0)
            {
                line.Append('/');
                line.Append(std::string_view(name, nameLength));
            }
        }
        line.Append("] ");

        {
            std::size_t messageBytes = record.messageByteCount;
            if (messageBytes >= kLogMessageCapacity)
            {
                messageBytes = kLogMessageCapacity - 1;
            }
            while (messageBytes != 0 &&
                   (record.message[messageBytes - 1] == '\n' || record.message[messageBytes - 1] == '\r'))
            {
                --messageBytes;
            }
            line.Append(std::string_view(record.message, messageBytes));
        }

        if ((record.flags & LogRecordFlagTruncated) != 0)
        {
            line.Append(" [truncated]");
        }
        if ((record.flags & LogRecordFlagFormatError) != 0)
        {
            line.Append(" [format-error]");
        }
        if ((record.flags & LogRecordFlagEmergency) != 0)
        {
            line.Append(" [emergency]");
        }
        if (record.level >= LogLevel::Warning)
        {
            const std::string_view file = BaseName(record.sourceFile);
            if (!file.empty())
            {
                line.Append(" (");
                line.Append(file);
                line.Append(':');
                line.AppendUnsigned(record.sourceLine);
                line.Append(')');
            }
        }
        line.Append('\n');
        return line.size;
    }

    // ---- FileSink -----------------------------------------------------------------------------

    FileSink::~FileSink()
    {
        Close();
    }

    bool FileSink::Open(const wchar_t* path) noexcept
    {
        Close();
        if (path == nullptr || path[0] == L'\0')
        {
            return false;
        }
        constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
        // Append-only access: WriteFile then lands at end-of-file atomically per call, which is
        // what lets the emergency handle below interleave whole lines with the buffered writer.
        file_ = CreateFileW(path, FILE_APPEND_DATA | SYNCHRONIZE, kShare, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        emergency_ = CreateFileW(path, FILE_APPEND_DATA | SYNCHRONIZE, kShare, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        bufferSize_ = 0;
        bytesWritten_ = 0;
        writeFailed_ = false;
        return true;
    }

    void FileSink::Close() noexcept
    {
        if (file_ != INVALID_HANDLE_VALUE)
        {
            Commit();
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
        if (emergency_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(emergency_);
            emergency_ = INVALID_HANDLE_VALUE;
        }
        bufferSize_ = 0;
    }

    void FileSink::Append(std::string_view text) noexcept
    {
        if (file_ == INVALID_HANDLE_VALUE || text.empty())
        {
            return;
        }
        if (text.size() > kBufferCapacity - bufferSize_)
        {
            Commit();
        }
        if (text.size() > kBufferCapacity)
        {
            DWORD written = 0;
            if (!WriteFile(file_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
            {
                writeFailed_ = true;
            }
            bytesWritten_ += written;
            return;
        }
        std::memcpy(buffer_ + bufferSize_, text.data(), text.size());
        bufferSize_ += text.size();
    }

    void FileSink::Commit() noexcept
    {
        if (file_ == INVALID_HANDLE_VALUE || bufferSize_ == 0)
        {
            return;
        }
        DWORD written = 0;
        if (!WriteFile(file_, buffer_, static_cast<DWORD>(bufferSize_), &written, nullptr))
        {
            writeFailed_ = true;
        }
        bytesWritten_ += written;
        bufferSize_ = 0;
    }

    void FileSink::Sync() noexcept
    {
        Commit();
        if (file_ != INVALID_HANDLE_VALUE)
        {
            // Best effort: the OS cache already survives a process crash, this only adds
            // protection against a kernel crash/power loss and may be refused on some handles.
            FlushFileBuffers(file_);
        }
    }

    // ---- Debugger / console -------------------------------------------------------------------

    void WriteDebugger(std::string_view line) noexcept
    {
        wchar_t wide[kLineCapacity];
        if (Utf8ToWideStack(line, wide, kLineCapacity) != 0)
        {
            OutputDebugStringW(wide);
        }
    }

    bool ConsoleSink::Detect() noexcept
    {
        handle_ = INVALID_HANDLE_VALUE;
        isConsole_ = false;
        const HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD mode = 0;
        if (GetConsoleMode(handle, &mode))
        {
            handle_ = handle;
            isConsole_ = true;
            return true;
        }
        const DWORD type = GetFileType(handle);
        if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE)
        {
            handle_ = handle;
            return true;
        }
        return false;
    }

    void ConsoleSink::Write(std::string_view line) noexcept
    {
        if (handle_ == INVALID_HANDLE_VALUE || line.empty())
        {
            return;
        }
        DWORD written = 0;
        if (isConsole_)
        {
            wchar_t wide[kLineCapacity];
            const std::size_t length = Utf8ToWideStack(line, wide, kLineCapacity);
            if (length != 0)
            {
                WriteConsoleW(handle_, wide, static_cast<DWORD>(length), &written, nullptr);
            }
            return;
        }
        WriteFile(handle_, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }

    // ---- MemoryRing ---------------------------------------------------------------------------

    MemoryRing::~MemoryRing()
    {
        Destroy();
    }

    bool MemoryRing::Initialize(std::size_t capacity) noexcept
    {
        Destroy();
        if (capacity == 0)
        {
            return true; // disabled by configuration
        }
        LogRecord* records = new (std::nothrow) LogRecord[capacity];
        if (records == nullptr)
        {
            return false;
        }
        AcquireSRWLockExclusive(&lock_);
        records_ = records;
        capacity_ = capacity;
        written_ = 0;
        ReleaseSRWLockExclusive(&lock_);
        return true;
    }

    void MemoryRing::Destroy() noexcept
    {
        AcquireSRWLockExclusive(&lock_);
        LogRecord* records = records_;
        records_ = nullptr;
        capacity_ = 0;
        written_ = 0;
        ReleaseSRWLockExclusive(&lock_);
        delete[] records;
    }

    void MemoryRing::Push(const LogRecord& record) noexcept
    {
        AcquireSRWLockExclusive(&lock_);
        if (records_ != nullptr)
        {
            std::memcpy(&records_[written_ % capacity_], &record, UsedRecordBytes(record));
            ++written_;
        }
        ReleaseSRWLockExclusive(&lock_);
    }

    std::size_t MemoryRing::CopySince(
        std::uint64_t cursor,
        LogRecord* out,
        std::size_t maxCount,
        std::uint64_t* newCursor) noexcept
    {
        std::size_t copied = 0;
        AcquireSRWLockShared(&lock_);
        if (records_ != nullptr)
        {
            const std::uint64_t oldest = written_ > capacity_ ? written_ - capacity_ : 0;
            std::uint64_t position = cursor > oldest ? cursor : oldest;
            while (position < written_ && copied < maxCount)
            {
                const LogRecord& source = records_[position % capacity_];
                std::memcpy(&out[copied], &source, UsedRecordBytes(source));
                ++copied;
                ++position;
            }
            if (newCursor != nullptr)
            {
                *newCursor = position;
            }
        }
        else if (newCursor != nullptr)
        {
            *newCursor = cursor;
        }
        ReleaseSRWLockShared(&lock_);
        return copied;
    }

    std::uint64_t MemoryRing::Written() const noexcept
    {
        // Relaxed read of a counter only the writer thread advances; callers use it as a hint.
        return written_;
    }

    // ---- Session file helpers -----------------------------------------------------------------

    std::size_t BuildSessionPath(
        const SessionClock& clock, const char* buildTag, wchar_t* out, std::size_t capacity) noexcept
    {
        SYSTEMTIME local{};
        if (capacity == 0)
        {
            return 0;
        }
        if (!LocalTimeAt(clock, clock.qpcStart, local))
        {
            GetLocalTime(&local);
        }
        const int written = _snwprintf_s(
            out, capacity, _TRUNCATE, L"%s/session_%04u%02u%02u_%02u%02u%02u_%u_%hs.log",
            kLogDirectory, static_cast<unsigned>(local.wYear), static_cast<unsigned>(local.wMonth),
            static_cast<unsigned>(local.wDay), static_cast<unsigned>(local.wHour),
            static_cast<unsigned>(local.wMinute), static_cast<unsigned>(local.wSecond),
            static_cast<unsigned>(GetCurrentProcessId()), buildTag != nullptr ? buildTag : "unknown");
        return written > 0 ? static_cast<std::size_t>(written) : 0;
    }

    bool EnsureLogDirectory() noexcept
    {
        if (CreateDirectoryW(kLogDirectory, nullptr))
        {
            return true;
        }
        const DWORD error = GetLastError();
        return error == ERROR_ALREADY_EXISTS;
    }

    void WriteLatestHint(const wchar_t* sessionPath) noexcept
    {
        if (sessionPath == nullptr || sessionPath[0] == L'\0')
        {
            return;
        }
        wchar_t hintPath[64];
        _snwprintf_s(hintPath, _TRUNCATE, L"%s/latest.txt", kLogDirectory);
        const HANDLE file = CreateFileW(hintPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return;
        }
        char utf8[1024];
        const int length = WideCharToMultiByte(CP_UTF8, 0, sessionPath, -1, utf8, sizeof(utf8) - 2, nullptr, nullptr);
        if (length > 1)
        {
            utf8[length - 1] = '\n';
            DWORD written = 0;
            WriteFile(file, utf8, static_cast<DWORD>(length), &written, nullptr);
        }
        CloseHandle(file);
    }
}
