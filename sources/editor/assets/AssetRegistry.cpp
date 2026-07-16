#include "editor/assets/AssetRegistry.h"
#if WITH_EDITOR

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>
#include <system_error>
#include <unordered_map>
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

    bool IsImportImageExtension(const std::string& ext)
    {
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
            ext == ".tga" || ext == ".bmp";
    }

    bool IsCopiedImportExtension(const std::string& ext)
    {
        return ext == ".gltf" || ext == ".glb" || ext == ".bin";
    }

    bool IsImportSourceExtension(const std::string& ext)
    {
        return IsImportImageExtension(ext) || IsCopiedImportExtension(ext) || ext == ".hdr";
    }

    bool RelativePathUnder(const fs::path& path,
        const fs::path& root,
        fs::path& relativeOut)
    {
        std::error_code ec;
        const fs::path absolutePath = fs::absolute(path, ec).lexically_normal();
        if (ec) { return false; }
        const fs::path absoluteRoot = fs::absolute(root, ec).lexically_normal();
        if (ec) { return false; }

        relativeOut = absolutePath.lexically_relative(absoluteRoot);
        if (relativeOut.empty() || relativeOut == ".") { return false; }
        const auto first = relativeOut.begin();
        return first != relativeOut.end() && *first != "..";
    }

    bool ResolveImportPaths(const EditorAssetRecord& record,
        fs::path& sourceOut,
        fs::path& projectOut)
    {
        fs::path relative;
        const fs::path recordPath(record.path);

        if (RelativePathUnder(recordPath, "import_staging", relative))
        {
            const auto first = relative.begin();
            if (first == relative.end()) { return false; }
            sourceOut = fs::path("import_staging") / *first;
            projectOut = fs::path("models") / *first;
            return true;
        }

        if (RelativePathUnder(recordPath, "models", relative))
        {
            const auto first = relative.begin();
            if (first == relative.end() || std::next(first) == relative.end())
            {
                return false; // loose project-authored mesh, not an imported asset folder
            }
            sourceOut = fs::path("import_staging") / *first;
            projectOut = fs::path("models") / *first;
            return true;
        }

        if (RelativePathUnder(recordPath, "textures", relative))
        {
            const auto first = relative.begin();
            if (first == relative.end()) { return false; }
            if (std::next(first) != relative.end())
            {
                sourceOut = fs::path("import_staging") / *first;
                projectOut = fs::path("textures") / *first;
                return true;
            }

            // A top-level imported skybox is textures/<name>.dds paired with
            // import_staging/<name>.hdr — either loose at the staging root or
            // folder-wrapped (Poly Haven HDRIs unzip into their own folder).
            const std::string hdrName = recordPath.stem().string() + ".hdr";
            const fs::path sourceHdr = fs::path("import_staging") / hdrName;
            std::error_code ec;
            if (fs::is_regular_file(sourceHdr, ec))
            {
                sourceOut = sourceHdr;
                projectOut = recordPath;
                return true;
            }
            ec.clear();
            for (const fs::directory_entry& stagingEntry :
                fs::directory_iterator("import_staging", fs::directory_options::skip_permission_denied, ec))
            {
                if (ec) { break; }
                std::error_code dirEc;
                if (!stagingEntry.is_directory(dirEc)) { continue; }
                const fs::path wrapped = stagingEntry.path() / hdrName;
                std::error_code fileEc;
                if (fs::is_regular_file(wrapped, fileEc))
                {
                    sourceOut = wrapped;
                    projectOut = recordPath;
                    return true;
                }
            }
        }

        return false;
    }

    uint64_t ImportFileSize(const fs::path& path)
    {
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        return ec ? 0 : static_cast<uint64_t>(size);
    }

    uint64_t HashImportFile(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) { return 0; }

        uint64_t hash = 14695981039346656037ull;
        std::vector<char> buffer(1024 * 1024);
        while (file)
        {
            file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = file.gcount();
            for (std::streamsize i = 0; i < count; ++i)
            {
                hash ^= static_cast<unsigned char>(buffer[static_cast<size_t>(i)]);
                hash *= 1099511628211ull;
            }
        }
        return hash;
    }

    std::string NormalizeImportRelativePath(const fs::path& path)
    {
        return ToLower(path.lexically_normal().generic_string());
    }

    bool IsSafeImportRelativePath(const fs::path& path)
    {
        if (path.empty() || path.is_absolute()) { return false; }
        return std::none_of(path.begin(), path.end(),
            [](const fs::path& component) { return component == ".."; });
    }

    fs::path ImportManifestPath(const fs::path& project)
    {
        std::error_code ec;
        if (fs::is_regular_file(project, ec))
        {
            return project.parent_path() /
                (project.stem().string() + ".assetimport.json");
        }
        return project / ".assetimport.json";
    }

    bool InspectImportManifest(const fs::path& source,
        const fs::path& project,
        EditorAssetImportStatus& statusOut)
    {
        const fs::path manifestPath = ImportManifestPath(project);
        std::ifstream file(manifestPath);
        if (!file) { return false; }

        nlohmann::json manifest = nlohmann::json::parse(file, nullptr, false, true);
        if (manifest.is_discarded() || !manifest.is_object() ||
            manifest.value("version", 0) != 1 ||
            !manifest.contains("sources") || !manifest["sources"].is_array() ||
            !manifest.contains("outputs") || !manifest["outputs"].is_array())
        {
            statusOut = EditorAssetImportStatus::Incomplete;
            return true;
        }

        std::error_code ec;
        const bool sourceIsFile = fs::is_regular_file(source, ec);
        const bool projectIsFile = fs::is_regular_file(project, ec);
        const uint64_t recordedRootWriteTime = manifest.value("sourceRootWriteTime", 0ull);
        const bool rootWriteTimeChanged = WriteTimeOf(source) != recordedRootWriteTime;

        std::set<std::string> recordedSources;
        for (const nlohmann::json& entry : manifest["sources"])
        {
            if (!entry.is_object() || !entry.contains("path") || !entry["path"].is_string())
            {
                statusOut = EditorAssetImportStatus::Incomplete;
                return true;
            }
            const fs::path relative(entry["path"].get<std::string>());
            if (!IsSafeImportRelativePath(relative))
            {
                statusOut = EditorAssetImportStatus::Incomplete;
                return true;
            }
            recordedSources.insert(NormalizeImportRelativePath(relative));

            const fs::path current = sourceIsFile ? source : (source / relative);
            std::error_code currentEc;
            if (!fs::is_regular_file(current, currentEc))
            {
                statusOut = EditorAssetImportStatus::SourceNewer;
                return true;
            }

            const uint64_t recordedSize = entry.value("size", 0ull);
            const uint64_t recordedWriteTime = entry.value("writeTime", 0ull);
            if (rootWriteTimeChanged || ImportFileSize(current) != recordedSize ||
                WriteTimeOf(current) != recordedWriteTime)
            {
                if (HashImportFile(current) != entry.value("hash", 0ull))
                {
                    statusOut = EditorAssetImportStatus::SourceNewer;
                    return true;
                }
            }
        }

        if (manifest.value("trackAllSources", false))
        {
            std::set<std::string> currentSources;
            if (sourceIsFile)
            {
                currentSources.insert(NormalizeImportRelativePath(source.filename()));
            }
            else
            {
                ec.clear();
                for (fs::recursive_directory_iterator it(source,
                         fs::directory_options::skip_permission_denied, ec), end;
                     it != end;
                     it.increment(ec))
                {
                    if (ec) { break; }
                    std::error_code fileEc;
                    if (!it->is_regular_file(fileEc)) { continue; }
                    const std::string ext = ToLower(it->path().extension().string());
                    if (!IsImportImageExtension(ext) && !IsCopiedImportExtension(ext))
                    {
                        continue;
                    }
                    std::error_code relativeEc;
                    const fs::path relative = fs::relative(it->path(), source, relativeEc);
                    if (!relativeEc)
                    {
                        currentSources.insert(NormalizeImportRelativePath(relative));
                    }
                }
            }
            if (currentSources != recordedSources)
            {
                statusOut = EditorAssetImportStatus::SourceNewer;
                return true;
            }
        }

        std::set<std::string> recordedOutputs;
        for (const nlohmann::json& outputEntry : manifest["outputs"])
        {
            if (!outputEntry.is_string())
            {
                statusOut = EditorAssetImportStatus::Incomplete;
                return true;
            }
            const fs::path relative(outputEntry.get<std::string>());
            if (!IsSafeImportRelativePath(relative))
            {
                statusOut = EditorAssetImportStatus::Incomplete;
                return true;
            }
            recordedOutputs.insert(NormalizeImportRelativePath(relative));
            const fs::path current = projectIsFile ?
                (project.parent_path() / relative) : (project / relative);
            std::error_code outputEc;
            if (!fs::is_regular_file(current, outputEc))
            {
                statusOut = EditorAssetImportStatus::Incomplete;
                return true;
            }
        }

        if (!projectIsFile)
        {
            std::set<std::string> currentOutputs;
            ec.clear();
            for (fs::recursive_directory_iterator it(project,
                     fs::directory_options::skip_permission_denied, ec), end;
                 it != end;
                 it.increment(ec))
            {
                if (ec) { break; }
                std::error_code fileEc;
                if (!it->is_regular_file(fileEc)) { continue; }
                const std::string ext = ToLower(it->path().extension().string());
                if (ext != ".dds" && !IsCopiedImportExtension(ext)) { continue; }
                std::error_code relativeEc;
                const fs::path relative = fs::relative(it->path(), project, relativeEc);
                if (!relativeEc)
                {
                    currentOutputs.insert(NormalizeImportRelativePath(relative));
                }
            }
            if (currentOutputs != recordedOutputs)
            {
                statusOut = EditorAssetImportStatus::Incomplete;
                return true;
            }
        }

        statusOut = EditorAssetImportStatus::UpToDate;
        return true;
    }

    struct ImportSourceFingerprint
    {
        uint64_t size = 0;
        uint64_t writeTime = 0;
        uint64_t hash = 0;
    };

    struct ResourceImportInspection
    {
        EditorAssetImportStatus status = EditorAssetImportStatus::Untracked;
        std::vector<std::string> sourceFiles;
    };

    bool IsDiffuseImportStem(const std::string& stem)
    {
        return stem.find("_diff") != std::string::npos ||
            stem.find("_albedo") != std::string::npos ||
            stem.find("_col") != std::string::npos ||
            stem.find("basecolor") != std::string::npos;
    }

    std::string TextureSetNameFromDiffuse(std::string stem)
    {
        for (const char* token : { "_diff", "_albedo", "_col", "basecolor" })
        {
            const size_t position = stem.find(token);
            if (position != std::string::npos)
            {
                stem.resize(position);
                break;
            }
        }
        return stem;
    }

    std::string FlipbookSequenceBase(std::string stem)
    {
        size_t end = stem.size();
        while (end > 0 && std::isdigit(static_cast<unsigned char>(stem[end - 1]))) { --end; }
        if (end == stem.size() || end == 0) { return {}; }
        std::string base = stem.substr(0, end);
        while (!base.empty() && base.back() == '_') { base.pop_back(); }
        if (base.size() >= 6 && ToLower(base.substr(base.size() - 6)) == "_frame")
        {
            base.resize(base.size() - 6);
            while (!base.empty() && base.back() == '_') { base.pop_back(); }
        }
        return ToLower(base);
    }

    // Everything about an import folder that is identical for all of its resource records:
    // manifest contents + the source-candidate listing. Refresh() builds this ONCE per
    // (source, project) pair and reuses it for every record in the folder — per-record
    // manifest re-parsing + source-tree re-walking made Refresh O(records × files).
    struct ImportFolderContext
    {
        bool sourceExists = false;
        bool sourceIsFile = false;
        bool projectIsFile = false;
        bool hasManifest = false;
        bool manifestBroken = false; // present but malformed -> every resource Incomplete
        std::unordered_map<std::string, ImportSourceFingerprint> recorded;
        std::set<std::string> recordedOutputs;
        std::vector<fs::path> sourceCandidates;
    };

    ImportFolderContext BuildImportFolderContext(const fs::path& source, const fs::path& project)
    {
        ImportFolderContext ctx;
        std::error_code ec;
        ctx.sourceExists = fs::exists(source, ec);
        if (!ctx.sourceExists) { return ctx; }
        ec.clear();
        ctx.sourceIsFile = fs::is_regular_file(source, ec);
        ec.clear();
        ctx.projectIsFile = fs::is_regular_file(project, ec);

        const fs::path manifestPath = ImportManifestPath(project);
        std::ifstream manifestFile(manifestPath);
        if (manifestFile)
        {
            ctx.hasManifest = true;
            nlohmann::json manifest = nlohmann::json::parse(
                manifestFile, nullptr, false, true);
            if (manifest.is_discarded() || !manifest.is_object() ||
                manifest.value("version", 0) != 1 ||
                !manifest.contains("sources") || !manifest["sources"].is_array() ||
                !manifest.contains("outputs") || !manifest["outputs"].is_array())
            {
                ctx.manifestBroken = true;
                return ctx;
            }
            for (const nlohmann::json& entry : manifest["sources"])
            {
                if (!entry.is_object() || !entry.contains("path") ||
                    !entry["path"].is_string())
                {
                    ctx.manifestBroken = true;
                    return ctx;
                }
                const fs::path relative(entry["path"].get<std::string>());
                if (!IsSafeImportRelativePath(relative))
                {
                    ctx.manifestBroken = true;
                    return ctx;
                }
                const std::string key = NormalizeImportRelativePath(relative);
                ctx.recorded[key] = {
                    entry.value("size", 0ull),
                    entry.value("writeTime", 0ull),
                    entry.value("hash", 0ull)
                };
                ctx.sourceCandidates.push_back(relative);
            }
            for (const nlohmann::json& entry : manifest["outputs"])
            {
                if (!entry.is_string())
                {
                    ctx.manifestBroken = true;
                    return ctx;
                }
                const fs::path relative(entry.get<std::string>());
                if (!IsSafeImportRelativePath(relative))
                {
                    ctx.manifestBroken = true;
                    return ctx;
                }
                ctx.recordedOutputs.insert(NormalizeImportRelativePath(relative));
            }
        }

        if (ctx.sourceIsFile)
        {
            ctx.sourceCandidates.push_back(source.filename());
        }
        else
        {
            ec.clear();
            for (fs::recursive_directory_iterator it(source,
                     fs::directory_options::skip_permission_denied, ec), end;
                 it != end;
                 it.increment(ec))
            {
                if (ec) { break; }
                std::error_code fileEc;
                if (!it->is_regular_file(fileEc) ||
                    !IsImportSourceExtension(ToLower(it->path().extension().string())))
                {
                    continue;
                }
                std::error_code relativeEc;
                const fs::path relative = fs::relative(it->path(), source, relativeEc);
                if (!relativeEc) { ctx.sourceCandidates.push_back(relative); }
            }
        }

        std::sort(ctx.sourceCandidates.begin(), ctx.sourceCandidates.end(),
            [](const fs::path& a, const fs::path& b)
            {
                return NormalizeImportRelativePath(a) < NormalizeImportRelativePath(b);
            });
        ctx.sourceCandidates.erase(std::unique(ctx.sourceCandidates.begin(), ctx.sourceCandidates.end(),
            [](const fs::path& a, const fs::path& b)
            {
                return NormalizeImportRelativePath(a) == NormalizeImportRelativePath(b);
            }), ctx.sourceCandidates.end());
        return ctx;
    }

    ResourceImportInspection InspectImportResource(const ImportFolderContext& ctx,
        const fs::path& source,
        const fs::path& project,
        const fs::path& resource)
    {
        ResourceImportInspection result;
        if (!ctx.sourceExists) { return result; }
        std::error_code ec;
        if (!fs::exists(resource, ec))
        {
            result.status = EditorAssetImportStatus::Incomplete;
            return result;
        }
        if (ctx.manifestBroken)
        {
            result.status = EditorAssetImportStatus::Incomplete;
            return result;
        }

        const bool sourceIsFile = ctx.sourceIsFile;
        const bool projectIsFile = ctx.projectIsFile;
        fs::path outputRelative = projectIsFile ? resource.filename() :
            resource.lexically_relative(project);
        if (!IsSafeImportRelativePath(outputRelative))
        {
            result.status = EditorAssetImportStatus::Untracked;
            return result;
        }

        const bool hasManifest = ctx.hasManifest;
        const std::unordered_map<std::string, ImportSourceFingerprint>& recorded = ctx.recorded;
        const std::set<std::string>& recordedOutputs = ctx.recordedOutputs;
        const std::vector<fs::path>& sourceCandidates = ctx.sourceCandidates;

        std::vector<fs::path> dependencies;
        if (sourceIsFile)
        {
            dependencies.push_back(source.filename());
        }
        else
        {
            const std::string outputExt = ToLower(outputRelative.extension().string());
            const std::string outputStem = ToLower(outputRelative.stem().string());
            const fs::path outputDirectory = outputRelative.parent_path();
            if (IsCopiedImportExtension(outputExt))
            {
                for (const fs::path& candidate : sourceCandidates)
                {
                    if (NormalizeImportRelativePath(candidate) ==
                        NormalizeImportRelativePath(outputRelative))
                    {
                        dependencies.push_back(candidate);
                    }
                }
            }
            else if (outputExt == ".dds")
            {
                std::string flipbookBase = outputStem;
                constexpr std::string_view suffix = "_flipbook";
                if (flipbookBase.size() > suffix.size() &&
                    flipbookBase.compare(flipbookBase.size() - suffix.size(),
                        suffix.size(), suffix) == 0)
                {
                    flipbookBase.resize(flipbookBase.size() - suffix.size());
                    for (const fs::path& candidate : sourceCandidates)
                    {
                        if (candidate.parent_path() == outputDirectory &&
                            IsImportImageExtension(ToLower(candidate.extension().string())) &&
                            FlipbookSequenceBase(candidate.stem().string()) == flipbookBase)
                        {
                            dependencies.push_back(candidate);
                        }
                    }
                }

                if (dependencies.empty())
                {
                    for (const fs::path& diffuse : sourceCandidates)
                    {
                        if (diffuse.parent_path() != outputDirectory ||
                            !IsImportImageExtension(ToLower(diffuse.extension().string())))
                        {
                            continue;
                        }
                        const std::string diffuseStem = ToLower(diffuse.stem().string());
                        if (!IsDiffuseImportStem(diffuseStem)) { continue; }
                        const std::string setName = TextureSetNameFromDiffuse(diffuseStem);
                        if (outputStem == setName + "_albedo")
                        {
                            dependencies.push_back(diffuse);
                        }
                        else if (outputStem == setName + "_normal")
                        {
                            for (const fs::path& candidate : sourceCandidates)
                            {
                                const std::string stem = ToLower(candidate.stem().string());
                                if (candidate.parent_path() == outputDirectory &&
                                    IsImportImageExtension(ToLower(candidate.extension().string())) &&
                                    (stem.find("_nor") != std::string::npos ||
                                     stem.find("normal") != std::string::npos))
                                {
                                    dependencies.push_back(candidate);
                                }
                            }
                        }
                        else if (outputStem == setName + "_mr")
                        {
                            for (const fs::path& candidate : sourceCandidates)
                            {
                                const std::string stem = ToLower(candidate.stem().string());
                                if (candidate.parent_path() == outputDirectory &&
                                    IsImportImageExtension(ToLower(candidate.extension().string())) &&
                                    (stem.find("_rough") != std::string::npos ||
                                     stem.find("_metal") != std::string::npos ||
                                     stem.find("metallic") != std::string::npos))
                                {
                                    dependencies.push_back(candidate);
                                }
                            }
                        }
                        if (!dependencies.empty()) { break; }
                    }
                }

                if (dependencies.empty())
                {
                    for (const fs::path& candidate : sourceCandidates)
                    {
                        if (candidate.parent_path() == outputDirectory &&
                            IsImportImageExtension(ToLower(candidate.extension().string())) &&
                            ToLower(candidate.stem().string()) == outputStem)
                        {
                            dependencies.push_back(candidate);
                        }
                    }
                }
            }
        }

        std::sort(dependencies.begin(), dependencies.end(),
            [](const fs::path& a, const fs::path& b)
            {
                return NormalizeImportRelativePath(a) < NormalizeImportRelativePath(b);
            });
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
            [](const fs::path& a, const fs::path& b)
            {
                return NormalizeImportRelativePath(a) == NormalizeImportRelativePath(b);
            }), dependencies.end());
        for (const fs::path& dependency : dependencies)
        {
            result.sourceFiles.push_back(dependency.generic_string());
        }

        // An output with no producing source is an orphan. Keep the status on
        // that resource alone so a deleted source never dirties its siblings.
        if (dependencies.empty())
        {
            result.status = EditorAssetImportStatus::SourceNewer;
            return result;
        }
        if (hasManifest && recordedOutputs.count(
                NormalizeImportRelativePath(outputRelative)) == 0)
        {
            result.status = EditorAssetImportStatus::SourceNewer;
            return result;
        }

        for (const fs::path& dependency : dependencies)
        {
            const fs::path current = sourceIsFile ? source : (source / dependency);
            std::error_code currentEc;
            if (!fs::is_regular_file(current, currentEc))
            {
                result.status = EditorAssetImportStatus::SourceNewer;
                return result;
            }
            if (hasManifest)
            {
                const auto found = recorded.find(NormalizeImportRelativePath(dependency));
                if (found == recorded.end())
                {
                    result.status = EditorAssetImportStatus::SourceNewer;
                    return result;
                }
                const ImportSourceFingerprint& fingerprint = found->second;
                if ((ImportFileSize(current) != fingerprint.size ||
                     WriteTimeOf(current) != fingerprint.writeTime) &&
                    HashImportFile(current) != fingerprint.hash)
                {
                    result.status = EditorAssetImportStatus::SourceNewer;
                    return result;
                }
            }
            else if (WriteTimeOf(current) > WriteTimeOf(resource))
            {
                result.status = EditorAssetImportStatus::SourceNewer;
                return result;
            }
        }

        result.status = EditorAssetImportStatus::UpToDate;
        return result;
    }

    bool LegacyTextureSetHasOrphanOutput(const fs::path& source,
        const fs::path& project)
    {
        std::unordered_map<std::string, std::vector<fs::path>> imagesByDirectory;
        std::error_code ec;
        for (fs::recursive_directory_iterator it(source,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end;
             it.increment(ec))
        {
            if (ec) { break; }
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) ||
                !IsImportImageExtension(ToLower(it->path().extension().string())))
            {
                continue;
            }
            imagesByDirectory[it->path().parent_path().generic_string()].push_back(it->path());
        }

        for (const auto& [directoryText, images] : imagesByDirectory)
        {
            fs::path diffuse;
            fs::path roughness;
            fs::path normal;
            for (const fs::path& image : images)
            {
                const std::string stem = ToLower(image.stem().string());
                const auto has = [&stem](const char* token)
                {
                    return stem.find(token) != std::string::npos;
                };
                if (diffuse.empty() &&
                    (has("_diff") || has("_albedo") || has("_col") || has("basecolor")))
                {
                    diffuse = image;
                }
                else if (roughness.empty() && has("_rough"))
                {
                    roughness = image;
                }
                else if (normal.empty() && (has("_nor") || has("normal")))
                {
                    normal = image;
                }
            }
            if (diffuse.empty() || roughness.empty()) { continue; }

            std::string setName = ToLower(diffuse.stem().string());
            for (const char* token : { "_diff", "_albedo", "_col", "basecolor" })
            {
                const size_t position = setName.find(token);
                if (position != std::string::npos)
                {
                    setName.resize(position);
                    break;
                }
            }

            std::set<std::string> expectedNames;
            for (const fs::path& image : images)
            {
                expectedNames.insert(ToLower(image.stem().string()) + ".dds");
            }
            expectedNames.insert(setName + "_albedo.dds");
            expectedNames.insert(setName + "_mr.dds");
            if (!normal.empty()) { expectedNames.insert(setName + "_normal.dds"); }

            const fs::path sourceDirectory(directoryText);
            const fs::path relativeDirectory = sourceDirectory.lexically_relative(source);
            const fs::path projectDirectory = project / relativeDirectory;
            ec.clear();
            if (!fs::is_directory(projectDirectory, ec)) { continue; }
            for (const fs::directory_entry& entry : fs::directory_iterator(projectDirectory, ec))
            {
                if (ec) { break; }
                std::error_code fileEc;
                if (!entry.is_regular_file(fileEc) ||
                    ToLower(entry.path().extension().string()) != ".dds")
                {
                    continue;
                }
                if (expectedNames.count(ToLower(entry.path().filename().string())) == 0)
                {
                    return true;
                }
            }
        }
        return false;
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

