#include "editor/assets/AssetThumbnailCache.h"
#if WITH_EDITOR

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "editor/assets/AssetRegistry.h"
#include "materials/MaterialData.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/meshes/Mesh.h"

namespace
{
    // Bumping this would invalidate a future on-disk thumbnail cache. The
    // in-memory cache keys on the asset id plus source write time; the version is
    // recorded here so the disk layer (a later pass) can reuse it verbatim.
    constexpr std::uint32_t kThumbnailSchemaVersion = 1;

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
    const EditorAssetRecord& record)
{
    View view;

    PendingLoad::Kind kind;
    switch (record.id.type)
    {
    case EditorAssetType::Texture:        kind = PendingLoad::Kind::Texture; break;
    case EditorAssetType::Mesh:           kind = PendingLoad::Kind::Mesh; break;
    case EditorAssetType::MaterialPreset: kind = PendingLoad::Kind::Material; break;
    default:
        return view; // Levels, shaders, unknown: no preview.
    }

    const std::string& key = record.id.key;
    auto it = entries_.find(key);
    if (it != entries_.end() && it->second.sourceWriteTime != record.fileWriteTime)
    {
        // The source file changed since we generated this thumbnail: drop it and
        // regenerate so a refresh only re-does the assets that actually changed.
        ReleaseEntry(renderer, it->second);
        entries_.erase(it);
        it = entries_.end();
    }

    if (it == entries_.end())
    {
        Entry entry;
        entry.state = State::Queued;
        entry.sourceWriteTime = record.fileWriteTime;
        entry.lastRequestedFrame = frameCounter_;
        entries_.emplace(key, std::move(entry));

        PendingLoad load;
        load.key = key;
        load.kind = kind;
        load.path = record.path;
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
        if (it == entries_.end() || it->second.state != State::Queued)
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

    (void)kThumbnailSchemaVersion;

    renderer.WaitForPreviousFrame();

    bool needsPreview = false;
    for (const PendingLoad& load : batch)
    {
        if (load.kind != PendingLoad::Kind::Texture)
        {
            needsPreview = true;
            break;
        }
    }
    bool previewReady = false;
    if (needsPreview)
    {
        preview_.EnsurePresets();
        previewReady = preview_.EnsureInitialized(renderer);
    }

    struct Loaded
    {
        Texture2D texture;
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
            {
                Texture2D::CreateDesc desc;
                desc.path = WidenAscii(load.path);
                desc.usage = load.usage;
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

    // Phase B: render mesh/material thumbnails (one command list each so the
    // single-slot preview descriptor heaps stay correct at execute time).
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> rendered(batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i)
    {
        const PendingLoad& load = batch[i];
        if (load.kind == PendingLoad::Kind::Texture || !loaded[i].ok)
        {
            continue;
        }
        Mesh* mesh = loaded[i].mesh.get();
        if (!mesh)
        {
            loaded[i].ok = false;
            continue;
        }

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

        UploadBatch render;
        if (!render.Begin(&renderer))
        {
            loaded[i].ok = false;
            continue;
        }
        rendered[i] = preview_.RecordThumbnail(renderer, render.CommandList(),
            *mesh, albedo, albedoFormat, hasAlbedo, kPreviewRenderSize);
        render.SubmitAndWait(&renderer);
        if (!rendered[i])
        {
            loaded[i].ok = false;
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

        if (load.kind == PendingLoad::Kind::Texture)
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
                entry.failureReason = load.kind == PendingLoad::Kind::Mesh
                    ? "Mesh preview could not be rendered."
                    : "Material preview could not be rendered.";
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
