#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/shadows/SdsmPartitions.h"

class Renderer;
class Material;
struct RenderGraphPassContext;

// ---------------------------------------------------------------------------------------------
// SDSM (docs/csm_improvement_plan.md S15) -- the owner of everything the third shadow mode adds:
// the partition buffer, the reduction scratch, the GPU-written cull-plane buffer, the five
// analyze PSOs and the readout ring.
//
// WHY A SEPARATE CLASS. ShadowGpuData owns the caster set and the cull; VirtualShadowMap owns the
// page pool. SDSM owns neither -- it REPLACES the CPU cascade fit with a compute reduction and
// hands the result to both of them. Putting it inside ShadowGpuData would have made a class that
// is already 3k lines also responsible for a camera-depth analysis it does not otherwise know
// about, and putting it in Scene would have split the resource lifetime from the passes.
//
// LIFETIME. Resources are created on first use IN SDSM MODE and released when the mode leaves it
// (Step 24b's rule for VSM, for the same reason: only one mode's GPU memory is ever resident).
// ---------------------------------------------------------------------------------------------
class SdsmShadows
{
public:
    SdsmShadows();
    ~SdsmShadows();
    SdsmShadows(const SdsmShadows&) = delete;
    SdsmShadows& operator=(const SdsmShadows&) = delete;

    // Allocate the buffers + build the PSOs. Idempotent; safe to call every frame. Returns false
    // if anything failed -- the caller then keeps the mode's passes out of the graph, which is
    // what makes a failure a fallback to Legacy rather than a black screen.
    bool EnsureResources(Renderer* renderer);
    // Free the GPU memory (a mode switch away from SDSM). Keeps the PSOs: they are shared, cheap,
    // and rebuilding them on every toggle would stutter the switch.
    void ReleaseResources(Renderer* renderer);
    bool Ready() const { return ready_; }

    // Once per frame, BEFORE the graph is built (SceneRenderer::DecideFrame): everything the
    // analysis needs that only the CPU knows -- the camera, the sun's light frame, the cascade
    // config's bias knobs, the atlas geometry and how many cull-view slots to carry over.
    void SetFrameParams(const render::sdsm::AnalyzeConstants& cb);
    const render::sdsm::AnalyzeConstants& FrameParams() const { return cb_; }
    // ShadowGpuData's CPU-uploaded per-view planes for THIS frame's ring region. The analyze pass
    // copies the non-directional slots out of it, which is what lets shadow_cull_cs.hlsl read one
    // buffer and stay unaware that SDSM exists. Set every frame -- it is a ring region, so the
    // handle moves.
    void SetSourceFrustumSrv(D3D12_CPU_DESCRIPTOR_HANDLE h) { srcFrustumSrv_ = h; }

