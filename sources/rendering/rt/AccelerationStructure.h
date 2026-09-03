#pragma once

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <array>
#include <span>
#include <vector>

#include "third_party/robin_hood.h"
#include "rendering/core/RenderConstants.h" // render::kFrameCount

class Mesh;

namespace rt {

// A bottom-level acceleration structure (the built result buffer). Geometry in
// this engine is static, so a BLAS is built once and cached per mesh.
struct Blas {
    Microsoft::WRL::ComPtr<ID3D12Resource> result;
    D3D12_GPU_VIRTUAL_ADDRESS Address() const {
        return result ? result->GetGPUVirtualAddress() : 0;
    }
};

// One TLAS instance: which mesh's BLAS to reference, its world transform
// (row-major, row-vector convention — exactly as InstanceData.world is stored,
// already premultiplied with any object/base world), and an index that maps the
// hit back to the instance/mesh (becomes InstanceID +
// InstanceContributionToHitGroupIndex, consumed later by S10's hit shading).
struct InstanceEntry {
    Mesh* mesh = nullptr;
    DirectX::XMFLOAT4X4 world{};
    uint32_t instanceId = 0;
    // RW: non-zero = this instance uses a wind-deformed per-instance BLAS instead of the shared
    // static per-mesh one (filled by RtSceneAs after the deform+refit recording).
    D3D12_GPU_VIRTUAL_ADDRESS blasOverride = 0;
    // Part C alpha test: bit s set = the BLAS geometry for submesh s is built WITHOUT
    // D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE, so masked foliage surfaces as a non-opaque
    // candidate the RayQuery loops alpha-test (UE marks whole instances non-force-opaque and
    // runs an any-hit shader — RayTracingInstanceMask.cpp:219; per-geometry is strictly finer).
    // Submeshes past bit 63 stay opaque. Only the FIRST instance to reach a mesh builds its
    // BLAS (build-once cache), so this must be derived from the mesh's own slot materials,
    // not from anything per-instance.
    uint64_t nonOpaqueSlots = 0;
};

// Builds and caches BLASes for meshes and assembles a per-frame TLAS from a list
// of instances. AS result buffers live in
// D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE and must NOT be routed
// through the normal resource-state tracker (see S5). BLAS scratch is retained
// until the caller confirms the build's fence has signaled, then released via
// ReleaseCompletedScratch(). The per-frame TLAS buffers are triple-buffered
// (render::kFrameCount), so the caller must not reuse a frameIndex still in
// flight on the GPU.
class AccelerationStructureManager {
public:
    void Init(ID3D12Device5* device5) { device5_ = device5; }

    // Returns the cached BLAS for `mesh`, building it on `cmdList4` on first use
    // (with a UAV barrier). On failure the returned Blas has a null result.
    // nonOpaqueSlots: see InstanceEntry — consumed only by the first (building) call.
    const Blas& GetOrBuildBlas(Mesh* mesh, uint64_t nonOpaqueSlots,
                               ID3D12GraphicsCommandList4* cmdList4);

    // (Re)build the TLAS for `frameIndex` from `instances`. BLASes are built on
    // demand on the same cmdList4 (so the per-BLAS UAV barrier already orders
    // them before the TLAS build). Records the build + a UAV barrier and
    // (re)creates the per-frame TLAS SRV. frameIndex must be < kFrameCount.
    void BuildTlas(std::span<const InstanceEntry> instances,
                   ID3D12GraphicsCommandList4* cmdList4, UINT frameIndex);

    // CPU handle of the per-frame TLAS SRV (RAYTRACING_ACCELERATION_STRUCTURE),
    // valid after a successful BuildTlas(frameIndex). Copy it into a
    // shader-visible table like any other SRV. {0} if not yet built.
    D3D12_CPU_DESCRIPTOR_HANDLE TlasSrvCpu(UINT frameIndex) const;

    // Number of instances in the last BuildTlas(frameIndex) (0 if none).
    UINT TlasInstanceCount(UINT frameIndex) const {
        return frameIndex < tlasFrames_.size() ? tlasFrames_[frameIndex].instanceCount : 0;
    }

    // ---- RW (wind) subset: per-instance wind-deformed BLAS slots. -------------------------
    // A small pool of dynamic BLASes for NEAR wind casters: a deform pass writes each slot's
    // position stream, then the slot's BLAS is refit (ALLOW_UPDATE) from it -- or fully rebuilt
    // on a round-robin cadence, because frond tips travel on the order of a metre and a pure
    // refit degrades traversal quality. Distant plants keep the shared static per-mesh BLAS.
    // All slot buffers are single-buffered ON PURPOSE: every access is recorded on the one
    // direct queue, which executes command lists serially, so frame N+1's deform can never
    // overlap frame N's build on the GPU (unlike the TLAS instance uploads, which the CPU
    // writes and therefore must triple-buffer).
    static constexpr UINT kMaxWindBlasSlots = 24;

