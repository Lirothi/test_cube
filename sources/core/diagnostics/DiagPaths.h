#pragma once

#include <filesystem>
#include <string>

// Every diagnostic file the engine writes goes under ONE directory instead of being scattered
// across the project root. There were ~20 of them at the root by the end of the barrier work and
// picking the real source files out of that listing had become genuinely annoying.
//
// Usage: `fopen_s(&f, diag::LogPath("scene_stress.log").c_str(), "w")`.
namespace diag {

// Directory for all engine diagnostic output, relative to the working directory.
inline const char* kLogDir = "logs";

// Returns "logs/<name>", creating the directory on first use. Falls back to the bare name if the
// directory cannot be created — a diagnostic must never be the reason a run fails.
inline std::string LogPath(const char* name)
{
    std::error_code ec;
    std::filesystem::create_directories(kLogDir, ec);
    if (ec) { return name; }
    return std::string(kLogDir) + "/" + name;
}

} // namespace diag
