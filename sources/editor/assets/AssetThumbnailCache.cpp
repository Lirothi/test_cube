#include "editor/assets/AssetThumbnailCache.h"
#include "rendering/core/BarrierTranslation.h"
#if WITH_EDITOR

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <wincodec.h>

#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/task/TaskSystem.h"
#include "core/diagnostics/DiagPaths.h"
#include "editor/assets/AssetRegistry.h"
#include "materials/MaterialData.h"
#include "materials/MaterialDataManager.h"
#include "materials/TextureDecodeCache.h"
#include "materials/TextureCube.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/meshes/Mesh.h"

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace editor_thumbnail_detail
{
    struct DiskCacheInfo
    {
        bool enabled = false;
        bool hit = false;
        bool invalidFile = false;
        std::uint64_t signature = 0;
        std::string stablePrefix;
        std::string path;
    };

    struct PreflightInput
    {
        std::string key;
        std::string path;
        PreflightKind kind = PreflightKind::Mesh;
        std::uint64_t sourceWriteTime = 0;
        std::uint64_t registryRevision = 0;
        std::uint64_t token = 0;
        std::uint64_t epoch = 0;
    };

    struct PreflightResult
    {
        PreflightInput input;
        DiskCacheInfo cache;
        std::vector<std::string> staleCachePaths;
        std::string resolvedPath;
        std::shared_ptr<MeshCpuData> meshData;
        std::vector<std::string> meshMaterialSlots;
        std::vector<uint32_t> recomputeNormalSlots;
        std::string failureReason;
    };

    struct PreflightState
    {
        std::mutex mutex;
        std::deque<PreflightResult> completed;
        std::atomic<std::size_t> jobsInFlight{ 0 };
    };

    struct PreviewInitState
    {
        // 0 = not started, 1 = running, 2 = ready, -1 = failed.
        std::atomic<int> status{ 0 };
    };
}

namespace
{
    // Bump this whenever preview rendering or PNG encoding semantics change.
    constexpr std::uint32_t kThumbnailSchemaVersion = 8;

    // The render graph already occupies the worker pool. Keep filesystem and JSON
    // preflight work bounded so opening a large folder does not compete with it.
    constexpr std::size_t kMaxPreflightJobsInFlight = 2;

    // LRU cap. Each resident thumbnail also holds kFrameCount ImGui preview
    // descriptors, so this stays comfortably under ImGuiLayer's editor SRV heap
    // (512): 96 * 3 = 288, leaving room for the font/icon atlases and debug views.
    constexpr std::size_t kMaxCachedThumbnails = 96;

    // Offscreen thumbnail render resolution (matches EditorPreviewRenderer).
    constexpr std::uint32_t kPreviewRenderSize = 256;
    constexpr const char* kThumbnailCacheDirectory = "editor_cache/thumbnails";
    constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;

    namespace fs = std::filesystem;
    using Microsoft::WRL::ComPtr;

    using editor_thumbnail_detail::DiskCacheInfo;
    using editor_thumbnail_detail::PreflightInput;
    using editor_thumbnail_detail::PreflightKind;
    using editor_thumbnail_detail::PreflightResult;
    using editor_thumbnail_detail::PreflightState;

    class ScopedComInitialization
    {
    public:
        ScopedComInitialization()
            : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
            , uninitialize_(SUCCEEDED(result_))
        {
        }

        ~ScopedComInitialization()
        {
            if (uninitialize_)
            {
                CoUninitialize();
            }
        }

        bool IsAvailable() const
        {
            // The current thread can already belong to an STA. WIC remains usable
            // in that case; only a real initialization failure must stop encoding.
            return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
        }

    private:
        HRESULT result_ = E_FAIL;
        bool uninitialize_ = false;
    };

    struct ThumbnailReadback
    {
        ComPtr<ID3D12Resource> resource;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT width = 0;
        UINT height = 0;
    };

    void HashBytes(std::uint64_t& hash, const void* bytes, std::size_t count)
    {
        const auto* data = static_cast<const std::uint8_t*>(bytes);
        for (std::size_t i = 0; i < count; ++i)
        {
            hash ^= data[i];
            hash *= kFnvPrime;
        }
    }

    template <typename T>
    void HashValue(std::uint64_t& hash, const T& value)
    {
        HashBytes(hash, &value, sizeof(value));
    }

    void HashText(std::uint64_t& hash, const std::string& value)
    {
        const std::uint64_t length = value.size();
        HashValue(hash, length);
        HashBytes(hash, value.data(), value.size());
    }

    std::uint64_t FileWriteTime(const fs::path& path)
    {
        std::error_code error;
        const fs::file_time_type time = fs::last_write_time(path, error);
        return error ? 0ull : static_cast<std::uint64_t>(time.time_since_epoch().count());
    }

    std::string FilePart(const std::string& path)
    {
        const size_t fragment = path.find('#');
        return fragment == std::string::npos ? path : path.substr(0, fragment);
    }

    bool EndsWithNoCase(const std::string& text, const char* suffix)
    {
        const size_t length = std::strlen(suffix);
        if (text.size() < length) { return false; }
        const size_t offset = text.size() - length;
        for (size_t i = 0; i < length; ++i)
        {
            if (std::tolower(static_cast<unsigned char>(text[offset + i])) !=
                std::tolower(static_cast<unsigned char>(suffix[i])))
            {
                return false;
            }
        }
        return true;
    }

    bool ResolveMeshSource(const std::string& assetPath,
        std::string& resolvedPath,
        std::vector<std::string>& materialSlots,
        std::vector<uint32_t>& recomputeNormalSlots,
        std::string& failureReason)
    {
        resolvedPath = assetPath;
        materialSlots.clear();
        recomputeNormalSlots.clear();
        if (!EndsWithNoCase(FilePart(assetPath), ".mesh.json"))
        {
            return true;
        }

        std::ifstream file(FilePart(assetPath));
        if (!file)
        {
            failureReason = "Mesh asset JSON could not be opened.";
            return false;
        }
        const nlohmann::json asset = nlohmann::json::parse(file, nullptr, false, true);
        if (asset.is_discarded() || !asset.is_object())
        {
            failureReason = "Mesh asset JSON is invalid.";
            return false;
        }
        const auto geometry = asset.find("geometry");
        const auto model = asset.find("model");
        if (geometry != asset.end() && geometry->is_string())
        {
            resolvedPath = geometry->get<std::string>();
        }
        else if (model != asset.end() && model->is_string())
        {
            resolvedPath = model->get<std::string>();
        }
        else
        {
            failureReason = "Mesh asset JSON has no geometry path.";
            return false;
        }

        // Match SceneObjectFactory's material precedence: per-submesh `materials`
        // wins; a legacy scalar `material` is a slot-zero fallback.
        const auto materials = asset.find("materials");
        if (materials != asset.end() && materials->is_array())
        {
            for (const nlohmann::json& value : *materials)
            {
                materialSlots.push_back(value.is_string()
                    ? value.get<std::string>()
                    : std::string{});
            }
        }
        else
        {
            const auto material = asset.find("material");
            if (material != asset.end() && material->is_string())
            {
                materialSlots.push_back(material->get<std::string>());
            }
        }

        const auto normalSlots = asset.find("recomputeNormalSlots");
        if (normalSlots != asset.end() && normalSlots->is_array())
        {
            for (const nlohmann::json& value : *normalSlots)
            {
                if (!value.is_number_integer()) { continue; }
                const int64_t slot = value.get<int64_t>();
                if (slot >= 0)
                {
                    recomputeNormalSlots.push_back(static_cast<uint32_t>(slot));
                }
            }
            std::sort(recomputeNormalSlots.begin(), recomputeNormalSlots.end());
            recomputeNormalSlots.erase(
                std::unique(recomputeNormalSlots.begin(), recomputeNormalSlots.end()),
                recomputeNormalSlots.end());
        }
        return !resolvedPath.empty();
    }

