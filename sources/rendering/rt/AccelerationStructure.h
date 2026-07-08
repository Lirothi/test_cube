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
    const Blas& GetOrBuildBlas(Mesh* mesh, ID3D12GraphicsCommandList4* cmdList4);

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

    // Release scratch buffers retained from BLAS builds. Call ONLY after the
    // fence for the command list(s) that ran the builds has completed.
    void ReleaseCompletedScratch() { pendingScratch_.clear(); }

    // Drop every cached BLAS/TLAS, retained scratch, and the SRV heap.
    void Reset();

    bool HasPendingScratch() const { return !pendingScratch_.empty(); }

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

    ID3D12Device5* device5_ = nullptr;
    robin_hood::unordered_map<Mesh*, Blas> blasCache_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> pendingScratch_;
    bool buildFailed_ = false; // sticky: an AS allocation failed (see BuildFailed())

    std::array<PerFrameTlas, render::kFrameCount> tlasFrames_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_; // CPU-only, kFrameCount slots
    UINT srvIncrement_ = 0;
};

} // namespace rt
