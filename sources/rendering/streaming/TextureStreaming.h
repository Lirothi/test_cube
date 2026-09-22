#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "rendering/core/ResourceDeclarations.h"
#include "rendering/streaming/MipBiasFade.h"
#include "rendering/streaming/TextureRetireBin.h"
#include "rendering/streaming/TextureStreamingIo.h"
#include "rendering/streaming/TextureStreamingManager.h"
#include "rendering/streaming/TextureUploadRing.h"

class Renderer;
class Texture2D;
struct RenderGraphPassContext;
struct SceneFrameData;

namespace streaming {

// Plan A2: mips travel disk -> upload ring (worker) -> new resource (graphics-queue copies in
// Main_TextureStreaming, the frame's first pass) -> Texture2D::AdoptResource at the next frame's
// boundary; the old resource waits kFrameCount frames in the retire bin. UE's AsyncCreate +
// CopySharedMips + FinalizeStreaming, with the copies inside the graph (plan §5). Owned by
// Renderer; main thread only, except the worker's reads into the ring.
class TextureStreaming
{
public:
    void Init(ID3D12Device* device);
    void Shutdown(); // GPU idle by contract of the caller
    bool Ready() const { return device_ != nullptr; }

    // Registry of streamable textures (DDS owners whose mip table passed A1's self-check).
    int  Register(Texture2D* tex);
    void Unregister(Texture2D* tex);

    // Frame boundary, after BeginFrame's fence wait: release what the GPU is done with, adopt
    // last frame's copies, hand out new requests (A2: streaming.forceMips).
    void OnFrameBegin(Renderer* renderer, std::uint64_t frameNo);
    // Main_TextureStreaming builder: creates this frame's new resources, declares the copy
    // states, returns the body that records the copies (empty when nothing is ready).
    std::function<void(RenderGraphPassContext)> BuildPass(Renderer* renderer, RenderGraphPassContext& ctx);

    UINT64 RetiredBytes() const { return retire_.Bytes(); }
    std::size_t RegisteredCount() const { return registered_; }
    std::size_t SwapsInFlight() const { return swaps_.size(); }

    // A3: the manager decides, this executes. Tick once per frame from SceneRenderer::Render
    // (needs the camera and the object list); requests land in this frame's Main_TextureStreaming.
    void TickManager(Renderer* renderer, const SceneFrameData& frame);
    TextureStreamingManager& Manager() { return manager_; }
    std::size_t EntryCount() const { return entries_.size(); }
    Texture2D* EntryTexture(std::uint32_t i) const { return i < entries_.size() ? entries_[i].tex : nullptr; }
    std::uint32_t EntryGen(std::uint32_t i) const { return i < entries_.size() ? entries_[i].gen : 0u; }
    bool EntryAlive(std::uint32_t i, std::uint32_t gen) const { return i < entries_.size() && entries_[i].tex != nullptr && entries_[i].gen == gen; }
    UINT InFlightTarget(std::uint32_t i) const; // mips the in-flight swap lands on; 0 = idle
    // `lastRenderAge` = seconds since the texture was last seen (UE LastRenderTime): a texture not
    // seen for 0.5 s pops instead of fading (FMipBiasFade age threshold).
    bool RequestResident(std::uint32_t i, UINT mips, float lastRenderAge = 0.0f); // false: busy, ring full, or nothing to do
    bool CancelRequest(std::uint32_t i);             // false: nothing cancellable (already copied)
    UINT64 ResidentBytes() const;
    std::uint64_t LastFrame() const { return frameNo_; }

private:
    struct Swap;
    struct Entry
    {
        Texture2D* tex = nullptr;
        std::uint32_t gen = 0; // bumped on every (un)register so a stale swap cannot match
        Swap* swap = nullptr;
        MipBiasFade fade;      // A4
        bool fading = false;
    };
    struct Swap
    {
        enum class Stage { WaitIo, ReadyToCopy, Copied };
        std::uint32_t entry = 0;
        std::uint32_t gen = 0;
        UINT oldResident = 0;
        UINT newResident = 0;
        Stage stage = Stage::WaitIo;
        std::unique_ptr<IoRequest> io;   // stream-in only
        bool hasRing = false;
        std::uint32_t ringEntry = 0;
        D3D12_RESOURCE_DESC newDesc{};
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> ringFootprints; // per NEW mip, ring-absolute
        GpuResource newRes;
        std::uint64_t copyFrame = 0;
        UINT64 newBytes = 0;
        float lastRenderAge = 0.0f;   // A4: decides whether the arrival fades
    };

    bool EnsureRing_();
    bool Alive_(const Swap& s) const;
    void IssueRequests_(std::uint64_t frameNo);
    bool IssueSwap_(std::uint32_t entryIdx, UINT wanted, float lastRenderAge = 0.0f);
    void UpdateFades_(Renderer* renderer);
    float Now_() const;
    void DropSwap_(Swap* s, std::uint64_t frameNo);
    void Readout_(std::uint64_t frameNo);

    ID3D12Device* device_ = nullptr;
    TextureUploadRing ring_;
    TextureRetireBin retire_;
    TextureStreamingIo io_;
    std::vector<Entry> entries_;
    std::vector<std::uint32_t> freeEntries_;
    std::vector<std::unique_ptr<Swap>> swaps_;
    std::vector<std::unique_ptr<IoRequest>> orphanIo_; // cancelled while the worker may still hold them
    TextureStreamingManager manager_;
    std::uint64_t frameNo_ = 0;
    std::chrono::steady_clock::time_point start_{};
    unsigned fadingCount_ = 0;
    std::size_t registered_ = 0;
    std::uint64_t swapsDone_ = 0;
    std::uint64_t swapsFailed_ = 0;
    std::uint64_t bytesStreamedIn_ = 0;
    std::chrono::steady_clock::time_point lastReadout_{};
    // CPU frame time, split by whether the frame adopted or recorded swaps: a stall shows up as a
    // max that only the swap frames have. Reset at every readout.
    std::chrono::steady_clock::time_point lastFrameTime_{};
    bool frameHadSwaps_ = false;
    double dtSumMs_ = 0.0;
    unsigned dtCount_ = 0;
    double dtMaxQuietMs_ = 0.0;
    double dtMaxSwapMs_ = 0.0;
    unsigned swapFrames_ = 0;
    unsigned lagMax_ = 0;        // most textures with wanted - resident > 2 seen by any cycle since the last readout
    unsigned lagCycles_ = 0;     // cycles (of those seen) that had any such texture
    unsigned lastCycleSeen_ = 0;
};

} // namespace streaming
