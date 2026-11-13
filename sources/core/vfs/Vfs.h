#pragma once

// The one door every runtime read of game content goes through (docs/distribution_build_plan.md §2).
//
// A path is VIRTUAL when relative -- forward slashes from the content root, the way levels, mesh.json and materials
// already write them -- and goes to the mounts; an ABSOLUTE path (a drive or a UNC path) goes straight to the OS, so
// `--level=C:/x.json` and a scratch level keep working. The mounts (§2.3): the loose files under the content root --
// the process's current directory at the first call, the repo root in every dev build -- and .tcpak packages
// (core/vfs/Pak.h), highest priority first. `--vfs=loose` (the default) / `packages` / `both` (loose files over the
// packages) and `--pak-dir=<folder>` choose them before the first read (Configure / RunCommandLine).
//
// What WRITES does not come through here: a file the game writes is the user's, not content, and gets a real OS path
// from WritablePath. tools/check_file_io.py keeps direct reads out of runtime code.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <istream>
#include <memory>
#include <optional>
#include <streambuf>
#include <string>
#include <string_view>
#include <vector>

namespace vfs::pak
{
    class Package;
}

namespace vfs
{
    // A path as the caller has it -- UTF-8 (JSON, literals) or wide (Win32 code) -- normalised once: forward
    // slashes, no leading "./", no doubled slashes.
    class Path
    {
    public:
        Path(std::string_view utf8);
        Path(const char* utf8);
        Path(const std::string& utf8);
        Path(std::wstring_view wide);
        Path(const wchar_t* wide);
        Path(const std::wstring& wide);
        Path(const std::filesystem::path& path);

        [[nodiscard]] const std::string& Utf8() const noexcept { return text_; }
        [[nodiscard]] bool IsAbsolute() const noexcept;
        [[nodiscard]] bool Empty() const noexcept { return text_.empty(); }

    private:
        std::string text_;
    };

    struct Stat
    {
        std::uint64_t size = 0;
        // What a cache key may hold to notice a changed file. A loose file: its last write time as FILETIME ticks --
        // the very number std::filesystem::last_write_time gave the keys that hold it today (the water mask bake,
        // the physics shape cache), so their stored keys stay valid. A packaged file (D2): its content hash.
        std::uint64_t version = 0;
        bool directory = false;
    };

    [[nodiscard]] std::optional<Stat> GetStat(const Path& path);
    [[nodiscard]] bool Exists(const Path& path); // a file or a directory
    [[nodiscard]] bool IsFile(const Path& path);
    [[nodiscard]] bool IsDirectory(const Path& path);

    // The whole file. False (and `out` empty) when it is missing or cannot be read.
    bool ReadAll(const Path& path, std::vector<std::uint8_t>& out);
    bool ReadText(const Path& path, std::string& out); // the bytes as they are, no newline translation

    struct ListEntry
    {
        std::string name;  // the file or folder name
        std::string path;  // the directory's path joined with the name, in the caller's form
        bool directory = false;
    };
    // The entries of one directory (not recursive) whose names match `pattern` ("*", "*.json"), in the order the
    // file system gives them -- the order std::filesystem::directory_iterator gave the callers before.
    void List(const Path& directory, std::string_view pattern, const std::function<void(const ListEntry&)>& visit);

    // A file kept open for positional reads. Read is safe from any number of threads at once: every call names its
    // offset, nothing shares a file position (the texture streaming thread, the collision reader).
    class File
    {
    public:
        File() = default;
        ~File();
        File(File&& other) noexcept;
        File& operator=(File&& other) noexcept;
        File(const File&) = delete;
        File& operator=(const File&) = delete;

        [[nodiscard]] bool IsOpen() const noexcept { return handle_ != nullptr || package_ != nullptr; }
        explicit operator bool() const noexcept { return IsOpen(); }
        [[nodiscard]] std::uint64_t Size() const noexcept { return size_; }
        // Exactly `bytes` from `offset`, or false.
        bool Read(std::uint64_t offset, void* destination, std::size_t bytes) const;

        // A loose file's OS handle, or a package's entry.
        static File FromHandle(void* handle, std::uint64_t size);
        static File FromPackage(const pak::Package& package, int entry, std::uint64_t size);

