#pragma once
#if WITH_EDITOR

#include <cstddef>
#include <cstdint>
#include <deque>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <wrl/client.h>

#include "imgui.h"
#include "editor/assets/EditorPreviewRenderer.h"
#include "materials/Texture2D.h"

struct EditorAssetRecord;
class Renderer;
namespace editor_thumbnail_detail
{
    enum class PreflightKind : std::uint32_t
    {
        Mesh,
        Material,
        Cube
    };

    struct PreflightState;
    struct PreviewInitState;
}

// Editor-owned cache of real asset thumbnails for the Content Browser (Step 12F).
//
// Texture assets are previewed by loading the source image on the GPU; mesh and
// material and cubemap assets are previewed by rendering them offscreen through
// EditorPreviewRenderer. Rendered previews are also persisted as 256px PNGs in
// editor_cache/thumbnails, so later editor sessions can upload them directly.
// Every resident thumbnail is left shader-readable and handed to ImGui via a
// per-frame texture id.
//
// Lifetime / threading: uploads, draws, and cache commits stay on the editor
// thread. Worker jobs prepare CPU mesh data, encode cached PNGs, and initialize
// the preview pipeline from a retained D3D12 device reference; they publish only
// immutable results for the editor thread to consume.
class AssetThumbnailCache
{
public:
    AssetThumbnailCache();
    ~AssetThumbnailCache();

    enum class State
    {
        Missing,     // never requested
        Preflighting,// CPU cache/signature probe is running
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
    void BeginFrame(Renderer& renderer);

    // Select the scene environment used by generated mesh/material thumbnails.
    // The source identity is folded into the persistent PNG key so previews from
    // different skyboxes or exposures are never reused interchangeably.
    void SetEnvironment(const TextureCube* environment,
        const std::string& sourcePath,
        std::uint64_t sourceWriteTime,
        float exposure);

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
        std::uint64_t environmentSignature = 0;
        std::string sourcePath;             // original asset path before resolution
        std::string path;                   // texture / mesh source file
        std::string cachePath;              // rendered cache PNG, if any
        std::string presetKey;              // material preset name
        std::shared_ptr<MeshCpuData> meshData; // worker-prepared geometry
        std::vector<std::string> meshMaterialSlots; // `.mesh.json` material names by submesh slot
        Texture2D::Usage usage = Texture2D::Usage::AlbedoSRGB; // texture only
    };

    struct Entry
    {
        State state = State::Missing;
        Microsoft::WRL::ComPtr<ID3D12Resource> resource; // loaded texture or rendered target
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::uint32_t mipLevels = 1;
        editor_thumbnail_detail::PreflightKind generationKind =
            editor_thumbnail_detail::PreflightKind::Mesh;
        std::uint64_t sourceWriteTime = 0;
        std::uint64_t cacheSignature = 0;
        std::uint64_t environmentSignature = 0;
        std::uint64_t registryRevision = 0;
        std::uint64_t preflightToken = 0;
        bool preflightPending = false;
        std::string cachePath;
        std::string failureReason;
        std::uint64_t lastRequestedFrame = 0;
    };

    struct PreflightRequest
    {
        std::string key;
        std::string path;
        editor_thumbnail_detail::PreflightKind generationKind =
            editor_thumbnail_detail::PreflightKind::Mesh;
        std::uint64_t sourceWriteTime = 0;
        std::uint64_t registryRevision = 0;
        std::uint64_t environmentSignature = 0;
        std::uint64_t token = 0;
        std::uint64_t epoch = 0;
    };

    struct GpuJob;

    void QueuePreflight(const EditorAssetRecord& record,
        PendingLoad::Kind generationKind,
        std::uint64_t assetRegistryRevision,
        Entry& entry);
    // Dispatch the material textures of a queued load to a worker for CPU decoding.
    // Dispatch a queued load's material textures to a worker for CPU decoding (the GPU upload
    // stays on the main thread). Best-effort: a miss just means CreateFromFile decodes inline.
    void PrewarmMaterialTextures(const PendingLoad& load);
    void LaunchPreflightJobs();
    void CommitPreflightResults(Renderer& renderer);
    void StartPreviewInitialization(Renderer& renderer);
    bool CommitGpuJob(Renderer& renderer);
    void StartGpuJob(Renderer& renderer);
    void EvictIfNeeded(Renderer& renderer);
    void ReleaseEntry(Renderer& renderer, Entry& entry);
    // Free thumbnails retired at least kFrameCount frames ago (force = at shutdown, after an idle).
    void FlushRetired(Renderer& renderer, bool force);

    std::unordered_map<std::string, Entry> entries_;
    std::deque<PendingLoad> queue_;
    std::deque<PreflightRequest> preflightQueue_;
    std::shared_ptr<editor_thumbnail_detail::PreflightState> preflightState_;
    std::shared_ptr<editor_thumbnail_detail::PreviewInitState> previewInitState_;
    std::unique_ptr<GpuJob> gpuJob_;

    // Evicted thumbnails wait here instead of idling the GPU. ReleaseEntry used to call
    // WaitForPreviousFrame() per victim, and once the cache is full EVERY new thumbnail evicts one
    // — so scrolling a folder meant a full pipeline flush per icon, on the main thread. The ComPtr
    // keeps the resource alive until its frame is provably retired, which is the same guarantee
    // the idle was buying.
    struct RetiredThumbnail
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        std::uint64_t retiredAtFrame = 0;
    };
    std::vector<RetiredThumbnail> retired_;

    // A queued load's material textures are decoded on a WORKER; the job must not start until
    // that finishes. Without this gate the prewarm was pure decoration: the decode takes seconds
    // and StartGpuJob runs a frame later, so it lost the race every single time and decoded the
    // same textures again on the main thread (measured — the profile was byte-identical).
    //
    // Waiting here does NOT block: the load goes back on the queue and is retried next frame.
    struct DecodeTicket { std::atomic<bool> done{ false }; };
    std::unordered_map<std::string, std::shared_ptr<DecodeTicket>> decodeTickets_;
    std::uint32_t lastPrewarmSlots_ = 0; // diagnostics: what the last prewarm actually saw
    std::uint32_t lastPrewarmDescs_ = 0;
    std::shared_ptr<EditorPreviewRenderer> preview_;
    const TextureCube* environment_ = nullptr;
    float environmentExposure_ = 1.0f;
    std::uint64_t environmentSignature_ = 0;
    std::uint64_t frameCounter_ = 0;
    std::uint64_t preflightEpoch_ = 1;
};

#endif // WITH_EDITOR
