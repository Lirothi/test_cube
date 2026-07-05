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
    inline constexpr std::uint32_t kVirtualRes = 8192;                               // virtual shadow res per view/level
    inline constexpr std::uint32_t kVirtualPagesPerAxis = kVirtualRes / kPageSize;   // 64
    inline constexpr std::uint32_t kVirtualPagesPerView = kVirtualPagesPerAxis * kVirtualPagesPerAxis; // 4096

    inline constexpr std::uint32_t kPoolTexels = 4096;                               // physical pool edge (~32 MB @ D16)
    inline constexpr std::uint32_t kPoolPagesPerAxis = kPoolTexels / kPageSize;      // 32
    inline constexpr std::uint32_t kPoolPageCount = kPoolPagesPerAxis * kPoolPagesPerAxis; // 1024

    // First cut: one virtual view per Rung-0 shadow-view slot (4 CSM cascades + spots + point
    // faces). Step 24's directional clipmap will re-slice this into clipmap levels.
    inline constexpr std::uint32_t kMaxVirtualViews = render::kMaxShadowViews;        // 36
    inline constexpr std::uint32_t kPageTableEntries = kVirtualPagesPerView * kMaxVirtualViews; // ~147456

    // Page-table entry packing (a single uint per virtual page). Unused until Step 20 fills it.
    //   bit 31      : resident (1 = mapped to a physical page)
    //   bits 0..15  : physical page index (0..kPoolPageCount-1)
    inline constexpr std::uint32_t kPageResidentBit = 0x80000000u;
    inline constexpr std::uint32_t kPagePhysicalMask = 0x0000FFFFu;

    // Step 19: the page-request set is a bitfield (1 bit per virtual page).
    inline constexpr std::uint32_t kRequestWords = (kPageTableEntries + 31u) / 32u; // ~4608 uints

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

    // Step 19 (temporary): once, a few frames in, read back the request bitfield and log the
    // total + per-first-few-views requested-page counts, so the mechanism is verifiable
    // (counts change as the camera moves). Call once per frame (main thread).
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
    // consumed in-frame (single buffer, cleared each frame). srvUavHeap slot 3.
    Microsoft::WRL::ComPtr<ID3D12Resource> requestBuffer_;
    D3D12_CPU_DESCRIPTOR_HANDLE           requestUav_{};

    void EnsureShaderResources(Renderer* renderer); // lazily create the request compute PSOs
    std::shared_ptr<Material> pageRequestClearMat_;  // vsm_page_request_clear_cs.hlsl
    std::shared_ptr<Material> pageRequestMat_;       // vsm_page_request_cs.hlsl
    bool shaderResourcesTried_ = false;

    // Step 19 validation: deferred readback of the request bitfield.
    Microsoft::WRL::ComPtr<ID3D12Resource> requestReadback_;
    std::uint64_t requestReadbackFrame_ = 0;
    int           requestReadbackState_ = 0; // 0 = not started, 1 = pending, 2 = done
};
