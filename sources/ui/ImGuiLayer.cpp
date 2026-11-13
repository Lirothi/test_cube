#include "ui/ImGuiLayer.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <stdexcept>

#include "imgui.h"
#if WITH_EDITOR
#include "imgui_internal.h"
#endif
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/Renderer.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
#if WITH_EDITOR
// The editor Content Browser draws many real texture thumbnails at once, each
// costing kFrameCount preview descriptors, so it needs a much larger heap than
// the handful the engine debug tools use. Non-editor builds keep the original 64.
constexpr uint32_t kSrvDescriptorCapacity = 512;
#else
constexpr uint32_t kSrvDescriptorCapacity = 64;
#endif
constexpr float kUiScale = 1.5f;
// Shipped with the repo (SIL OFL 1.1, fonts/JetBrainsMono-OFL.txt), and picked over a system
// font for that reason. Covers Latin, Greek and the Russian / Ukrainian / Belarusian /
// Serbian Cyrillic the command bar and the model chat need (U+0400-U+045F but for the four
// grave-accented Bulgarian/Macedonian letters), and the punctuation they print. Consolas,
// the font before it, is Microsoft's and may not be redistributed (owner, 2026-11-13).
constexpr const char* kUiFontPath = "fonts/JetBrainsMono-Regular.ttf";
// ImGui sizes a font by its ascender-to-descender height, and JetBrains Mono's is 1.32 em
// where Consolas' is 1.0: at the same 13 px its glyphs would be 18 % smaller. The extra
// raster scale brings its capitals to Consolas' height (8.3 px at 13) without changing the
// line height ImGui lays rows out by, so no panel's layout moves.
constexpr float kUiFontGlyphScale = 1.15f;

