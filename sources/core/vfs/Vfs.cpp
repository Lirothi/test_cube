#include "core/vfs/Vfs.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <system_error>

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

    std::optional<Stat> GetStat(const Path& path)
    {
        g_stats.fetch_add(1, std::memory_order_relaxed);
        if (path.Empty()) { return std::nullopt; }
        const std::wstring os = OsWide(path);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!::GetFileAttributesExW(os.c_str(), GetFileExInfoStandard, &data))
        {
            g_misses.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }
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

    File::~File()
    {
        if (handle_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(handle_)); }
    }

    File::File(File&& other) noexcept : handle_(other.handle_), size_(other.size_)
    {
        other.handle_ = nullptr;
        other.size_ = 0;
    }

    File& File::operator=(File&& other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(handle_)); }
            handle_ = other.handle_;
            size_ = other.size_;
            other.handle_ = nullptr;
            other.size_ = 0;
        }
        return *this;
    }

    bool File::Read(std::uint64_t offset, void* destination, std::size_t bytes) const
    {
        if (handle_ == nullptr || offset > size_ || bytes > size_ - offset) { return false; }
        if (bytes == 0) { return true; }
        return ReadAt(static_cast<HANDLE>(handle_), offset, destination, bytes);
    }

    File Open(const Path& path)
    {
        File file;
        if (path.Empty()) { return file; }
        const HANDLE handle = OpenForRead(OsWide(path), 0);
        if (handle == INVALID_HANDLE_VALUE)
        {
            g_misses.fetch_add(1, std::memory_order_relaxed);
            return file;
        }
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(handle, &size))
        {
            ::CloseHandle(handle);
            return file;
        }
        g_opens.fetch_add(1, std::memory_order_relaxed);
        file.handle_ = handle;
        file.size_ = static_cast<std::uint64_t>(size.QuadPart);
        return file;
    }

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
        g_lists.fetch_add(1, std::memory_order_relaxed);
        std::wstring search = directory.Empty() ? ContentRoot().wstring() : OsWide(directory);
        if (!search.empty() && search.back() != L'\\' && search.back() != L'/') { search.push_back(L'\\'); }
        search += L"*";
        WIN32_FIND_DATAW found{};
        const HANDLE find = ::FindFirstFileExW(search.c_str(), FindExInfoBasic, &found, FindExSearchNameMatch, nullptr,
                                               FIND_FIRST_EX_LARGE_FETCH);
        if (find == INVALID_HANDLE_VALUE)
        {
            g_misses.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::string prefix = directory.Empty() || directory.Utf8().back() == '/' ? directory.Utf8()
                                                                                        : directory.Utf8() + "/";
        do
        {
            const std::wstring_view wideName(found.cFileName);
            if (wideName == L"." || wideName == L"..") { continue; }
            ListEntry entry;
            entry.name = ToUtf8(wideName);
            if (!Matches(entry.name, pattern)) { continue; }
            entry.directory = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.path = prefix + entry.name;
            visit(entry);
        } while (::FindNextFileW(find, &found));
        ::FindClose(find);
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

    InStream::~InStream() = default;

    std::filesystem::path WritablePath(Writable kind, std::string_view relative)
    {
        (void)kind; // dev builds: everything stays where it was, under the content root
        return ContentRoot() / ToWide(Normalise(std::string(relative)));
    }

    std::wstring Widen(std::string_view utf8) { return ToWide(utf8); }
    std::string Narrow(std::wstring_view wide) { return ToUtf8(wide); }

    Counters GetCounters()
    {
        Counters counters;
        counters.opens = g_opens.load(std::memory_order_relaxed);
        counters.misses = g_misses.load(std::memory_order_relaxed);
        counters.stats = g_stats.load(std::memory_order_relaxed);
        counters.lists = g_lists.load(std::memory_order_relaxed);
        counters.reads = g_reads.load(std::memory_order_relaxed);
        counters.bytesRead = g_bytesRead.load(std::memory_order_relaxed);
        return counters;
    }
}
