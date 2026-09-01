#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace logging
{
    enum class LogCategory : std::uint8_t
    {
        Core,
        App,
        Scene,
        Asset,
        Editor,
        Task,
        Render,
        RenderRhi,
        RenderGraph,
        RenderShadow,
        RenderRt,
        RenderValidation,
        Vfx,
        Ocean,
        Profiling,
        Count
    };

    inline constexpr std::size_t kLogCategoryCount = static_cast<std::size_t>(LogCategory::Count);

    [[nodiscard]] constexpr bool IsValid(LogCategory category) noexcept
    {
        return static_cast<std::size_t>(category) < kLogCategoryCount;
    }

    [[nodiscard]] constexpr std::string_view LogCategoryName(LogCategory category) noexcept
    {
        constexpr std::string_view names[kLogCategoryCount] =
        {
            "core",
            "app",
            "scene",
            "asset",
            "editor",
            "task",
            "render",
            "render.rhi",
            "render.graph",
            "render.shadow",
            "render.rt",
            "render.validation",
            "vfx",
            "ocean",
            "profiling"
        };
        return IsValid(category) ? names[static_cast<std::size_t>(category)] : "invalid";
    }
}
