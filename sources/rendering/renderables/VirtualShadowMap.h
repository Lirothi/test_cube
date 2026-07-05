#pragma once

#include <cstdint>
#include <d3d12.h>
#include <wrl/client.h>

#include "rendering/core/RenderConstants.h"

class Renderer;

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

private:
    Microsoft::WRL::ComPtr<ID3D12Resource>       pagePool_;   // R16_TYPELESS depth atlas (kPoolTexels²)
    Microsoft::WRL::ComPtr<ID3D12Resource>       pageTable_;  // StructuredBuffer<uint>[kPageTableEntries]
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;    // pool DSV
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvUavHeap_; // pool SRV + page-table SRV/UAV

    D3D12_CPU_DESCRIPTOR_HANDLE poolDsv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE poolSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageTableSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE pageTableUav_{};
};
