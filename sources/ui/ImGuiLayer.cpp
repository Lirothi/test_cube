#include "ui/ImGuiLayer.h"

#include <algorithm>
#include <cassert>
#include <stdexcept>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/Renderer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr uint32_t kSrvDescriptorCapacity = 64;
constexpr float kUiScale = 1.5f;
} // namespace

void ImGuiLayer::Init(HWND hwnd, Renderer& renderer)
{
    if (initialized_)
    {
        return;
    }

    ID3D12Device* device = renderer.GetDevice();
    if (!hwnd || !device || !renderer.GetCommandQueue())
    {
        throw std::runtime_error("ImGuiLayer::Init requires a valid window, D3D12 device, and command queue");
    }

    descriptorCapacity_ = kSrvDescriptorCapacity;
    descriptorSize_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    nextDescriptorIndex_ = 0;
    freeDescriptorIndices_.clear();

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = descriptorCapacity_;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.NodeMask = 0;

    if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(srvHeap_.ReleaseAndGetAddressOf()))))
    {
        throw std::runtime_error("ImGuiLayer::Init failed to create ImGui SRV descriptor heap");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    //io.IniFilename = nullptr;

    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleMain = kUiScale;
    style.ScaleAllSizes(kUiScale);

    if (!ImGui_ImplWin32_Init(hwnd))
    {
        Shutdown();
        throw std::runtime_error("ImGui_ImplWin32_Init failed");
    }

    ImGui_ImplDX12_InitInfo initInfo{};
    initInfo.Device = device;
    initInfo.CommandQueue = renderer.GetCommandQueue();
    initInfo.NumFramesInFlight = static_cast<int>(render::kFrameCount);
    initInfo.RTVFormat = renderer.GetBackbufferFormat();
    initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
    initInfo.UserData = this;
    initInfo.SrvDescriptorHeap = srvHeap_.Get();
    initInfo.SrvDescriptorAllocFn = ImGuiLayer::AllocateSrvDescriptorCallback;
    initInfo.SrvDescriptorFreeFn = ImGuiLayer::FreeSrvDescriptorCallback;

    if (!ImGui_ImplDX12_Init(&initInfo))
    {
        Shutdown();
        throw std::runtime_error("ImGui_ImplDX12_Init failed");
    }

    initialized_ = true;
}

void ImGuiLayer::Shutdown()
{
    if (!initialized_)
    {
        srvHeap_.Reset();
        previewSrvCpu_.fill({});
        previewSrvGpu_.fill({});
        descriptorSize_ = 0;
        descriptorCapacity_ = 0;
        nextDescriptorIndex_ = 0;
        freeDescriptorIndices_.clear();
        frameBegun_ = false;
        pendingRightClickFocusClear_ = false;
        return;
    }

    if (frameBegun_)
    {
        ImGui::EndFrame();
        frameBegun_ = false;
    }

    ReleasePreviewDescriptors();

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    srvHeap_.Reset();
    previewSrvCpu_.fill({});
    previewSrvGpu_.fill({});
    descriptorSize_ = 0;
    descriptorCapacity_ = 0;
    nextDescriptorIndex_ = 0;
    freeDescriptorIndices_.clear();
    frameBegun_ = false;
    pendingRightClickFocusClear_ = false;
    initialized_ = false;
}

void ImGuiLayer::BeginFrame()
{
    if (!initialized_)
    {
        return;
    }

    if (frameBegun_)
    {
        ImGui::EndFrame();
        frameBegun_ = false;
    }

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    frameBegun_ = true;

    ApplyPendingRightClickFocusClear();
}

void ImGuiLayer::Render(ID3D12GraphicsCommandList* commandList)
{
    if (!initialized_ || !commandList || !frameBegun_)
    {
        return;
    }

    ImGui::Render();
    frameBegun_ = false;
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
}

bool ImGuiLayer::HandleWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (!initialized_)
    {
        return false;
    }

    const bool handled = ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam) != 0;
    if (message == WM_RBUTTONDOWN || message == WM_RBUTTONDBLCLK)
    {
        pendingRightClickFocusClear_ = true;
    }
    return handled;
}

void ImGuiLayer::ApplyPendingRightClickFocusClear()
{
    if (!pendingRightClickFocusClear_)
    {
        return;
    }
    pendingRightClickFocusClear_ = false;

    constexpr ImGuiHoveredFlags hoverFlags =
        ImGuiHoveredFlags_AnyWindow |
        ImGuiHoveredFlags_AllowWhenBlockedByPopup |
        ImGuiHoveredFlags_AllowWhenBlockedByActiveItem;
    if (ImGui::IsWindowHovered(hoverFlags))
    {
        return;
    }

    ImGui::SetWindowFocus(nullptr);

    ImGuiIO& io = ImGui::GetIO();
    io.WantCaptureMouse = false;
    io.WantCaptureKeyboard = false;
    io.WantTextInput = false;
}

bool ImGuiLayer::WantsMouse() const
{
    return initialized_ && ImGui::GetIO().WantCaptureMouse;
}

bool ImGuiLayer::WantsKeyboard() const
{
    return initialized_ && ImGui::GetIO().WantCaptureKeyboard;
}

