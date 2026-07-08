#include "editor/assets/AssetThumbnailCache.h"
#if WITH_EDITOR

#include <algorithm>
#include <utility>
#include <vector>

#include "editor/assets/AssetRegistry.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/UploadBatch.h"

namespace
{
    // Bumping this would invalidate a future on-disk thumbnail cache. The
    // in-memory cache keys on the asset id plus source write time; the version is
    // recorded here so the disk layer (a later pass) can reuse it verbatim.
    constexpr std::uint32_t kThumbnailSchemaVersion = 1;

    // Bound the per-frame GPU stall. Each processed frame idles the GPU once and
    // uploads up to this many textures in a single batch, so opening a large
    // folder fills in over a few frames instead of stalling on open.
    constexpr std::size_t kMaxLoadsPerFrame = 3;

    // LRU cap. Each resident thumbnail also holds kFrameCount ImGui preview
    // descriptors, so this stays comfortably under ImGuiLayer's editor SRV heap
    // (512): 96 * 3 = 288, leaving room for the font/icon atlases and debug
    // views. The repo's texture set is far smaller, so this rarely trips.
    constexpr std::size_t kMaxCachedThumbnails = 96;

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
    if (record.id.type != EditorAssetType::Texture)
    {
        return view; // Only textures are previewed in this pass.
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
        load.physicalPath = WidenAscii(record.path);
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

    if (entry.state == State::Ready && entry.texture.GetResource())
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = entry.srvFormat;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MostDetailedMip = 0;
        // Expose the full mip chain so ImGui's sampler can minify smoothly when
        // the thumbnail is drawn much smaller than the source (DDS ships mips).
        srvDesc.Texture2D.MipLevels = entry.mipLevels;
        view.texture =
            renderer.CreateImGuiTextureId(entry.texture.GetResource(), srvDesc);
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

    // Pull the next batch of still-wanted loads out of the queue and mark them
    // Generating so a second Request this frame does not re-enqueue them.
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
    UploadBatch uploads;
    if (!uploads.Begin(&renderer))
    {
        // Could not open an upload batch this frame: put the work back as Queued.
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

    for (PendingLoad& load : batch)
    {
        auto it = entries_.find(load.key);
        if (it == entries_.end())
        {
            continue;
        }
        Entry& entry = it->second;

        Texture2D::CreateDesc desc;
        desc.path = load.physicalPath;
        desc.usage = load.usage;
        const bool ok = entry.texture.CreateFromFile(
            &renderer, uploads.CommandList(), desc, uploads.KeepAlive());

        if (ok && entry.texture.GetResource())
        {
            const D3D12_RESOURCE_DESC resourceDesc = entry.texture.GetResource()->GetDesc();
            entry.state = State::Ready;
            entry.srvFormat = entry.texture.GetSrvFormat();
            entry.width = entry.texture.GetWidth();
            entry.height = entry.texture.GetHeight();
            entry.mipLevels = std::max<UINT>(resourceDesc.MipLevels, 1u);
            entry.failureReason.clear();
        }
        else
        {
            entry.state = State::Failed;
            entry.failureReason = "Texture could not be loaded for preview.";
            entry.texture = Texture2D{}; // Drop any partially created resource.
        }
    }

    uploads.SubmitAndWait(&renderer);
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
    if (entry.texture.GetResource())
    {
        // Idle the GPU before freeing a resource an in-flight frame may still
        // reference through its ImGui preview descriptor, then drop that
        // descriptor before the resource address can be recycled.
        renderer.WaitForPreviousFrame();
        renderer.ReleaseImGuiTextureDescriptors(entry.texture.GetResource());
        entry.texture = Texture2D{};
    }
    entry.state = State::Missing;
    entry.failureReason.clear();
}

void AssetThumbnailCache::ReleaseAll(Renderer& renderer)
{
    bool anyResident = false;
    for (auto& pair : entries_)
    {
        if (pair.second.texture.GetResource())
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
        if (pair.second.texture.GetResource())
        {
            renderer.ReleaseImGuiTextureDescriptors(pair.second.texture.GetResource());
            pair.second.texture = Texture2D{};
        }
    }
    entries_.clear();
    queue_.clear();
}

#endif // WITH_EDITOR
