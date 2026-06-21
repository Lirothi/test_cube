#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

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

private:
    void ApplyPendingRightClickFocusClear();

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
    UINT descriptorSize_ = 0;
    uint32_t descriptorCapacity_ = 0;
    uint32_t nextDescriptorIndex_ = 0;
    std::vector<uint32_t> freeDescriptorIndices_;
    bool initialized_ = false;
    bool frameBegun_ = false;
    bool pendingRightClickFocusClear_ = false;
};