    std::string Hex(std::uint64_t value)
    {
        constexpr char digits[] = "0123456789abcdef";
        std::string text(16, '0');
        for (int index = 15; index >= 0; --index)
        {
            text[static_cast<std::size_t>(index)] = digits[value & 0xfu];
            value >>= 4u;
        }
        return text;
    }

    void HashMaterialDependencies(std::uint64_t& hash, const std::string& presetKey)
    {
        // I0: materials are per-file assets (data/materials/<name>.json, flat object). The
        // monolithic data/materials.json remains a legacy fallback during migration.
        nlohmann::json preset;
        const std::string perFilePath = "data/materials/" + presetKey + ".json";
        if (std::ifstream perFile{ perFilePath })
        {
            HashText(hash, perFilePath);
            HashValue(hash, FileWriteTime(perFilePath));

            std::stringstream contents;
            contents << perFile.rdbuf();
            const nlohmann::json document =
                nlohmann::json::parse(contents.str(), nullptr, false, /*ignore_comments=*/true);
            if (document.is_discarded() || !document.is_object())
            {
                HashText(hash, "invalid-material-file");
                return;
            }
            preset = document;
        }
        else
        {
            constexpr const char* kMaterialsJson = "data/materials.json";
            HashText(hash, kMaterialsJson);
            HashValue(hash, FileWriteTime(kMaterialsJson));

            std::ifstream file(kMaterialsJson);
            if (!file)
            {
                HashText(hash, "missing-materials-json");
                return;
            }

            std::stringstream contents;
            contents << file.rdbuf();
            const nlohmann::json document =
                nlohmann::json::parse(contents.str(), nullptr, false, /*ignore_comments=*/true);
            if (document.is_discarded() || !document.contains("presets") ||
                !document["presets"].is_object())
            {
                HashText(hash, "invalid-materials-json");
                return;
            }

            const auto entry = document["presets"].find(presetKey);
            if (entry == document["presets"].end() || !entry->is_object())
            {
                HashText(hash, "missing-material-preset");
                return;
            }
            preset = *entry;
        }

        for (const char* name : { "albedo", "mr", "normal" })
        {
            HashText(hash, name);
            const auto dependency = preset.find(name);
            const std::string path = dependency != preset.end() && dependency->is_string()
                ? dependency->get<std::string>()
                : std::string{};
            HashText(hash, path);
            HashValue(hash, FileWriteTime(path));

            // Texture2D prefers an imported DDS sibling over the source path.
            // Track both so a texture re-import also invalidates the preview.
            if (!path.empty() && !EndsWithNoCase(path, ".dds"))
            {
                fs::path ddsPath(path);
                ddsPath.replace_extension(".dds");
                const std::string dds = ddsPath.generic_string();
                HashText(hash, dds);
                HashValue(hash, FileWriteTime(dds));
            }
        }
    }

    void HashGltfMaterialDependencies(std::uint64_t& hash,
        const std::string& gltfSelector, int groupOrdinal)
    {
        const GltfMaterialDesc material =
            MeshManager::DescribeGltfMaterial(gltfSelector, groupOrdinal);
        HashValue(hash, material.valid);
        for (const std::string* path : { &material.albedoPath, &material.mrPath,
                                         &material.normalPath, &material.emissivePath })
        {
            HashText(hash, *path);
            HashValue(hash, FileWriteTime(*path));
            if (!path->empty() && !EndsWithNoCase(*path, ".dds"))
            {
                fs::path ddsPath(*path);
                ddsPath.replace_extension(".dds");
                const std::string dds = ddsPath.generic_string();
                HashText(hash, dds);
                HashValue(hash, FileWriteTime(dds));
            }
        }
    }

    DiskCacheInfo BuildDiskCacheInfo(const PreflightInput& input,
        const std::string& resolvedPath,
        const std::vector<std::string>& meshMaterialSlots,
        const std::vector<uint32_t>& recomputeNormalSlots)
    {
        DiskCacheInfo info;
        const std::uint32_t type = input.kind == PreflightKind::Mesh
            ? static_cast<std::uint32_t>(EditorAssetType::Mesh)
            : input.kind == PreflightKind::Material
                ? static_cast<std::uint32_t>(EditorAssetType::MaterialPreset)
                : static_cast<std::uint32_t>(EditorAssetType::Texture);

        std::uint64_t stableHash = kFnvOffset;
        HashValue(stableHash, type);
        HashText(stableHash, input.key);

        std::uint64_t signature = kFnvOffset;
        HashValue(signature, kThumbnailSchemaVersion);
        HashValue(signature, type);
        HashText(signature, input.key);
        HashText(signature, input.path);
        HashValue(signature, input.sourceWriteTime);
        if (input.kind == PreflightKind::Mesh)
        {
            HashText(signature, resolvedPath);
            HashValue(signature, FileWriteTime(FilePart(resolvedPath)));
            HashValue(signature, recomputeNormalSlots.size());
            for (const uint32_t slot : recomputeNormalSlots) { HashValue(signature, slot); }
            const std::string geometryPath = FilePart(resolvedPath);
            const bool gltfGeometry = EndsWithNoCase(geometryPath, ".gltf") ||
                EndsWithNoCase(geometryPath, ".glb");
            const std::size_t gltfSlots = gltfGeometry
                ? std::max<std::size_t>(1, MeshManager::CountSubmeshes(resolvedPath))
                : 0;
            const std::size_t materialSlotCount = std::max(
                meshMaterialSlots.size(), gltfSlots);
            for (std::size_t slot = 0; slot < materialSlotCount; ++slot)
            {
                const std::string& material = slot < meshMaterialSlots.size()
                    ? meshMaterialSlots[slot]
                    : std::string("auto");
                HashText(signature, material);
                if (material == "auto" || (material.empty() && gltfGeometry))
                {
                    HashGltfMaterialDependencies(signature, resolvedPath,
                        static_cast<int>(slot));
                }
                else if (!material.empty())
                {
                    HashMaterialDependencies(signature, material);
                }
            }
        }
        if (input.kind == PreflightKind::Material)
        {
            HashMaterialDependencies(signature, input.key);
        }

        info.enabled = true;
        info.signature = signature;
        info.stablePrefix = Hex(stableHash) + "_";
        info.path = (fs::path(kThumbnailCacheDirectory) /
            (info.stablePrefix + Hex(signature) + ".png")).generic_string();
        std::error_code error;
        info.hit = fs::is_regular_file(fs::path(info.path), error) && !error;
        const std::uintmax_t cacheSize = info.hit
            ? fs::file_size(fs::path(info.path), error)
            : 0;
        if (info.hit && !error && cacheSize == 0)
        {
            // The main thread removes this after validating the async result. A
            // worker must not race a later cache write for the same asset.
            info.hit = false;
            info.invalidFile = true;
        }
        return info;
    }