    // Ensures the slot's deformed-VB exists for `mesh` (reallocating on a mesh change) and
    // hands back CPU descriptors for the deform dispatch: a raw SRV over the mesh's static VB
    // and a raw UAV over the slot's position stream (both live in a manager-owned CPU-only
    // heap, so the caller stages them into the frame's shader-visible tables like any other
    // pass input). Returns false on alloc failure (sticky buildFailed_).
    bool PrepareWindSlot(UINT slot, Mesh* mesh, ID3D12GraphicsCommandList4* cmdList4,
                         D3D12_CPU_DESCRIPTOR_HANDLE& srcVbSrvCpu,
                         D3D12_CPU_DESCRIPTOR_HANDLE& dstUavCpu, UINT& vertexCount);

    // After the deform dispatch: transitions the position stream for AS input, then refits or
    // (on the cadence / first use / mesh change) rebuilds the slot's BLAS from it. Returns the
    // BLAS address, 0 on failure. nonOpaqueSlots: same per-submesh mask as GetOrBuildBlas.
    // NOTE: emits NO barriers of its own -- the caller batches the stream transitions before
    // all builds and the AS-read barriers after them, so the builds pipeline on the GPU.
    // `frameIndex` picks the per-frame-in-flight copy; see WindBlasSlot::frames for why there is
    // one. `frameNumber` now only staggers the rebuild cadence across slots.
    D3D12_GPU_VIRTUAL_ADDRESS BuildOrRefitWindSlot(UINT slot, uint64_t nonOpaqueSlots,
                                                   uint64_t frameNumber, UINT frameIndex,
                                                   ID3D12GraphicsCommandList4* cmdList4);

    // The slot's deformed position stream / BLAS result (null when unbound) -- for the caller's
    // batched barriers.
    ID3D12Resource* WindStreamResource(UINT slot) const;
    ID3D12Resource* WindBlasResource(UINT slot, UINT frameIndex) const;

    // ---- Fence-guarded retire bin ------------------------------------------------------------
    // Buffers the GPU may still be reading -- one-time BLAS scratch, an evicted wind slot's
    // stream / BLAS / scratch -- are RETIRED here instead of destroyed. Each entry carries the
    // frame it may be freed at: the retirer (GetOrBuildBlas, PrepareWindSlot) does not know the
    // frame number, so entries start unstamped and StampRetired() gives every unstamped one its
    // frame at the end of the build; ReleaseRetired(frameNo) frees the entries whose frame has
    // come (kFrameCount frames later, when that slot's fence has surely been waited on).
    //
    // Until 2026-09-03 this was one flat vector, released ALL AT ONCE when a single deadline
    // passed -- and the deadline was re-armed every frame the vector was non-empty, so it never
    // came: every buffer ever retired stayed alive for the rest of the session. A fly-through of
    // a palm grove evicts wind slots continuously, seven buffers each, which is how it surfaced:
    // memory growing by gigabytes while circling the atoll.
    void StampRetired(uint64_t retireFrame);
    void ReleaseRetired(uint64_t frameNo);
    // GPU idle only (Reset, the smoke harness): frees everything regardless of stamps.
    void ReleaseAllRetired() { retireBin_.clear(); }
    uint64_t RetireBinBytes() const;
    size_t RetireBinCount() const { return retireBin_.size(); }
    // Bytes ever handed to the bin this session. Against RetireBinBytes() it says whether the
    // bin drains: a total that climbs by gigabytes while the bin stays at a few MB is the
    // fly-through working as designed; a bin that tracks the total is the leak coming back.
    uint64_t RetiredTotalBytes() const { return retiredTotalBytes_; }

    // Drop every cached BLAS/TLAS, retained scratch, and the SRV heap.
    void Reset();

    // S13 robustness: true once any AS allocation (BLAS/TLAS result/scratch/instance
    // buffer) has failed this session. Sticky until Reset(); the caller gates RT off
    // and falls back to SSR so a low-VRAM / device-lost condition disables RT cleanly
    // instead of crashing.
    bool BuildFailed() const { return buildFailed_; }

    // Total bytes currently held by AS buffers (cached BLAS results + per-frame TLAS
    // result/scratch/instance-desc + retained scratch). For VRAM visibility/budgeting.
    uint64_t GetAsMemoryBytes() const;

    // Test/dev hook: when enabled, every AS buffer allocation fails — exercises the
    // graceful-disable → SSR-fallback path (driven by the `rt-force-as-fail` launch flag).
    static void SetForceAllocFailureForTest(bool enable);

private:
    void EnsureSrvHeap();