EditorAssetImportStatus InspectAssetImportStatus(std::string_view sourcePath,
    std::string_view projectPath)
{
    const fs::path source(sourcePath);
    const fs::path project(projectPath);
    std::error_code ec;
    if (!fs::exists(source, ec))
    {
        return EditorAssetImportStatus::Untracked;
    }
    ec.clear();
    if (!fs::exists(project, ec))
    {
        return EditorAssetImportStatus::Staged;
    }

    EditorAssetImportStatus manifestStatus = EditorAssetImportStatus::Untracked;
    if (InspectImportManifest(source, project, manifestStatus))
    {
        return manifestStatus;
    }

    ec.clear();
    if (fs::is_regular_file(source, ec))
    {
        ec.clear();
        if (!fs::is_regular_file(project, ec))
        {
            return EditorAssetImportStatus::Incomplete;
        }
        return WriteTimeOf(source) > WriteTimeOf(project) ?
            EditorAssetImportStatus::SourceNewer :
            EditorAssetImportStatus::UpToDate;
    }

    ec.clear();
    if (!fs::is_directory(source, ec))
    {
        return EditorAssetImportStatus::Untracked;
    }
    ec.clear();
    if (!fs::is_directory(project, ec))
    {
        return EditorAssetImportStatus::Incomplete;
    }

    bool hasRelevantSource = false;
    bool hasImageSource = false;
    bool missingOutput = false;
    bool sourceIsNewer = false;
    uint64_t newestImageSource = 0;

    for (fs::recursive_directory_iterator it(source,
             fs::directory_options::skip_permission_denied, ec), end;
         it != end;
         it.increment(ec))
    {
        if (ec) { break; }
        std::error_code fileEc;
        if (!it->is_regular_file(fileEc)) { continue; }

        const fs::path& path = it->path();
        const std::string ext = ToLower(path.extension().string());
        if (IsImportImageExtension(ext))
        {
            hasRelevantSource = true;
            hasImageSource = true;
            newestImageSource = std::max(newestImageSource, WriteTimeOf(path));
            continue;
        }
        if (!IsCopiedImportExtension(ext)) { continue; }

        hasRelevantSource = true;
        std::error_code relativeEc;
        const fs::path relative = fs::relative(path, source, relativeEc);
        const fs::path output = project / relative;
        std::error_code outputEc;
        if (relativeEc || !fs::is_regular_file(output, outputEc))
        {
            missingOutput = true;
        }
        else if (WriteTimeOf(path) > WriteTimeOf(output))
        {
            sourceIsNewer = true;
        }
    }

    if (hasImageSource)
    {
        bool hasDds = false;
        uint64_t oldestDds = std::numeric_limits<uint64_t>::max();
        ec.clear();
        for (fs::recursive_directory_iterator it(project,
                 fs::directory_options::skip_permission_denied, ec), end;
             it != end;
             it.increment(ec))
        {
            if (ec) { break; }
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc) ||
                ToLower(it->path().extension().string()) != ".dds")
            {
                continue;
            }
            hasDds = true;
            oldestDds = std::min(oldestDds, WriteTimeOf(it->path()));
        }
        if (!hasDds)
        {
            missingOutput = true;
        }
        else if (newestImageSource > oldestDds)
        {
            sourceIsNewer = true;
        }
    }

    if (!hasRelevantSource)
    {
        return EditorAssetImportStatus::Untracked;
    }
    if (LegacyTextureSetHasOrphanOutput(source, project))
    {
        sourceIsNewer = true;
    }
    if (missingOutput)
    {
        return EditorAssetImportStatus::Incomplete;
    }
    return sourceIsNewer ? EditorAssetImportStatus::SourceNewer :
        EditorAssetImportStatus::UpToDate;
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

                        // Imported texture-set presets point at
                        // textures/<staging-folder>/... . Carry the same import
                        // status as their generated texture records so the
                        // badge and one-click reimport also work from Materials.
                        if (it.value().is_object())
                        {
                            for (const char* textureKey : { "albedo", "mr", "normal" })
                            {
                                const auto textureIt = it.value().find(textureKey);
                                if (textureIt == it.value().end() || !textureIt->is_string())
                                {
                                    continue;
                                }
                                fs::path relativeTexture;
                                if (!RelativePathUnder(fs::path(textureIt->get<std::string>()),
                                        "textures", relativeTexture))
                                {
                                    continue;
                                }
                                const auto first = relativeTexture.begin();
                                if (first == relativeTexture.end() ||
                                    std::next(first) == relativeTexture.end())
                                {
                                    continue;
                                }
                                const fs::path source = fs::path("import_staging") / *first;
                                const fs::path project = fs::path("textures") / *first;
                                record.importStatus = InspectAssetImportStatus(
                                    source.generic_string(), project.generic_string());
                                if (record.importStatus != EditorAssetImportStatus::Untracked)
                                {
                                    record.importSourcePath = source.generic_string();
                                }
                                break;
                            }
                        }
                        assets_.push_back(std::move(record));
                    }
                }
            }
        }
    }

    // Resolve freshness per project resource. Generated siblings can depend on
    // different source maps, so editing/deleting one map must not dirty every
    // DDS in its import folder. The folder-wide work (manifest parse + source
    // listing) is cached per import folder — a palm has ~17 records that would
    // otherwise each re-parse the manifest and re-walk the staging tree.
    std::unordered_map<std::string, ImportFolderContext> folderContexts;
    for (EditorAssetRecord& record : assets_)
    {
        fs::path source;
        fs::path project;
        if (!ResolveImportPaths(record, source, project)) { continue; }

        bool isProjectResource = false;
        std::error_code ec;
        if (fs::is_regular_file(project, ec))
        {
            ec.clear();
            isProjectResource = fs::equivalent(fs::path(record.path), project, ec);
        }
        else
        {
            fs::path outputRelative;
            isProjectResource = RelativePathUnder(fs::path(record.path), project, outputRelative);
        }

        if (isProjectResource)
        {
            const std::string contextKey = project.generic_string();
            auto contextIt = folderContexts.find(contextKey);
            if (contextIt == folderContexts.end())
            {
                contextIt = folderContexts.emplace(
                    contextKey, BuildImportFolderContext(source, project)).first;
            }
            ResourceImportInspection inspection = InspectImportResource(
                contextIt->second, source, project, fs::path(record.path));
            record.importStatus = inspection.status;
            record.importSourceFiles = std::move(inspection.sourceFiles);
        }
        else
        {
            record.importStatus = InspectAssetImportStatus(
                source.generic_string(), project.generic_string());
        }
        if (record.importStatus != EditorAssetImportStatus::Untracked)
        {
            record.importSourcePath = source.generic_string();
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

    // The staging registry intentionally indexes only glTF/GLB assets, but H4
    // freshness must also react when a raw image, HDR, or external glTF buffer
    // changes. Include those source files in the lightweight disk signature
    // without flooding /Game/Staging with every texture in a download.
    const fs::path stagingPath("import_staging");
    std::error_code stagingEc;
    if (fs::is_directory(stagingPath, stagingEc))
    {
        for (fs::recursive_directory_iterator it(stagingPath,
                 fs::directory_options::skip_permission_denied, stagingEc), end;
             it != end;
             it.increment(stagingEc))
        {
            if (stagingEc) { break; }
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc)) { continue; }
            const std::string ext = ToLower(it->path().extension().string());
            if (!IsImportSourceExtension(ext)) { continue; }
            fingerprints.push_back(FingerprintPath(it->path(), 6, FileSizeOf(it->path())));
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

bool AssetRegistry::IsFileIndexedUnder(std::string_view virtualFolder, std::string_view fileName) const
{
    const std::string folder(virtualFolder);
    for (const DirRoot& root : AssetRoots())
    {
        const size_t rootLen = std::strlen(root.virtualRoot);
        const bool underRoot = folder.compare(0, rootLen, root.virtualRoot) == 0 &&
            (folder.size() == rootLen || folder[rootLen] == '/');
        if (!underRoot)
        {
            continue;
        }
        return !MatchExtension(fs::path(std::string(fileName)), root.extensions).empty();
    }
    return false;
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

const char* ToString(EditorAssetImportStatus status)
{
    switch (status)
    {
    case EditorAssetImportStatus::Untracked:   return "Untracked";
    case EditorAssetImportStatus::Staged:      return "Staged";
    case EditorAssetImportStatus::UpToDate:    return "Up to date";
    case EditorAssetImportStatus::SourceNewer: return "Source changed";
    case EditorAssetImportStatus::Incomplete:  return "Incomplete";
    }
    return "Untracked";
}

#endif // WITH_EDITOR
