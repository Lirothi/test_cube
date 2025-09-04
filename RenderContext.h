#pragma once
#include <d3d12.h>
#include <array>
#include <string>
#include <vector>

// CBV, SRV, UAV по ключу (например, "viewProj", "instanceBuffer" и т.д.)
struct RenderContext {
    static constexpr size_t kMaxBindings = 16;

    std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxBindings> cbv{};
    std::array<std::vector<uint32_t>, kMaxBindings> constants{};
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxBindings> srv{};
    std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxBindings> uav{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxBindings> table{};
    std::array<D3D12_GPU_DESCRIPTOR_HANDLE, kMaxBindings> samplerTable{};

    void ClearFast() {
        cbv.fill(0);
        for (auto& v : constants) { v.clear(); }
        srv.fill(0);
        uav.fill(0);
        table.fill({0});
        samplerTable.fill({0});
    }
};