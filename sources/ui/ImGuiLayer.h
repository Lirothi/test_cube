#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "rendering/core/RenderConstants.h"

class Renderer;
struct ImGui_ImplDX12_InitInfo;

class ImGuiLayer
{
public:
    void Init(HWND hwnd, Renderer& renderer);
    void Shutdown();

    void BeginFrame();
    void Render(ID3D12GraphicsCommandList* commandList);

    bool HandleWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    bool WantsMouse() const;
    bool WantsKeyboard() const;
    bool IsInitialized() const { return initialized_; }
    ImTextureID CreateTextureIdForSrv(ID3D12Device* device,
        ID3D12Resource* resource,
        const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc,
        UINT frameIndex);
#if WITH_EDITOR
    // Free the cached preview descriptors for a single resource (used by the
    // editor thumbnail cache when it evicts a GPU thumbnail). Idle the GPU first.
    void ReleasePreviewDescriptorsForResource(ID3D12Resource* resource);
#endif

private:
    struct PreviewSrvDescriptors
    {
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::kFrameCount> cpu{};
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, render::kFrameCount> gpu{};
    };

    void ApplyPendingRightClickFocusClear();
    void ReleasePreviewDescriptors();

    static void AllocateSrvDescriptorCallback(ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle);
    static void FreeSrvDescriptorCallback(ImGui_ImplDX12_InitInfo* info,
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle);

    D3D12_CPU_DESCRIPTOR_HANDLE AllocateSrvDescriptor();
    void FreeSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle);
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleForCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) const;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    std::unordered_map<ID3D12Resource*, PreviewSrvDescriptors> previewSrvs_;
    UINT descriptorSize_ = 0;
    uint32_t descriptorCapacity_ = 0;
    uint32_t nextDescriptorIndex_ = 0;
    std::vector<uint32_t> freeDescriptorIndices_;
    bool initialized_ = false;
    bool frameBegun_ = false;
    bool pendingRightClickFocusClear_ = false;
};
