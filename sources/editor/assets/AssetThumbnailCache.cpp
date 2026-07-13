#include "editor/assets/AssetThumbnailCache.h"
#if WITH_EDITOR

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#include <wincodec.h>

#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "editor/assets/AssetRegistry.h"
#include "materials/MaterialData.h"
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

namespace
{
    // Bump this whenever preview rendering or PNG encoding semantics change.
    constexpr std::uint32_t kThumbnailSchemaVersion = 2;

    // Bound the per-frame GPU work. Each processed frame idles the GPU once, then
    // uploads/renders up to this many thumbnails, so opening a large folder fills
    // in over a few frames instead of stalling on open.
    constexpr std::size_t kMaxLoadsPerFrame = 3;

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

    struct DiskCacheInfo
    {
        bool enabled = false;
        bool hit = false;
        bool discardedInvalidFile = false;
        std::uint64_t signature = 0;
        std::string stablePrefix;
        std::string path;
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

        const auto preset = document["presets"].find(presetKey);
        if (preset == document["presets"].end() || !preset->is_object())
        {
            HashText(hash, "missing-material-preset");
            return;
        }

        for (const char* name : { "albedo", "mr", "normal" })
        {
            HashText(hash, name);
            const auto dependency = preset->find(name);
            const std::string path = dependency != preset->end() && dependency->is_string()
                ? dependency->get<std::string>()
                : std::string{};
            HashText(hash, path);
            HashValue(hash, FileWriteTime(path));
        }
    }

    DiskCacheInfo BuildDiskCacheInfo(const EditorAssetRecord& record)
    {
        DiskCacheInfo info;
        const bool isPersistentPreview = record.id.type == EditorAssetType::Mesh ||
            record.id.type == EditorAssetType::MaterialPreset ||
            (record.id.type == EditorAssetType::Texture &&
                record.texture.kind == EditorTextureKind::TextureCube);
        if (!isPersistentPreview)
        {
            return info;
        }

        std::uint64_t stableHash = kFnvOffset;
        const std::uint32_t type = static_cast<std::uint32_t>(record.id.type);
        HashValue(stableHash, type);
        HashText(stableHash, record.id.key);

        std::uint64_t signature = kFnvOffset;
        HashValue(signature, kThumbnailSchemaVersion);
        HashValue(signature, type);
        HashText(signature, record.id.key);
        HashText(signature, record.path);
        HashValue(signature, record.fileWriteTime);
        if (record.id.type == EditorAssetType::MaterialPreset)
        {
            HashMaterialDependencies(signature, record.id.key);
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
            // A previous interrupted/failed WIC encode is never a cache hit.
            // Remove it here so the source renderer regenerates immediately.
            std::error_code removeError;
            fs::remove(fs::path(info.path), removeError);
            info.hit = false;
            info.discardedInvalidFile = true;
        }
        return info;
    }

