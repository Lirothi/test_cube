#include "core/vfs/Pak.h"

#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "zstd/zstd.h"

#include <Windows.h>
#include <intrin.h>
#include <nmmintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>
#include <unordered_map>

namespace vfs
{
    void NoteBlockDecoded(bool crcFailed); // Vfs.cpp: the counters
}

namespace vfs::pak
{
    namespace
    {
        // ---- CRC32C (Castagnoli): SSE4.2 when the CPU has it, a table otherwise ----
        bool CpuHasSse42()
        {
            int info[4] = {};
            __cpuid(info, 1);
            return (info[2] & (1 << 20)) != 0;
        }

        std::uint32_t Crc32cTable(const std::uint8_t* p, std::size_t n, std::uint32_t crc)
        {
            static const auto table = []
            {
                std::array<std::uint32_t, 256> t{};
                for (std::uint32_t i = 0; i < 256; ++i)
                {
                    std::uint32_t c = i;
                    for (int k = 0; k < 8; ++k) { c = (c & 1u) ? (c >> 1) ^ 0x82F63B78u : (c >> 1); }
                    t[i] = c;
                }
                return t;
            }();
            for (std::size_t i = 0; i < n; ++i) { crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8); }
            return crc;
        }

        std::uint32_t Crc32cHardware(const std::uint8_t* p, std::size_t n, std::uint32_t crc)
        {
            std::uint64_t c = crc;
            while (n >= 8)
            {
                std::uint64_t v;
                std::memcpy(&v, p, 8);
                c = _mm_crc32_u64(c, v);
                p += 8;
                n -= 8;
            }
            std::uint32_t c32 = static_cast<std::uint32_t>(c);
            while (n-- > 0) { c32 = _mm_crc32_u8(c32, *p++); }
            return c32;
        }

        // ---- zstd contexts, one per thread ----
        struct DecodeContext
        {
            ZSTD_DCtx* context = nullptr;
            ~DecodeContext() { if (context) { ZSTD_freeDCtx(context); } }
        };

        ZSTD_DCtx* ThreadDecoder()
        {
            thread_local DecodeContext decoder;
            if (!decoder.context) { decoder.context = ZSTD_createDCtx(); }
            return decoder.context;
        }

        // The last two blocks this thread decoded: an InStream's 64 KB window rarely lines up with the blocks, so the
        // next window starts in the block the last one ended in.
        struct CachedBlock
        {
            const Package* package = nullptr;
            std::uint32_t block = UINT32_MAX;
            std::vector<std::uint8_t> bytes;
        };
        thread_local CachedBlock t_blocks[2];
        thread_local int t_nextBlock = 0;

        template <typename T>
        void AppendPod(std::vector<std::uint8_t>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const std::uint8_t*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }

