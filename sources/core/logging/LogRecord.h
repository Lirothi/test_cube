#pragma once

#include "LogCategory.h"
#include "LogLevel.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace logging
{
    inline constexpr std::size_t kLogMessageCapacity = 1024;
    inline constexpr std::uint64_t kInvalidLogFrame = (std::numeric_limits<std::uint64_t>::max)();

    enum LogRecordFlags : std::uint8_t
    {
        LogRecordFlagNone = 0,
        LogRecordFlagTruncated = 1u << 0,
        LogRecordFlagEmergency = 1u << 1,
        LogRecordFlagFormatError = 1u << 2
    };

    struct LogRecord
    {
        std::int64_t qpcTimestamp = 0;
        std::uint64_t sequence = 0;
        std::uint64_t frame = kInvalidLogFrame;
        std::uint32_t processId = 0;
        std::uint32_t threadId = 0;
        LogLevel level = LogLevel::Info;
        LogCategory category = LogCategory::Core;
        std::uint8_t flags = LogRecordFlagNone;
        std::uint8_t reserved = 0;
        std::uint16_t messageByteCount = 0;
        const char* sourceFile = "";
        const char* sourceFunction = "";
        std::uint32_t sourceLine = 0;
        // Deliberately without an initializer: a default-initialised record ("LogRecord r;")
        // leaves the buffer indeterminate and the formatter terminates what it writes, so the
        // producer path never pays for zeroing 1 KiB. Only messageByteCount bytes are ever read.
        // "LogRecord r{};" still zeroes it for code that wants a blank record.
        char message[kLogMessageCapacity];
    };

    static_assert(std::is_trivially_copyable_v<LogRecord>);

    // Number of bytes of a record that carry information: everything up to and including the
    // message terminator. Copying only this many bytes keeps ring traffic proportional to the
    // message; the stale tail of a copied record is never read because every consumer honours
    // messageByteCount.
    [[nodiscard]] inline std::size_t UsedRecordBytes(const LogRecord& record) noexcept
    {
        const std::size_t messageBytes =
            record.messageByteCount < kLogMessageCapacity ? record.messageByteCount : kLogMessageCapacity - 1;
        return offsetof(LogRecord, message) + messageBytes + 1;
    }
}