    std::vector<std::string> FindStaleDiskCacheFiles(const DiskCacheInfo& info)
    {
        std::vector<std::string> stalePaths;
        if (!info.enabled)
        {
            return stalePaths;
        }

        const fs::path cachePath(info.path);
        std::error_code error;
        const fs::path directory = cachePath.parent_path();
        for (fs::directory_iterator it(directory, error), end; !error && it != end;
             it.increment(error))
        {
            if (!it->is_regular_file(error) || error)
            {
                error.clear();
                continue;
            }
            const fs::path candidate = it->path();
            const std::string filename = candidate.filename().string();
            if (candidate != cachePath && filename.rfind(info.stablePrefix, 0) == 0 &&
                candidate.extension() == ".png")
            {
                stalePaths.push_back(candidate.generic_string());
            }
        }
        return stalePaths;
    }

    void RemoveDiskCacheFile(const std::string& path)
    {
        if (path.empty())
        {
            return;
        }
        std::error_code error;
        fs::remove(fs::path(path), error);
    }

    bool RecordThumbnailReadback(ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        ID3D12Resource* color,
        ThumbnailReadback& out)
    {
        if (!device || !commandList || !color)
        {
            return false;
        }

        const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
        if (colorDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            colorDesc.Width == 0 || colorDesc.Height == 0 ||
            colorDesc.Width > UINT_MAX || colorDesc.Height > UINT_MAX)
        {
            return false;
        }

        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 totalBytes = 0;
        device->GetCopyableFootprints(&colorDesc, 0, 1, 0, &out.footprint,
            &rows, &rowSize, &totalBytes);
        if (rows != colorDesc.Height || rowSize != colorDesc.Width * 4 || totalBytes == 0)
        {
            return false;
        }

        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = totalBytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&out.resource))))
        {
            return false;
        }

        auto barrier = [commandList](ID3D12Resource* resource,
            D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
        {
            D3D12_RESOURCE_BARRIER transition{};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition.pResource = resource;
            transition.Transition.StateBefore = before;
            transition.Transition.StateAfter = after;
            transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers::EmitOne(commandList, transition);
        };

        barrier(color, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = color;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = out.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = out.footprint;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier(color, D3D12_RESOURCE_STATE_COPY_SOURCE,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        out.width = static_cast<UINT>(colorDesc.Width);
        out.height = colorDesc.Height;
        return true;
    }

    bool WriteThumbnailPng(const ThumbnailReadback& readback, const std::string& path)
    {
        if (!readback.resource || readback.width == 0 || readback.height == 0 || path.empty())
        {
            return false;
        }

        ScopedComInitialization com;
        if (!com.IsAvailable())
        {
            return false;
        }

        const fs::path outputPath(path);
        fs::path temporaryPath = outputPath;
        temporaryPath += ".tmp";
        std::error_code error;
        fs::create_directories(outputPath.parent_path(), error);
        if (error)
        {
            return false;
        }
        fs::remove(temporaryPath, error);
        error.clear();

        const UINT stride = readback.width * 4;
        const std::size_t byteCount = static_cast<std::size_t>(stride) * readback.height;
        std::vector<std::uint8_t> pixels(byteCount);
        const UINT64 readEnd = readback.footprint.Offset +
            static_cast<UINT64>(readback.footprint.Footprint.RowPitch) * readback.height;
        D3D12_RANGE readRange{ static_cast<SIZE_T>(readback.footprint.Offset),
            static_cast<SIZE_T>(readEnd) };
        void* mapped = nullptr;
        if (FAILED(readback.resource->Map(0, &readRange, &mapped)))
        {
            return false;
        }
        const auto* source = static_cast<const std::uint8_t*>(mapped) + readback.footprint.Offset;
        for (UINT row = 0; row < readback.height; ++row)
        {
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * stride,
                source + static_cast<std::size_t>(row) * readback.footprint.Footprint.RowPitch,
                stride);
        }
        readback.resource->Unmap(0, nullptr);

        // GPU readback is RGBA; WIC's PNG encoder consumes the source bytes as BGRA.
        // The encoder may report GUID_WICPixelFormatDontCare after accepting this format.
        for (std::size_t offset = 0; offset < pixels.size(); offset += 4)
        {
            std::swap(pixels[offset], pixels[offset + 2]);
        }

        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory))))
        {
            return false;
        }
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        const std::wstring temporaryPathWide = temporaryPath.wstring();
        const auto cleanTemporary = [&]()
        {
            frame.Reset();
            options.Reset();
            encoder.Reset();
            stream.Reset();
            factory.Reset();
            std::error_code removeError;
            fs::remove(temporaryPath, removeError);
        };
        if (FAILED(factory->CreateStream(&stream)) ||
            FAILED(stream->InitializeFromFilename(temporaryPathWide.c_str(), GENERIC_WRITE)) ||
            FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, &options)) ||
            FAILED(frame->Initialize(options.Get())) ||
            FAILED(frame->SetSize(readback.width, readback.height)))
        {
            cleanTemporary();
            return false;
        }

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&format)) ||
            FAILED(frame->WritePixels(readback.height, stride,
                static_cast<UINT>(pixels.size()), pixels.data())) ||
            FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        {
            cleanTemporary();
            return false;
        }

        frame.Reset();
        options.Reset();
        encoder.Reset();
        stream.Reset();
        factory.Reset();
        fs::remove(outputPath, error);
        error.clear();
        fs::rename(temporaryPath, outputPath, error);
        if (error)
        {
            fs::remove(temporaryPath, error);
            return false;
        }
        return true;
    }

    std::wstring WidenAscii(const std::string& text)
    {
        // Asset paths under the content roots are ASCII; a widening copy is enough
        // and avoids pulling in a locale-aware conversion.
        return std::wstring(text.begin(), text.end());
    }

    Texture2D::Usage UsageForRecord(const EditorAssetRecord& record)
    {
        // PNGs here are authored as sRGB 8-bit; DDS files carry their sRGB intent
        // in the recorded format string. Anything else is shown as raw linear
        // data. This only drives the preview's tone, not the stored bytes.
        if (record.extension == ".png")
        {
            return Texture2D::Usage::AlbedoSRGB;
        }
        if (record.texture.format.find("SRGB") != std::string::npos ||
            record.texture.format.find("Srgb") != std::string::npos)
        {
            return Texture2D::Usage::AlbedoSRGB;
        }
        return Texture2D::Usage::LinearData;
    }
}