    // ---- S16 (EVSM4) --------------------------------------------------------------------
    // The MOMENTS atlas: RGBA32F of the same edge as the depth atlas, same 2x2 tile layout.
    // Allocated only while EVSM is on AND the mode is SDSM, and freed the moment either stops
    // -- it is four times the bytes per texel of the D16 it is derived from (2048: 67 MB against
    // 8.4), which is exactly why the mode pays for it by needing a SMALLER atlas, not a bigger
    // one. fp16 is not an option: the second moment of exp(42) overflows it by 30 orders.
    // Returns false if it could not be provided; the caller then keeps the PCF arm.
    //
    // BOTH OF THESE FREE A TEXTURE IMMEDIATELY, so they may ONLY be called with the GPU idle.
    // `Scene::ReconcileShadowMode` is that place -- it already stalls once for the VSM pool, the
    // SDSM buffers and the atlas edge, and the moments atlas is the same kind of decision.
    // Calling them from the per-frame update (which is where the EVSM toggle used to land) frees
    // a texture that the frames still in flight are reading: device removed, 2026-09-14.
    bool EnsureMoments(Renderer* renderer, UINT atlasRes);
    void ReleaseMoments(Renderer* renderer);
    bool MomentsReady() const { return static_cast<bool>(moments_) && momentsRes_ != 0; }
    UINT MomentsRes() const { return momentsRes_; }
    ID3D12Resource* MomentsTexture() const { return moments_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE MomentsSrv() const { return momentsSrv_; }
    // The conversion pass (Main_SdsmMoments): depth atlas -> moments. Same builder/record
    // contract as everything else here.
    struct MomentsDecisions
    {
        bool active = false;
        std::uint32_t write = 0;   // atlas -> NON_PIXEL, moments -> UAV
        std::uint32_t consume = 0; // moments -> SRV (lighting, fog, glass)
    };
    MomentsDecisions PrepareMomentsPass(RenderGraphPassContext& ctx);
    void RecordMoments(Renderer* renderer, ID3D12GraphicsCommandList* cl, const MomentsDecisions& dec);

    // pass-flow S7a contract, the same as ShadowGpuData's: the BUILDER decides and declares, the
    // record takes the decision by value so the two cannot disagree.
    struct AnalyzeDecisions
    {
        bool active = false;
        std::uint32_t prevCopy = 0; // partitions -> COPY_SOURCE, prevPartitions -> COPY_DEST
        std::uint32_t write = 0;    // every output buffer -> UAV
        std::uint32_t consume = 0;  // partitions + frustums + the two CBs -> their read states
        bool readback = false;      // copy this frame's partitions into the readout ring
        std::uint32_t readCopy = 0; // ...which needs COPY_SOURCE first
    };
    AnalyzeDecisions PrepareAnalyzePass(RenderGraphPassContext& ctx);
    void RecordAnalyze(Renderer* renderer, ID3D12GraphicsCommandList* cl, const AnalyzeDecisions& dec);

    // ---- what the consumers bind ----
    ID3D12Resource* PartitionBuffer() const { return partitions_.Get(); }
    ID3D12Resource* FrustumBuffer() const { return frustums_.Get(); }
    ID3D12Resource* ViewCbBuffer() const { return viewCBs_.Get(); }
    ID3D12Resource* CullCbBuffer() const { return cullCB_.Get(); }
    // THE ADDRESS OF A GPU-WRITTEN CONSTANT BUFFER. A D3D12 root CBV takes a GPU virtual address,
    // so a constant buffer a compute shader produced binds exactly like one the CPU uploaded --
    // which is what lets the shadow depth VS and the caster cull stay completely SDSM-unaware.
    // Alignment holds by construction: a buffer's GVA is 64 KiB-aligned and the stride is 256.
    D3D12_GPU_VIRTUAL_ADDRESS ViewCbAddress(std::uint32_t partition) const;
    D3D12_GPU_VIRTUAL_ADDRESS CullCbAddress() const;
    // SRV over the partition buffer (StructuredBuffer<SdsmPartition>) -- the depth VS, the cull,
    // lighting, fog and glass all read this one.
    D3D12_CPU_DESCRIPTOR_HANDLE PartitionSrv() const { return partitionSrv_; }
    // SRV over the GPU-written cull planes, laid out exactly like ShadowGpuData's CPU ring
    // (ShadowViewFrustum[kMaxShadowViews]) so shadow_cull_cs.hlsl binds it at t1 unchanged.
    D3D12_CPU_DESCRIPTOR_HANDLE FrustumSrv() const { return frustumSrv_; }

    // ---- readout (dev window + --sdsm-readout) ----
    // Copy of the partitions of frame N - kFrameCount, mapped once its fence has passed. Purely a
    // display path: nothing in the render chain reads it (the Anno lesson -- a readback in the
    // chain "lags several frames behind and is just too outdated").
    void PollReadout(Renderer* renderer);
    const std::array<render::sdsm::Partition, render::sdsm::kMaxPartitions>& Readout() const { return readout_; }
    bool ReadoutValid() const { return readoutValid_; }
    // Casters the GPU path does not carry this frame (GPU-instanced objects the GI fold left
    // behind). In SDSM they cannot be drawn at all -- their light matrix exists only on the GPU --
    // so this is the number that has to stay 0, and the readout is where it is visible.
    void SetCpuCasterCount(std::uint32_t n) { cpuCasters_ = n; }
    std::uint32_t CpuCasterCount() const { return cpuCasters_; }

private:
    bool EnsureBuffers(Renderer* renderer);
    bool EnsureDescriptors(Renderer* renderer);
    void EnsurePipelines(Renderer* renderer);

    render::sdsm::AnalyzeConstants cb_{};
    D3D12_CPU_DESCRIPTOR_HANDLE srcFrustumSrv_{};

    GpuResource partitions_;  // SdsmPartition[kMaxPartitions]
    // LAST frame's partitions, copied out of `partitions_` at the head of the analyze, before
    // anything overwrites them. The HZB occlusion test needs the matrix the pyramid was RENDERED
    // with, and that is last frame's.
    GpuResource prevPartitions_;
    GpuResource zbounds_;     // SdsmZBounds[1]
    GpuResource bounds_;      // SdsmBoundsUint[kMaxPartitions]
    GpuResource frustums_;    // float4[(kMaxShadowViews + 1) * kShadowViewPlanes]
    GpuResource viewCBs_;     // kMaxPartitions x 256 B, each a scene_internal::PerViewCB
    GpuResource cullCB_;      // one render::CascadeHzb::GpuParams, 560 B
    GpuResource moments_;     // S16: RGBA32F EVSM moments, atlas-sized
    UINT momentsRes_ = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> momentsHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE momentsSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE momentsUav_{};
    std::shared_ptr<Material> evsmMat_;
    std::shared_ptr<Material> evsmBlurMat_;
    // Blur scratch: ONE TILE, not one atlas. The separable pass needs an intermediate, and the
    // partitions are blurred one after another, so a tile-sized target is reused four times --
    // a quarter of the bytes of an atlas-sized ping-pong (at 2048: 16 MB against 64).
    GpuResource blurScratch_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> blurScratchHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE blurScratchSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE blurScratchUav_{};

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    D3D12_CPU_DESCRIPTOR_HANDLE zboundsUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE boundsUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE partitionUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE frustumUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE viewCbUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE cullCbUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE partitionSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE prevPartitionSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE frustumSrv_{};

    std::shared_ptr<Material> clearMat_;
    std::shared_ptr<Material> reduceZMat_;
    std::shared_ptr<Material> logMat_;
    std::shared_ptr<Material> reduceBoundsMat_;
    std::shared_ptr<Material> finalizeMat_;
    std::shared_ptr<Material> carryMat_;
    bool pipelinesTried_ = false;
    bool ready_ = false;

    // Readout ring: one partition set per frame slot, copied on the frame that wrote it and read
    // kFrameCount frames later, when the natural per-frame fence has passed it.
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    std::array<std::uint64_t, render::kFrameCount> readbackFrame_{};
    std::array<render::sdsm::Partition, render::sdsm::kMaxPartitions> readout_{};
    bool readoutValid_ = false;
    std::uint32_t cpuCasters_ = 0;
};
