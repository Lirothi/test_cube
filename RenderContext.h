#pragma once
#include <d3d12.h>
#include <unordered_map>
#include <string>
#include <vector>

// CBV, SRV, UAV по ключу (например, "viewProj", "instanceBuffer" и т.д.)
struct RenderContext {
    std::unordered_map<uint32_t, D3D12_GPU_VIRTUAL_ADDRESS> cbv;
    std::unordered_map<uint32_t, std::vector<uint32_t>> constants;
    std::unordered_map<uint32_t, D3D12_GPU_VIRTUAL_ADDRESS> srv;
    std::unordered_map<uint32_t, D3D12_GPU_VIRTUAL_ADDRESS> uav;
    std::unordered_map<uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE> table;
    std::unordered_map<uint32_t, D3D12_GPU_DESCRIPTOR_HANDLE> samplerTable;
};