struct AssetThumbnailCache::GpuJob
{
    PendingLoad load;
    std::unique_ptr<UploadBatch> commands;
    Texture2D texture;
    TextureCube cube;
    std::shared_ptr<Mesh> mesh;
    std::shared_ptr<MaterialData> material;
    std::vector<std::shared_ptr<MaterialData>> meshMaterials;
    Microsoft::WRL::ComPtr<ID3D12Resource> rendered;
    ThumbnailReadback readback;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::uint64_t fenceValue = 1;
    bool hasReadback = false;
    bool ok = false;
};

AssetThumbnailCache::AssetThumbnailCache()
    : preflightState_(std::make_shared<PreflightState>())
    , previewInitState_(std::make_shared<editor_thumbnail_detail::PreviewInitState>())
    , preview_(std::make_shared<EditorPreviewRenderer>())
{
}

AssetThumbnailCache::~AssetThumbnailCache() = default;

void AssetThumbnailCache::StartPreviewInitialization(Renderer& renderer)
{
    if (!previewInitState_ || !preview_) { return; }
    int expected = 0;
    if (!previewInitState_->status.compare_exchange_strong(expected, 1,
            std::memory_order_acq_rel))
    {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device = renderer.GetDevice();
    if (!device)
    {
        previewInitState_->status.store(-1, std::memory_order_release);
        return;
    }

    const std::shared_ptr<EditorPreviewRenderer> preview = preview_;
    const std::shared_ptr<editor_thumbnail_detail::PreviewInitState> state =
        previewInitState_;
    auto initialize = [preview, state, device]()
    {
        const bool initialized = preview->EnsureInitialized(device.Get());
        state->status.store(initialized ? 2 : -1, std::memory_order_release);
    };
#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    TaskSystem::Get().SubmitDetach(std::move(initialize));
#else
    initialize();
#endif
}

void AssetThumbnailCache::BeginFrame(Renderer& renderer)
{
    ++frameCounter_;
    CommitPreflightResults(renderer);
    LaunchPreflightJobs();
}

void AssetThumbnailCache::QueuePreflight(const EditorAssetRecord& record,
    PendingLoad::Kind generationKind,
    std::uint64_t assetRegistryRevision,
    Entry& entry)
{
    PreflightKind kind;
    switch (generationKind)
    {
    case PendingLoad::Kind::Mesh:     kind = PreflightKind::Mesh; break;
    case PendingLoad::Kind::Material: kind = PreflightKind::Material; break;
    case PendingLoad::Kind::Cube:     kind = PreflightKind::Cube; break;
    default:                          return;
    }

    entry.generationKind = kind;
    entry.sourceWriteTime = record.fileWriteTime;
    entry.registryRevision = assetRegistryRevision;
    entry.preflightPending = true;
    ++entry.preflightToken;
    preflightQueue_.push_back({
        record.id.key,
        record.id.type == EditorAssetType::Mesh ? record.id.key : record.path,
        kind,
        record.fileWriteTime,
        assetRegistryRevision,
        entry.preflightToken,
        preflightEpoch_
    });
}

// Hand this load's material textures to a worker to DECODE, long before StartGpuJob wants them.
//
// The main thread keeps doing the GPU upload — that needs the command list — but the expensive
// half (WIC decode + CPU box-filter mip chain) no longer happens on it. Measured before this:
// 2108 ms inside `materials` for one rock whose preset points at unimported 25/21/12 MB staging
// PNGs, all of it on the main thread to draw a 256-pixel icon (logs/thumbnail_profile.log).
//
// Reading the preset table happens HERE, on the main thread, because that table is reloaded from
// the main thread too; only the resolved paths cross to the worker. And nothing waits on the
// result: a job that reaches StartGpuJob before its decode lands simply decodes inline, exactly as
// it always did. The cache is an accelerator, never a dependency.
void AssetThumbnailCache::PrewarmMaterialTextures(const PendingLoad& load)
{
    if (!preview_) { return; }
    if (load.kind != PendingLoad::Kind::Mesh && load.kind != PendingLoad::Kind::Material) { return; }

    // The preset table is loaded lazily by StartGpuJob, which runs AFTER this in ProcessPending.
    // Reading it first meant FindPreset returned null and `work` came out empty — no ticket, no
    // gate, and the decode happened on the main thread after all. Idempotent and cheap.
    preview_->EnsurePresets();

    std::vector<Texture2D::CreateDesc> work;
    const MaterialDataManager& materials = preview_->Materials();
    const auto add = [&work](const std::wstring& path, Texture2D::Usage usage, bool normalIsRG)
    {
        if (path.empty()) { return; }
        Texture2D::CreateDesc d;
        d.path = path;
        d.usage = usage;
        d.normalIsRG = normalIsRG;
        work.push_back(std::move(d));
    };
    const auto addPreset = [&](const std::string& name)
    {
        if (name.empty() || name == "auto") { return; }
        const MaterialPreset* preset = materials.FindPreset(name);
        if (!preset) { return; }
        add(preset->albedoPath, Texture2D::Usage::AlbedoSRGB, false);
        add(preset->mrPath, Texture2D::Usage::MetalRough, false);
        add(preset->normalPath, Texture2D::Usage::NormalMap, preset->normalIsRG);
    };

    if (load.kind == PendingLoad::Kind::Material) { addPreset(load.presetKey); }
    for (const std::string& slot : load.meshMaterialSlots) { addPreset(slot); }
    lastPrewarmSlots_ = static_cast<std::uint32_t>(load.meshMaterialSlots.size());
    lastPrewarmDescs_ = static_cast<std::uint32_t>(work.size());
    if (work.empty()) { return; }

    auto ticket = std::make_shared<DecodeTicket>();
    decodeTickets_[load.key] = ticket;
    // SubmitDetach, not DispatchTrack: the frame joins TRACKED tasks at its top (App.cpp), so a
    // tracked decode would stall the very frame it is meant to keep free.
    TaskSystem::Get().SubmitDetach([work = std::move(work), ticket]() mutable
    {
        CPU_SCOPE(ProfilerScopes::kAssetThumbnailPreflight);
        for (const Texture2D::CreateDesc& d : work) { texdecode::Prewarm(d); }
        ticket->done.store(true, std::memory_order_release);
    });
}

void AssetThumbnailCache::LaunchPreflightJobs()
{
    if (!preflightState_)
    {
        return;
    }

    while (!preflightQueue_.empty() &&
           preflightState_->jobsInFlight.load(std::memory_order_acquire) <
               kMaxPreflightJobsInFlight)
    {
        PreflightRequest request = std::move(preflightQueue_.front());
        preflightQueue_.pop_front();

        PreflightInput input;
        input.key = std::move(request.key);
        input.path = std::move(request.path);
        input.kind = request.generationKind;
        input.sourceWriteTime = request.sourceWriteTime;
        input.registryRevision = request.registryRevision;
        input.token = request.token;
        input.epoch = request.epoch;

        const std::shared_ptr<PreflightState> state = preflightState_;
        state->jobsInFlight.fetch_add(1, std::memory_order_acq_rel);
        auto job = [state, input = std::move(input)]() mutable
        {
            CPU_SCOPE(ProfilerScopes::kAssetThumbnailPreflight);
            PreflightResult result;
            result.input = std::move(input);
            result.resolvedPath = result.input.path;
            if (result.input.kind == PreflightKind::Mesh)
            {
                ResolveMeshSource(result.input.path, result.resolvedPath,
                    result.meshMaterialSlots,
                    result.recomputeNormalSlots,
                    result.failureReason);
            }
            result.cache = BuildDiskCacheInfo(result.input, result.resolvedPath,
                result.meshMaterialSlots, result.recomputeNormalSlots);
            result.staleCachePaths = FindStaleDiskCacheFiles(result.cache);
            if (result.input.kind == PreflightKind::Mesh &&
                !result.cache.hit && result.failureReason.empty())
            {
                result.meshData = std::make_shared<MeshCpuData>();
                MeshManager parser;
                MeshLoadOptions options;
                options.wantCW = false;
                options.recomputeNormalSlots = result.recomputeNormalSlots;
                if (!parser.ParseFileCpu(result.resolvedPath, *result.meshData, options))
                {
                    result.meshData.reset();
                    result.failureReason = "Referenced mesh geometry could not be parsed.";
                }
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->completed.push_back(std::move(result));
            }
            state->jobsInFlight.fetch_sub(1, std::memory_order_release);
        };

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
        TaskSystem::Get().SubmitDetach(std::move(job));
#else
        job();
#endif
    }
}

void AssetThumbnailCache::CommitPreflightResults(Renderer& renderer)
{
    CPU_SCOPE(ProfilerScopes::kAssetThumbnailCommitPreflight);
    if (!preflightState_)
    {
        return;
    }

    std::deque<PreflightResult> completed;
    {
        std::lock_guard<std::mutex> lock(preflightState_->mutex);
        completed.swap(preflightState_->completed);
    }

    for (PreflightResult& result : completed)
    {
        const PreflightInput& input = result.input;
        if (input.epoch != preflightEpoch_)
        {
            continue;
        }

        const auto it = entries_.find(input.key);
        if (it == entries_.end())
        {
            continue;
        }

        Entry& entry = it->second;
        if (!entry.preflightPending ||
            entry.preflightToken != input.token ||
            entry.generationKind != input.kind ||
            entry.sourceWriteTime != input.sourceWriteTime ||
            entry.registryRevision != input.registryRevision)
        {
            continue;
        }

        entry.preflightPending = false;
        if (result.cache.invalidFile)
        {
            RemoveDiskCacheFile(result.cache.path);
        }
        for (const std::string& stalePath : result.staleCachePaths)
        {
            RemoveDiskCacheFile(stalePath);
        }

        const bool cacheChanged = entry.cacheSignature != result.cache.signature ||
            result.cache.invalidFile;
        const bool needsLoad = entry.state == State::Preflighting ||
            entry.state == State::Missing || cacheChanged;
        entry.cacheSignature = result.cache.signature;
        entry.cachePath = result.cache.path;
        if (!needsLoad)
        {
            continue;
        }

        if (entry.resource)
        {
            ReleaseEntry(renderer, entry);
        }

        if (!result.cache.hit && !result.failureReason.empty())
        {
            entry.state = State::Failed;
            entry.failureReason = result.failureReason;
            continue;
        }

        PendingLoad::Kind generationKind = PendingLoad::Kind::Mesh;
        switch (input.kind)
        {
        case PreflightKind::Mesh:     generationKind = PendingLoad::Kind::Mesh; break;
        case PreflightKind::Material: generationKind = PendingLoad::Kind::Material; break;
        case PreflightKind::Cube:     generationKind = PendingLoad::Kind::Cube; break;
        }

        entry.state = State::Queued;
        entry.failureReason.clear();
        PendingLoad load;
        load.key = input.key;
        load.kind = result.cache.hit ? PendingLoad::Kind::DiskCache : generationKind;
        load.generationKind = generationKind;
        load.sourceWriteTime = input.sourceWriteTime;
        load.cacheSignature = result.cache.signature;
        load.sourcePath = input.path;
        load.path = result.resolvedPath.empty() ? input.path : result.resolvedPath;
        load.cachePath = result.cache.path;
        load.presetKey = input.key;
        load.meshData = std::move(result.meshData);
        load.meshMaterialSlots = std::move(result.meshMaterialSlots);
        PrewarmMaterialTextures(load);
        queue_.push_back(std::move(load));
    }
}

AssetThumbnailCache::View AssetThumbnailCache::Request(Renderer& renderer,
    const EditorAssetRecord& record, std::uint64_t assetRegistryRevision)
{
    CPU_SCOPE(ProfilerScopes::kAssetThumbnailRequest);
    View view;

    PendingLoad::Kind kind;
    switch (record.id.type)
    {
    case EditorAssetType::Texture:
        kind = record.texture.kind == EditorTextureKind::TextureCube
            ? PendingLoad::Kind::Cube
            : PendingLoad::Kind::Texture;
        break;
    case EditorAssetType::Mesh:           kind = PendingLoad::Kind::Mesh; break;
    case EditorAssetType::MaterialPreset: kind = PendingLoad::Kind::Material; break;
    default:
        return view; // Levels, shaders, unknown: no preview.
    }

    const bool requiresPreflight = kind == PendingLoad::Kind::Mesh ||
        kind == PendingLoad::Kind::Material || kind == PendingLoad::Kind::Cube;
    const std::string& key = record.id.key;
    auto it = entries_.find(key);
    if (it == entries_.end())
    {
        Entry entry;
        entry.state = requiresPreflight ? State::Preflighting : State::Queued;
        entry.sourceWriteTime = record.fileWriteTime;
        entry.registryRevision = assetRegistryRevision;
        entry.lastRequestedFrame = frameCounter_;
        it = entries_.emplace(key, std::move(entry)).first;

        if (requiresPreflight)
        {
            QueuePreflight(record, kind, assetRegistryRevision, it->second);
        }
        else
        {
            PendingLoad load;
            load.key = key;
            load.kind = kind;
            load.generationKind = kind;
            load.sourceWriteTime = record.fileWriteTime;
            load.sourcePath = record.path;
            load.path = record.path;
            load.presetKey = record.id.key;
            load.usage = UsageForRecord(record);
            queue_.push_back(std::move(load));
        }
    }
    else
    {
        Entry& entry = it->second;
        const bool sourceChanged = entry.sourceWriteTime != record.fileWriteTime;
        const bool registryChanged = entry.registryRevision != assetRegistryRevision;
        if (sourceChanged)
        {
            // Do not display a thumbnail from an edited source while a fresh
            // preflight determines the cache and dependency signature.
            ReleaseEntry(renderer, entry);
            entry.state = requiresPreflight ? State::Preflighting : State::Queued;
            entry.cacheSignature = 0;
            entry.cachePath.clear();
            if (requiresPreflight)
            {
                QueuePreflight(record, kind, assetRegistryRevision, entry);
            }
            else
            {
                entry.sourceWriteTime = record.fileWriteTime;
                entry.registryRevision = assetRegistryRevision;
                PendingLoad load;
                load.key = key;
                load.kind = kind;
                load.generationKind = kind;
                load.sourceWriteTime = record.fileWriteTime;
                load.sourcePath = record.path;
                load.path = record.path;
                load.presetKey = record.id.key;
                load.usage = UsageForRecord(record);
                queue_.push_back(std::move(load));
            }
        }
        else if (registryChanged)
        {
            if (requiresPreflight)
            {
                // A queued source load must wait for the new dependency probe;
                // ready previews remain drawable until a signature changes.
                if (entry.state == State::Queued)
                {
                    entry.state = State::Preflighting;
                }
                QueuePreflight(record, kind, assetRegistryRevision, entry);
            }
            else
            {
                entry.registryRevision = assetRegistryRevision;
            }
        }
    }

    Entry& entry = it->second;
    entry.lastRequestedFrame = frameCounter_;
    view.state = entry.state;
    view.width = entry.width;
    view.height = entry.height;

    if (entry.state == State::Ready && entry.resource)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = entry.srvFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        // Expose the full mip chain so ImGui's sampler can minify smoothly when
        // the thumbnail is drawn much smaller than the source (DDS ships mips;
        // rendered previews are single-mip).
        srvDesc.Texture2D.MipLevels = entry.mipLevels;
        renderer.MarkImGuiTextureShaderReadable(entry.resource.Get());
        view.texture = renderer.CreateImGuiTextureId(entry.resource.Get(), srvDesc);
    }
    else if (entry.state == State::Failed)
    {
        view.failureReason = entry.failureReason.empty()
            ? "Preview generation failed."
            : entry.failureReason.c_str();
    }

    return view;
}