#if WITH_EDITOR
void SubmitEditorDockSpace()
{
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (!viewport)
    {
        return;
    }
    // A MINIMIZED WINDOW REPORTS A 0 x 0 WORK AREA. The layout below is built ONCE, on the first
    // frame the node is missing, and DockBuilderSetNodeSize asserts on a zero size in Debug
    // (imgui.cpp: IM_ASSERT(size.x > 0 && size.y > 0)) -- a modal MessageBox on a headless gate
    // launched with -WindowStyle Minimized. Skipping only the builder would not do: the dockspace
    // submitted below creates the node itself, and the custom layout would then never be built. So
    // the whole dockspace waits for a frame with a real client area; a minimized window shows nothing.
    if (viewport->WorkSize.x <= 0.0f || viewport->WorkSize.y <= 0.0f)
    {
        return;
    }

    // An explicit id lets us build the layout before DockSpaceOverViewport submits its host.
    const ImGuiID dockspaceId = ImHashStr("EditorDockSpace");
    if (ImGui::DockBuilderGetNode(dockspaceId) == nullptr)
    {
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodePos(dockspaceId, viewport->WorkPos);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        ImGuiID center = dockspaceId;
        const ImGuiID left = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Left, 0.22f, nullptr, &center);
        ImGuiID right = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Right, 0.24f, nullptr, &center);
        const ImGuiID bottom = ImGui::DockBuilderSplitNode(
            center, ImGuiDir_Down, 0.30f, nullptr, &center);

        const ImGuiID levelEditor = ImGui::DockBuilderSplitNode(
            right, ImGuiDir_Down, 0.35f, nullptr, &right);
        const ImGuiID commandHistory = ImGui::DockBuilderSplitNode(
            right, ImGuiDir_Down, 0.26f, nullptr, &right);

        ImGui::DockBuilderDockWindow("Scene Outliner", left);
        ImGui::DockBuilderDockWindow("Content Browser", bottom);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Command History", commandHistory);
        // The visible title changes when the document is dirty, but ###LevelEditor keeps this id.
        ImGui::DockBuilderDockWindow("LevelEditor", levelEditor);
        ImGui::DockBuilderFinish(dockspaceId);
    }

    constexpr ImGuiDockNodeFlags flags =
        ImGuiDockNodeFlags_PassthruCentralNode |
        ImGuiDockNodeFlags_NoDockingOverCentralNode;
    ImGui::DockSpaceOverViewport(dockspaceId, viewport, flags);
}
#endif
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

    if (FAILED(gpu::CreateDescriptorHeap(device, &heapDesc, MemTag::Ui, L"ImGui.SrvHeap",
            IID_PPV_ARGS(srvHeap_.ReleaseAndGetAddressOf()))))
    {
        throw std::runtime_error("ImGuiLayer::Init failed to create ImGui SRV descriptor heap");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
#if WITH_EDITOR
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
#endif
    io.ConfigWindowsMoveFromTitleBarOnly = true;
#if TC_PACKAGED
    io.IniFilename = "settings/imgui.ini"; // the current directory is the user's folder (main.cpp, plan §6)
#endif

    ImGuiStyle& style = ImGui::GetStyle();
    style.FontScaleMain = kUiScale;
    style.ScaleAllSizes(kUiScale);

    // ImGui's built-in font is ASCII-only, and a glyph it does not have is drawn as '?'.
    // That is what every Cyrillic character typed into the command bar and the model chat
    // turned into. Nothing was wrong with the text: the same phrases reach the model intact
    // and come back with the right answer, and the headless harness has been running
    // Russian through this layer all along. Only the atlas was missing the glyphs.
    //
    // The font ships with the repo (kUiFontPath), so this needs no system font path and no
    // download. It is monospace like the built-in font, so the editor's metrics do not shift;
    // 13.0f is the built-in font's own size (the line height), kUiFontGlyphScale the glyphs',
    // and FontScaleMain above still does the UI scaling. ImGui 1.92 rasterises glyphs on
    // demand, so the glyph-range tables older versions required are not needed here.
    // Read through the VFS; the atlas owns the copy (IM_ALLOC, freed by ImGui) and rasterises from it on demand.
    std::vector<std::uint8_t> fontBytes;
    if (vfs::ReadAll(kUiFontPath, fontBytes) && !fontBytes.empty())
    {
        void* fontData = IM_ALLOC(fontBytes.size());
        std::memcpy(fontData, fontBytes.data(), fontBytes.size());
        ImFontConfig fontConfig;
        fontConfig.ExtraSizeScale = kUiFontGlyphScale;
        io.Fonts->AddFontFromMemoryTTF(fontData, static_cast<int>(fontBytes.size()), 13.0f, &fontConfig);
    }
    else
    {
        // Not fatal -- the built-in font still draws Latin. But it is the whole reason
        // Cyrillic would be question marks again, so it must not fail silently.
        LOG_WARNING(logging::LogCategory::App,
            "UI font '{}' not found (cwd must be the repo root); falling back to the "
            "built-in ASCII-only font, so non-Latin text will render as '?'", kUiFontPath);
    }

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
        previewSrvs_.clear();
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
    FreeDrawDataSnapshot();
    FreeSnapshot(renderSnapshot_);
    renderReady_ = false;
    textureRequests_.clear();

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    srvHeap_.Reset();
    previewSrvs_.clear();
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

    drawDataReady_ = false; // a closed frame nobody recorded is dropped with it

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    frameBegun_ = true;

#if WITH_EDITOR
    SubmitEditorDockSpace();
#endif

    ApplyPendingRightClickFocusClear();
}

void ImGuiLayer::EndFrame()
{
    if (!initialized_ || !frameBegun_)
    {
        return;
    }
    ImGui::Render();
    frameBegun_ = false;
    TakeDrawDataSnapshot();
    drawDataReady_ = true;
}