        bool IsSafeVirtualPath(std::string_view path)
        {
            if (path.empty() || path.front() == '/' || (path.size() >= 2 && path[1] == ':')) { return false; }
            std::size_t start = 0;
            while (start <= path.size())
            {
                const std::size_t slash = path.find('/', start);
                const std::string_view part = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
                if (part.empty() || part == "." || part == "..") { return false; }
                if (slash == std::string_view::npos) { break; }
                start = slash + 1;
            }
            return true;
        }
    }

    std::string FoldKey(std::string_view normalisedPath)
    {
        std::string key(normalisedPath);
        for (char& c : key)
        {
            if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
        }
        return key;
    }

    std::uint32_t Crc32c(const void* data, std::size_t bytes)
    {
        static const bool hardware = CpuHasSse42();
        const auto* p = static_cast<const std::uint8_t*>(data);
        const std::uint32_t crc = hardware ? Crc32cHardware(p, bytes, 0xFFFFFFFFu) : Crc32cTable(p, bytes, 0xFFFFFFFFu);
        return crc ^ 0xFFFFFFFFu;
    }

    std::uint64_t ContentHash(const void* data, std::size_t bytes)
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        std::uint64_t h = 1469598103934665603ull;
        for (std::size_t i = 0; i < bytes; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // ===================================== Build =====================================

    BuildResult Build(const std::filesystem::path& out, std::vector<BuildFile> files, const BuildOptions& options)
    {
        BuildResult result;
        const auto started = std::chrono::steady_clock::now();
        const auto fail = [&result](std::string why) { result.ok = false; result.error = std::move(why); return result; };

        struct Item
        {
            std::string path;
            std::string key;
            std::filesystem::path source;
            bool deleted = false;
        };
        std::vector<Item> items;
        items.reserve(files.size());
        for (BuildFile& f : files)
        {
            Item item;
            item.path = vfs::Path(f.path).Utf8();
            if (!IsSafeVirtualPath(item.path)) { return fail("not a content path: '" + f.path + "'"); }
            item.key = FoldKey(item.path);
            item.source = std::move(f.source);
            item.deleted = f.deleted;
            items.push_back(std::move(item));
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.key < b.key; });
        for (std::size_t i = 1; i < items.size(); ++i)
        {
            if (items[i].key == items[i - 1].key) { return fail("named twice: '" + items[i].path + "'"); }
        }

        std::filesystem::path temp = out;
        temp += L".tmp";
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) { return fail("cannot write " + vfs::Narrow(temp.wstring())); }
        std::vector<char> zeros(kAlign, 0);
        file.write(zeros.data(), kHeaderBytes);
        std::uint64_t offset = kHeaderBytes;

        std::vector<TocEntry> entries(items.size());
        std::vector<TocBlock> blocks;
        std::string strings;
        const unsigned threads = options.threads != 0 ? options.threads : std::max(1u, std::thread::hardware_concurrency());

        // Batches of whole files, ~256 MB raw: their blocks compress on every core, then go out in order.
        std::size_t next = 0;
        while (next < items.size())
        {
            struct Job
            {
                std::size_t item = 0;
                const std::uint8_t* raw = nullptr;
                std::size_t rawSize = 0;
                std::vector<std::uint8_t> stored;
                Method method = Method::Store;
            };
            std::vector<std::vector<std::uint8_t>> contents;
            std::vector<std::size_t> batch;
            std::uint64_t batchBytes = 0;
            while (next < items.size() && (batch.empty() || batchBytes < (256ull << 20)))
            {
                std::vector<std::uint8_t> bytes;
                if (!items[next].deleted && !vfs::ReadAll(items[next].source, bytes))
                {
                    file.close();
                    std::error_code ignored;
                    std::filesystem::remove(temp, ignored);
                    return fail("cannot read '" + vfs::Narrow(items[next].source.wstring()) + "' for '" + items[next].path + "'");
                }
                batchBytes += bytes.size();
                contents.push_back(std::move(bytes));
                batch.push_back(next++);
            }

            std::vector<Job> jobs;
            for (std::size_t b = 0; b < batch.size(); ++b)
            {
                const std::vector<std::uint8_t>& bytes = contents[b];
                for (std::size_t at = 0; at < bytes.size(); at += kBlockSize)
                {
                    Job job;
                    job.item = b;
                    job.raw = bytes.data() + at;
                    job.rawSize = std::min<std::size_t>(kBlockSize, bytes.size() - at);
                    jobs.push_back(std::move(job));
                }
            }
            std::atomic<std::size_t> cursor{ 0 };
            const auto worker = [&]()
            {
                ZSTD_CCtx* context = ZSTD_createCCtx();
                for (std::size_t j = cursor.fetch_add(1); j < jobs.size(); j = cursor.fetch_add(1))
                {
                    Job& job = jobs[j];
                    std::vector<std::uint8_t> packed(ZSTD_compressBound(job.rawSize));
                    const std::size_t size = ZSTD_compressCCtx(context, packed.data(), packed.size(), job.raw, job.rawSize, options.level);
                    // Kept as it is unless compression saves 5 %: the decode is not worth less.
                    if (!ZSTD_isError(size) && size + job.rawSize / 20 < job.rawSize)
                    {
                        packed.resize(size);
                        job.stored = std::move(packed);
                        job.method = Method::Zstd;
                    }
                    else
                    {
                        job.stored.assign(job.raw, job.raw + job.rawSize);
                        job.method = Method::Store;
                    }
                }
                ZSTD_freeCCtx(context);
            };
            std::vector<std::thread> pool;
            const unsigned count = std::min<unsigned>(threads, static_cast<unsigned>(std::max<std::size_t>(jobs.size(), 1)));
            for (unsigned t = 1; t < count; ++t) { pool.emplace_back(worker); }
            worker();
            for (std::thread& t : pool) { t.join(); }

            std::size_t job = 0;
            for (std::size_t b = 0; b < batch.size(); ++b)
            {
                const Item& item = items[batch[b]];
                TocEntry& entry = entries[batch[b]];
                entry.pathOffset = static_cast<std::uint32_t>(strings.size());
                entry.pathLength = static_cast<std::uint32_t>(item.path.size());
                strings += item.path;
                entry.size = contents[b].size();
                entry.contentHash = ContentHash(contents[b].data(), contents[b].size());
                entry.flags = item.deleted ? kEntryDeleted : 0u;
                entry.firstBlock = static_cast<std::uint32_t>(blocks.size());
                const std::uint64_t padding = (kAlign - offset % kAlign) % kAlign;
                if (entry.size > 0 && padding > 0)
                {
                    file.write(zeros.data(), static_cast<std::streamsize>(padding));
                    offset += padding;
                }
                while (job < jobs.size() && jobs[job].item == b)
                {
                    const Job& j = jobs[job++];
                    TocBlock block;
                    block.offset = offset;
                    block.storedSize = static_cast<std::uint32_t>(j.stored.size());
                    block.rawSize = static_cast<std::uint32_t>(j.rawSize);
                    block.method = static_cast<std::uint8_t>(j.method);
                    block.crc = Crc32c(j.stored.data(), j.stored.size());
                    file.write(reinterpret_cast<const char*>(j.stored.data()), static_cast<std::streamsize>(j.stored.size()));
                    offset += j.stored.size();
                    blocks.push_back(block);
                    result.storedBlocks += j.method == Method::Store ? 1u : 0u;
                }
                entry.blockCount = static_cast<std::uint32_t>(blocks.size()) - entry.firstBlock;
                result.rawBytes += entry.size;
                ++result.files;
            }
        }

        // The table of contents, compressed, from the next 4 KB boundary.
        std::vector<std::uint8_t> toc;
        TocHeader tocHeader;
        tocHeader.entryCount = static_cast<std::uint32_t>(entries.size());
        tocHeader.blockCount = static_cast<std::uint32_t>(blocks.size());
        tocHeader.stringBytes = static_cast<std::uint32_t>(strings.size());
        AppendPod(toc, tocHeader);
        for (const TocEntry& e : entries) { AppendPod(toc, e); }
        for (const TocBlock& b : blocks) { AppendPod(toc, b); }
        toc.insert(toc.end(), strings.begin(), strings.end());
        std::vector<std::uint8_t> packedToc(ZSTD_compressBound(toc.size()));
        const std::size_t tocSize = ZSTD_compress(packedToc.data(), packedToc.size(), toc.data(), toc.size(), options.level);
        if (ZSTD_isError(tocSize)) { return fail(std::string("the table of contents did not compress: ") + ZSTD_getErrorName(tocSize)); }
        packedToc.resize(tocSize);
        const std::uint64_t padding = (kAlign - offset % kAlign) % kAlign;
        file.write(zeros.data(), static_cast<std::streamsize>(padding));
        offset += padding;

        Header header;
        header.tocOffset = offset;
        header.tocStoredSize = packedToc.size();
        header.tocRawSize = toc.size();
        header.tocMethod = static_cast<std::uint32_t>(Method::Zstd);
        header.tocCrc = Crc32c(packedToc.data(), packedToc.size());
        header.contentId = ContentHash(toc.data(), toc.size());
        header.priority = options.priority;
        header.entryCount = static_cast<std::uint32_t>(entries.size());
        std::memcpy(header.name, options.name.data(), std::min(options.name.size(), sizeof(header.name) - 1));
        file.write(reinterpret_cast<const char*>(packedToc.data()), static_cast<std::streamsize>(packedToc.size()));
        offset += packedToc.size();
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.close();
        if (!file)
        {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            return fail("writing " + vfs::Narrow(temp.wstring()) + " failed");
        }
        if (!::MoveFileExW(temp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            return fail("cannot put " + vfs::Narrow(out.wstring()) + " in place (error " + std::to_string(::GetLastError()) + ")");
        }
        result.ok = true;
        result.packageBytes = offset;
        result.blocks = blocks.size();
        result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        return result;
    }

    // ===================================== Package =====================================

    Package::~Package()
    {
        if (view_) { ::UnmapViewOfFile(view_); }
        if (mapping_) { ::CloseHandle(static_cast<HANDLE>(mapping_)); }
        if (fileHandle_) { ::CloseHandle(static_cast<HANDLE>(fileHandle_)); }
    }

    std::unique_ptr<Package> Package::Open(const std::filesystem::path& path, std::string& error)
    {
        std::unique_ptr<Package> package(new Package());
        package->file_ = path;
        const auto refuse = [&error, &path](const std::string& why)
        {
            error = vfs::Narrow(path.wstring()) + ": " + why;
            return std::unique_ptr<Package>();
        };
        const HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) { return refuse("cannot open"); }
        package->fileHandle_ = handle;
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(handle, &size) || size.QuadPart < static_cast<LONGLONG>(kHeaderBytes)) { return refuse("too small to be a package"); }
        package->fileBytes_ = static_cast<std::uint64_t>(size.QuadPart);
        const HANDLE mapping = ::CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping) { return refuse("cannot map"); }
        package->mapping_ = mapping;
        package->view_ = static_cast<const std::uint8_t*>(::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (!package->view_) { return refuse("cannot map a view"); }

        std::memcpy(&package->header_, package->view_, sizeof(Header));
        const Header& h = package->header_;
        if (h.magic != kMagic) { return refuse("not a .tcpak"); }
        if (h.version != kVersion) { return refuse("format version " + std::to_string(h.version) + ", this build reads " + std::to_string(kVersion)); }
        if (h.blockSize != kBlockSize) { return refuse("block size " + std::to_string(h.blockSize)); }
        if ((h.flags & 1u) != 0) { return refuse("encrypted, and this build has no key (D3)"); }
        if (h.tocOffset > package->fileBytes_ || h.tocStoredSize > package->fileBytes_ - h.tocOffset) { return refuse("the table of contents lies past the end"); }
        const std::uint8_t* storedToc = package->view_ + h.tocOffset;
        if (Crc32c(storedToc, static_cast<std::size_t>(h.tocStoredSize)) != h.tocCrc) { return refuse("the table of contents is damaged (CRC)"); }
        package->toc_.resize(static_cast<std::size_t>(h.tocRawSize));
        const std::size_t got = ZSTD_decompress(package->toc_.data(), package->toc_.size(), storedToc, static_cast<std::size_t>(h.tocStoredSize));
        if (ZSTD_isError(got) || got != package->toc_.size()) { return refuse("the table of contents does not decompress"); }
        if (package->toc_.size() < sizeof(TocHeader)) { return refuse("the table of contents is truncated"); }
        TocHeader th;
        std::memcpy(&th, package->toc_.data(), sizeof(th));
        const std::uint64_t need = sizeof(TocHeader) + std::uint64_t{ th.entryCount } * sizeof(TocEntry) +
                                   std::uint64_t{ th.blockCount } * sizeof(TocBlock) + th.stringBytes;
        if (need != package->toc_.size()) { return refuse("the table of contents has the wrong size"); }
        package->entryCount_ = th.entryCount;
        package->blockCount_ = th.blockCount;
        package->entries_ = reinterpret_cast<const TocEntry*>(package->toc_.data() + sizeof(TocHeader));
        package->blocks_ = reinterpret_cast<const TocBlock*>(package->entries_ + th.entryCount);
        package->strings_ = reinterpret_cast<const char*>(package->blocks_ + th.blockCount);
        for (std::size_t b = 0; b < package->blockCount_; ++b)
        {
            const TocBlock& block = package->blocks_[b];
            if (block.offset > package->fileBytes_ || block.storedSize > package->fileBytes_ - block.offset ||
                block.rawSize > kBlockSize || block.method > static_cast<std::uint8_t>(Method::Zstd))
            {
                return refuse("block " + std::to_string(b) + " is out of bounds");
            }
        }
        package->keys_.reserve(package->entryCount_);
        for (std::size_t e = 0; e < package->entryCount_; ++e)
        {
            const TocEntry& entry = package->entries_[e];
            if (std::uint64_t{ entry.pathOffset } + entry.pathLength > th.stringBytes ||
                std::uint64_t{ entry.firstBlock } + entry.blockCount > package->blockCount_)
            {
                return refuse("entry " + std::to_string(e) + " is out of bounds");
            }
            std::uint64_t raw = 0;
            for (std::uint32_t b = 0; b < entry.blockCount; ++b) { raw += package->blocks_[entry.firstBlock + b].rawSize; }
            if (raw != entry.size) { return refuse("entry " + std::to_string(e) + " does not add up"); }
            package->keys_.push_back(FoldKey(package->PathOf(entry)));
        }
        if (!std::is_sorted(package->keys_.begin(), package->keys_.end())) { return refuse("the table of contents is not sorted"); }
        package->name_ = std::string(h.name, strnlen(h.name, sizeof(h.name)));
        if (package->name_.empty()) { package->name_ = vfs::Narrow(path.stem().wstring()); }
        return package;
    }

    const TocEntry& Package::EntryAt(int entry) const { return entries_[static_cast<std::size_t>(entry)]; }
    const TocBlock& Package::BlockAt(std::uint32_t block) const { return blocks_[block]; }

    std::string_view Package::PathOf(const TocEntry& entry) const
    {
        return std::string_view(strings_ + entry.pathOffset, entry.pathLength);
    }

    int Package::Find(std::string_view normalisedPath) const
    {
        const std::string key = FoldKey(normalisedPath);
        const auto it = std::lower_bound(keys_.begin(), keys_.end(), key);
        if (it == keys_.end() || *it != key) { return -1; }
        return static_cast<int>(it - keys_.begin());
    }

    bool Package::Deleted(int entry) const
    {
        return entry >= 0 && (EntryAt(entry).flags & kEntryDeleted) != 0;
    }

    bool Package::HasDirectory(std::string_view normalisedPath) const
    {
        std::string prefix = FoldKey(normalisedPath);
        if (!prefix.empty() && prefix.back() != '/') { prefix.push_back('/'); }
        for (auto it = std::lower_bound(keys_.begin(), keys_.end(), prefix);
             it != keys_.end() && it->compare(0, prefix.size(), prefix) == 0; ++it)
        {
            if (!Deleted(static_cast<int>(it - keys_.begin()))) { return true; }
        }
        return false;
    }

    void Package::ListDirectory(std::string_view normalisedPath,
                                const std::function<void(std::string_view name, bool directory)>& visit) const
    {
        std::string prefix = FoldKey(normalisedPath);
        if (!prefix.empty() && prefix.back() != '/') { prefix.push_back('/'); }
        std::string lastDirectory;
        for (auto it = std::lower_bound(keys_.begin(), keys_.end(), prefix);
             it != keys_.end() && it->compare(0, prefix.size(), prefix) == 0; ++it)
        {
            const int index = static_cast<int>(it - keys_.begin());
            if (Deleted(index)) { continue; }
            const std::string_view path = PathOf(EntryAt(index));
            const std::string_view rest = path.substr(prefix.size());
            const std::size_t slash = rest.find('/');
            if (slash == std::string_view::npos)
            {
                visit(rest, false);
                continue;
            }
            const std::string_view folder = rest.substr(0, slash);
            if (FoldKey(folder) != lastDirectory)
            {
                lastDirectory = FoldKey(folder);
                visit(folder, true);
            }
        }
    }

    Package::EntryInfo Package::Entry(int entry) const
    {
        const TocEntry& e = EntryAt(entry);
        EntryInfo info;
        info.path = PathOf(e);
        info.size = e.size;
        info.contentHash = e.contentHash;
        info.blocks = e.blockCount;
        info.deleted = (e.flags & kEntryDeleted) != 0;
        return info;
    }

    const std::uint8_t* Package::DecodeBlock(std::uint32_t block, std::string_view path) const
    {
        for (CachedBlock& cached : t_blocks)
        {
            if (cached.package == this && cached.block == block) { return cached.bytes.data(); }
        }
        const TocBlock& b = BlockAt(block);
        const std::uint8_t* stored = view_ + b.offset;
        if (Crc32c(stored, b.storedSize) != b.crc)
        {
            vfs::NoteBlockDecoded(true);
            LOG_ERROR(logging::LogCategory::Vfs, "vfs: '{}' in {} is damaged: block {} fails its CRC", path, name_, block);
            return nullptr;
        }
        if (b.method == static_cast<std::uint8_t>(Method::Store)) { return stored; }
        CachedBlock& slot = t_blocks[t_nextBlock];
        t_nextBlock ^= 1;
        slot.package = nullptr;
        slot.bytes.resize(b.rawSize);
        const std::size_t got = ZSTD_decompressDCtx(ThreadDecoder(), slot.bytes.data(), slot.bytes.size(), stored, b.storedSize);
        if (ZSTD_isError(got) || got != b.rawSize)
        {
            vfs::NoteBlockDecoded(true);
            LOG_ERROR(logging::LogCategory::Vfs, "vfs: '{}' in {} is damaged: block {} does not decompress", path, name_, block);
            return nullptr;
        }
        vfs::NoteBlockDecoded(false);
        slot.package = this;
        slot.block = block;
        return slot.bytes.data();
    }

    bool Package::Read(int entry, std::uint64_t offset, void* destination, std::size_t bytes) const
    {
        if (entry < 0 || static_cast<std::size_t>(entry) >= entryCount_) { return false; }
        const TocEntry& e = EntryAt(entry);
        if (offset > e.size || bytes > e.size - offset) { return false; }
        auto* out = static_cast<std::uint8_t*>(destination);
        std::uint64_t at = offset;
        std::size_t left = bytes;
        while (left > 0)
        {
            const std::uint32_t index = static_cast<std::uint32_t>(at / kBlockSize);
            const std::uint32_t block = e.firstBlock + index;
            const std::uint8_t* raw = DecodeBlock(block, PathOf(e));
            if (!raw) { return false; }
            const std::size_t within = static_cast<std::size_t>(at - std::uint64_t{ index } * kBlockSize);
            const std::size_t take = std::min<std::size_t>(left, BlockAt(block).rawSize - within);
            std::memcpy(out, raw + within, take);
            out += take;
            at += take;
            left -= take;
        }
        return true;
    }

    bool Package::Verify(int entry, std::string& error) const
    {
        const TocEntry& e = EntryAt(entry);
        if ((e.flags & kEntryDeleted) != 0) { return true; }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(e.size));
        if (!Read(entry, 0, bytes.data(), bytes.size()))
        {
            error = std::string(PathOf(e)) + ": a block is damaged";
            return false;
        }
        if (ContentHash(bytes.data(), bytes.size()) != e.contentHash)
        {
            error = std::string(PathOf(e)) + ": the content hash does not match";
            return false;
        }
        return true;
    }
}
