#pragma once

// The .tcpak package (docs/distribution_build_plan.md §3): a file is 64 KB blocks, each compressed (zstd) or stored and
// checked (CRC32C; D3 adds AES-GCM per block), so a range read decodes only the blocks it covers; the table of contents
// sits at the end, compressed. The package is mapped into memory whole: a read is a decode from the view, from any
// thread, with no file position to share.
//
// Layout (little-endian, packed):
//   [Header, padded to 4 KB] [file data: every file from a 4 KB boundary, its blocks back to back] [TOC]
//   TOC = TocHeader, TocEntry[entryCount] (sorted by the folded path), TocBlock[blockCount], path strings (UTF-8).
// Same files in -> the same bytes out: entries sorted, no times or build numbers inside, a fixed compression level.

#include "core/vfs/PakCrypto.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace vfs::pak
{
    inline constexpr std::uint32_t kMagic = 0x4B504354u; // "TCPK"
    inline constexpr std::uint32_t kVersion = 1;
    inline constexpr std::uint32_t kBlockSize = 64u * 1024u;
    inline constexpr std::uint32_t kAlign = 4096u;
    inline constexpr std::uint32_t kHeaderBytes = 4096u;

    enum class Method : std::uint8_t
    {
        Store = 0,
        Zstd = 1,
    };

    inline constexpr std::uint32_t kEntryDeleted = 1u << 0; // a patch's record that hides the file of a package below it
    inline constexpr std::uint32_t kPackageEncrypted = 1u << 0; // Header::flags

#pragma pack(push, 1)
    struct Header
    {
        std::uint32_t magic = kMagic;
        std::uint32_t version = kVersion;
        std::uint32_t flags = 0;            // D3: bit 0 = encrypted
        std::uint32_t blockSize = kBlockSize;
        std::uint64_t tocOffset = 0;
        std::uint64_t tocStoredSize = 0;    // on disk
        std::uint64_t tocRawSize = 0;       // decompressed
        std::uint32_t tocMethod = 0;
        std::uint32_t tocCrc = 0;           // CRC32C of the stored TOC bytes
        std::uint8_t keyGuid[16] = {};      // D3: which key; zero = not encrypted
        std::uint8_t tocNonce[12] = {};
        std::uint8_t tocTag[16] = {};
        std::uint64_t contentId = 0;        // FNV-1a 64 of the raw TOC: the package's identity, not a time
        std::uint32_t priority = 0;         // higher mounts first
        std::uint32_t entryCount = 0;
        char name[32] = {};                 // "base", "shaders", "patch_001"
    };

    struct TocHeader
    {
        std::uint32_t entryCount = 0;
        std::uint32_t blockCount = 0;
        std::uint32_t stringBytes = 0;
        std::uint32_t reserved = 0;
    };

    struct TocEntry
    {
        std::uint32_t pathOffset = 0;
        std::uint32_t pathLength = 0;
        std::uint64_t size = 0;
        std::uint64_t contentHash = 0;      // FNV-1a 64 of the file: vfs::Stat::version for a packaged file
        std::uint32_t firstBlock = 0;
        std::uint32_t blockCount = 0;
        std::uint32_t flags = 0;
        std::uint32_t reserved = 0;
    };

    struct TocBlock
    {
        std::uint64_t offset = 0;
        std::uint32_t storedSize = 0;
        std::uint32_t rawSize = 0;
        std::uint8_t method = 0;
        std::uint8_t reserved[3] = {};
        std::uint32_t crc = 0;              // CRC32C of the stored bytes
        std::uint8_t nonce[12] = {};        // D3
        std::uint8_t tag[16] = {};          // D3
    };
#pragma pack(pop)

    // The case-folded form a path is looked up and sorted by: ASCII upper case (the order NTFS lists ASCII names in;
    // the content's paths are ASCII), forward slashes.
    [[nodiscard]] std::string FoldKey(std::string_view normalisedPath);
    [[nodiscard]] std::uint32_t Crc32c(const void* data, std::size_t bytes);
    [[nodiscard]] std::uint64_t ContentHash(const void* data, std::size_t bytes);

    struct BuildFile
    {
        std::string path;                   // virtual: forward slashes from the content root
        std::filesystem::path source;       // where its bytes are (ignored for a deleted record or when `bytes` is set)
        bool deleted = false;
        std::shared_ptr<const std::vector<std::uint8_t>> bytes; // the content itself (a compiled shader), instead of `source`
    };

    struct BuildOptions
    {
        std::string name = "base";
        std::uint32_t priority = 0;
        int level = 19;                     // zstd level; decompression speed does not depend on it
        unsigned threads = 0;               // 0 = the machine's cores
        const Key* key = nullptr;           // encrypt every block and the table of contents with it (§4)
    };

    struct BuildResult
    {
        bool ok = false;
        std::string error;
        std::uint64_t files = 0;
        std::uint64_t rawBytes = 0;
        std::uint64_t packageBytes = 0;
        std::uint64_t blocks = 0;
        std::uint64_t storedBlocks = 0;     // blocks kept as they were: compression saved under 5 %
        double seconds = 0.0;
    };

    // Written to `out` + ".tmp" and renamed over `out` at the end: a failed or cancelled build leaves no half package.
    BuildResult Build(const std::filesystem::path& out, std::vector<BuildFile> files, const BuildOptions& options);

    class Package
    {
    public:
        ~Package();
        Package(const Package&) = delete;
        Package& operator=(const Package&) = delete;

        // An encrypted package needs its key in `keys` (by the GUID in its header).
        static std::unique_ptr<Package> Open(const std::filesystem::path& file, std::string& error, const KeyRing* keys = nullptr);

        [[nodiscard]] const std::string& Name() const noexcept { return name_; }
        [[nodiscard]] std::uint32_t Priority() const noexcept { return header_.priority; }
        [[nodiscard]] std::uint64_t ContentId() const noexcept { return header_.contentId; }
        [[nodiscard]] const std::filesystem::path& FilePath() const noexcept { return file_; }
        [[nodiscard]] std::uint64_t FileBytes() const noexcept { return fileBytes_; }
        [[nodiscard]] std::size_t EntryCount() const noexcept { return entryCount_; }
        [[nodiscard]] bool Encrypted() const noexcept { return cipher_ != nullptr; }
        [[nodiscard]] std::string KeyGuid() const { return GuidText(std::to_array(header_.keyGuid)); }

        // The entry of a normalised virtual path (case-insensitive), or -1. A deleted record answers its index too:
        // ask Deleted().
        [[nodiscard]] int Find(std::string_view normalisedPath) const;
        [[nodiscard]] bool Deleted(int entry) const;
        [[nodiscard]] bool HasDirectory(std::string_view normalisedPath) const;
        // One directory's entries (not recursive), files and folders, in the TOC's order; deleted records are left out.
        void ListDirectory(std::string_view normalisedPath,
                           const std::function<void(std::string_view name, bool directory)>& visit) const;

        struct EntryInfo
        {
            std::string_view path;
            std::uint64_t size = 0;
            std::uint64_t contentHash = 0;
            std::uint32_t blocks = 0;
            bool deleted = false;
        };
        [[nodiscard]] EntryInfo Entry(int entry) const;

        // Exactly `bytes` of the file from `offset`, every block checked; false (and an Error naming the file) on damage.
        bool Read(int entry, std::uint64_t offset, void* destination, std::size_t bytes) const;
        // Every block decoded and the content hash compared.
        bool Verify(int entry, std::string& error) const;
        // A block's bytes as they lie in the file (the self-test: encrypted blocks of equal files differ).
        [[nodiscard]] const std::uint8_t* StoredBytes(int entry, std::uint32_t index, std::uint32_t& bytes) const;

    private:
        Package() = default;
        const TocEntry& EntryAt(int entry) const;
        const TocBlock& BlockAt(std::uint32_t block) const;
        std::string_view PathOf(const TocEntry& entry) const;
        const std::uint8_t* DecodeBlock(int entry, std::uint32_t index) const;

        std::filesystem::path file_;
        std::string name_;
        void* fileHandle_ = nullptr;
        void* mapping_ = nullptr;
        const std::uint8_t* view_ = nullptr;
        std::uint64_t fileBytes_ = 0;
        Header header_{};
        std::vector<std::uint8_t> toc_;
        std::size_t entryCount_ = 0;
        std::size_t blockCount_ = 0;
        const TocEntry* entries_ = nullptr;
        const TocBlock* blocks_ = nullptr;
        const char* strings_ = nullptr;
        std::vector<std::string> keys_;     // folded, in entry order (sorted)
        std::vector<std::uint64_t> pathHashes_; // ContentHash of each folded path: the blocks' AAD
        std::unique_ptr<Cipher> cipher_;    // encrypted packages only
    };

    // What every block and the table of contents authenticate besides their bytes (§4): the package's name and format,
    // and the block's file and place in it -- a block moved elsewhere fails its tag.
#pragma pack(push, 1)
    struct BlockAad
    {
        char name[32] = {};
        std::uint32_t version = kVersion;
        std::uint64_t pathHash = 0;
        std::uint32_t index = 0;
        std::uint8_t method = 0;
        std::uint32_t rawSize = 0;
    };
    struct TocAad
    {
        char name[32] = {};
        std::uint32_t version = kVersion;
        std::uint32_t priority = 0;
        char what[4] = { 'T', 'O', 'C', 0 };
    };
#pragma pack(pop)
}
