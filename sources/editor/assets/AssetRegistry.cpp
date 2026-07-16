#include "editor/assets/AssetRegistry.h"
#if WITH_EDITOR

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

#include <dxgiformat.h>

#include "core/StringMatch.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"

// nlohmann/json — single header
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

namespace fs = std::filesystem;

namespace
{
    std::string ToLower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    uint64_t WriteTimeOf(const fs::path& p)
    {
        std::error_code ec;
        const fs::file_time_type t = fs::last_write_time(p, ec);
        if (ec)
        {
            return 0;
        }
        return static_cast<uint64_t>(t.time_since_epoch().count());
    }

    // Returns the matching allowed extension (lowercased, with leading dot),
    // preferring the longest match so ".mesh.txt" wins over ".txt". Empty if no
    // allowed extension matches.
    std::string MatchExtension(const fs::path& p,
        const std::vector<std::string>& allowed)
    {
        const std::string name = ToLower(p.filename().string());
        std::string best;
        for (const std::string& ext : allowed)
        {
            if (name.size() >= ext.size() &&
                name.compare(name.size() - ext.size(), ext.size(), ext) == 0 &&
                ext.size() > best.size())
            {
                best = ext;
            }
        }
        return best;
    }

    constexpr uint32_t MakeFourCC(char a, char b, char c, char d)
    {
        return static_cast<uint32_t>(static_cast<unsigned char>(a)) |
            (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
            (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
            (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
    }

    template <size_t N>
    uint32_t ReadLe32(const std::array<uint8_t, N>& bytes, size_t offset)
    {
        return static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    }

    template <size_t N>
    uint32_t ReadBe32(const std::array<uint8_t, N>& bytes, size_t offset)
    {
        return (static_cast<uint32_t>(bytes[offset]) << 24) |
            (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
            (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
            static_cast<uint32_t>(bytes[offset + 3]);
    }

    std::string DxgiFormatName(uint32_t format)
    {
        switch (static_cast<DXGI_FORMAT>(format))
        {
        case DXGI_FORMAT_UNKNOWN:               return "DXGI_FORMAT_UNKNOWN";
        case DXGI_FORMAT_R32G32B32A32_FLOAT:   return "DXGI_FORMAT_R32G32B32A32_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_FLOAT:   return "DXGI_FORMAT_R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_UNORM:   return "DXGI_FORMAT_R16G16B16A16_UNORM";
        case DXGI_FORMAT_R10G10B10A2_UNORM:    return "DXGI_FORMAT_R10G10B10A2_UNORM";
        case DXGI_FORMAT_R11G11B10_FLOAT:      return "DXGI_FORMAT_R11G11B10_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_UNORM:       return "DXGI_FORMAT_R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:  return "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R16_FLOAT:            return "DXGI_FORMAT_R16_FLOAT";
        case DXGI_FORMAT_R16_UNORM:            return "DXGI_FORMAT_R16_UNORM";
        case DXGI_FORMAT_R8_UNORM:             return "DXGI_FORMAT_R8_UNORM";
        case DXGI_FORMAT_R32_FLOAT:            return "DXGI_FORMAT_R32_FLOAT";
        case DXGI_FORMAT_BC1_UNORM:            return "DXGI_FORMAT_BC1_UNORM";
        case DXGI_FORMAT_BC1_UNORM_SRGB:       return "DXGI_FORMAT_BC1_UNORM_SRGB";
        case DXGI_FORMAT_BC2_UNORM:            return "DXGI_FORMAT_BC2_UNORM";
        case DXGI_FORMAT_BC2_UNORM_SRGB:       return "DXGI_FORMAT_BC2_UNORM_SRGB";
        case DXGI_FORMAT_BC3_UNORM:            return "DXGI_FORMAT_BC3_UNORM";
        case DXGI_FORMAT_BC3_UNORM_SRGB:       return "DXGI_FORMAT_BC3_UNORM_SRGB";
        case DXGI_FORMAT_BC4_UNORM:            return "DXGI_FORMAT_BC4_UNORM";
        case DXGI_FORMAT_BC4_SNORM:            return "DXGI_FORMAT_BC4_SNORM";
        case DXGI_FORMAT_BC5_UNORM:            return "DXGI_FORMAT_BC5_UNORM";
        case DXGI_FORMAT_BC5_SNORM:            return "DXGI_FORMAT_BC5_SNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM:       return "DXGI_FORMAT_B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8X8_UNORM:       return "DXGI_FORMAT_B8G8R8X8_UNORM";
        case DXGI_FORMAT_BC6H_UF16:            return "DXGI_FORMAT_BC6H_UF16";
        case DXGI_FORMAT_BC6H_SF16:            return "DXGI_FORMAT_BC6H_SF16";
        case DXGI_FORMAT_BC7_UNORM:            return "DXGI_FORMAT_BC7_UNORM";
        case DXGI_FORMAT_BC7_UNORM_SRGB:       return "DXGI_FORMAT_BC7_UNORM_SRGB";
        default:                               return "DXGI_FORMAT_" + std::to_string(format);
        }
    }

    std::string DdsLegacyFormatName(uint32_t fourCC, uint32_t flags,
        uint32_t rgbBitCount, uint32_t rMask, uint32_t gMask, uint32_t bMask,
        uint32_t aMask)
    {
        constexpr uint32_t kDdsPfAlphaPixels = 0x1;
        constexpr uint32_t kDdsPfFourCC = 0x4;
        constexpr uint32_t kDdsPfRgb = 0x40;
        constexpr uint32_t kDdsPfLuminance = 0x20000;

        if ((flags & kDdsPfFourCC) != 0)
        {
            switch (fourCC)
            {
            case MakeFourCC('D', 'X', 'T', '1'): return "DXGI_FORMAT_BC1_UNORM";
            case MakeFourCC('D', 'X', 'T', '3'): return "DXGI_FORMAT_BC2_UNORM";
            case MakeFourCC('D', 'X', 'T', '5'): return "DXGI_FORMAT_BC3_UNORM";
            case MakeFourCC('A', 'T', 'I', '1'): return "DXGI_FORMAT_BC4_UNORM";
            case MakeFourCC('B', 'C', '4', 'U'): return "DXGI_FORMAT_BC4_UNORM";
            case MakeFourCC('B', 'C', '4', 'S'): return "DXGI_FORMAT_BC4_SNORM";
            case MakeFourCC('A', 'T', 'I', '2'): return "DXGI_FORMAT_BC5_UNORM";
            case MakeFourCC('B', 'C', '5', 'U'): return "DXGI_FORMAT_BC5_UNORM";
            case MakeFourCC('B', 'C', '5', 'S'): return "DXGI_FORMAT_BC5_SNORM";
            default:                             return "DDS fourCC";
            }
        }

        if ((flags & kDdsPfRgb) != 0)
        {
            if (rgbBitCount == 32 && rMask == 0x000000ff && gMask == 0x0000ff00 &&
                bMask == 0x00ff0000 && aMask == 0xff000000)
            {
                return "DXGI_FORMAT_R8G8B8A8_UNORM";
            }
            if (rgbBitCount == 32 && rMask == 0x00ff0000 && gMask == 0x0000ff00 &&
                bMask == 0x000000ff && aMask == 0xff000000)
            {
                return "DXGI_FORMAT_B8G8R8A8_UNORM";
            }
            if (rgbBitCount == 32 && rMask == 0x00ff0000 && gMask == 0x0000ff00 &&
                bMask == 0x000000ff && aMask == 0x00000000)
            {
                return "DXGI_FORMAT_B8G8R8X8_UNORM";
            }
            if (rgbBitCount == 16 && rMask == 0x0000ffff && gMask == 0x00000000 &&
                bMask == 0x00000000 && aMask == 0x00000000)
            {
                return "DXGI_FORMAT_R16_UNORM";
            }
            if (rgbBitCount == 8 && rMask == 0x000000ff && gMask == 0x00000000 &&
                bMask == 0x00000000 && aMask == 0x00000000)
            {
                return "DXGI_FORMAT_R8_UNORM";
            }

            const bool hasAlpha = (flags & kDdsPfAlphaPixels) != 0;
            return hasAlpha ? "DDS RGB+A" : "DDS RGB";
        }

        if ((flags & kDdsPfLuminance) != 0)
        {
            return rgbBitCount == 16 ? "DXGI_FORMAT_R16_UNORM" : "DXGI_FORMAT_R8_UNORM";
        }

        return "DDS legacy";
    }

    const char* PngColorTypeName(uint8_t colorType)
    {
        switch (colorType)
        {
        case 0: return "grayscale";
        case 2: return "RGB";
        case 3: return "indexed";
        case 4: return "grayscale+alpha";
        case 6: return "RGBA";
        default: return "unknown";
        }
    }

    EditorTextureInfo ReadDdsTextureInfo(const fs::path& path)
    {
        constexpr uint32_t kDdsMagic = MakeFourCC('D', 'D', 'S', ' ');
        constexpr uint32_t kDdsPfDx10 = MakeFourCC('D', 'X', '1', '0');
        constexpr uint32_t kDdsCaps2Cubemap = 0x00000200;
        constexpr uint32_t kD3D11ResourceMiscTextureCube = 0x4;

        EditorTextureInfo info;
        info.scanned = true;

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return info;
        }

        std::array<uint8_t, 148> bytes{};
        file.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        const size_t readSize = static_cast<size_t>(file.gcount());
        if (readSize < 128 || ReadLe32(bytes, 0) != kDdsMagic ||
            ReadLe32(bytes, 4) != 124)
        {
            return info;
        }

        const uint32_t height = ReadLe32(bytes, 12);
        const uint32_t width = ReadLe32(bytes, 16);
        const uint32_t depth = ReadLe32(bytes, 24);
        const uint32_t mipLevels = ReadLe32(bytes, 28);
        const uint32_t pfFlags = ReadLe32(bytes, 80);
        const uint32_t fourCC = ReadLe32(bytes, 84);
        const uint32_t rgbBitCount = ReadLe32(bytes, 88);
        const uint32_t rMask = ReadLe32(bytes, 92);
        const uint32_t gMask = ReadLe32(bytes, 96);
        const uint32_t bMask = ReadLe32(bytes, 100);
        const uint32_t aMask = ReadLe32(bytes, 104);
        const uint32_t caps2 = ReadLe32(bytes, 112);

        if (width == 0 || height == 0)
        {
            return info;
        }

        bool isCube = (caps2 & kDdsCaps2Cubemap) != 0;
        uint32_t arraySize = isCube ? 6u : 1u;
        std::string format;

        if (fourCC == kDdsPfDx10)
        {
            if (readSize < 148)
            {
                return info;
            }

            const uint32_t dxgiFormat = ReadLe32(bytes, 128);
            const uint32_t miscFlag = ReadLe32(bytes, 136);
            const uint32_t dxArraySize = ReadLe32(bytes, 140);
            isCube = isCube || ((miscFlag & kD3D11ResourceMiscTextureCube) != 0);
            arraySize = dxArraySize != 0 ? dxArraySize : (isCube ? 6u : 1u);
            format = DxgiFormatName(dxgiFormat);
        }
        else
        {
            format = DdsLegacyFormatName(fourCC, pfFlags, rgbBitCount,
                rMask, gMask, bMask, aMask);
        }

        info.valid = true;
        info.kind = isCube ? EditorTextureKind::TextureCube : EditorTextureKind::Texture2D;
        info.format = std::move(format);
        info.width = width;
        info.height = height;
        info.depth = depth != 0 ? depth : 1u;
        info.mipLevels = mipLevels != 0 ? mipLevels : 1u;
        info.arraySize = arraySize != 0 ? arraySize : 1u;
        return info;
    }

    EditorTextureInfo ReadPngTextureInfo(const fs::path& path)
    {
        EditorTextureInfo info;
        info.scanned = true;

        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return info;
        }

        std::array<uint8_t, 33> bytes{};
        file.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (file.gcount() < static_cast<std::streamsize>(bytes.size()))
        {
            return info;
        }

        constexpr uint8_t kPngSignature[8] = {
            0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
        };
        if (std::memcmp(bytes.data(), kPngSignature, sizeof(kPngSignature)) != 0 ||
            std::memcmp(bytes.data() + 12, "IHDR", 4) != 0)
        {
            return info;
        }

        const uint32_t width = ReadBe32(bytes, 16);
        const uint32_t height = ReadBe32(bytes, 20);
        if (width == 0 || height == 0)
        {
            return info;
        }

        const uint8_t bitDepth = bytes[24];
        const uint8_t colorType = bytes[25];

        info.valid = true;
        info.kind = EditorTextureKind::Texture2D;
        info.format = "PNG " + std::to_string(static_cast<int>(bitDepth)) +
            "-bit " + PngColorTypeName(colorType);
        info.width = width;
        info.height = height;
        info.depth = 1;
        info.mipLevels = 1;
        info.arraySize = 1;
        return info;
    }

    EditorTextureInfo ReadTextureInfo(const fs::path& path, const std::string& ext)
    {
        if (ext == ".dds")
        {
            return ReadDdsTextureInfo(path);
        }
        if (ext == ".png")
        {
            return ReadPngTextureInfo(path);
        }
        return {};
    }

    // Texture files can live under any root — e.g. a mesh's sibling DDS in models/<name>/ — so type
    // them as Texture by extension instead of inheriting the root's default (Mesh). Without this the
    // palm's DDS siblings index as "Mesh" and never show as textures in the content browser.
    EditorAssetType TypeForExtension(const std::string& ext, EditorAssetType rootType)
    {
        if (ext == ".dds" || ext == ".png") { return EditorAssetType::Texture; }
        return rootType;
    }

    struct DirRoot
    {
        const char* dir;
        const char* virtualRoot;
        EditorAssetType type;
        std::vector<std::string> extensions;
    };

    const std::array<DirRoot, 5>& AssetRoots()
    {
        static const std::array<DirRoot, 5> roots = { {
            { "models",         "/Game/Models",   EditorAssetType::Mesh,    { ".obj", ".mesh.txt", ".txt", ".gltf", ".glb", ".dds", ".png" } },
            { "textures",       "/Game/Textures", EditorAssetType::Texture, { ".dds", ".png" } },
            { "data/levels",    "/Game/Levels",   EditorAssetType::Level,   { ".json" } },
            { "shaders",        "/Game/Shaders",  EditorAssetType::Shader,  { ".hlsl" } },
            // Raw drop zone: spawn glTF/GLB straight from staging until the H importer converts
            // them into models/ (they're gitignored; harmless if the folder is absent).
            { "import_staging", "/Game/Staging",  EditorAssetType::Mesh,    { ".gltf", ".glb" } },
        } };
        return roots;
    }

    void HashByte(uint64_t& hash, uint8_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }

    void HashText(uint64_t& hash, std::string_view text)
    {
        for (const unsigned char ch : text)
        {
            HashByte(hash, ch);
        }
        HashByte(hash, 0xffu);
    }

    void HashValue(uint64_t& hash, uint64_t value)
    {
        for (int byte = 0; byte < 8; ++byte)
        {
            HashByte(hash, static_cast<uint8_t>((value >> (byte * 8)) & 0xffu));
        }
    }

    uint64_t FileSizeOf(const fs::path& path)
    {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        return ec ? 0 : static_cast<uint64_t>(size);
    }

    uint64_t FingerprintPath(const fs::path& path, uint64_t kind, uint64_t size = 0)
    {
        uint64_t hash = 14695981039346656037ull;
        HashText(hash, path.generic_string());
        HashValue(hash, kind);
        HashValue(hash, WriteTimeOf(path));
        HashValue(hash, size);
        return hash;
    }

    constexpr const char* kVirtualRoot = "/Game";

    std::string NormalizeVirtualPath(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        while (path.size() > 1 && path.back() == '/')
        {
            path.pop_back();
        }
        return path.empty() ? std::string(kVirtualRoot) : path;
    }

    std::string ParentVirtualPath(std::string_view path)
    {
        if (path == kVirtualRoot || path.empty())
        {
            return {};
        }

        const size_t slash = path.find_last_of('/');
        if (slash == std::string_view::npos || slash == 0)
        {
            return {};
        }
        return std::string(path.substr(0, slash));
    }

    std::string FolderNameFromVirtualPath(std::string_view path)
    {
        if (path == kVirtualRoot)
        {
            return "Game";
        }

        const size_t slash = path.find_last_of('/');
        if (slash == std::string_view::npos)
        {
            return std::string(path);
        }
        return std::string(path.substr(slash + 1));
    }

    EditorAssetFolder* FindFolderMutable(std::vector<EditorAssetFolder>& folders,
        std::string_view path)
    {
        for (EditorAssetFolder& folder : folders)
        {
            if (folder.path == path)
            {
                return &folder;
            }
        }
        return nullptr;
    }

    EditorAssetFolder& EnsureFolder(std::vector<EditorAssetFolder>& folders,
        std::string path)
    {
        path = NormalizeVirtualPath(std::move(path));
        if (EditorAssetFolder* existing = FindFolderMutable(folders, path))
        {
            return *existing;
        }

        const std::string parent = ParentVirtualPath(path);
        if (!parent.empty())
        {
            EnsureFolder(folders, parent);
        }

        EditorAssetFolder folder;
        folder.path = path;
        folder.name = FolderNameFromVirtualPath(path);
        folder.parentPath = parent;
        folders.push_back(std::move(folder));

        EditorAssetFolder& inserted = folders.back();
        if (!parent.empty())
        {
            EditorAssetFolder* parentFolder = FindFolderMutable(folders, parent);
            if (parentFolder)
            {
                parentFolder->childPaths.push_back(inserted.path);
            }
        }
        return inserted;
    }

    bool IsSameOrChildFolder(std::string_view folder, std::string_view parent)
    {
        if (parent.empty() || parent == kVirtualRoot)
        {
            return true;
        }
        if (folder == parent)
        {
            return true;
        }
        if (folder.size() <= parent.size() || folder.compare(0, parent.size(), parent) != 0)
        {
            return false;
        }
        return folder[parent.size()] == '/';
    }

    bool MatchesAssetQuery(const EditorAssetRecord& record,
        const std::string& lowerNeedle,
        EditorAssetType typeFilter)
    {
        if (typeFilter != EditorAssetType::Unknown && record.id.type != typeFilter)
        {
            return false;
        }
        if (!lowerNeedle.empty())
        {
            if (!textmatch::ContainsCaseInsensitive(record.displayName, lowerNeedle) &&
                !textmatch::ContainsCaseInsensitive(record.path, lowerNeedle) &&
                !textmatch::ContainsCaseInsensitive(record.virtualPath, lowerNeedle))
            {
                return false;
            }
        }
        return true;
    }

    size_t ComputeRecursiveAssetCount(std::vector<EditorAssetFolder>& folders,
        const std::string& path)
    {
        EditorAssetFolder* folder = FindFolderMutable(folders, path);
        if (!folder)
        {
            return 0;
        }

        size_t count = folder->directAssetCount;
        const std::vector<std::string> children = folder->childPaths;
        for (const std::string& childPath : children)
        {
            count += ComputeRecursiveAssetCount(folders, childPath);
        }
        folder = FindFolderMutable(folders, path);
        if (folder)
        {
            folder->recursiveAssetCount = count;
        }
        return count;
    }
}

void AssetRegistry::Refresh()
{
    CPU_SCOPE(ProfilerScopes::kAssetRegistryRefresh);
    assets_.clear();
    folders_.clear();

    EnsureFolder(folders_, kVirtualRoot);

    for (const DirRoot& root : AssetRoots())
    {
        EnsureFolder(folders_, root.virtualRoot);

        std::error_code ec;
        if (!fs::is_directory(root.dir, ec))
        {
            continue;
        }

        const fs::path rootPath(root.dir);
        for (fs::recursive_directory_iterator it(root.dir,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end;
             it.increment(ec))
        {
            if (ec)
            {
                break;
            }

            const fs::path& p = it->path();
            std::error_code dirEc;
            if (it->is_directory(dirEc))
            {
                const std::string relativeFolder =
                    p.lexically_relative(rootPath).generic_string();
                std::string virtualFolder(root.virtualRoot);
                if (!relativeFolder.empty() && relativeFolder != ".")
                {
                    virtualFolder += "/";
                    virtualFolder += relativeFolder;
                }
                EnsureFolder(folders_, std::move(virtualFolder));
                continue;
            }

            std::error_code fileEc;
            if (!it->is_regular_file(fileEc))
            {
                continue;
            }

            const std::string ext = MatchExtension(p, root.extensions);
            if (ext.empty())
            {
                continue;
            }

            const std::string relativeFolder =
                p.parent_path().lexically_relative(rootPath).generic_string();
            std::string virtualFolder(root.virtualRoot);
            if (!relativeFolder.empty() && relativeFolder != ".")
            {
                virtualFolder += "/";
                virtualFolder += relativeFolder;
            }

            EditorAssetRecord record;
            record.id.type = TypeForExtension(ext, root.type);
            record.path = p.generic_string();
            record.id.key = record.path;
            record.virtualFolder = NormalizeVirtualPath(std::move(virtualFolder));
            record.virtualPath = record.virtualFolder + "/" + p.filename().generic_string();
            record.displayName = p.filename().string();
            record.extension = ext;
            record.fileWriteTime = WriteTimeOf(p);
            if (record.id.type == EditorAssetType::Texture)
            {
                record.texture = ReadTextureInfo(p, ext);
            }
            assets_.push_back(std::move(record));
        }
    }

    // Material presets: names under "presets" in data/materials.json. Parsed
    // directly here (no MaterialDataManager / GPU dependency).
    {
        EnsureFolder(folders_, "/Game/Materials");

        const char* materialsPath = "data/materials.json";
        std::error_code ec;
        if (fs::exists(materialsPath, ec))
        {
            std::ifstream file(materialsPath);
            nlohmann::json doc;
            bool parsed = false;
            if (file)
            {
                try
                {
                    file >> doc;
                    parsed = true;
                }
                catch (const std::exception&)
                {
                    parsed = false;
                }
            }

            if (parsed)
            {
                const auto presetsIt = doc.find("presets");
                if (presetsIt != doc.end() && presetsIt->is_object())
                {
                    const uint64_t writeTime = WriteTimeOf(materialsPath);
                    for (auto it = presetsIt->begin(); it != presetsIt->end(); ++it)
                    {
                        EditorAssetRecord record;
                        record.id.type = EditorAssetType::MaterialPreset;
                        record.id.key = it.key();
                        record.path = materialsPath;
                        record.virtualFolder = "/Game/Materials";
                        record.virtualPath = record.virtualFolder + "/" + it.key();
                        record.displayName = it.key();
                        record.fileWriteTime = writeTime;
                        assets_.push_back(std::move(record));
                    }
                }
            }
        }
    }

    // Stable order (type, then display name) so counts and the later content
    // browser are deterministic across refreshes.
    std::sort(assets_.begin(), assets_.end(),
        [](const EditorAssetRecord& a, const EditorAssetRecord& b)
        {
            if (a.id.type != b.id.type)
            {
                return static_cast<int>(a.id.type) < static_cast<int>(b.id.type);
            }
            return a.displayName < b.displayName;
        });

    for (const EditorAssetRecord& record : assets_)
    {
        EditorAssetFolder& folder = EnsureFolder(folders_, record.virtualFolder);
        folder.directAssetIds.push_back(record.id);
        folder.directAssetCount = folder.directAssetIds.size();
    }

    for (EditorAssetFolder& folder : folders_)
    {
        std::sort(folder.childPaths.begin(), folder.childPaths.end());
    }
    ComputeRecursiveAssetCount(folders_, kVirtualRoot);
    contentSignature_ = ComputeContentSignature();
    contentSignatureInitialized_ = true;
    ++revision_;
}

bool AssetRegistry::HasChangedOnDisk() const
{
    CPU_SCOPE(ProfilerScopes::kAssetRegistryHasChangedOnDisk);
    return !contentSignatureInitialized_ ||
        ComputeContentSignature() != contentSignature_;
}

uint64_t AssetRegistry::ComputeContentSignature() const
{
    std::vector<uint64_t> fingerprints;
    fingerprints.reserve(128);

    for (const DirRoot& root : AssetRoots())
    {
        const fs::path rootPath(root.dir);
        std::error_code ec;
        if (!fs::is_directory(rootPath, ec))
        {
            fingerprints.push_back(FingerprintPath(rootPath, 0));
            continue;
        }

        fingerprints.push_back(FingerprintPath(rootPath, 1));
        for (fs::recursive_directory_iterator it(rootPath,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end;
             it.increment(ec))
        {
            if (ec)
            {
                fingerprints.push_back(FingerprintPath(rootPath, 2));
                break;
            }

            const fs::path& path = it->path();
            std::error_code typeEc;
            if (it->is_directory(typeEc))
            {
                fingerprints.push_back(FingerprintPath(path, 1));
                continue;
            }
            if (typeEc || !it->is_regular_file(typeEc) ||
                MatchExtension(path, root.extensions).empty())
            {
                continue;
            }

            fingerprints.push_back(FingerprintPath(path, 3, FileSizeOf(path)));
        }
    }

    const fs::path materialsPath("data/materials.json");
    std::error_code materialsEc;
    fingerprints.push_back(FingerprintPath(materialsPath,
        fs::is_regular_file(materialsPath, materialsEc) ? 4 : 5,
        materialsEc ? 0 : FileSizeOf(materialsPath)));

    std::sort(fingerprints.begin(), fingerprints.end());
    uint64_t hash = 14695981039346656037ull;
    for (const uint64_t fingerprint : fingerprints)
    {
        HashValue(hash, fingerprint);
    }
    HashValue(hash, static_cast<uint64_t>(fingerprints.size()));
    return hash;
}

std::vector<const EditorAssetRecord*> AssetRegistry::Search(std::string_view text,
    EditorAssetType typeFilter) const
{
    std::vector<const EditorAssetRecord*> results;
    const std::string needle = ToLower(std::string(text));

    for (const EditorAssetRecord& record : assets_)
    {
        if (MatchesAssetQuery(record, needle, typeFilter))
        {
            results.push_back(&record);
        }
    }
    return results;
}

std::vector<const EditorAssetRecord*> AssetRegistry::SearchInFolder(
    std::string_view folderPath,
    bool recursive,
    std::string_view text,
    EditorAssetType typeFilter) const
{
    std::vector<const EditorAssetRecord*> results;
    const std::string folder = NormalizeVirtualPath(std::string(folderPath));
    const std::string needle = ToLower(std::string(text));

    for (const EditorAssetRecord& record : assets_)
    {
        const bool inFolder = recursive ?
            IsSameOrChildFolder(record.virtualFolder, folder) :
            record.virtualFolder == folder;
        if (!inFolder)
        {
            continue;
        }
        if (MatchesAssetQuery(record, needle, typeFilter))
        {
            results.push_back(&record);
        }
    }
    return results;
}

const EditorAssetRecord* AssetRegistry::FindByPath(std::string_view path) const
{
    for (const EditorAssetRecord& record : assets_)
    {
        if (record.path == path)
        {
            return &record;
        }
    }
    return nullptr;
}

const EditorAssetRecord* AssetRegistry::FindById(const EditorAssetId& id) const
{
    for (const EditorAssetRecord& record : assets_)
    {
        if (record.id.type == id.type && record.id.key == id.key)
        {
            return &record;
        }
    }
    return nullptr;
}

const EditorAssetFolder* AssetRegistry::FindFolder(std::string_view virtualPath) const
{
    const std::string path = NormalizeVirtualPath(std::string(virtualPath));
    for (const EditorAssetFolder& folder : folders_)
    {
        if (folder.path == path)
        {
            return &folder;
        }
    }
    return nullptr;
}

size_t AssetRegistry::CountByType(EditorAssetType type) const
{
    size_t count = 0;
    for (const EditorAssetRecord& record : assets_)
    {
        if (record.id.type == type)
        {
            ++count;
        }
    }
    return count;
}

const char* ToString(EditorAssetType type)
{
    switch (type)
    {
    case EditorAssetType::Mesh:           return "Mesh";
    case EditorAssetType::MaterialPreset: return "Material";
    case EditorAssetType::Texture:        return "Texture";
    case EditorAssetType::Level:          return "Level";
    case EditorAssetType::Shader:         return "Shader";
    case EditorAssetType::Unknown:        return "Unknown";
    }
    return "Unknown";
}

const char* ToString(EditorTextureKind kind)
{
    switch (kind)
    {
    case EditorTextureKind::Texture2D:   return "Texture2D";
    case EditorTextureKind::TextureCube: return "TextureCube";
    case EditorTextureKind::Unknown:     return "Unknown";
    }
    return "Unknown";
}

#endif // WITH_EDITOR
