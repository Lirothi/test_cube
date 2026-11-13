#include "core/vfs/Vfs.h"

#include "core/vfs/Pak.h"
#include "core/vfs/VfsMounts.h"
#include "core/logging/Log.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <system_error>
#include <unordered_set>

namespace vfs
{
    namespace
    {
        std::atomic<std::uint64_t> g_opens{ 0 };
        std::atomic<std::uint64_t> g_misses{ 0 };
        std::atomic<std::uint64_t> g_stats{ 0 };
        std::atomic<std::uint64_t> g_lists{ 0 };
        std::atomic<std::uint64_t> g_reads{ 0 };
        std::atomic<std::uint64_t> g_bytesRead{ 0 };
        std::atomic<std::uint64_t> g_packageOpens{ 0 };
        std::atomic<std::uint64_t> g_blocksDecoded{ 0 };
        std::atomic<std::uint64_t> g_damagedBlocks{ 0 };

        std::string ToUtf8(std::wstring_view wide)
        {
            if (wide.empty()) { return {}; }
            const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                                    nullptr, 0, nullptr, nullptr);
            std::string out(static_cast<std::size_t>(std::max(bytes, 0)), '\0');
            if (bytes > 0)
            {
                ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), bytes,
                                      nullptr, nullptr);
            }
            return out;
        }

        std::wstring ToWide(std::string_view utf8)
        {
            if (utf8.empty()) { return {}; }
            const int chars = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            std::wstring out(static_cast<std::size_t>(std::max(chars, 0)), L'\0');
            if (chars > 0)
            {
                ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), chars);
            }
            return out;
        }

        std::string Normalise(std::string text)
        {
            std::replace(text.begin(), text.end(), '\\', '/');
            const bool unc = text.size() >= 2 && text[0] == '/' && text[1] == '/';
            std::string out;
            out.reserve(text.size());
            for (std::size_t i = 0; i < text.size(); ++i)
            {
                const char c = text[i];
                // Doubled slashes collapse -- except the two that open a UNC path.
                if (c == '/' && !out.empty() && out.back() == '/' && !(unc && i == 1)) { continue; }
                out.push_back(c);
            }
            while (out.size() >= 2 && out[0] == '.' && out[1] == '/') { out.erase(0, 2); }
            return out;
        }

        // Case-insensitive match of a file name against "*" / "?" patterns, as FindFirstFile means them but without
        // its 8.3 quirk ("*.htm" matching "x.html").
        bool Matches(std::string_view name, std::string_view pattern)
        {
            std::size_t n = 0, p = 0, starP = std::string_view::npos, starN = 0;
            const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
            while (n < name.size())
            {
                if (p < pattern.size() && (pattern[p] == '?' || lower(pattern[p]) == lower(name[n])))
                {
                    ++n;
                    ++p;
                }
                else if (p < pattern.size() && pattern[p] == '*')
                {
                    starP = p++;
                    starN = n;
                }
                else if (starP != std::string_view::npos)
                {
                    p = starP + 1;
                    n = ++starN;
                }
                else
                {
                    return false;
                }
            }
            while (p < pattern.size() && pattern[p] == '*') { ++p; }
            return p == pattern.size();
        }

        std::wstring OsWide(const Path& path)
        {
            if (path.IsAbsolute()) { return ToWide(path.Utf8()); }
            return (ContentRoot() / ToWide(path.Utf8())).wstring();
        }

        // Every content handle lets others read, write, rename and delete the file under it: the editor rewrites a
        // shader or a bake while the game holds it, and a tmp + rename over an open file needs FILE_SHARE_DELETE.
        HANDLE OpenForRead(const std::wstring& os, DWORD flags)
        {
            return ::CreateFileW(os.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | flags, nullptr);
        }

        bool ReadAt(HANDLE handle, std::uint64_t offset, void* destination, std::size_t bytes)
        {
            auto* out = static_cast<std::uint8_t*>(destination);
            std::size_t done = 0;
            while (done < bytes)
            {
                const DWORD want = static_cast<DWORD>(std::min<std::size_t>(bytes - done, std::size_t{ 64 } << 20));
                OVERLAPPED at{};
                const std::uint64_t position = offset + done;
                at.Offset = static_cast<DWORD>(position & 0xFFFFFFFFull);
                at.OffsetHigh = static_cast<DWORD>(position >> 32);
                DWORD got = 0;
                if (!::ReadFile(handle, out + done, want, &got, &at) || got == 0) { return false; }
                done += got;
            }
            g_reads.fetch_add(1, std::memory_order_relaxed);
            g_bytesRead.fetch_add(bytes, std::memory_order_relaxed);
            return true;
        }

        // The window a binary InStream reads through: seeks move the position, only what is read is read.
        class FileBuffer final : public std::streambuf
        {
        public:
            explicit FileBuffer(File file) : file_(std::move(file)), window_(kWindow) {}

        protected:
            int_type underflow() override
            {
                if (gptr() < egptr()) { return traits_type::to_int_type(*gptr()); }
                const std::uint64_t next = windowStart_ + static_cast<std::uint64_t>(egptr() - eback());
                return Fill(next) ? traits_type::to_int_type(*gptr()) : traits_type::eof();
            }

            std::streamsize xsgetn(char* destination, std::streamsize count) override
            {
                std::streamsize done = 0;
                const std::streamsize buffered = std::min<std::streamsize>(egptr() - gptr(), count);
                if (buffered > 0)
                {
                    std::memcpy(destination, gptr(), static_cast<std::size_t>(buffered));
                    gbump(static_cast<int>(buffered));
                    done = buffered;
                }
                if (done < count && count - done >= static_cast<std::streamsize>(kWindow))
                {
                    // A large read goes straight into the caller's memory, not through the window.
                    const std::uint64_t at = Position();
                    const std::uint64_t left = at < file_.Size() ? file_.Size() - at : 0;
                    const std::uint64_t take = std::min<std::uint64_t>(left, static_cast<std::uint64_t>(count - done));
                    if (take > 0 && file_.Read(at, destination + done, static_cast<std::size_t>(take)))
                    {
                        done += static_cast<std::streamsize>(take);
                        EmptyAt(at + take);
                    }
                    else
                    {
                        EmptyAt(at);
                    }
                    return done;
                }
                while (done < count && underflow() != traits_type::eof())
                {
                    const std::streamsize take = std::min<std::streamsize>(egptr() - gptr(), count - done);
                    std::memcpy(destination + done, gptr(), static_cast<std::size_t>(take));
                    gbump(static_cast<int>(take));
                    done += take;
                }
                return done;
            }

            pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which) override
            {
                if ((which & std::ios_base::in) == 0) { return pos_type(off_type(-1)); }
                const std::int64_t base = direction == std::ios_base::beg ? 0
                    : direction == std::ios_base::cur ? static_cast<std::int64_t>(Position())
                    : static_cast<std::int64_t>(file_.Size());
                const std::int64_t target = base + static_cast<std::int64_t>(offset);
                if (target < 0 || static_cast<std::uint64_t>(target) > file_.Size()) { return pos_type(off_type(-1)); }
                return seekpos(pos_type(off_type(target)), which);
            }

            pos_type seekpos(pos_type position, std::ios_base::openmode which) override
            {
                if ((which & std::ios_base::in) == 0) { return pos_type(off_type(-1)); }
                const std::uint64_t target = static_cast<std::uint64_t>(off_type(position));
                if (target > file_.Size()) { return pos_type(off_type(-1)); }
                const std::uint64_t windowBytes = static_cast<std::uint64_t>(egptr() - eback());
                if (eback() != nullptr && target >= windowStart_ && target <= windowStart_ + windowBytes)
                {
                    setg(eback(), eback() + (target - windowStart_), egptr());
                }
                else
                {
                    EmptyAt(target);
                }
                return position;
            }

            std::streamsize showmanyc() override
            {
                const std::uint64_t at = Position();
                return at < file_.Size() ? static_cast<std::streamsize>(file_.Size() - at) : -1;
            }

        private:
            static constexpr std::size_t kWindow = 64 * 1024;

            std::uint64_t Position() const
            {
                return windowStart_ + static_cast<std::uint64_t>(gptr() - eback());
            }

            void EmptyAt(std::uint64_t position)
            {
                windowStart_ = position;
                setg(nullptr, nullptr, nullptr);
            }

            bool Fill(std::uint64_t position)
            {
                if (position >= file_.Size())
                {
                    EmptyAt(position);
                    return false;
                }
                const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(kWindow, file_.Size() - position));
                if (!file_.Read(position, window_.data(), take))
                {
                    EmptyAt(position);
                    return false;
                }
                windowStart_ = position;
                setg(window_.data(), window_.data(), window_.data() + take);
                return true;
            }

            File file_;
            std::vector<char> window_;
            std::uint64_t windowStart_ = 0;
        };

        // A text InStream: the whole file, CRLF turned into LF as a text-mode ifstream did.
        class TextBuffer final : public std::streambuf
        {
        public:
            explicit TextBuffer(std::string text) : text_(std::move(text))
            {
                setg(text_.data(), text_.data(), text_.data() + text_.size());
            }

        protected:
            pos_type seekoff(off_type offset, std::ios_base::seekdir direction, std::ios_base::openmode which) override
            {
                if ((which & std::ios_base::in) == 0) { return pos_type(off_type(-1)); }
                const off_type base = direction == std::ios_base::beg ? 0
                    : direction == std::ios_base::cur ? static_cast<off_type>(gptr() - eback())
                    : static_cast<off_type>(text_.size());
                return seekpos(pos_type(base + offset), which);
            }

            pos_type seekpos(pos_type position, std::ios_base::openmode which) override
            {
                const off_type target = off_type(position);
                if ((which & std::ios_base::in) == 0 || target < 0 || target > static_cast<off_type>(text_.size()))
                {
                    return pos_type(off_type(-1));
                }
                setg(eback(), eback() + target, egptr());
                return position;
            }

        private:
            std::string text_;
        };
    }

    Path::Path(std::string_view utf8) : text_(Normalise(std::string(utf8))) {}
    Path::Path(const char* utf8) : text_(Normalise(utf8 ? std::string(utf8) : std::string())) {}
    Path::Path(const std::string& utf8) : text_(Normalise(utf8)) {}
    Path::Path(std::wstring_view wide) : text_(Normalise(ToUtf8(wide))) {}
    Path::Path(const wchar_t* wide) : text_(Normalise(wide ? ToUtf8(wide) : std::string())) {}
    Path::Path(const std::wstring& wide) : text_(Normalise(ToUtf8(wide))) {}
    Path::Path(const std::filesystem::path& path) : text_(Normalise(ToUtf8(path.native()))) {}

    bool Path::IsAbsolute() const noexcept
    {
        return (text_.size() >= 2 && text_[1] == ':') || (!text_.empty() && text_[0] == '/');
    }

    const std::filesystem::path& ContentRoot()
    {
        static const std::filesystem::path root = []
        {
            std::error_code error;
            std::filesystem::path current = std::filesystem::current_path(error);
            return error ? std::filesystem::path(L".") : current;
        }();
        return root;
    }

    std::filesystem::path OsPath(const Path& path)
    {
        return std::filesystem::path(OsWide(path));
    }

    namespace
    {
        // A loose file's stat by its OS path (no mount logic): what GetStat answered in D1.
        std::optional<Stat> OsStat(const std::wstring& os)
        {
            WIN32_FILE_ATTRIBUTE_DATA data{};
            if (!::GetFileAttributesExW(os.c_str(), GetFileExInfoStandard, &data)) { return std::nullopt; }
            Stat stat;
            stat.directory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && !stat.directory)
            {
                // A link's own attributes are not its target's: ask the file it leads to, as std::filesystem did.
                const HANDLE handle = OpenForRead(os, 0);
                BY_HANDLE_FILE_INFORMATION info{};
                const bool ok = handle != INVALID_HANDLE_VALUE && ::GetFileInformationByHandle(handle, &info);
                if (handle != INVALID_HANDLE_VALUE) { ::CloseHandle(handle); }
                if (ok)
                {
                    data.nFileSizeHigh = info.nFileSizeHigh;
                    data.nFileSizeLow = info.nFileSizeLow;
                    data.ftLastWriteTime = info.ftLastWriteTime;
                }
            }
            stat.size = stat.directory ? 0 : (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            stat.version = (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                           data.ftLastWriteTime.dwLowDateTime;
            return stat;
        }

        File OsOpen(const std::wstring& os)
        {
            const HANDLE handle = OpenForRead(os, 0);
            if (handle == INVALID_HANDLE_VALUE) { return File(); }
            LARGE_INTEGER size{};
            if (!::GetFileSizeEx(handle, &size))
            {
                ::CloseHandle(handle);
                return File();
            }
            return File::FromHandle(handle, static_cast<std::uint64_t>(size.QuadPart));
        }

        // --vfs-record: every content file found, once, the moment it is first found (a run that is killed keeps it).
        struct Recorder
        {
            std::mutex mutex;
            std::unordered_set<std::string> seen;
            std::FILE* file = nullptr;
        };
        Recorder& TheRecorder()
        {
            static Recorder recorder;
            return recorder;
        }
        std::atomic<bool> g_recording{ false };

        // A content path not found, once a path (Debug): what a package lacks shows here (--log-level=debug).
        void NoteMiss(const Path& path)
        {
            g_misses.fetch_add(1, std::memory_order_relaxed);
            if (!logging::ShouldLog(logging::LogLevel::Debug, logging::LogCategory::Vfs)) { return; }
            static std::mutex mutex;
            static std::unordered_set<std::string> seen;
            std::lock_guard<std::mutex> lock(mutex);
            if (seen.size() < 4096 && seen.insert(path.Utf8()).second)
            {
                LOG_DEBUG(logging::LogCategory::Vfs, "vfs: not found: {}", path.Utf8());
            }
        }

        void Record(const Path& path)
        {
            if (!g_recording.load(std::memory_order_relaxed) || path.IsAbsolute()) { return; }
            Recorder& r = TheRecorder();
            std::lock_guard<std::mutex> lock(r.mutex);
            if (r.file && r.seen.insert(pak::FoldKey(path.Utf8())).second)
            {
                std::fprintf(r.file, "%s\n", path.Utf8().c_str());
                std::fflush(r.file);
            }
        }

        // The order a path is looked for in: the loose files first (or last), the packages by priority.
        template <typename LooseFn, typename PackageFn>
        bool Visit(const detail::Mounts& mounts, LooseFn&& loose, PackageFn&& package)
        {
            if (mounts.loose && mounts.looseFirst && loose()) { return true; }
            for (const std::unique_ptr<pak::Package>& p : mounts.packages)
            {
                if (package(*p)) { return true; }
            }
            if (mounts.loose && !mounts.looseFirst && loose()) { return true; }
            return false;
        }
    }

    namespace detail
    {
        Mounts& Global()
        {
            static Mounts mounts;
            return mounts;
        }

        pak::KeyRing& Keys()
        {
            static pak::KeyRing keys = []
            {
                pak::KeyRing ring;
                if (const std::optional<pak::Key> embedded = pak::EmbeddedKey()) { ring.Add(*embedded); }
                return ring;
            }();
            return keys;
        }

        void SortPackages(std::vector<std::unique_ptr<pak::Package>>& packages)
        {
            std::stable_sort(packages.begin(), packages.end(), [](const auto& a, const auto& b)
            {
                if (a->Priority() != b->Priority()) { return a->Priority() > b->Priority(); }
                return a->Name() < b->Name();
            });
        }

        std::optional<Stat> StatIn(const Mounts& mounts, const Path& path)
        {
            g_stats.fetch_add(1, std::memory_order_relaxed);
            if (path.Empty()) { return std::nullopt; }
            if (path.IsAbsolute())
            {
                std::optional<Stat> stat = OsStat(ToWide(path.Utf8()));
                if (!stat) { g_misses.fetch_add(1, std::memory_order_relaxed); }
                return stat;
            }
            std::optional<Stat> found;
            bool hidden = false; // a patch's "deleted" record: the packages below it are not asked
            Visit(mounts,
                [&]
                {
                    found = OsStat(OsWide(path));
                    return found.has_value();
                },
                [&](const pak::Package& p)
                {
                    if (hidden) { return false; }
                    const int entry = p.Find(path.Utf8());
                    if (entry >= 0)
                    {
                        if (p.Deleted(entry))
                        {
                            hidden = true;
                            return false;
                        }
                        const pak::Package::EntryInfo info = p.Entry(entry);
                        found = Stat{ info.size, info.contentHash, false };
                        return true;
                    }
                    if (p.HasDirectory(path.Utf8()))
                    {
                        found = Stat{ 0, 0, true };
                        return true;
                    }
                    return false;
                });
            if (!found) { NoteMiss(path); }
            else if (!found->directory) { Record(path); }
            return found;
        }

        File OpenIn(const Mounts& mounts, const Path& path)
        {
            File file;
            if (path.Empty()) { return file; }
            if (path.IsAbsolute())
            {
                file = OsOpen(ToWide(path.Utf8()));
            }
            else
            {
                bool hidden = false;
                Visit(mounts,
                    [&]
                    {
                        file = OsOpen(OsWide(path));
                        return file.IsOpen();
                    },
                    [&](const pak::Package& p)
                    {
                        if (hidden) { return false; }
                        const int entry = p.Find(path.Utf8());
                        if (entry < 0) { return false; }
                        if (p.Deleted(entry))
                        {
                            hidden = true;
                            return false;
                        }
                        file = File::FromPackage(p, entry, p.Entry(entry).size);
                        g_packageOpens.fetch_add(1, std::memory_order_relaxed);
                        return true;
                    });
                if (file) { Record(path); }
            }
            if (file) { g_opens.fetch_add(1, std::memory_order_relaxed); }
            else { NoteMiss(path); }
            return file;
        }

        void ListIn(const Mounts& mounts, const Path& directory, std::string_view pattern,
                    const std::function<void(const ListEntry&)>& visit)
        {
            g_lists.fetch_add(1, std::memory_order_relaxed);
            const std::string prefix = directory.Empty() || directory.Utf8().back() == '/' ? directory.Utf8()
                                                                                            : directory.Utf8() + "/";
            // A name a higher mount listed (or a patch deleted) is not listed again from a lower one.
            std::unordered_set<std::string> named;
            const auto emit = [&](std::string name, bool isDirectory)
            {
                if (!Matches(name, pattern) || !named.insert(pak::FoldKey(name)).second) { return; }
                ListEntry entry;
                entry.name = std::move(name);
                entry.directory = isDirectory;
                entry.path = prefix + entry.name;
                visit(entry);
            };
            const auto listLoose = [&](const std::wstring& folder)
            {
                std::wstring search = folder;
                if (!search.empty() && search.back() != L'\\' && search.back() != L'/') { search.push_back(L'\\'); }
                search += L"*";
                WIN32_FIND_DATAW found{};
                const HANDLE find = ::FindFirstFileExW(search.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch,
                                                       nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (find == INVALID_HANDLE_VALUE) { return; }
                do
                {
                    const std::wstring_view wideName(found.cFileName);
                    if (wideName == L"." || wideName == L"..") { continue; }
                    emit(ToUtf8(wideName), (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
                } while (::FindNextFileW(find, &found));
                ::FindClose(find);
            };
            if (directory.IsAbsolute())
            {
                listLoose(ToWide(directory.Utf8()));
                return;
            }
            const std::wstring looseFolder = directory.Empty() ? ContentRoot().wstring() : OsWide(directory);
            if (mounts.loose && mounts.looseFirst) { listLoose(looseFolder); }
            for (const std::unique_ptr<pak::Package>& p : mounts.packages)
            {
                p->ListDirectory(directory.Utf8(), [&](std::string_view name, bool isDirectory) { emit(std::string(name), isDirectory); });
                // This package's deleted records hide those names from the packages below it.
                for (std::size_t e = 0; e < p->EntryCount(); ++e)
                {
                    const pak::Package::EntryInfo info = p->Entry(static_cast<int>(e));
                    if (!info.deleted) { continue; }
                    const std::string folded = pak::FoldKey(info.path);
                    const std::string folder = pak::FoldKey(prefix);
                    if (folded.compare(0, folder.size(), folder) == 0 && folded.find('/', folder.size()) == std::string::npos)
                    {
                        named.insert(folded.substr(folder.size()));
                    }
                }
            }
            if (mounts.loose && !mounts.looseFirst) { listLoose(looseFolder); }
        }

        bool LooseServesIn(const Mounts& mounts, const Path& path)
        {
            if (path.IsAbsolute()) { return true; }
            bool loose = false;
            bool hidden = false;
            Visit(mounts,
                [&]
                {
                    loose = OsStat(OsWide(path)).has_value();
                    return loose;
                },
                [&](const pak::Package& p)
                {
                    if (hidden) { return false; }
                    const int entry = p.Find(path.Utf8());
                    if (entry < 0) { return false; }
                    hidden = p.Deleted(entry);
                    return !hidden;
                });
            return loose;
        }
    }

    std::optional<Stat> GetStat(const Path& path) { return detail::StatIn(detail::Global(), path); }

    bool Exists(const Path& path) { return GetStat(path).has_value(); }

    bool IsFile(const Path& path)
    {
        const std::optional<Stat> stat = GetStat(path);
        return stat && !stat->directory;
    }

    bool IsDirectory(const Path& path)
    {
        const std::optional<Stat> stat = GetStat(path);
        return stat && stat->directory;
    }

    bool IsLooseFile(const Path& path) { return detail::LooseServesIn(detail::Global(), path); }

    File File::FromHandle(void* handle, std::uint64_t size)
    {
        File file;
        file.handle_ = handle;
        file.size_ = size;
        return file;
    }

    File File::FromPackage(const pak::Package& package, int entry, std::uint64_t size)
    {
        File file;
        file.package_ = &package;
        file.entry_ = entry;
        file.size_ = size;
        return file;
    }

    File::~File()
    {
        if (handle_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(handle_)); }
    }

    File::File(File&& other) noexcept
        : handle_(other.handle_), package_(other.package_), entry_(other.entry_), size_(other.size_)
    {
        other.handle_ = nullptr;
        other.package_ = nullptr;
        other.entry_ = -1;
        other.size_ = 0;
    }

    File& File::operator=(File&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(handle_)); }
            handle_ = other.handle_;
            package_ = other.package_;
            entry_ = other.entry_;
            size_ = other.size_;
            other.handle_ = nullptr;
            other.package_ = nullptr;
            other.entry_ = -1;
            other.size_ = 0;
        }
        return *this;
    }

    bool File::Read(std::uint64_t offset, void* destination, std::size_t bytes) const
    {
        if (!IsOpen() || offset > size_ || bytes > size_ - offset) { return false; }
        if (bytes == 0) { return true; }
        if (package_ != nullptr)
        {
            if (!package_->Read(entry_, offset, destination, bytes)) { return false; }
            g_reads.fetch_add(1, std::memory_order_relaxed);
            g_bytesRead.fetch_add(bytes, std::memory_order_relaxed);
            return true;
        }
        return ReadAt(static_cast<HANDLE>(handle_), offset, destination, bytes);
    }

    File Open(const Path& path) { return detail::OpenIn(detail::Global(), path); }

    bool ReadAll(const Path& path, std::vector<std::uint8_t>& out)
    {
        out.clear();
        const File file = Open(path);
        if (!file) { return false; }
        out.resize(static_cast<std::size_t>(file.Size()));
        if (!file.Read(0, out.data(), out.size()))
        {
            out.clear();
            return false;
        }
        return true;
    }

    bool ReadText(const Path& path, std::string& out)
    {
        out.clear();
        const File file = Open(path);
        if (!file) { return false; }
        out.resize(static_cast<std::size_t>(file.Size()));
        if (!file.Read(0, out.data(), out.size()))
        {
            out.clear();
            return false;
        }
        return true;
    }

    void List(const Path& directory, std::string_view pattern, const std::function<void(const ListEntry&)>& visit)
    {
        detail::ListIn(detail::Global(), directory, pattern, visit);
    }

    InStream::InStream(const Path& path, bool binary) : std::istream(nullptr)
    {
        if (binary)
        {
            File file = Open(path);
            if (!file)
            {
                setstate(std::ios_base::failbit);
                return;
            }
            buffer_ = std::make_unique<FileBuffer>(std::move(file));
        }
        else
        {
            std::string raw;
            if (!ReadText(path, raw))
            {
                setstate(std::ios_base::failbit);
                return;
            }
            std::string text;
            text.reserve(raw.size());
            for (std::size_t i = 0; i < raw.size(); ++i)
            {
                if (raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') { continue; }
                text.push_back(raw[i]);
            }
            buffer_ = std::make_unique<TextBuffer>(std::move(text));
        }
        rdbuf(buffer_.get()); // also clears the badbit std::istream(nullptr) set
        open_ = true;
    }

    InStream::InStream(File file) : std::istream(nullptr)
    {
        if (!file)
        {
            setstate(std::ios_base::failbit);
            return;
        }
        buffer_ = std::make_unique<FileBuffer>(std::move(file));
        rdbuf(buffer_.get());
        open_ = true;
    }

    InStream::~InStream() = default;

    bool Configure(const Config& config, std::string& error)
    {
        for (const std::filesystem::path& keyFile : config.keyFiles)
        {
            std::string why;
            const std::optional<pak::Key> key = pak::LoadKeyFile(keyFile, why);
            if (!key)
            {
                error = why;
                return false;
            }
            detail::Keys().Add(*key);
        }
        detail::Mounts mounts;
        mounts.loose = config.mode != Config::Mode::Packages;
        mounts.looseFirst = true;
        if (config.mode != Config::Mode::Loose)
        {
            const std::filesystem::path folder = config.packageDir.empty() ? ContentRoot() / L"content" : config.packageDir;
            std::error_code ec;
            for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder, ec))
            {
                if (!entry.is_regular_file(ec) || entry.path().extension() != L".tcpak") { continue; }
                std::string why;
                std::unique_ptr<pak::Package> package = pak::Package::Open(entry.path(), why, &detail::Keys());
                if (!package)
                {
                    error = why;
                    return false;
                }
                mounts.packages.push_back(std::move(package));
            }
            if (ec || mounts.packages.empty())
            {
                error = "no .tcpak in " + Narrow(folder.wstring());
                return false;
            }
            detail::SortPackages(mounts.packages);
        }
        detail::Global() = std::move(mounts);
        if (!config.recordFile.empty())
        {
            Recorder& r = TheRecorder();
            std::lock_guard<std::mutex> lock(r.mutex);
            if (r.file) { std::fclose(r.file); }
            r.file = nullptr;
            if (_wfopen_s(&r.file, config.recordFile.c_str(), L"wN") != 0) { r.file = nullptr; }
            g_recording.store(r.file != nullptr, std::memory_order_relaxed);
            if (!r.file)
            {
                error = "cannot write " + Narrow(config.recordFile.wstring());
                return false;
            }
        }
        return true;
    }

    std::vector<std::string> DescribeMounts()
    {
        std::vector<std::string> out;
        const detail::Mounts& mounts = detail::Global();
        if (mounts.loose && mounts.looseFirst) { out.push_back("loose"); }
        for (const std::unique_ptr<pak::Package>& p : mounts.packages)
        {
            out.push_back(p->Name() + " (" + Narrow(p->FilePath().filename().wstring()) + ", " +
                          std::to_string(p->EntryCount()) + " files" + (p->Encrypted() ? ", encrypted" : "") + ")");
        }
        if (mounts.loose && !mounts.looseFirst) { out.push_back("loose"); }
        return out;
    }

    std::filesystem::path WritablePath(Writable kind, std::string_view relative)
    {
        (void)kind; // dev builds: everything stays where it was, under the content root
        return ContentRoot() / ToWide(Normalise(std::string(relative)));
    }

    std::wstring Widen(std::string_view utf8) { return ToWide(utf8); }
    std::string Narrow(std::wstring_view wide) { return ToUtf8(wide); }

    void NoteBlockDecoded(bool damaged)
    {
        (damaged ? g_damagedBlocks : g_blocksDecoded).fetch_add(1, std::memory_order_relaxed);
    }

    Counters GetCounters()
    {
        Counters counters;
        counters.opens = g_opens.load(std::memory_order_relaxed);
        counters.packageOpens = g_packageOpens.load(std::memory_order_relaxed);
        counters.blocksDecoded = g_blocksDecoded.load(std::memory_order_relaxed);
        counters.damagedBlocks = g_damagedBlocks.load(std::memory_order_relaxed);
        counters.misses = g_misses.load(std::memory_order_relaxed);
        counters.stats = g_stats.load(std::memory_order_relaxed);
        counters.lists = g_lists.load(std::memory_order_relaxed);
        counters.reads = g_reads.load(std::memory_order_relaxed);
        counters.bytesRead = g_bytesRead.load(std::memory_order_relaxed);
        return counters;
    }
}
