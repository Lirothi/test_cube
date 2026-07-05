#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include "rendering/core/RenderConstants.h"

class Renderer;
class Material;
class Camera;

// Rung 2 (VSM-lite) addressing constants. A virtual shadow map is a conceptually huge
// (kVirtualRes²) shadow surface split into kPageSize² pages; only the pages on-screen pixels
// need are made resident in a shared physical page pool. The page table maps a virtual page
// (per view/light-level) to a physical page in the pool.
namespace vsm
{
    inline constexpr std::uint32_t kPageSize = 128;                                  // texels per page edge

    // Step 19b: LOCAL-light virtual res (spot lights + point-light cube faces). The Step-18/19
    // first cut used kVirtualRes=8192 at a single (finest) level, which over-subscribed the pool
    // ~56× (~57k requested pages vs 1024). 2048² is ~4× the old spot/point maps' texel count and,
    // combined with the mip pyramid below (per-pixel level selection), keeps the request set
    // within the pool. Directional shadows stay on the existing CSM (Pass_CSM) until Step 24 gives
    // them a clipmap with its own resolution.
    inline constexpr std::uint32_t kVirtualRes = 2048;                               // finest-level virtual res per local view
    inline constexpr std::uint32_t kVirtualPagesL0Axis = kVirtualRes / kPageSize;    // 16 (level-0 pages per axis)

    // Mip pyramid: level L has (kVirtualRes>>L)/kPageSize pages per axis, down to 1×1. Each pixel
    // marks exactly ONE page, at the level whose shadow-texel density ≈ its screen-pixel density,
    // so far receivers request coarse (few) pages instead of finest-level ones — the 57k→pool fix.
    inline constexpr std::uint32_t kNumMipLevels = 5;                                // 16,8,4,2,1 for 2048²
    inline constexpr std::uint32_t kMaxMipLevel = kNumMipLevels - 1;                 // 4

    inline constexpr std::uint32_t LevelPagesPerAxis(std::uint32_t level) { return kVirtualPagesL0Axis >> level; }
    inline constexpr std::uint32_t LevelPageCount(std::uint32_t level) { return LevelPagesPerAxis(level) * LevelPagesPerAxis(level); }
    // Prefix sum of level page counts within a view: LevelPageOffset(L) = pages of levels 0..L-1.
    inline constexpr std::uint32_t LevelPageOffset(std::uint32_t level)
    {
        std::uint32_t sum = 0;
        for (std::uint32_t l = 0; l < level; ++l) { sum += LevelPageCount(l); }
        return sum;
    }
    inline constexpr std::uint32_t kPagesPerView = LevelPageOffset(kNumMipLevels);   // 341 = 256+64+16+4+1

    inline constexpr std::uint32_t kPoolTexels = 4096;                               // physical pool edge (~32 MB @ D16)
    inline constexpr std::uint32_t kPoolPagesPerAxis = kPoolTexels / kPageSize;      // 32
    inline constexpr std::uint32_t kPoolPageCount = kPoolPagesPerAxis * kPoolPagesPerAxis; // 1024

    // Step 19b: LOCAL lights only — spots + point-light cube faces, NO CSM cascades (directional
    // stays on Pass_CSM until Step 24). render::kMaxShadowViews (36) = 4 cascades + this set, so
    // subtract the cascades. Layout: [spots (kMaxShadowedSpotLights) | point faces
    // (kMaxShadowedPointLights*6)] = 8 + 24 = 32.
    inline constexpr std::uint32_t kNumCascades = 4;                                 // == SceneFrameData::kCascades
    inline constexpr std::uint32_t kMaxVirtualViews = render::kMaxShadowViews - kNumCascades; // 32
    inline constexpr std::uint32_t kPageTableEntries = kPagesPerView * kMaxVirtualViews; // 341*32 = 10912

    // Page-table entry packing (a single uint per virtual page). Unused until Step 20 fills it.
    //   bit 31      : resident (1 = mapped to a physical page)
    //   bits 0..15  : physical page index (0..kPoolPageCount-1)
    inline constexpr std::uint32_t kPageResidentBit = 0x80000000u;
    inline constexpr std::uint32_t kPagePhysicalMask = 0x0000FFFFu;

    // Step 19: the page-request set is a bitfield (1 bit per virtual page).
    inline constexpr std::uint32_t kRequestWords = (kPageTableEntries + 31u) / 32u; // 341 uints