// Game thread plan G6: the frame's draw lists as the game half closed them -- the render half records THESE, not
// ImGui's own lists, which the next NewFrame starts rewriting. Their texture IDs are made real at the hand-over (G8b).
void ImGuiLayer::TakeDrawDataSnapshot()
{
    FreeDrawDataSnapshot();
    const ImDrawData* src = ImGui::GetDrawData();
    if (!src)
    {
        return;
    }
    snapshot_.Valid = src->Valid;
    snapshot_.TotalIdxCount = src->TotalIdxCount;
    snapshot_.TotalVtxCount = src->TotalVtxCount;
    snapshot_.DisplayPos = src->DisplayPos;
    snapshot_.DisplaySize = src->DisplaySize;
    snapshot_.FramebufferScale = src->FramebufferScale;
    snapshot_.OwnerViewport = src->OwnerViewport;
    snapshot_.Textures = src->Textures;
    for (ImDrawList* list : src->CmdLists)
    {
        snapshot_.CmdLists.push_back(list->CloneOutput());
    }
    snapshot_.CmdListsCount = snapshot_.CmdLists.Size;
}

void ImGuiLayer::FreeSnapshot(ImDrawData& data)
{
    for (ImDrawList* list : data.CmdLists)
    {
        IM_DELETE(list);
    }
    data.Clear();
}

void ImGuiLayer::FreeDrawDataSnapshot()
{
    FreeSnapshot(snapshot_);
}