bool AssetThumbnailCache::CommitGpuJob(Renderer& renderer)
{
    if (!gpuJob_) { return true; }
    if (!gpuJob_->fence ||
        gpuJob_->fence->GetCompletedValue() < gpuJob_->fenceValue)
    {
        return false;
    }

    std::unique_ptr<GpuJob> job = std::move(gpuJob_);
    const auto entryIt = entries_.find(job->load.key);
    if (entryIt != entries_.end())
    {
        Entry& entry = entryIt->second;
        const bool current = entry.state == State::Generating &&
            entry.sourceWriteTime == job->load.sourceWriteTime &&
            entry.cacheSignature == job->load.cacheSignature;
        if (current)
        {
            if (job->load.kind == PendingLoad::Kind::Texture ||
                job->load.kind == PendingLoad::Kind::DiskCache)
            {
                if (job->ok && job->texture.GetResource())
                {
                    const D3D12_RESOURCE_DESC desc =
                        job->texture.GetResource()->GetDesc();
                    entry.resource = job->texture.GetResource();
                    entry.srvFormat = job->texture.GetSrvFormat();
                    entry.width = job->texture.GetWidth();
                    entry.height = job->texture.GetHeight();
                    entry.mipLevels = std::max<UINT>(desc.MipLevels, 1u);
                    entry.state = State::Ready;
                    entry.failureReason.clear();
                }
                else if (job->load.kind == PendingLoad::Kind::DiskCache)
                {
                    // Re-run preflight so a corrupt PNG falls back to worker-side
                    // mesh parsing instead of doing heavy CPU work here.
                    RemoveDiskCacheFile(job->load.cachePath);
                    entry.state = State::Preflighting;
                    entry.failureReason.clear();
                    entry.preflightPending = true;
                    ++entry.preflightToken;
                    preflightQueue_.push_back({
                        job->load.key,
                        job->load.sourcePath.empty()
                            ? job->load.path
                            : job->load.sourcePath,
                        entry.generationKind,
                        entry.sourceWriteTime,
                        entry.registryRevision,
                        entry.preflightToken,
                        preflightEpoch_
                    });
                }
                else
                {
                    entry.state = State::Failed;
                    entry.failureReason = "Texture could not be loaded for preview.";
                    entry.resource.Reset();
                }
            }
            else if (job->ok && job->rendered)
            {
                entry.resource = job->rendered;
                entry.srvFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                entry.width = kPreviewRenderSize;
                entry.height = kPreviewRenderSize;
                entry.mipLevels = 1;
                entry.state = State::Ready;
                entry.failureReason.clear();
            }
            else
            {
                entry.state = State::Failed;
                switch (job->load.kind)
                {
                case PendingLoad::Kind::Mesh:
                    entry.failureReason = "Mesh preview could not be rendered.";
                    break;
                case PendingLoad::Kind::Material:
                    entry.failureReason = "Material preview could not be rendered.";
                    break;
                case PendingLoad::Kind::Cube:
                    entry.failureReason = "Cubemap preview could not be rendered.";
                    break;
                default:
                    entry.failureReason = "Preview could not be rendered.";
                    break;
                }
                entry.resource.Reset();
            }
        }
    }

    if (job->ok && job->rendered && job->hasReadback &&
        !job->load.cachePath.empty())
    {
        const ThumbnailReadback readback = job->readback;
        const std::string cachePath = job->load.cachePath;
        auto encode = [readback, cachePath]()
        {
            (void)WriteThumbnailPng(readback, cachePath);
        };
#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
        TaskSystem::Get().SubmitDetach(std::move(encode));
#else
        encode();
#endif
    }

    EvictIfNeeded(renderer);
    return true;
}