    struct PerFrameTlas {
        Microsoft::WRL::ComPtr<ID3D12Resource> instanceUpload; // UPLOAD: INSTANCE_DESC[]
        UINT instanceCapacity = 0;                             // in instances
        Microsoft::WRL::ComPtr<ID3D12Resource> result;         // DEFAULT, AS state
        UINT64 resultSize = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch;        // DEFAULT, UAV state
        UINT64 scratchSize = 0;
        UINT instanceCount = 0;
        // TLAS refit (S12 perf): the result is built with ALLOW_UPDATE, so when the instance set is
        // unchanged frame-to-frame (only transforms move — the rotating instanced casters) we
        // PERFORM_UPDATE in place instead of a full rebuild (~2-3x cheaper). Periodically full-rebuild
        // to keep BVH traversal quality from drifting as transforms accumulate.
        bool canUpdate = false;       // result was built updatable (safe to refit)
        UINT refitsSinceBuild = 0;    // in-place updates since the last full rebuild (bounded)
    };

    // RW: one dynamic-BLAS slot (see kMaxWindBlasSlots). Buffers persist across frames; the
    // deformed VB's state is tracked here because these are manager-internal resources that,
    // like every AS buffer, stay outside the render graph's declared-state machinery.
    struct WindBlasSlot {
        Microsoft::WRL::ComPtr<ID3D12Resource> deformedVb; // float3 * vertexCount, UAV
        Mesh* mesh = nullptr;
        UINT vertexCount = 0;

        // ONE BLAS+SCRATCH PER FRAME IN FLIGHT, and this is load-bearing rather than tidy.
        //
        // The refit writes IN PLACE (Source == Dest), and Main_BuildAS runs on the ASYNC COMPUTE
        // queue with no prereqs, so its segment is submitted first with no cross-queue wait. With
        // one shared BLAS, frame N+1's refit therefore overwrote a structure that frame N's
        // graphics RT passes (RTResolve / GlassReflections / RTDebug) were still tracing — a
        // traversal over a half-rewritten BVH, which the GPU answers with DXGI_ERROR_DEVICE_HUNG
        // and no page fault. Measured before the fix: 4 hangs in 6 long sessions with async on, 0
        // in 6 under --no-async-compute, 0 in 4 with the wind refit disabled; DRED put the stall
        // inside BUILD_AS itself.
        //
        // The deformed VB does NOT need this: only the compute queue touches it, and that queue is
        // serial, so the write and the build it feeds can never overlap.
        struct PerFrame {
            Microsoft::WRL::ComPtr<ID3D12Resource> blas;    // ALLOW_UPDATE result
            Microsoft::WRL::ComPtr<ID3D12Resource> scratch; // max(build, update) size, persistent
            UINT64 blasSize = 0;
            UINT64 scratchSize = 0;
            bool builtOnce = false;
            UINT updatesSinceBuild = 0;
        };
        std::array<PerFrame, render::kFrameCount> frames;
    };

    // How many refits a COPY absorbs before it is rebuilt from scratch.
    //
    // The cadence has to be counted in USES OF THIS COPY, not in frame numbers: a copy is only
    // touched every kFrameCount frames, so each refit now has to absorb kFrameCount frames of
    // motion instead of one. The old rule was "rebuild every 16 frames", i.e. a chain that
    // absorbed 16 frames of movement; 8 uses x 3 frames = 24, so this trades a slightly longer
    // chain for 2x the rebuild rate rather than taking 3x of either. Lower it if fronds start
    // shadowing wrong, raise it if the rebuild spikes hurt.
    static constexpr UINT kWindRefitsBeforeRebuild = 8;

    void EnsureWindDescHeap();

    ID3D12Device5* device5_ = nullptr;
    std::array<WindBlasSlot, kMaxWindBlasSlots> windSlots_;
    // CPU-only heap: 2 descriptors per wind slot (raw SRV over the source VB, raw UAV over the
    // deformed position stream), created alongside the slot's buffers.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> windDescHeap_;
    UINT windDescIncrement_ = 0;
    robin_hood::unordered_map<Mesh*, Blas> blasCache_;
    struct Retired {
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        uint64_t retireFrame = kUnstamped; // free once the build's frame number reaches this
    };
    static constexpr uint64_t kUnstamped = ~0ull;
    std::vector<Retired> retireBin_;
    void Retire(Microsoft::WRL::ComPtr<ID3D12Resource>&& buffer)
    {
        if (!buffer) { return; }
        retiredTotalBytes_ += buffer->GetDesc().Width;
        retireBin_.push_back(Retired{ std::move(buffer), kUnstamped });
    }
    uint64_t retiredTotalBytes_ = 0;
    bool buildFailed_ = false; // sticky: an AS allocation failed (see BuildFailed())

    std::array<PerFrameTlas, render::kFrameCount> tlasFrames_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_; // CPU-only, kFrameCount slots
    UINT srvIncrement_ = 0;
};

} // namespace rt
