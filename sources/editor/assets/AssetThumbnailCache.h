#pragma once
#if WITH_EDITOR

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include "imgui.h"
#include "editor/assets/EditorPreviewRenderer.h"
#include "materials/Texture2D.h"

struct EditorAssetRecord;
class Renderer;

// Editor-owned cache of real asset thumbnails for the Content Browser (Step 12F).
//
// Texture assets are previewed by loading the source image on the GPU; mesh and
// material and cubemap assets are previewed by rendering them offscreen through
// EditorPreviewRenderer. Rendered previews are also persisted as 256px PNGs in
// editor_cache/thumbnails, so later editor sessions can upload them directly.
// Every resident thumbnail is left shader-readable and handed to ImGui via a
// per-frame texture id.
//
// Lifetime / threading: every method runs inside the editor draw/tick window. All
// GPU work (texture upload, offscreen render, resource release) is fenced with
// Renderer::WaitForPreviousFrame, matching the rest of the editor. Nothing here
// touches the Scene, selection, command history, or document dirty state.
class AssetThumbnailCache
{
public:
    enum class State
    {
        Missing,     // never requested
        Queued,      // requested, waiting for a generation slot
        Generating,  // being generated this frame (transient)
        Ready,       // GPU thumbnail available
        Failed       // generation failed (unreadable/unsupported source)
    };

    struct View
    {
        State state = State::Missing;
        ImTextureID texture = ImTextureID_Invalid; // valid only when Ready
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        const char* failureReason = nullptr; // set when Failed
    };

    // Advance the internal frame counter (used for LRU eviction). Call once per
    // editor frame before issuing any Request.
    void BeginFrame();

    // Request the thumbnail for a previewable asset (texture, mesh, material, or
    // cubemap)
    // and, when Ready, obtain a drawable per-frame ImGui id for it. Marks the
    // asset used this frame. Non-previewable records return State::Missing.
    View Request(Renderer& renderer, const EditorAssetRecord& record,
        std::uint64_t assetRegistryRevision);

    // Generate a bounded number of queued thumbnails (fenced upload + offscreen
    // render). Call once per editor frame after the browser has issued Requests.
    // A no-op with no GPU stall when the queue is empty.
    void ProcessPending(Renderer& renderer);

    // Release every GPU thumbnail under a full GPU wait. Optional: natural
    // teardown is also safe because the app idles the GPU before shutting the
    // renderer down and ImGuiLayer::Shutdown drops the preview descriptors.
    void ReleaseAll(Renderer& renderer);

private:
    struct Entry
    {
        State state = State::Missing;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource; // loaded texture or rendered target
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mipLevels = 1;
        std::uint64_t sourceWriteTime = 0;
        std::uint64_t cacheSignature = 0;
        std::uint64_t registryRevision = 0;
        std::string cachePath;
        std::string failureReason;
        std::uint64_t lastRequestedFrame = 0;
    };

    struct PendingLoad
    {
        enum class Kind
        {
            Texture,
            Mesh,
            Material,
            Cube,
            DiskCache
        };

        std::string key;
        Kind kind = Kind::Texture;
        Kind generationKind = Kind::Texture; // source kind when `kind` is DiskCache
        std::uint64_t sourceWriteTime = 0;
        std::uint64_t cacheSignature = 0;
        std::string path;                   // texture / mesh source file
        std::string cachePath;              // rendered cache PNG, if any
        std::string presetKey;              // material preset name
        Texture2D::Usage usage = Texture2D::Usage::AlbedoSRGB; // texture only
    };

    void EvictIfNeeded(Renderer& renderer);
    void ReleaseEntry(Renderer& renderer, Entry& entry);

    std::unordered_map<std::string, Entry> entries_;
    std::deque<PendingLoad> queue_;
    EditorPreviewRenderer preview_;
    std::uint64_t frameCounter_ = 0;
};

#endif // WITH_EDITOR
