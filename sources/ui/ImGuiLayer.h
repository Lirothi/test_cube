#pragma once

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "rendering/core/RenderConstants.h"

class Renderer;
struct ImGui_ImplDX12_InitInfo;

// While set, the UI is built as usual (input, state, the editor's own bookkeeping) but not DRAWN: the
// presented frame is the scene and the HUD only, as a headless --shot shows it. The editor MCP's
// screenshot sets it for a `clean` capture and clears it once the picture is taken.
inline std::atomic<bool> g_suppressImGuiDraw{ false };

class ImGuiLayer
{
public:
    void Init(HWND hwnd, Renderer& renderer);
    void Shutdown();

    void BeginFrame();
    // The game half's end (game thread plan G1): ImGui::Render() closes the frame and the draw data is final; the
    // hand-over gives it to the render half (HandOver), which only records it (Render).
    void EndFrame();
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

    // Game thread plan G8b. The game half's texture IDs are VIRTUAL: a request (the resource and its view) and its
    // index under a tag, because the UI may run while the render thread draws the previous frame, before this frame's
    // descriptor slot is free. HandOver (after BeginFrame, nothing rendering) uploads the atlas textures ImGui changed
    // -- on the game thread, the queue idle -- writes each request's descriptor into the frame's slot and puts the
    // real IDs into the closed frame's draw lists (the atlas's too: the render half then never reads ImTextureData).
    // The closed frame then becomes the render half's (renderSnapshot_): the game half's next frame -- which may run
    // while the render thread records this one -- only ever touches its own.
    ImTextureID RequestTexture(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);
    void DropTextureRequests(ID3D12Resource* resource); // its descriptors are going: this frame's draws of it are cut
    void HandOver(ID3D12Device* device, UINT frameIndex, std::vector<ID3D12Resource*>& shown,
                  std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& keepAlive);

private:
    struct PreviewSrvDescriptors
    {
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::kFrameCount> cpu{};
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE, render::kFrameCount> gpu{};
        // Last SRV written into each per-frame slot, so CreateTextureIdForSrv can
        // skip the driver CreateShaderResourceView when nothing changed (previews
        // are stable across frames once Ready). Reset implicitly: releasing a
        // resource erases its whole map entry.
        std::array<D3D12_SHADER_RESOURCE_VIEW_DESC, render::kFrameCount> writtenSrv{};
        std::array<bool, render::kFrameCount> srvWritten{};
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
    bool drawDataReady_ = false; // EndFrame closed the frame; the hand-over has not taken it yet (game half)
    ImDrawData snapshot_;        // G6: the closed frame's draw lists, cloned (TakeDrawDataSnapshot) -- the game half's
    ImDrawData renderSnapshot_;  // G8b: the frame the hand-over gave the render half, with real texture IDs
    bool renderReady_ = false;   // ... and not recorded yet (render half)
    static void FreeSnapshot(ImDrawData& data);
    struct TextureRequest
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource; // null once dropped
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    };
    std::vector<TextureRequest> textureRequests_;
    static constexpr ImTextureID kVirtualTextureTag = ImTextureID(1) << 62; // above any GPU descriptor address
    void TakeDrawDataSnapshot();
    void FreeDrawDataSnapshot();
    bool pendingRightClickFocusClear_ = false;
};
