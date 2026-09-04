#pragma once
// Occlusion plan S3a: the GPU half -- the occlusion query heap, its per-frame readback, the
// 16-box index buffer and the box PSO, and the recording of a frame's OcclusionQueryPlan.
//
// Heap: D3D12_QUERY_HEAP_TYPE_OCCLUSION (a sample COUNT -- the history's stochastic re-test needs
// LastPixelsPercentage, so BINARY_OCCLUSION would not do), kMaxOcclusionQueries queries per frame
// in flight, one region per frame slot. After the batches are drawn the used range is resolved
// into that slot's region of a READBACK buffer; ReadResults() copies it out once the frame's fence
// has been waited on (BeginFrame waited for the slot being reused; a shorter latency waits for the
// frame explicitly -- UE's Map blocks the same way, SceneOcclusion.cpp:846-853).
//
// Geometry as UE draws it (SceneOcclusion.cpp:335-363, :465-534): a dynamic vertex buffer of
// 8 corners per box, one static index buffer for 16 boxes, DrawIndexedInstanced(36 * n) with the
// batch's first box as the base vertex. The PSO tests depth GREATER_EQUAL (our reverse-Z reading
// of CF_DepthNearOrEqual), writes neither depth nor colour. One delta: both faces are drawn
// (UE draws front faces only to halve the pixels). A back-face-only test would count the far
// side of the box, which sits BEHIND the object's own surface -- a wall would occlude itself --
// so the delta is on the conservative side and costs 12 triangles per box.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "rendering/visibility/OcclusionHistory.h"

class Material;
class Renderer;

namespace vis
{
class OcclusionQueryHeap
{
public:
    // Device objects on first use; false (sticky) when anything failed, which switches the
    // method off for the session rather than crashing in a pass body.
    bool EnsureResources(Renderer* renderer);
    bool Failed() const { return failed_; }

    // The Main_OcclusionQueries body: depth bound read-only by the caller, viewport set; draws
    // every batch of `plan` between Begin/EndQuery and resolves the used range for `frameSlot`.
    void Record(Renderer* renderer, ID3D12GraphicsCommandList* cl, const OcclusionQueryPlan& plan, UINT frameSlot);

    // The slot a frame's queries were recorded into, or kNoSlot.
    static constexpr UINT kNoSlot = ~0u;
    UINT SlotOfFrame(std::uint64_t frameNumber) const;
    // Copies the sample counts recorded for `frameNumber` into `out` (sized to the query count).
    // Only after that frame's fence: the readback is written by the GPU's resolve.
    bool ReadResults(std::uint64_t frameNumber, std::vector<std::uint64_t>& out);

    void Reset();

private:
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    Microsoft::WRL::ComPtr<ID3D12Resource> indexBuffer_;
    std::shared_ptr<Material> material_;
    std::array<std::uint64_t, kOcclusionBufferedFrames> recordedFrame_{};
    std::array<std::uint32_t, kOcclusionBufferedFrames> recordedCount_{};
    bool failed_ = false;
};
} // namespace vis
