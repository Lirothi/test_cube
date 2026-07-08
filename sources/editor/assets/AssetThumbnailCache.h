#pragma once
#if WITH_EDITOR

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

#include <d3d12.h>

#include "imgui.h"
#include "materials/Texture2D.h"

struct EditorAssetRecord;
class Renderer;

// Editor-owned cache of real asset thumbnails for the Content Browser (Step 12D).
//
// Scope of this pass ("safe core"): TEXTURE assets are previewed by loading the
// source image once on the GPU and handing the browser a per-frame ImGui texture
// id to draw. Mesh and material previews (an offscreen 3D render pass) are a
// separate, later pass; the browser leaves those types on their Step 12C
// icons/badges. Cube textures are not previewed here (a cube face needs a
// sampling pass) and are also left on their badge by the browser.
//
// Lifetime / threading: every method must run inside the editor draw/tick window
// (the same place the Content Browser runs). All GPU work (texture upload,
// resource release) is fenced with Renderer::WaitForPreviousFrame, matching the
// rest of the editor. Nothing here touches the Scene, selection, command
// history, or document dirty state, so generation is side-effect free.
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

    // Request the thumbnail for a texture asset and, when Ready, obtain a
    // drawable per-frame ImGui id for it. Marks the asset as used this frame.
    // Safe to call for any record; only texture records ever produce a preview.
    View Request(Renderer& renderer, const EditorAssetRecord& record);

    // Generate a bounded number of queued thumbnails (one fenced upload batch).
    // Call once per editor frame after the browser has issued its Requests. A
    // no-op with no GPU stall when the queue is empty.
    void ProcessPending(Renderer& renderer);

    // Release every GPU thumbnail under a full GPU wait. Optional: natural
    // teardown is also safe because the app idles the GPU before shutting the
    // renderer down and ImGuiLayer::Shutdown drops the preview descriptors.
    void ReleaseAll(Renderer& renderer);

private:
    struct Entry
    {
        State state = State::Missing;
        Texture2D texture;
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mipLevels = 1;
        std::uint64_t sourceWriteTime = 0;
        std::string failureReason;
        std::uint64_t lastRequestedFrame = 0;
    };

    struct PendingLoad
    {
        std::string key;
        std::wstring physicalPath;
        Texture2D::Usage usage = Texture2D::Usage::AlbedoSRGB;
    };

    void EvictIfNeeded(Renderer& renderer);
    void ReleaseEntry(Renderer& renderer, Entry& entry);

    std::unordered_map<std::string, Entry> entries_;
    std::deque<PendingLoad> queue_;
    std::uint64_t frameCounter_ = 0;
};

#endif // WITH_EDITOR