    private:
        void* handle_ = nullptr;
        const pak::Package* package_ = nullptr;
        int entry_ = -1;
        std::uint64_t size_ = 0;
    };
    [[nodiscard]] File Open(const Path& path);

    // A std::istream over a content file, for the readers written against std::ifstream: read, seekg, tellg,
    // getline, `>> json`, istreambuf_iterator all work. Binary reads the file through a 64 KB window and seeks
    // without reading what it skips. Text mode (binary = false) reads the file whole and turns CRLF into LF, as an
    // ifstream opened without std::ios::binary did.
    class InStream : public std::istream
    {
    public:
        explicit InStream(const Path& path, bool binary = true);
        explicit InStream(File file); // binary, over a file already open (a package entry in the self-test)
        ~InStream() override;
        [[nodiscard]] bool is_open() const noexcept { return open_; }

    private:
        std::unique_ptr<std::streambuf> buffer_;
        bool open_ = false;
    };

    // Where a file the game writes lives. Dev builds: the content root, as before (graphics_settings.json,
    // data/bot_memory/...). Shipping (D6) maps each kind to its folder under %LOCALAPPDATA%.
    enum class Writable
    {
        Settings, // graphics_settings.json, imgui.ini
        Saved,    // what the game learns and keeps: data/bot_memory/**
        Cache,    // shader and PSO caches
    };
    [[nodiscard]] std::filesystem::path WritablePath(Writable kind, std::string_view relative);

    // The loose mount's folder (the current directory at the first call) and where a content path lies on disk,
    // for code that must hand an OS path to a writer or a tool beside it. Not for reading: use the calls above.
    [[nodiscard]] const std::filesystem::path& ContentRoot();
    [[nodiscard]] std::filesystem::path OsPath(const Path& path);

    // UTF-8 <-> UTF-16 for the Win32 / WIC code that keeps wide strings -- not std::wstring(s.begin(), s.end()),
    // which widens bytes and breaks every non-ASCII path.
    [[nodiscard]] std::wstring Widen(std::string_view utf8);
    [[nodiscard]] std::string Narrow(std::wstring_view wide);

    struct Counters
    {
        std::uint64_t opens = 0;          // files opened (ReadAll, Open, InStream)
        std::uint64_t packageOpens = 0;   // ...of them, served by a package
        std::uint64_t misses = 0;         // opens and stats of a path that is not there
        std::uint64_t stats = 0;          // GetStat / Exists / IsFile / IsDirectory
        std::uint64_t lists = 0;
        std::uint64_t reads = 0;          // read calls that reached a file or a package
        std::uint64_t bytesRead = 0;      // bytes handed out (a package's: decoded)
        std::uint64_t blocksDecoded = 0;  // package blocks decompressed (a cache hit is not counted)
        std::uint64_t damagedBlocks = 0;  // package blocks whose CRC or decode failed
    };
    [[nodiscard]] Counters GetCounters();

    struct Config
    {
        enum class Mode
        {
            Loose,     // the loose files only (every dev build's default)
            Packages,  // the packages only: a dev build reads what a shipped one would
            Both,      // loose files over the packages
        };
        Mode mode = Mode::Loose;
        std::filesystem::path packageDir;   // every *.tcpak in it is mounted
        std::filesystem::path recordFile;   // --vfs-record: each content file found, once, as it is first found
    };
    // Before the first read. False (and why) when a package does not open; nothing is mounted then.
    bool Configure(const Config& config, std::string& error);
    // "loose" and "<name> (<file>, N files)" for each package, in lookup order.
    [[nodiscard]] std::vector<std::string> DescribeMounts();
    // Whether a loose file (or an absolute path) serves this path -- not a package. What writes a derived file beside
    // its source (a water mask bake) does so only then.
    [[nodiscard]] bool IsLooseFile(const Path& path);

    // The command line: --vfs / --pak-dir / --vfs-record configure the mounts; --pak-build / --pak-list / --pak-verify /
    // --vfs-selftest run a tool (no window). The tool's exit code, or -1 when the command line names none.
    int RunCommandLine(const char* commandLine);
}