    void PurgeStaleDiskCacheFiles(const DiskCacheInfo& info)
    {
        if (!info.enabled)
        {
            return;
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
                std::error_code removeError;
                fs::remove(candidate, removeError);
            }
        }
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
            commandList->ResourceBarrier(1, &transition);
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

void AssetThumbnailCache::BeginFrame()
{
    ++frameCounter_;
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

    const std::string& key = record.id.key;
    auto it = entries_.find(key);
    DiskCacheInfo diskCache;
    bool hasDiskCacheInfo = false;

    if (it != entries_.end())
    {
        Entry& entry = it->second;
        const bool sourceChanged = entry.sourceWriteTime != record.fileWriteTime;
        const bool registryChanged = entry.registryRevision != assetRegistryRevision;
        if (sourceChanged || registryChanged)
        {
            // Disk/cache dependency probing can involve filesystem metadata and
            // materials.json parsing. Do it only when the registry has changed,
            // not once per visible item every editor frame.
            diskCache = BuildDiskCacheInfo(record);
            hasDiskCacheInfo = true;
            const bool cacheChanged = entry.cacheSignature != diskCache.signature ||
                diskCache.discardedInvalidFile;
            if (sourceChanged || cacheChanged)
            {
                // The source or one of its tracked dependencies changed. Drop the
                // GPU thumbnail and lazily replace the stale on-disk PNG.
                ReleaseEntry(renderer, entry);
                entries_.erase(it);
                it = entries_.end();
            }
            else
            {
                entry.registryRevision = assetRegistryRevision;
            }
        }
    }

    if (it == entries_.end())
    {
        if (!hasDiskCacheInfo)
        {
            diskCache = BuildDiskCacheInfo(record);
        }
        Entry entry;
        entry.state = State::Queued;
        entry.sourceWriteTime = record.fileWriteTime;
        entry.cacheSignature = diskCache.signature;
        entry.registryRevision = assetRegistryRevision;
        entry.cachePath = diskCache.path;
        entry.lastRequestedFrame = frameCounter_;
        entries_.emplace(key, std::move(entry));

        PurgeStaleDiskCacheFiles(diskCache);

        PendingLoad load;
        load.key = key;
        load.kind = diskCache.hit ? PendingLoad::Kind::DiskCache : kind;
        load.generationKind = kind;
        load.sourceWriteTime = record.fileWriteTime;
        load.cacheSignature = diskCache.signature;
        load.path = record.path;
        load.cachePath = diskCache.path;
        load.presetKey = record.id.key;
        load.usage = UsageForRecord(record);
        queue_.push_back(std::move(load));

        view.state = State::Queued;
        return view;
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

void AssetThumbnailCache::ProcessPending(Renderer& renderer)
{
    CPU_SCOPE(ProfilerScopes::kAssetThumbnailProcessPending);
    if (queue_.empty())
    {
        return;
    }

    // Pull the next batch of still-wanted loads and mark them Generating so a
    // second Request this frame does not re-enqueue them.
    std::vector<PendingLoad> batch;
    while (!queue_.empty() && batch.size() < kMaxLoadsPerFrame)
    {
        PendingLoad load = std::move(queue_.front());
        queue_.pop_front();

        auto it = entries_.find(load.key);
        if (it == entries_.end() || it->second.state != State::Queued ||
            it->second.sourceWriteTime != load.sourceWriteTime ||
            it->second.cacheSignature != load.cacheSignature)
        {
            continue; // Stale queue entry (evicted or already handled).
        }
        it->second.state = State::Generating;
        batch.push_back(std::move(load));
    }

    if (batch.empty())
    {
        return;
    }

    renderer.WaitForPreviousFrame();

    bool needsPreview = false;
    bool needsMaterials = false;
    for (const PendingLoad& load : batch)
    {
        if (load.kind == PendingLoad::Kind::Mesh ||
            load.kind == PendingLoad::Kind::Material ||
            load.kind == PendingLoad::Kind::Cube)
        {
            needsPreview = true;
        }
        if (load.kind == PendingLoad::Kind::Material)
        {
            needsMaterials = true;
        }
    }
    bool previewReady = false;
    if (needsPreview)
    {
        previewReady = preview_.EnsureInitialized(renderer);
        if (previewReady && needsMaterials)
        {
            // Material keys include all referenced maps. Reload the preview-only
            // cache before a miss so the rendered image matches those changes.
            preview_.ReloadPresets();
        }
    }

    struct Loaded
    {
        Texture2D texture;
        TextureCube cube;
        std::shared_ptr<Mesh> mesh;
        std::shared_ptr<MaterialData> material;
        bool ok = false;
    };
    std::vector<Loaded> loaded(batch.size());

    // Phase A: uploads (textures + ensure mesh/material assets resident). One
    // fenced batch, so the mesh/texture resources are in a stable state before the
    // render phase draws them.
    {
        UploadBatch uploads;
        if (!uploads.Begin(&renderer))
        {
            for (PendingLoad& load : batch)
            {
                auto it = entries_.find(load.key);
                if (it != entries_.end() && it->second.state == State::Generating)
                {
                    it->second.state = State::Queued;
                    queue_.push_back(std::move(load));
                }
            }
            return;
        }

        for (std::size_t i = 0; i < batch.size(); ++i)
        {
            PendingLoad& load = batch[i];
            Loaded& out = loaded[i];
            switch (load.kind)
            {
            case PendingLoad::Kind::Texture:
            case PendingLoad::Kind::DiskCache:
            {
                Texture2D::CreateDesc desc;
                desc.path = WidenAscii(load.kind == PendingLoad::Kind::DiskCache
                    ? load.cachePath
                    : load.path);
                desc.usage = load.kind == PendingLoad::Kind::DiskCache
                    ? Texture2D::Usage::AlbedoSRGB
                    : load.usage;
                out.ok = out.texture.CreateFromFile(&renderer,
                    uploads.CommandList(), desc, uploads.KeepAlive());
                break;
            }
            case PendingLoad::Kind::Mesh:
                if (previewReady)
                {
                    out.mesh = preview_.Meshes().Load(load.path, &renderer,
                        uploads.CommandList(), uploads.KeepAlive());
                    out.ok = out.mesh != nullptr;
                }
                break;
            case PendingLoad::Kind::Cube:
                if (previewReady)
                {
                    out.ok = out.cube.CreateFromDDS(&renderer, uploads.CommandList(),
                        WidenAscii(load.path), uploads.KeepAlive());
                }
                break;
            case PendingLoad::Kind::Material:
                if (previewReady)
                {
                    out.material = preview_.Materials().GetOrCreate(&renderer,
                        uploads.CommandList(), uploads.KeepAlive(), load.presetKey);
                    out.mesh = preview_.EnsureSphere(renderer, uploads);
                    out.ok = out.material != nullptr && out.mesh != nullptr;
                }
                break;
            }
        }
        uploads.SubmitAndWait(&renderer);
    }

    // Phase B: render source previews and, in the same bounded submission,
    // copy their 256px color targets into readback buffers for the disk cache.
    // One command list per thumbnail keeps the single-slot descriptor heaps
    // correct at execute time.
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> rendered(batch.size());
    std::vector<ThumbnailReadback> readbacks(batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i)
    {
        const PendingLoad& load = batch[i];
        if (load.kind == PendingLoad::Kind::Texture ||
            load.kind == PendingLoad::Kind::DiskCache || !loaded[i].ok)
        {
            continue;
        }
        if (load.kind != PendingLoad::Kind::Cube && !loaded[i].mesh)
        {
            loaded[i].ok = false;
            continue;
        }

        UploadBatch render;
        if (!render.Begin(&renderer))
        {
            loaded[i].ok = false;
            continue;
        }

        if (load.kind == PendingLoad::Kind::Cube)
        {
            rendered[i] = preview_.RecordCubeThumbnail(renderer, render.CommandList(),
                loaded[i].cube, kPreviewRenderSize);
        }
        else
        {
            Mesh* mesh = loaded[i].mesh.get();

            ID3D12Resource* albedo = nullptr;
            DXGI_FORMAT albedoFormat = DXGI_FORMAT_UNKNOWN;
            bool hasAlbedo = false;
            if (load.kind == PendingLoad::Kind::Material && loaded[i].material)
            {
                MaterialData* material = loaded[i].material.get();
                if (material->hasAlbedo && material->albedo.GetResource())
                {
                    albedo = material->albedo.GetResource();
                    albedoFormat = material->albedo.GetSrvFormat();
                    hasAlbedo = true;
                }
            }

            rendered[i] = preview_.RecordThumbnail(renderer, render.CommandList(),
                *mesh, albedo, albedoFormat, hasAlbedo, kPreviewRenderSize);
        }

        bool hasReadback = false;
        if (rendered[i] && !load.cachePath.empty())
        {
            hasReadback = RecordThumbnailReadback(renderer.GetDevice(), render.CommandList(),
                rendered[i].Get(), readbacks[i]);
        }
        render.SubmitAndWait(&renderer);
        if (!rendered[i])
        {
            loaded[i].ok = false;
        }
        else if (hasReadback)
        {
            // Disk cache failures are non-fatal: the fresh GPU thumbnail stays
            // usable and a later source miss can try writing the PNG again.
            (void)WriteThumbnailPng(readbacks[i], load.cachePath);
        }
    }

    // Phase C: commit results into the cache entries.
    for (std::size_t i = 0; i < batch.size(); ++i)
    {
        const PendingLoad& load = batch[i];
        auto it = entries_.find(load.key);
        if (it == entries_.end())
        {
            continue;
        }
        Entry& entry = it->second;

        if (load.kind == PendingLoad::Kind::Texture ||
            load.kind == PendingLoad::Kind::DiskCache)
        {
            if (loaded[i].ok && loaded[i].texture.GetResource())
            {
                const D3D12_RESOURCE_DESC desc = loaded[i].texture.GetResource()->GetDesc();
                entry.resource = loaded[i].texture.GetResource();
                entry.srvFormat = loaded[i].texture.GetSrvFormat();
                entry.width = loaded[i].texture.GetWidth();
                entry.height = loaded[i].texture.GetHeight();
                entry.mipLevels = std::max<UINT>(desc.MipLevels, 1u);
                entry.state = State::Ready;
                entry.failureReason.clear();
            }
            else if (load.kind == PendingLoad::Kind::DiskCache)
            {
                // A corrupt or partial PNG must not leave a thumbnail stuck
                // failed. Delete it and rerun the original source renderer.
                RemoveDiskCacheFile(load.cachePath);
                PendingLoad retry = load;
                retry.kind = load.generationKind;
                entry.state = State::Queued;
                entry.failureReason.clear();
                queue_.push_back(std::move(retry));
            }
            else
            {
                entry.state = State::Failed;
                entry.failureReason = "Texture could not be loaded for preview.";
                entry.resource.Reset();
            }
        }
        else
        {
            if (loaded[i].ok && rendered[i])
            {
                entry.resource = rendered[i];
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
                switch (load.kind)
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

    EvictIfNeeded(renderer);
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

void AssetThumbnailCache::ReleaseEntry(Renderer& renderer, Entry& entry)
{
    if (entry.resource)
    {
        // Idle the GPU before freeing a resource an in-flight frame may still
        // reference through its ImGui preview descriptor, then drop that
        // descriptor before the resource address can be recycled.
        renderer.WaitForPreviousFrame();
        renderer.ReleaseImGuiTextureDescriptors(entry.resource.Get());
        entry.resource.Reset();
    }
    entry.state = State::Missing;
    entry.failureReason.clear();
}

void AssetThumbnailCache::ReleaseAll(Renderer& renderer)
{
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
}

#endif // WITH_EDITOR