namespace {

// Per-thumbnail phase timing, one line per job in logs/thumbnail_profile.log.
//
// The thumbnail path stalls the main thread and the first two suspects (a full material-library
// reload per job, a GPU idle per cache eviction) did not account for it. Guessing a third time is
// not a plan: this attributes every millisecond of StartGpuJob to a named phase, so the next
// content-browser scroll says outright where it goes. Debug-only in practice (WITH_EDITOR), one
// fprintf per generated thumbnail.
class JobPhaseLog
{
public:
    JobPhaseLog(const char* kind, std::string what)
        : kind_(kind), what_(std::move(what)), start_(Clock::now()), phase_(start_) {}

    void Mark(const char* name)
    {
        const auto now = Clock::now();
        if (used_ < kMaxPhases)
        {
            names_[used_] = name;
            ms_[used_] = std::chrono::duration<double, std::milli>(now - phase_).count();
            ++used_;
        }
        phase_ = now;
    }

    ~JobPhaseLog()
    {
        const double total = std::chrono::duration<double, std::milli>(Clock::now() - start_).count();
        if (total < 0.5) { return; } // sub-millisecond jobs are not the problem being chased
        static std::mutex mtx;
        static FILE* f = nullptr;
        std::lock_guard<std::mutex> lk(mtx);
        if (f == nullptr) { fopen_s(&f, diag::LogPath("thumbnail_profile.log").c_str(), "w"); }
        if (f == nullptr) { return; }
        std::fprintf(f, "%-10s total %8.2f ms |", kind_, total);
        for (int i = 0; i < used_; ++i) { std::fprintf(f, " %s %.2f", names_[i], ms_[i]); }
        std::uint32_t taken = 0, missed = 0;
        texdecode::Stats(taken, missed);
        std::fprintf(f, "  decode cache=%u inline=%u prewarm slots=%u descs=%u  | %s",
                     taken, missed, slots_, descs_, what_.c_str());
        std::fputc('\n', f);
        std::fflush(f);
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr int kMaxPhases = 8;
    const char* kind_ = "";
    std::string what_;
public:
    std::uint32_t slots_ = 0;   // material slots the load carried
    std::uint32_t descs_ = 0;   // textures handed to the worker
private:
    Clock::time_point start_;
    Clock::time_point phase_;
    const char* names_[kMaxPhases]{};
    double ms_[kMaxPhases]{};
    int used_ = 0;
};

const char* KindName(int kind)
{
    switch (kind)
    {
    case 0: return "Texture";
    case 1: return "Mesh";
    case 2: return "Cube";
    case 3: return "Material";
    default: return "DiskCache";
    }
}

} // namespace

void AssetThumbnailCache::StartGpuJob(Renderer& renderer)
{
    if (gpuJob_) { return; }

    PendingLoad load;
    Entry* entry = nullptr;
    while (!queue_.empty())
    {
        load = std::move(queue_.front());
        queue_.pop_front();

        const auto it = entries_.find(load.key);
        if (it == entries_.end() || it->second.state != State::Queued ||
            it->second.sourceWriteTime != load.sourceWriteTime ||
            it->second.cacheSignature != load.cacheSignature)
        {
            continue;
        }
        entry = &it->second;
        entry->state = State::Generating;
        break;
    }
    if (!entry) { return; }

    // The material textures are being decoded on a worker — come back next frame rather than
    // decoding them here. This is the whole point of the prewarm: without the wait it loses the
    // race every time and the main thread does the work anyway.
    if (const auto ticket = decodeTickets_.find(load.key); ticket != decodeTickets_.end())
    {
        if (!ticket->second->done.load(std::memory_order_acquire))
        {
            entry->state = State::Queued;
            queue_.push_front(std::move(load));
            return;
        }
        decodeTickets_.erase(ticket);
    }

    texdecode::ResetStats(); // per-job: the counters below describe THIS thumbnail
    JobPhaseLog phases(KindName(static_cast<int>(load.kind)),
                       load.path.empty() ? load.presetKey : load.path);
    phases.slots_ = lastPrewarmSlots_;
    phases.descs_ = lastPrewarmDescs_;

    const bool needsPreview = load.kind == PendingLoad::Kind::Mesh ||
        load.kind == PendingLoad::Kind::Material ||
        load.kind == PendingLoad::Kind::Cube;
    if (needsPreview)
    {
        StartPreviewInitialization(renderer);
        const int initStatus = previewInitState_
            ? previewInitState_->status.load(std::memory_order_acquire)
            : -1;
        if (initStatus != 2)
        {
            if (initStatus < 0)
            {
                entry->state = State::Failed;
                entry->failureReason = "Thumbnail renderer could not be initialized.";
            }
            else
            {
                entry->state = State::Queued;
                queue_.push_front(std::move(load));
            }
            return;
        }
    }
    if (load.kind == PendingLoad::Kind::Material || load.kind == PendingLoad::Kind::Mesh)
    {
        preview_->ReloadPresetsIfChanged();
    }
    phases.Mark("presets");

    std::unique_ptr<GpuJob> job = std::make_unique<GpuJob>();
    job->load = std::move(load);
    job->commands = std::make_unique<UploadBatch>();
    const bool batchBegun = job->commands->Begin(&renderer);
    phases.Mark("batch");
    if (!batchBegun)
    {
        entry->state = State::Queued;
        queue_.push_front(std::move(job->load));
        return;
    }

    // FULL ASYNC: inside this scope nothing decodes an image on the main thread. A texture that
    // is not ready yet fails its load, sets AnyPending(), and a worker starts on it. We then throw
    // this attempt away and retry on a later frame — the thumbnail simply stays a placeholder for
    // as long as the decode takes, and the frame never stalls.
    //
    // This does NOT depend on predicting which textures the material will pull in, which is what
    // the earlier prewarm-only attempt got wrong: it guessed the list, guessed empty, and the main
    // thread did the work anyway.
    Texture2D::DeferDecodeScope deferDecodes;
    std::vector<std::string> touchedMaterials;

    switch (job->load.kind)
    {
    case PendingLoad::Kind::Texture:
    case PendingLoad::Kind::DiskCache:
    {
        Texture2D::CreateDesc desc;
        desc.path = WidenAscii(job->load.kind == PendingLoad::Kind::DiskCache
            ? job->load.cachePath
            : job->load.path);
        desc.usage = job->load.kind == PendingLoad::Kind::DiskCache
            ? Texture2D::Usage::AlbedoSRGB
            : job->load.usage;
        job->ok = job->texture.CreateFromFile(&renderer,
            job->commands->CommandList(), desc, job->commands->KeepAlive());
        break;
    }
    case PendingLoad::Kind::Mesh:
        if (job->load.meshData)
        {
            job->mesh = preview_->Meshes().CreateFromCpuData(job->load.path,
                &renderer, *job->load.meshData, job->commands->CommandList(),
                job->commands->KeepAlive());
            std::size_t materialSlotCount = 1;
            if (job->mesh)
            {
                for (const Mesh::Submesh& submesh : job->mesh->GetSubmeshes())
                {
                    materialSlotCount = std::max<std::size_t>(materialSlotCount,
                        static_cast<std::size_t>(submesh.materialSlot) + 1);
                }
            }
            phases.Mark("meshupload");
            job->meshMaterials.reserve(materialSlotCount);
            for (std::size_t slot = 0; slot < materialSlotCount; ++slot)
            {
                // This intentionally mirrors GBufferRenderable: unspecified
                // glTF slots use their imported material, while a scalar
                // `material` only overrides slot zero.
                const std::string materialName = slot < job->load.meshMaterialSlots.size()
                    ? job->load.meshMaterialSlots[slot]
                    : "auto";
                if (!materialName.empty() && materialName != "auto") { touchedMaterials.push_back(materialName); }
                if (materialName.empty() || materialName == "auto")
                {
                    job->meshMaterials.push_back(
                        preview_->Materials().GetOrCreateFromGltf(&renderer,
                            job->commands->CommandList(), job->commands->KeepAlive(),
                            job->load.path, static_cast<int>(slot)));
                }
                else
                {
                    job->meshMaterials.push_back(preview_->Materials().GetOrCreate(
                        &renderer, job->commands->CommandList(),
                        job->commands->KeepAlive(), materialName));
                }
            }
            job->ok = job->mesh != nullptr;
            phases.Mark("materials");
        }
        break;
    case PendingLoad::Kind::Cube:
        job->ok = job->cube.CreateFromDDS(&renderer,
            job->commands->CommandList(), WidenAscii(job->load.path),
            job->commands->KeepAlive());
        break;
    case PendingLoad::Kind::Material:
        touchedMaterials.push_back(job->load.presetKey);
        job->material = preview_->Materials().GetOrCreate(&renderer,
            job->commands->CommandList(), job->commands->KeepAlive(),
            job->load.presetKey);
        job->mesh = preview_->EnsureSphere(renderer, *job->commands);
        job->ok = job->material != nullptr && job->mesh != nullptr;
        break;
    }

    phases.Mark("assetload");

    if (deferDecodes.AnyPending())
    {
        // At least one texture is still being decoded on a worker. Drop this attempt whole: the
        // MaterialData built during it is missing maps, so it must not stay in the cache, and the
        // UploadBatch closes itself without executing (see ~UploadBatch).
        for (const std::string& name : touchedMaterials) { preview_->Materials().EvictCached(name); }
        entry->state = State::Queued;
        queue_.push_front(std::move(job->load));
        return;
    }

    if (needsPreview && job->ok)
    {
        if (job->load.kind == PendingLoad::Kind::Cube)
        {
            job->rendered = preview_->RecordCubeThumbnail(renderer,
                job->commands->CommandList(), job->cube, kPreviewRenderSize);
        }
        else
        {
            std::vector<std::shared_ptr<MaterialData>> materials;
            if (job->load.kind == PendingLoad::Kind::Mesh)
            {
                materials = job->meshMaterials;
            }
            else if (job->load.kind == PendingLoad::Kind::Material && job->material)
            {
                materials.push_back(job->material);
            }
            job->rendered = preview_->RecordThumbnail(renderer,
                job->commands->CommandList(), *job->mesh, materials,
                kPreviewRenderSize);
        }

        job->ok = job->rendered != nullptr;
        if (job->rendered && !job->load.cachePath.empty())
        {
            job->hasReadback = RecordThumbnailReadback(renderer.GetDevice(),
                job->commands->CommandList(), job->rendered.Get(), job->readback);
        }
    }

    phases.Mark("record");

    if (FAILED(renderer.GetDevice()->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&job->fence))) ||
        !job->commands->Submit(&renderer))
    {
        entry->state = State::Failed;
        entry->failureReason = "Thumbnail GPU submission failed.";
        return;
    }
    if (FAILED(renderer.GetCommandQueue()->Signal(
            job->fence.Get(), job->fenceValue)))
    {
        // Exceptional device/queue failure: keep upload intermediates alive until
        // the already submitted command list is known to be finished.
        renderer.WaitForPreviousFrame();
        entry->state = State::Failed;
        entry->failureReason = "Thumbnail GPU fence failed.";
        return;
    }

    phases.Mark("submit");
    gpuJob_ = std::move(job);
}

