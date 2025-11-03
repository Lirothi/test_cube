#pragma once
#include <d3d12.h>
#include <array>
#include <string>
#include <vector>
#include "core/containers/inl_vector.h"

struct RenderContext {
    static constexpr size_t kMaxBindings = 4;
    static constexpr size_t kMaxConstantsBindings = 1;
    static constexpr size_t kMaxConstants = 16;

    std::array<D3D12_GPU_VIRTUAL_ADDRESS, kMaxBindings> cbv{};
    std::array<tc::inl_vector<uint32_t, kMaxConstants>, kMaxConstantsBindings> constants{};
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