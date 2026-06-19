#pragma once
#include <d3d12.h>
#include <array>
#include <string>
#include <vector>
#include "core/containers/inl_vector.h"

// Per-draw binding scratch, filled by the pass/renderable then consumed by
// Material::Bind. Every array is indexed by SHADER REGISTER (b#/t#/u#/s#),
// mirroring how root descriptors bind — a descriptor table occupies the slot of
// its first range's base register (e.g. an SRV table starting at t0 -> srvTable[0],
// a UAV table at u0 -> uavTable[0]). This makes binding invariant to root-parameter
// declaration order and to dropping unused tables (no positional counter).
struct RenderContext {
    static constexpr size_t kMaxBindings = 4;          // shader registers 0..3 per type
    static constexpr size_t kMaxConstantsBindings = 1; // root constants live at b0
    static constexpr size_t kMaxConstants = 16;

    std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxBindings> cbv{};            // root CBV (GPU VA) by b#
    std::array<tc::inl_vector<uint32_t, kMaxConstants>, kMaxConstantsBindings> constants{}; // root 32-bit constants (b0)
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxBindings> srvTable{};     // SRV descriptor table by base t#
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxBindings> uavTable{};     // UAV descriptor table by base u#
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxBindings> samplerTable{}; // sampler descriptor table by base s#

    void ClearFast() {
        cbv.fill(0);
        for (auto& v : constants) { v.clear(); }
        srvTable.fill({0});
        uavTable.fill({0});
        samplerTable.fill({0});
    }
};
