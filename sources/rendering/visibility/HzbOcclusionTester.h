#pragma once
// Occlusion plan S3b: the GPU half of the HZB occlusion tester -- transcription of UE's
// FHZBOcclusionTester (SceneRendering.h:398, SceneOcclusion.cpp:783-1058) onto the S3a history.
//
// UE: every primitive the visibility pass considers adds its bounds (AddBounds), one draw of
// HZBTestPS tests them all against the furthest HZB, the BGRA8 result is copied to a staging
// texture and Locked next frame (MapResults blocks). Ours: the history writes the boxes into
// this frame's region of an upload ring, Main_VisTest (after Main_Hzb) runs vis_test_cs.hlsl on
// them and copies the uint-per-box results into this frame slot's region of a readback buffer;
// the history reads them `vis.queryLatency` frames later through the same FrameResults it takes
// the S3a sample counts from. The result is binary and definite (UE :2727-2733); there is no
// stochastic re-test -- every considered primitive is tested every frame, as UE's AddHZBBounds
// (:2859-2862), because the whole set costs one dispatch.
//
// Shapes that differ from UE (no arithmetic does; see vis_test_cs.hlsl): a structured buffer of
// boxes instead of two 256x256 textures, a uint per box instead of a pixel, and the capacity
// kMaxHzbTests instead of SizeX*SizeY -- the same 65536.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "rendering/core/ResourceDeclarations.h"
#include "rendering/visibility/OcclusionHistory.h"

class Material;
class Renderer;

namespace vis
{
// Mirrors VisTestCB of vis_test_cs.hlsl.
struct HzbTestConstants
{
    Math::mat4 worldToClip;
    Math::mat4 viewToClip;
    std::int32_t viewRect[4];
    std::uint32_t hzbWidth = 0;
    std::uint32_t hzbHeight = 0;
    std::uint32_t boxCount = 0;
    std::uint32_t pad0 = 0;
};
static_assert(sizeof(HzbTestConstants) == 160, "VisTestCB layout");

class HzbOcclusionTester
{
public:
    // Device objects on first use; false (sticky) when anything failed -- the method falls back
    // to off for the session, as the query heap does.
    bool EnsureResources(Renderer* renderer);
    bool Ready() const { return !failed_ && results_ && boxes_ && readback_ && material_; }
    bool Failed() const { return failed_; }

    // The buffer the builder declares: UNORDERED_ACCESS for the dispatch, COPY_SOURCE for the
    // readback copy (its canonical, frame-end state).
    ID3D12Resource* ResultsBuffer() const { return results_.Get(); }

    // The Main_VisTest body, in two halves around the builder's second barrier point: the
    // dispatch (HZB in NON_PIXEL, results in UAV) and the copy (results in COPY_SOURCE).
    void RecordTest(Renderer* renderer, ID3D12GraphicsCommandList* cl, const OcclusionQueryPlan& plan,
                    UINT frameSlot, UINT viewWidth, UINT viewHeight,
                    D3D12_CPU_DESCRIPTOR_HANDLE hzbSrv, UINT hzbWidth, UINT hzbHeight);
    void RecordReadback(ID3D12GraphicsCommandList* cl, UINT frameSlot);

    static constexpr UINT kNoSlot = ~0u;
    UINT SlotOfFrame(std::uint64_t frameNumber) const;
    // Copies the per-box verdicts recorded for `frameNumber` (1 = visible) into `out`. Only
    // after that frame's fence: the readback is written by the GPU's copy.
    bool ReadResults(std::uint64_t frameNumber, std::vector<std::uint32_t>& out);

    void Reset();

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> boxes_;       // UPLOAD, kOcclusionBufferedFrames regions, persistently mapped
    std::uint8_t* boxesMapped_ = nullptr;
    GpuResource results_;                                // DEFAULT + UAV, one region (consumed within the pass)
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;    // READBACK, kOcclusionBufferedFrames regions
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descHeap_; // non-shader-visible: boxes SRV per region + results UAV
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kOcclusionBufferedFrames> boxesSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE resultsUav_{};
    std::shared_ptr<Material> material_;
    std::array<std::uint64_t, kOcclusionBufferedFrames> recordedFrame_{};
    std::array<std::uint32_t, kOcclusionBufferedFrames> recordedCount_{};
    std::array<std::uint32_t, kOcclusionBufferedFrames> pendingCount_{}; // boxes dispatched, awaiting the copy
    bool failed_ = false;
};
} // namespace vis
