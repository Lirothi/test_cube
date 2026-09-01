#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace logging
{
    enum class LogLevel : std::uint8_t
    {
        Trace,
        Debug,
        Info,
        Warning,
        Error,
        Fatal
    };

    inline constexpr std::size_t kLogLevelCount = 6;

    [[nodiscard]] constexpr bool IsValid(LogLevel level) noexcept
    {
        return static_cast<std::size_t>(level) < kLogLevelCount;
    }

    [[nodiscard]] constexpr std::string_view LogLevelName(LogLevel level) noexcept
    {
        constexpr std::string_view names[kLogLevelCount] =
        {
            "trace", "debug", "info", "warning", "error", "fatal"
        };
        return IsValid(level) ? names[static_cast<std::size_t>(level)] : "invalid";
    }
}