    // Step 19b perf: run the request pass at 1/kRequestDownscale per axis (≈1/64 the threads at 8),
    // the biggest win over the ~1.2 ms full-res single-level pass. Coarse 128² pages + mip
    // selection keep sub-sampled screen coverage adequate for page discovery.
    inline constexpr std::uint32_t kRequestDownscale = 8;

    // Step 19b level selection: level = clamp(log2(distCamera / kLodRefDist), 0, kMaxMipLevel).
    // Receivers within ~kLodRefDist of the camera use the finest level; each doubling of distance
    // steps one level coarser (dense-near / sparse-far). Tuned against the request-count log.
    inline constexpr float kLodRefDist = 10.0f;

    // Step 20 allocation: free a resident physical page that has not been requested for this many
    // frames (LRU). Small enough that pages the camera moved off release promptly; large enough
    // not to thrash pages that flicker in/out of the sub-sampled request set.
    inline constexpr std::uint32_t kLruFrameThreshold = 16;

    // Per-shadow-view data the request pass projects screen pixels through (mirrors the HLSL
    // cbuffer in vsm_page_request_cs.hlsl). params.x = valid (0/1), .y = zNear, .z = zFar.
    struct alignas(16) ViewProjEntry
    {
        DirectX::XMFLOAT4X4 viewProj;
        DirectX::XMFLOAT4   params;
    };
    struct alignas(16) PageRequestConstants
    {
        DirectX::XMFLOAT4X4 invView;
        DirectX::XMFLOAT4X4 invProj;
        DirectX::XMFLOAT4   camPosWS;   // xyz
        DirectX::XMFLOAT4   screen;     // x=w, y=h, z=1/w, w=1/h
        DirectX::XMFLOAT4   lodParams;  // x=refDist, y=maxLevel, z=downscale, w=unused
        std::uint32_t       numViews;
        std::uint32_t       _pad[3];
        ViewProjEntry       views[kMaxVirtualViews];
    };
}

// Rung 2 / Step 18: owns the PERSISTENT (cross-frame, NOT per-frame-tripled — the pool IS the
// cache) physical page pool + page table. Allocated once; still unused (no pass reads/writes
// them until Steps 19-22). Later steps add the screen-space page-request pass, allocation,
// virtual->physical sampling, and per-page caster rendering (reusing Rung 0's cull + indirect).
class VirtualShadowMap
{
public:
    // Allocate the pool + page table once (idempotent; persistent across level switches).
    void EnsureResources(Renderer* renderer);
    bool IsAllocated() const { return pagePool_ != nullptr && pageTable_ != nullptr; }

