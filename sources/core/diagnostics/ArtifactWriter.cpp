#include "core/diagnostics/ArtifactWriter.h"

#include "core/diagnostics/DiagPaths.h"
#include "core/logging/Log.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace diag {

namespace {

// Per-process bookkeeping: which names have been opened (PerRunTruncate's "first open
// truncates", Append's session separator, and the one session-log event per name). Heap and a
// mutex are fine here — this is the explicit-diagnostics path, never a frame or a callback.
std::mutex g_registryMutex;
std::unordered_set<std::string> g_opened;
std::unordered_set<std::string> g_failed;

#if !defined(NDEBUG)
constexpr const char* kBuildTag = "debug";
#elif defined(WITH_EDITOR) && WITH_EDITOR
constexpr const char* kBuildTag = "release_editor";
#else
constexpr const char* kBuildTag = "release";
#endif

// True on the FIRST call for `name` in this process.
bool NoteFirstOpen(const std::string& name)
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    return g_opened.insert(name).second;
}

bool NoteFirstFailure(const std::string& name)
{
    std::lock_guard<std::mutex> lock(g_registryMutex);
    return g_failed.insert(name).second;
}

void SessionStamp(char* out, std::size_t capacity, bool forFileName)
{
    SYSTEMTIME local{};
    GetLocalTime(&local);
    std::snprintf(out, capacity,
                  forFileName ? "%04u%02u%02u_%02u%02u%02u_%lu" : "%04u-%02u-%02u %02u:%02u:%02u pid %lu",
                  static_cast<unsigned>(local.wYear), static_cast<unsigned>(local.wMonth),
                  static_cast<unsigned>(local.wDay), static_cast<unsigned>(local.wHour),
                  static_cast<unsigned>(local.wMinute), static_cast<unsigned>(local.wSecond),
                  static_cast<unsigned long>(GetCurrentProcessId()));
}