// The render half: the frame the hand-over gave it, nothing of the game half's (whose next UI frame may be open now).
void ImGuiLayer::Render(ID3D12GraphicsCommandList* commandList)
{
    if (!initialized_ || !commandList)
    {
        return;
    }
    if (!renderReady_)
    {
        return;
    }
    renderReady_ = false;
    if (g_suppressImGuiDraw.load(std::memory_order_relaxed))
    {
        return; // the frame is still closed, only its draw data goes unrecorded
    }
    ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(&renderSnapshot_, commandList);
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

bool ImGuiLayer::WantsTextInput() const
{
    return initialized_ && ImGui::GetIO().WantTextInput;
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

    PreviewSrvDescriptors& descriptors = previewSrvs_[resource];
    if (descriptors.cpu[frameIndex].ptr == 0)
    {
        descriptors.cpu[frameIndex] = AllocateSrvDescriptor();
        descriptors.gpu[frameIndex] =
            GpuHandleForCpuHandle(descriptors.cpu[frameIndex]);
    }

    // The slot, resource, and srvDesc are stable across frames once a preview is
    // Ready, so only touch the driver when the descriptor is new or actually
    // changed. This turns the steady-state per-thumbnail cost into a map lookup
    // instead of a CreateShaderResourceView every frame.
    if (!descriptors.srvWritten[frameIndex] ||
        std::memcmp(&descriptors.writtenSrv[frameIndex], &srvDesc, sizeof(srvDesc)) != 0)
    {
        device->CreateShaderResourceView(resource,
            &srvDesc,
            descriptors.cpu[frameIndex]);
        descriptors.writtenSrv[frameIndex] = srvDesc;
        descriptors.srvWritten[frameIndex] = true;
    }
    return static_cast<ImTextureID>(descriptors.gpu[frameIndex].ptr);
}

ImTextureID ImGuiLayer::RequestTexture(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
    if (!initialized_ || !srvHeap_ || !resource)
    {
        return ImTextureID_Invalid;
    }
    // One request per view of a resource a frame: an icon atlas is asked for at every icon.
    for (std::size_t i = 0; i < textureRequests_.size(); ++i)
    {
        const TextureRequest& r = textureRequests_[i];
        if (r.resource.Get() == resource && std::memcmp(&r.srv, &srvDesc, sizeof(srvDesc)) == 0)
        {
            return kVirtualTextureTag | static_cast<ImTextureID>(i);
        }
    }
    textureRequests_.push_back(TextureRequest{ resource, srvDesc });
    return kVirtualTextureTag | static_cast<ImTextureID>(textureRequests_.size() - 1u);
}

bool ImGuiLayer::HasTextureUpdates() const
{
    if (!initialized_)
    {
        return false;
    }
    for (const ImTextureData* tex : ImGui::GetPlatformIO().Textures)
    {
        if (tex->Status != ImTextureStatus_OK)
        {
            return true;
        }
    }
    return false;
}

void ImGuiLayer::DropTextureRequests(ID3D12Resource* resource)
{
    for (TextureRequest& r : textureRequests_)
    {
        if (r.resource.Get() == resource)
        {
            r.resource.Reset();
        }
    }
}

void ImGuiLayer::HandOver(ID3D12Device* device, UINT frameIndex, std::vector<ID3D12Resource*>& shown,
                          std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& keepAlive)
{
    if (!initialized_)
    {
        return;
    }
    // The atlas textures ImGui created or changed this frame (1.92 rasterises glyphs on demand): uploaded here, where
    // nothing records, instead of inside RenderDrawData on the render half. The backend submits and waits on its own.
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
    {
        if (tex->Status != ImTextureStatus_OK)
        {
            ImGui_ImplDX12_UpdateTexture(tex);
        }
    }
    std::vector<ImTextureID> real(textureRequests_.size(), ImTextureID_Invalid);
    for (std::size_t i = 0; i < textureRequests_.size(); ++i)
    {
        TextureRequest& r = textureRequests_[i];
        if (!r.resource)
        {
            continue;
        }
        real[i] = CreateTextureIdForSrv(device, r.resource.Get(), r.srv, frameIndex);
        shown.push_back(r.resource.Get());
        keepAlive.push_back(std::move(r.resource));
    }
    textureRequests_.clear();
    if (drawDataReady_)
    {
        for (ImDrawList* list : snapshot_.CmdLists)
        {
            for (ImDrawCmd& cmd : list->CmdBuffer)
            {
                if (cmd.TexRef._TexData != nullptr)
                {
                    cmd.TexRef._TexID = cmd.TexRef._TexData->TexID; // uploaded just above
                    cmd.TexRef._TexData = nullptr;
                }
                else if ((cmd.TexRef._TexID & kVirtualTextureTag) != 0)
                {
                    const std::size_t index = static_cast<std::size_t>(cmd.TexRef._TexID & ~kVirtualTextureTag);
                    cmd.TexRef._TexID = index < real.size() ? real[index] : ImTextureID_Invalid;
                    if (cmd.TexRef._TexID == ImTextureID_Invalid)
                    {
                        cmd.ClipRect = ImVec4(0.0f, 0.0f, 0.0f, 0.0f); // a dropped texture: the backend skips it
                    }
                }
            }
        }
        snapshot_.Textures = nullptr; // uploaded above: the render half's RenderDrawData has nothing to update
        // The render half's now. Its previous one was recorded (nothing renders during the hand-over); it goes.
        FreeSnapshot(renderSnapshot_);
        std::swap(snapshot_, renderSnapshot_);
        renderReady_ = true;
        drawDataReady_ = false;
    }
}

void ImGuiLayer::ReleasePreviewDescriptors()
{
    for (auto& entry : previewSrvs_)
    {
        for (D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle : entry.second.cpu)
        {
            if (cpuHandle.ptr != 0)
            {
                FreeSrvDescriptor(cpuHandle);
                cpuHandle = {};
            }
        }
    }
    previewSrvs_.clear();
}

#if WITH_EDITOR
void ImGuiLayer::ReleasePreviewDescriptorsForResource(ID3D12Resource* resource)
{
    if (!resource)
    {
        return;
    }
    const auto it = previewSrvs_.find(resource);
    if (it == previewSrvs_.end())
    {
        return;
    }
    for (D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle : it->second.cpu)
    {
        if (cpuHandle.ptr != 0)
        {
            FreeSrvDescriptor(cpuHandle);
            cpuHandle = {};
        }
    }
    previewSrvs_.erase(it);
}
#endif // WITH_EDITOR

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