    // Resources + views for the future VSM passes; null/{0} until allocated.
    ID3D12Resource* PagePool() const { return pagePool_.Get(); }
    ID3D12Resource* PageTable() const { return pageTable_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE PagePoolDsv() const { return poolDsv_; }     // render pages (Step 22)
    D3D12_CPU_DESCRIPTOR_HANDLE PagePoolSrv() const { return poolSrv_; }     // sample (Step 21)
    D3D12_CPU_DESCRIPTOR_HANDLE PageTableSrv() const { return pageTableSrv_; }
    D3D12_CPU_DESCRIPTOR_HANDLE PageTableUav() const { return pageTableUav_; } // written by Step 20

    // Step 19: run the screen-space page-request pass into `cl` (an open command list). Clears
    // the request bitfield, then for each visible pixel projects into every active shadow view
    // and marks the virtual page it needs. `constants` carries the camera + per-view viewProj
    // (filled by the caller from the frame's shadow views); `depthSrv` is the camera depth.
    // Produces the request set; NOTHING consumes it yet (Step 20 allocates from it).
    void RecordPageRequest(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                           const vsm::PageRequestConstants& constants,
                           D3D12_CPU_DESCRIPTOR_HANDLE depthSrv, UINT screenW, UINT screenH);

    // Step 20: consume the request bitfield produced by RecordPageRequest (same command list,
    // right after it) and turn it into physical-page assignments with cross-frame caching: touch
    // resident+requested pages (LRU keep-alive), free LRU-stale pages back to a free list, then
    // allocate a physical page for each requested-but-not-resident page and append it to the
    // needs-render list (Step 22 input). Persists the page table + free-list/LRU state across
    // frames. Nothing samples/renders the pages yet (add-dormant). Also records the debug readback.
    void RecordPageAllocate(Renderer* renderer, ID3D12GraphicsCommandList* cl);

    // Step 19/20 (temporary): a few frames in, read back the request bitfield + allocation
    // counters and log the requested-page mip histogram + resident/newly-allocated/failed counts,
    // so caching is verifiable (resident stabilizes, newly-allocated ~0 when the scene is stable).
    // Re-armed periodically. Call once per frame (main thread).
    void PollPageRequestDebug(Renderer* renderer);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource>       pagePool_;   // R16_TYPELESS depth atlas (kPoolTexels²)
    Microsoft::WRL::ComPtr<ID3D12Resource>       pageTable_;  // StructuredBuffer<uint>[kPageTableEntries]
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;    // pool DSV
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvUavHeap_; // pool SRV + page-table SRV/UAV

    D3D12_CPU_DESCRIPTOR_HANDLE poolDsv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE poolSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageTableSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageTableUav_{};

    // Step 19: page-request bitfield (1 bit / virtual page) + its UAV. DEFAULT-heap; written +
    // consumed in-frame (single buffer, cleared each frame).
    Microsoft::WRL::ComPtr<ID3D12Resource> requestBuffer_;
    D3D12_CPU_DESCRIPTOR_HANDLE           requestUav_{};

    // Step 20: persistent (cross-frame — this IS the cache) page-allocation state. All DEFAULT-heap
    // RWStructuredBuffer<uint>, kept in UNORDERED_ACCESS. physOwner/physLastFrame/free-list/LRU
    // survive level switches with the pool + page table.
    Microsoft::WRL::ComPtr<ID3D12Resource> physOwner_;     // [kPoolPageCount] physical -> virtual owner / INVALID
    Microsoft::WRL::ComPtr<ID3D12Resource> physLastFrame_; // [kPoolPageCount] physical -> last requested frame
    Microsoft::WRL::ComPtr<ID3D12Resource> freeList_;      // [kPoolPageCount] free physical indices (rebuilt per frame)
    Microsoft::WRL::ComPtr<ID3D12Resource> needsRender_;   // [kPoolPageCount] pages allocated this frame (Step 22 input)
    Microsoft::WRL::ComPtr<ID3D12Resource> allocCounters_; // [4] free / needs / fail / resident
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> allocUavHeap_; // the 5 alloc-state UAVs
    D3D12_CPU_DESCRIPTOR_HANDLE physOwnerUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE physLastFrameUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE freeListUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE needsRenderUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE allocCountersUav_{};
    bool allocInitialized_ = false; // one-shot page-table/owner init done

    void EnsureShaderResources(Renderer* renderer); // lazily create all VSM compute PSOs
    std::shared_ptr<Material> pageRequestClearMat_;  // vsm_page_request_clear_cs.hlsl
    std::shared_ptr<Material> pageRequestMat_;       // vsm_page_request_cs.hlsl
    std::shared_ptr<Material> allocInitMat_;         // vsm_page_alloc_init_cs.hlsl
    std::shared_ptr<Material> allocTouchMat_;        // vsm_page_alloc_touch_cs.hlsl
    std::shared_ptr<Material> allocFreeMat_;         // vsm_page_alloc_freelist_cs.hlsl
    std::shared_ptr<Material> allocMapMat_;          // vsm_page_alloc_map_cs.hlsl
    bool shaderResourcesTried_ = false;

    // Step 19/20 validation: deferred readback of the request bitfield + alloc counters, re-armed
    // periodically so a live/stress run samples several times. The single readback buffer holds
    // [request words | alloc counters].
    static constexpr std::uint64_t kRequestReadbackPeriod = 180; // frames between samples
    Microsoft::WRL::ComPtr<ID3D12Resource> debugReadback_;
    std::uint64_t debugReadbackFrame_ = 0;     // frame the copy was scheduled
    std::uint64_t debugReadbackDoneFrame_ = 0; // frame the last sample logged
    int           debugReadbackState_ = 0;     // 0 = not started, 1 = pending, 2 = done

    void RecordDebugReadback(Renderer* renderer, ID3D12GraphicsCommandList* cl); // Step 20: copy request+counters
    void EnsureAllocResources(Renderer* renderer); // create the alloc buffers + descriptors
};