void AssetThumbnailCache::ProcessPending(Renderer& renderer)
{
    CPU_SCOPE(ProfilerScopes::kAssetThumbnailProcessPending);
    FlushRetired(renderer, /*force=*/false);
    CommitPreflightResults(renderer);
    LaunchPreflightJobs();

    // Never wait here. A single in-flight job protects the preview renderer's
    // shared descriptor/constant-buffer slots; completion is polled next frame.
    if (!CommitGpuJob(renderer))
    {
        return;
    }
    StartGpuJob(renderer);
}

void AssetThumbnailCache::EvictIfNeeded(Renderer& renderer)
{
    while (entries_.size() > kMaxCachedThumbnails)
    {
        auto victim = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
        {
            // Only finished entries are evictable; in-flight ones stay put.
            if (it->second.state != State::Ready && it->second.state != State::Failed)
            {
                continue;
            }
            if (victim == entries_.end() ||
                it->second.lastRequestedFrame < victim->second.lastRequestedFrame)
            {
                victim = it;
            }
        }
        if (victim == entries_.end())
        {
            break; // Nothing evictable right now.
        }
        ReleaseEntry(renderer, victim->second);
        entries_.erase(victim);
    }
}

// A retired thumbnail is safe to free once every frame that could still hold its ImGui descriptor
// has retired — kFrameCount frames, the same window BeginFrame waits on. `force` skips the window
// for shutdown, where the caller has already idled the GPU.
void AssetThumbnailCache::FlushRetired(Renderer& renderer, bool force)
{
    if (retired_.empty()) { return; }
    const std::uint64_t now = renderer.GetTotalFrameNumber();
    auto keep = retired_.begin();
    for (auto it = retired_.begin(); it != retired_.end(); ++it)
    {
        if (!force && now < it->retiredAtFrame + render::kFrameCount + 1)
        {
            *keep++ = std::move(*it);
            continue;
        }
        // Drop the descriptor BEFORE the resource dies, so the address cannot be recycled under
        // a live ImGui preview handle.
        renderer.ReleaseImGuiTextureDescriptors(it->resource.Get());
        it->resource.Reset();
    }
    retired_.erase(keep, retired_.end());
}

