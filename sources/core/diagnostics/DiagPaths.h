#pragma once

#include <string>

// Every diagnostic file the engine writes goes under ONE directory instead of being scattered
// across the project root. There were ~20 of them at the root by the end of the barrier work and
// picking the real source files out of that listing had become genuinely annoying.
//
// Prefer diag::ArtifactFile / diag::WriteArtifact (ArtifactWriter.h): they declare the file's
// mode and go straight to the OS. LogPath stays for the harnesses that own their own FILE*
// (benchmark CSVs, the importer's log) and only returns the path.
namespace diag {

// Directory for all engine diagnostic output, relative to the working directory.
inline const char* kLogDir = "logs";

// Defined in ArtifactWriter.cpp: creates logs/ ONCE per process (cached), safe from any thread.
bool EnsureLogDirectory() noexcept;

// Returns "logs/<name>". Falls back to the bare name if the directory cannot be created — a
// diagnostic must never be the reason a run fails.
inline std::string LogPath(const char* name)
{
    if (!EnsureLogDirectory()) { return name; }
    return std::string(kLogDir) + "/" + name;
}

} // namespace diag