ImTextureID ImGuiLayer::CreateTextureIdForSrv(ID3D12Device* device,
    ID3D12Resource* resource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc,
    UINT frameIndex)
{
    if (!initialized_ || !srvHeap_ || !device || !resource || frameIndex >= render::kFrameCount)
    {
        return ImTextureID_Invalid;
    }

    if (previewSrvCpu_[frameIndex].ptr == 0)
    {
        previewSrvCpu_[frameIndex] = AllocateSrvDescriptor();
        previewSrvGpu_[frameIndex] = GpuHandleForCpuHandle(previewSrvCpu_[frameIndex]);
    }

    device->CreateShaderResourceView(resource, &srvDesc, previewSrvCpu_[frameIndex]);
    return static_cast<ImTextureID>(previewSrvGpu_[frameIndex].ptr);
}

void ImGuiLayer::ReleasePreviewDescriptors()
{
    for (D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle : previewSrvCpu_)
    {
        if (cpuHandle.ptr != 0)
        {
            FreeSrvDescriptor(cpuHandle);
            cpuHandle = {};
        }
    }
    previewSrvGpu_.fill({});
}

void ImGuiLayer::AllocateSrvDescriptorCallback(ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    assert(info != nullptr);
    assert(outCpuHandle != nullptr);
    assert(outGpuHandle != nullptr);

    auto* layer = static_cast<ImGuiLayer*>(info ? info->UserData : nullptr);
    assert(layer != nullptr);
    if (!layer)
    {
        *outCpuHandle = {};
        *outGpuHandle = {};
        return;
    }

    *outCpuHandle = layer->AllocateSrvDescriptor();
    *outGpuHandle = layer->GpuHandleForCpuHandle(*outCpuHandle);
}

void ImGuiLayer::FreeSrvDescriptorCallback(ImGui_ImplDX12_InitInfo* info,
    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle,
    D3D12_GPU_DESCRIPTOR_HANDLE /*gpuHandle*/)
{
    auto* layer = static_cast<ImGuiLayer*>(info ? info->UserData : nullptr);
    assert(layer != nullptr);
    if (layer)
    {
        layer->FreeSrvDescriptor(cpuHandle);
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE ImGuiLayer::AllocateSrvDescriptor()
{
    if (!srvHeap_ || descriptorSize_ == 0)
    {
        throw std::runtime_error("ImGuiLayer::AllocateSrvDescriptor called before initialization");
    }

    uint32_t index = 0;
    if (!freeDescriptorIndices_.empty())
    {
        index = freeDescriptorIndices_.back();
        freeDescriptorIndices_.pop_back();
    }
    else
    {
        if (nextDescriptorIndex_ >= descriptorCapacity_)
        {
            throw std::runtime_error("ImGui SRV descriptor heap exhausted");
        }
        index = nextDescriptorIndex_++;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE handle = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
    return handle;
}

void ImGuiLayer::FreeSrvDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle)
{
    if (!srvHeap_ || descriptorSize_ == 0 || cpuHandle.ptr == 0)
    {
        return;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE start = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    if (cpuHandle.ptr < start.ptr)
    {
        assert(false && "ImGuiLayer::FreeSrvDescriptor received a handle before the heap start");
        return;
    }

    const SIZE_T byteOffset = cpuHandle.ptr - start.ptr;
    if ((byteOffset % descriptorSize_) != 0)
    {
        assert(false && "ImGuiLayer::FreeSrvDescriptor received an unaligned descriptor handle");
        return;
    }

    const uint32_t index = static_cast<uint32_t>(byteOffset / descriptorSize_);
    if (index >= descriptorCapacity_)
    {
        assert(false && "ImGuiLayer::FreeSrvDescriptor received a handle outside the heap");
        return;
    }

    if (std::find(freeDescriptorIndices_.begin(), freeDescriptorIndices_.end(), index) == freeDescriptorIndices_.end())
    {
        freeDescriptorIndices_.push_back(index);
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE ImGuiLayer::GpuHandleForCpuHandle(D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle{};
    if (!srvHeap_ || descriptorSize_ == 0 || cpuHandle.ptr == 0)
    {
        return gpuHandle;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    const D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = srvHeap_->GetGPUDescriptorHandleForHeapStart();
    if (cpuHandle.ptr < cpuStart.ptr)
    {
        assert(false && "ImGuiLayer::GpuHandleForCpuHandle received a handle before the heap start");
        return gpuHandle;
    }

    const SIZE_T byteOffset = cpuHandle.ptr - cpuStart.ptr;
    if ((byteOffset % descriptorSize_) != 0)
    {
        assert(false && "ImGuiLayer::GpuHandleForCpuHandle received an unaligned descriptor handle");
        return gpuHandle;
    }

    const uint32_t index = static_cast<uint32_t>(byteOffset / descriptorSize_);
    if (index >= descriptorCapacity_)
    {
        assert(false && "ImGuiLayer::GpuHandleForCpuHandle received a handle outside the heap");
        return gpuHandle;
    }

    gpuHandle = gpuStart;
    gpuHandle.ptr += static_cast<UINT64>(index) * descriptorSize_;
    return gpuHandle;
}