void AssetThumbnailCache::ReleaseEntry(Renderer& renderer, Entry& entry)
{
    if (entry.resource)
    {
        // An in-flight frame may still reference this through its ImGui preview descriptor, so it
        // cannot be freed now — but it does not need a GPU idle either. Retire it and let
        // FlushRetired drop it once its frame has certainly retired.
        retired_.push_back({ entry.resource, renderer.GetTotalFrameNumber() });
        entry.resource.Reset();
    }
    entry.state = State::Missing;
    entry.failureReason.clear();
}

void AssetThumbnailCache::ReleaseAll(Renderer& renderer)
{
    if (gpuJob_)
    {
        renderer.WaitForPreviousFrame();
        gpuJob_.reset();
    }
    // Shutdown: one idle already happened (or is about to), so the retire list can go now.
    if (!retired_.empty())
    {
        renderer.WaitForPreviousFrame();
        FlushRetired(renderer, /*force=*/true);
    }
    // Anything a worker decoded for a thumbnail nobody will now ask for.
    decodeTickets_.clear();
    texdecode::Clear();

    bool anyResident = false;
    for (auto& pair : entries_)
    {
        if (pair.second.resource)
        {
            anyResident = true;
            break;
        }
    }

    if (anyResident)
    {
        renderer.WaitForPreviousFrame();
    }
    for (auto& pair : entries_)
    {
        if (pair.second.resource)
        {
            renderer.ReleaseImGuiTextureDescriptors(pair.second.resource.Get());
            pair.second.resource.Reset();
        }
    }
    entries_.clear();
    queue_.clear();
    preflightQueue_.clear();
    ++preflightEpoch_;
    if (preflightState_)
    {
        std::lock_guard<std::mutex> lock(preflightState_->mutex);
        preflightState_->completed.clear();
    }
}

#endif // WITH_EDITOR
