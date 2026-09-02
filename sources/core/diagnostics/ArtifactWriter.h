#pragma once

#include <Windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Explicit diagnostic artifacts (logging plan L7). An artifact is a file under logs/ with a
// stable name and a format some human or script reads back: a stress verdict, a caps report, a
// DRED dump, a barrier census. It is NOT the event log — events go through core/logging; an
// artifact-producing subsystem writes ONE ordinary event naming the artifact (this module does
// that on the first open of each name per process, so call sites need not).
//
// Every call site declares the file's MODE, replacing the old "w" / "a" / static-first-line
// conventions that made "does this file carry last run's lines?" a per-file archaeology.
namespace diag {

enum class ArtifactMode : std::uint8_t {
    // First open in this process truncates; every later open in the same process appends. The
    // shape most per-run reports want (a rebuild summary per level, a dump per graph).
    PerRunTruncate,
    // History across sessions, explicitly (crash-class reports: invariant_failure.log,
    // device_removed.log, fence_stall.log, ...). The first open per process writes a
    // "---- session ... ----" separator so a line can be attributed to its run.
    Append,
    // logs/<stem>_<YYYYMMDD_HHMMSS>_<pid><ext>: two processes can never truncate each other.
    UniqueSession,
    // Written to <name>.tmp and renamed over <name> on Close: a reader never sees a partial
    // report. For one complete report written in one go (a cache summary, a readout table).
    AtomicReplace
};

[[nodiscard]] const char* ArtifactModeName(ArtifactMode mode) noexcept;

// One open artifact. Win32 handle: every Write reaches the OS immediately, so there is no CRT
// buffer to lose when the process ends through TerminateProcess and no per-line flush protocol.
// Not for hot paths — opening is a filesystem operation and each Write is a syscall. Never
// throws; a failed open leaves IsOpen() false and every write a no-op.
class ArtifactFile {
public:
    ArtifactFile() noexcept = default;
    ArtifactFile(const char* name, ArtifactMode mode) noexcept { Open(name, mode); }
    ~ArtifactFile() { Close(); }

    ArtifactFile(const ArtifactFile&) = delete;
    ArtifactFile& operator=(const ArtifactFile&) = delete;

    // Opens logs/<name> under `mode`, closing anything already open. False when the file could
    // not be created (reported once per name in the session log).
    bool Open(const char* name, ArtifactMode mode) noexcept;
    // Closes; for AtomicReplace this is the moment the report becomes visible.
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return IsOpen(); }

    void Write(std::string_view text) noexcept;
    // printf into a 4 KiB stack buffer, then Write; longer output is truncated. `%ls` accepted.
    void Printf(_Printf_format_string_ const char* format, ...) noexcept;
    void VPrintf(const char* format, va_list args) noexcept;

    // The path being written; for AtomicReplace the FINAL path, for UniqueSession the generated one.
    [[nodiscard]] const std::string& Path() const noexcept { return path_; }
    // For code that must write without the CRT or any lock (D3D message callbacks): WriteFile
    // on this handle appends atomically. Valid until Close().
    [[nodiscard]] HANDLE NativeHandle() const noexcept { return handle_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::string path_;
    std::string tempPath_;
    ArtifactMode mode_ = ArtifactMode::PerRunTruncate;
};

// One-shot: open under `mode`, write, close. True when the bytes reached the OS.
bool WriteArtifact(const char* name, ArtifactMode mode, std::string_view text) noexcept;
bool WriteArtifactf(const char* name, ArtifactMode mode, _Printf_format_string_ const char* format, ...) noexcept;

// Creates logs/ once per process (cached; safe from any thread). True when it exists.
bool EnsureLogDirectory() noexcept;

} // namespace diag