HANDLE OpenHandle(const std::string& path, bool truncate, bool appendOnly)
{
    // Append-only access makes every WriteFile land at end-of-file atomically, which is what
    // lets a second handle (a D3D callback's) interleave whole lines with this one.
    const DWORD access = appendOnly ? (FILE_APPEND_DATA | SYNCHRONIZE) : GENERIC_WRITE;
    const DWORD disposition = truncate ? CREATE_ALWAYS : OPEN_ALWAYS;
    return CreateFileA(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

const char* ArtifactModeName(ArtifactMode mode) noexcept
{
    switch (mode) {
    case ArtifactMode::PerRunTruncate: return "per-run";
    case ArtifactMode::Append:         return "append";
    case ArtifactMode::UniqueSession:  return "unique";
    case ArtifactMode::AtomicReplace:  return "atomic";
    }
    return "?";
}

bool EnsureLogDirectory() noexcept
{
    // Function-local static: computed once, thread-safe, and never again per call — the old
    // LogPath ran create_directories on EVERY invocation, including from D3D callbacks.
    static const bool exists = []() {
        if (CreateDirectoryA(kLogDir, nullptr)) { return true; }
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }();
    return exists;
}

bool ArtifactFile::Open(const char* name, ArtifactMode mode) noexcept
{
    Close();
    if (name == nullptr || name[0] == '\0') { return false; }
    mode_ = mode;
    EnsureLogDirectory();

    std::string finalPath = std::string(kLogDir) + "/" + name;
    if (mode == ArtifactMode::UniqueSession) {
        char stamp[64];
        SessionStamp(stamp, sizeof(stamp), /*forFileName=*/true);
        const std::string base(name);
        const std::size_t dot = base.find_last_of('.');
        const std::string stem = dot == std::string::npos ? base : base.substr(0, dot);
        const std::string ext = dot == std::string::npos ? std::string() : base.substr(dot);
        finalPath = std::string(kLogDir) + "/" + stem + "_" + stamp + ext;
    }

    const bool first = NoteFirstOpen(name);
    switch (mode) {
    case ArtifactMode::PerRunTruncate:
        handle_ = OpenHandle(finalPath, /*truncate=*/first, /*appendOnly=*/!first);
        break;
    case ArtifactMode::Append:
        handle_ = OpenHandle(finalPath, /*truncate=*/false, /*appendOnly=*/true);
        break;
    case ArtifactMode::UniqueSession: {
        // CREATE_NEW, never truncate: the stamp has one-second resolution, so a second open of
        // the same name within the second (same process, or a child spawned in that second
        // that happens to reuse a pid) gets a numbered sibling instead of clobbering the first.
        const std::string base = finalPath;
        const std::size_t dot = base.find_last_of('.');
        for (int attempt = 0; attempt < 100; ++attempt) {
            if (attempt != 0) {
                const std::string suffix = "_" + std::to_string(attempt + 1);
                finalPath = dot == std::string::npos ? base + suffix
                                                     : base.substr(0, dot) + suffix + base.substr(dot);
            }
            handle_ = CreateFileA(finalPath.c_str(), GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) { break; }
        }
        break;
    }
    case ArtifactMode::AtomicReplace:
        tempPath_ = finalPath + ".tmp";
        handle_ = OpenHandle(tempPath_, /*truncate=*/true, /*appendOnly=*/false);
        break;
    }
    path_ = finalPath;

    if (handle_ == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (NoteFirstFailure(name)) {
            LOG_ERROR(logging::LogCategory::Core, "artifact {} could not be opened ({} mode, error {})",
                      finalPath, ArtifactModeName(mode), static_cast<unsigned>(error));
        }
        return false;
    }

    if (first) {
        LOG_INFO(logging::LogCategory::Core, "artifact {} ({})", finalPath, ArtifactModeName(mode));
        if (mode == ArtifactMode::Append) {
            char stamp[64];
            SessionStamp(stamp, sizeof(stamp), /*forFileName=*/false);
            Printf("---- session %s (%s) ----\n", stamp, kBuildTag);
        }
    }
    return true;
}

void ArtifactFile::Close() noexcept
{
    if (handle_ == INVALID_HANDLE_VALUE) { return; }
    CloseHandle(handle_);
    handle_ = INVALID_HANDLE_VALUE;
    if (mode_ == ArtifactMode::AtomicReplace && !tempPath_.empty()) {
        if (!MoveFileExA(tempPath_.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            LOG_ERROR(logging::LogCategory::Core, "artifact {}: replace from {} failed (error {})",
                      path_, tempPath_, static_cast<unsigned>(GetLastError()));
        }
        tempPath_.clear();
    }
}

void ArtifactFile::Write(std::string_view text) noexcept
{
    if (handle_ == INVALID_HANDLE_VALUE || text.empty()) { return; }
    DWORD written = 0;
    WriteFile(handle_, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

void ArtifactFile::Printf(const char* format, ...) noexcept
{
    va_list args;
    va_start(args, format);
    VPrintf(format, args);
    va_end(args);
}

void ArtifactFile::VPrintf(const char* format, va_list args) noexcept
{
    if (handle_ == INVALID_HANDLE_VALUE || format == nullptr) { return; }
    char buffer[4096];
    const int length = std::vsnprintf(buffer, sizeof(buffer), format, args);
    if (length <= 0) { return; }
    const std::size_t count = static_cast<std::size_t>(length) < sizeof(buffer) - 1
        ? static_cast<std::size_t>(length)
        : sizeof(buffer) - 1;
    Write(std::string_view(buffer, count));
}

bool WriteArtifact(const char* name, ArtifactMode mode, std::string_view text) noexcept
{
    ArtifactFile file(name, mode);
    if (!file) { return false; }
    file.Write(text);
    return true;
}

bool WriteArtifactf(const char* name, ArtifactMode mode, const char* format, ...) noexcept
{
    ArtifactFile file(name, mode);
    if (!file) { return false; }
    va_list args;
    va_start(args, format);
    file.VPrintf(format, args);
    va_end(args);
    return true;
}

} // namespace diag
