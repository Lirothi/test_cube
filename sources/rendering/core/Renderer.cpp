#include "rendering/core/Renderer.h"
#include "rendering/core/RendererInvariantFailure.h"
#include "rendering/core/CommandListBindState.h"
#include "core/Helpers.h"
#include <cassert>
#include <cmath>
#include <vector>
#include <utility>
#include <dxgidebug.h>
#include <d3d12sdklayers.h> // ID3D12Debug*, ID3D12InfoQueue
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "app/Systems.h"
#include "app/camera/Camera.h"
#include "rendering/core/DlssHandler.h"
#include "streamline/include/sl.h"

#pragma comment(lib, "dxguid.lib")

Renderer::Renderer()
    : dlssHandler_(std::make_unique<DlssHandler>(*this))
{
}

Renderer::~Renderer() {
    // Report after everything has been reset
    ReportLiveObjects(); // when the debug build includes this helper
    // (the frame fence event is closed by FrameScheduler's destructor)
}

static void logFunctionCallback(sl::LogType type, const char* msg)
{
    OutputDebugStringA(msg);
}

ID3D12GraphicsCommandList4* Renderer::AsCmdList4(ID3D12GraphicsCommandList* cl) const
{
    if (!cl) {
        return nullptr;
    }
    // QueryInterface AddRefs the same underlying object; it stays alive through
    // the caller's `cl`, so release the extra ref immediately and hand back the
    // borrowed view (valid for as long as `cl` is).
    ID3D12GraphicsCommandList4* cl4 = nullptr;
    if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl4)))) {
        cl4->Release();
        return cl4;
    }
    return nullptr;
}

void Renderer::Shutdown()
{
    // Guard against repeated calls
    static bool inShutdown = false;
    if (inShutdown) {
        return;
    }
    inShutdown = true;

    // Nothing to do if the device is missing
    const bool hasDevice = (GetDevice() != nullptr);
    const bool hasQueue = (GetCommandQueue() != nullptr);
    const bool hasFence = frameScheduler_.HasFence();

    // 0) Fully wait for the GPU (and close all outstanding command lists)
    if (hasDevice && hasQueue && hasFence) {
        WaitForPreviousFrame(); // your full-synchronization helper :contentReference[oaicite:2]{index=2}
    }

#if PROF_GPU_ENABLED
    Profiler::Get().ShutdownGpu();
#endif

    ShutdownImGui();

    debugDrawSystem_.Shutdown();
    materialManager_.Clear();
    materialDataManager_.ClearAll();
    meshManager_.Clear();
    textManager_.Clear();
    fontManager_.Clear();
    samplerManager_.Clear();

    // 1) Stop the command “timeline”: prevent further submissions. Real clear —
    // releases the pooled per-batch memory (batches refer only to command lists
    // from the frame pools).
    submitTimeline_.Clear();

    // 2) Offscreen targets (G-Buffer/Light/Scene/Depth) — destroy these first
    DestroyDeferredTargets(); // properly resets resources and heaps, and clears knownStates_ :contentReference[oaicite:4]{index=4}

    // 3) Back buffers and RTV/DSV heaps
    swapchain_.ReleaseBuffers();

    // 4) Safety measure: clear resource state tracking. The canonical declarations go with it —
    // every resource they describe has just been destroyed, and a surviving entry would hand a
    // recycled address someone else's resting state.
    stateTracker_.ClearAllKnownStates();
    canonicalStates_.Clear();

    // 5) SwapChain — exit fullscreen (if needed) and release it
    swapchain_.ReleaseSwapchain();

    // 6) Frame resources: reset pool usage and clear the upload ring
    // (the actual ComPtrs release when Renderer is destroyed, but this removes dependencies)
    frameScheduler_.ResetFrameState(GetDevice());

    // 7) Fence/Queue
    frameScheduler_.ReleaseFence();
    graphicsDevice_.ReleaseQueue();

    if (dlssHandler_)
    {
        dlssHandler_->Shutdown();
    }

    slShutdown();

    // 8) Release the device last
    graphicsDevice_.ReleaseDevice();

    inShutdown = false;
}

void Renderer::InitD3D12(HWND window, UINT width, UINT height) {
    hWnd_ = window;
    width_ = width;
    height_ = height;

    sl::Preferences pref;

    pref.applicationId = 0x12345678U;

#if _DEBUG
    pref.showConsole = true;
    pref.logMessageCallback = &logFunctionCallback;
    pref.logLevel = sl::LogLevel::eDefault;
#else
    pref.showConsole = false;
    // Perf: in release keep Streamline silent. eDefault makes slEvaluateFeature format log
    // strings and invoke the callback (which calls OutputDebugStringA — a global-locked syscall)
    // every frame; measured ~80us/frame inside Pass_Tonemap. eOff + no callback removes it.
    pref.logMessageCallback = nullptr;
    pref.logLevel = sl::LogLevel::eOff;
#endif

    pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    sl::Feature featuresToLoad[] = {
        sl::kFeatureDLSS,
#ifdef STREAMLINE_FEATURE_NIS
        sl::kFeatureNIS,
#endif
#ifdef STREAMLINE_FEATURE_DLSS_FG
        sl::kFeatureDLSS_G,
#endif
#ifdef STREAMLINE_FEATURE_REFLEX
        sl::kFeatureReflex,
#endif
        // PCL is always implicitly loaded, but request it to ensure we never have 0-sized array
        sl::kFeaturePCL
    };
    pref.featuresToLoad = featuresToLoad;
    pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(featuresToLoad));

    pref.renderAPI = sl::RenderAPI::eD3D12;

    auto slRes = slInit(pref, sl::kSDKVersion);

    // --- Device (debug layer enabled inside, in debug builds) ---
    graphicsDevice_.InitDevice();

    slSetD3DDevice(GetDevice());

    if (dlssHandler_)
    {
        dlssHandler_->OnStreamlineInitialized(slRes);
    }

    UpdateRenderResolutionFromScale();

    //sl::AdapterInfo adapterInfo{};
    //if (SL_FAILED(result, slIsFeatureSupported(sl::kFeatureDLSS, adapterInfo)))
    //{
    //    int a = 0;
    //}

    graphicsDevice_.SetupDebugBreaks();

    // --- Queue ---
    graphicsDevice_.InitQueue();

    // --- SwapChain + RTVs (render::kFrameCount) ---
    CreateSwapChainAndRTVs(width_, height_);

    // --- Depth ---
    CreateDepthResources(width_, height_);
    CreateDeferredTargets(width_, height_);

    // --- Fence + event ---
    frameScheduler_.InitFence(GetDevice());

    // --- Frame resources ---
    frameScheduler_.CreateFrameResources(GetDevice());

    AllocateDlssResourcesIfNeeded();

    RefreshCurrentFrameCaches();

    samplerManager_.Init(GetDevice(), 512);
    InitImGui();

    InitFence();
}

void Renderer::InitFence() {
    // Compatibility with your main.cpp — safe no-op if initialization already happened
    frameScheduler_.InitFence(GetDevice());
}

void Renderer::InitTextSystem(ID3D12GraphicsCommandList* uploadCl,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const std::wstring& folder)
{
    fontManager_.Init(this);
    fontManager_.LoadFromFolder(this, uploadCl, uploadKeepAlive, folder);

    textManager_.Init(this);
    if (auto* def = fontManager_.Get(L"Consolas_32"))
    //if (auto* def = fontManager_.Get(L"Consolas_32_coverage"))
    //if (auto* def = fontManager_.Get(L"cons_32"))
    {
        textManager_.SetFont(def);
    }
    auto shadowDesc = TextManager::ShadowDesc();
    shadowDesc.offsetX = 2.0f;
    shadowDesc.offsetY = 2.0f;
    shadowDesc.color.w = 0.9f;
    shadowDesc.scaleWithTextSize = true;
    textManager_.SetShadow(shadowDesc);
}

void Renderer::CreateSwapChainAndRTVs(UINT width, UINT height) {
    // Forget tracked states of the old backbuffers before they are released
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        ClearResourceState(swapchain_.Backbuffer(i));
    }

    swapchain_.Create(GetDevice(), GetCommandQueue(), hWnd_, width, height,
        GetBackbufferResourceFormat(), GetBackbufferFormat());

    currentFrameIndex_ = swapchain_.CurrentBackBufferIndex();

    for (UINT i = 0; i < render::kFrameCount; ++i) {
        SetResourceState(swapchain_.Backbuffer(i), D3D12_RESOURCE_STATE_PRESENT);
        // Step 7: the backbuffer is written by hand-rolled barriers in RecordBindAndClear
        // and the present epilogue, which the compile never sees. Excluded from it.
        SetResourceUnmanaged(swapchain_.Backbuffer(i));
    }
}

void Renderer::CreateDepthResources(UINT width, UINT height) {
    swapchain_.CreateDepth(GetDevice(), width, height, render::kDepthBufferResourceFormat, render::kDepthBufferViewFormat);
}

void Renderer::WaitForFrame(UINT frameIndex) {
    frameScheduler_.WaitForFrame(frameIndex);
}

void Renderer::SignalFrame(UINT frameIndex) {
    frameScheduler_.SignalFrame(GetCommandQueue(), frameIndex);
}

void Renderer::RefreshCurrentFrameCaches() {
    currentFrameResource_ = nullptr;
    currentFrameDescriptorHeapCount_ = 0;
    currentFrameDescriptorHeaps_.fill(nullptr);

    if (currentFrameIndex_ >= render::kFrameCount) {
        return;
    }

    FrameResource* fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
    currentFrameResource_ = fr;
    if (!fr) {
        return;
    }

    if (auto* heap = fr->GetDescAlloc().GetShaderVisibleHeap()) {
        currentFrameDescriptorHeaps_[currentFrameDescriptorHeapCount_++] = heap;
    }
    if (auto* heap = fr->GetSamplerAlloc().GetShaderVisibleHeap()) {
        currentFrameDescriptorHeaps_[currentFrameDescriptorHeapCount_++] = heap;
    }
}

void Renderer::BeginFrame() {
    CPU_SCOPE(ProfilerScopes::kRendererBeginFrame);
    // Wait for the GPU using its back buffer fence value
    WaitForFrame(currentFrameIndex_);

    if (dlssHandler_)
    {
        dlssHandler_->OnBeginFrame();
    }

    RefreshCurrentFrameCaches();
    FrameResource* fr = currentFrameResource_;

    ++totalFrameNumber_;

    // Reset per-frame pools
    if (fr) {
        fr->ResetCommandAllocators(GetDevice());
        fr->ResetCommandListsUsage();
        fr->GetDescAlloc().ResetPerFrame();
        fr->GetSamplerAlloc().ResetPerFrame();
        fr->ResetUpload();
    }

    ctxPool_.ResetForFrame();
    debugDrawSystem_.BeginFrame();
    pendingImGuiTextureResources_.clear();
}

void Renderer::EndFrame() {
    ExecuteTimelineAndPresent();
}

#if WITH_EDITOR
bool Renderer::RequestObjectIdPick(float displayX, float displayY)
{
    if (!GetDevice() || !rtManager_.IsCreated() || objectIdPickRequested_ || objectIdPickInFlight_)
    {
        return false;
    }
    if (width_ == 0 || height_ == 0 || renderWidth_ == 0 || renderHeight_ == 0)
    {
        return false;
    }
    if (!std::isfinite(displayX) || !std::isfinite(displayY))
    {
        return false;
    }
    if (displayX < 0.0f || displayY < 0.0f ||
        displayX >= static_cast<float>(width_) || displayY >= static_cast<float>(height_))
    {
        return false;
    }

    const float renderX = displayX * static_cast<float>(renderWidth_) / static_cast<float>(width_);
    const float renderY = displayY * static_cast<float>(renderHeight_) / static_cast<float>(height_);
    objectIdPickX_ = std::min(renderWidth_ - 1u, static_cast<UINT>(std::max(renderX, 0.0f)));
    objectIdPickY_ = std::min(renderHeight_ - 1u, static_cast<UINT>(std::max(renderY, 0.0f)));
    objectIdPickRequested_ = true;
    objectIdPickResultValid_ = false;
    objectIdPickResult_ = 0;
    return true;
}

void Renderer::RecordObjectIdPickReadback(ID3D12GraphicsCommandList* cl)
{
    if (!cl || !objectIdPickRequested_ || objectIdPickInFlight_ || !GetDevice())
    {
        return;
    }

    const auto& D = rtManager_.Deferred(currentFrameIndex_);
    if (!D.objectID.Get())
    {
        objectIdPickRequested_ = false;
        return;
    }

    if (!objectIdReadback_)
    {
        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ThrowIfFailed(GetDevice()->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&objectIdReadback_)));
        objectIdReadback_->SetName(L"Editor.ObjectIdReadback");
    }

    // Out-of-graph (post-frame editor pick): the before state is known — objectID rests as a
    // render target, which is also its canonical. No tracker needed.
    TransitionExplicit(cl, D.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = objectIdReadback_.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = render::kObjectIdFormat;
    dst.PlacedFootprint.Footprint.Width = 1;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = D.objectID.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    const UINT x = std::min(objectIdPickX_, renderWidth_ - 1u);
    const UINT y = std::min(objectIdPickY_, renderHeight_ - 1u);
    D3D12_BOX srcBox{ x, y, 0, x + 1u, y + 1u, 1 };
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, &srcBox);

    objectIdPickRequested_ = false;
    objectIdPickInFlight_ = true;
}

void Renderer::ResolveObjectIdPickReadback()
{
    if (!objectIdPickInFlight_)
    {
        return;
    }

    WaitForPreviousFrame();

    objectIdPickResult_ = 0;
    if (objectIdReadback_)
    {
        D3D12_RANGE readRange{ 0, sizeof(uint32_t) };
        void* mapped = nullptr;
        if (SUCCEEDED(objectIdReadback_->Map(0, &readRange, &mapped)) && mapped)
        {
            objectIdPickResult_ = *static_cast<const uint32_t*>(mapped);
            D3D12_RANGE writtenRange{ 0, 0 };
            objectIdReadback_->Unmap(0, &writtenRange);
            objectIdPickResultValid_ = true;
        }
    }

    objectIdPickInFlight_ = false;
}

bool Renderer::ConsumeObjectIdPick(uint32_t& outObjectId)
{
    if (!objectIdPickResultValid_)
    {
        return false;
    }
    outObjectId = objectIdPickResult_;
    objectIdPickResult_ = 0;
    objectIdPickResultValid_ = false;
    return true;
}

void Renderer::ResetObjectIdPickState()
{
    objectIdReadback_.Reset();
    objectIdPickX_ = 0;
    objectIdPickY_ = 0;
    objectIdPickResult_ = 0;
    objectIdPickRequested_ = false;
    objectIdPickInFlight_ = false;
    objectIdPickResultValid_ = false;
}
#endif

void Renderer::InitImGui()
{
    if (imguiLayer_.IsInitialized())
    {
        return;
    }

    if (!hWnd_ || !GetDevice() || !GetCommandQueue())
    {
        return;
    }

    imguiLayer_.Init(hWnd_, *this);
}

void Renderer::BeginImGuiFrame()
{
    imguiLayer_.BeginFrame();
}

void Renderer::RenderImGui(ID3D12GraphicsCommandList* commandList)
{
    // Step 7 — out-of-graph, explicit before state. The list holds two kinds: editor-owned
    // textures (created shader-readable, so this is a no-op) and engine render targets shown by
    // TextureDebugViewer (left at their canonical by the frame's graph work, since the overlay
    // runs last). Both answers come from the canonical registry, so no tracker is involved.
    for (ID3D12Resource* resource : pendingImGuiTextureResources_)
    {
        TransitionExplicit(commandList, resource, GetCanonicalState(resource),
                           D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    imguiLayer_.Render(commandList);
    pendingImGuiTextureResources_.clear();
}

void Renderer::RestoreGraphicsStateAfterExternalDraw(ID3D12GraphicsCommandList* commandList)
{
    if (!commandList)
    {
        return;
    }

    // ImGui directly records D3D12 state, bypassing the engine's material bind
    // cache. Rebind the main output and engine heaps, then make the next native
    // Material::Bind fully establish its pipeline and root bindings.
    RecordBindDefaultsNoClear(commandList);
    BindDescriptorHeaps(commandList);
    render::g_clBindState.Reset();
}

ImTextureID Renderer::CreateImGuiTextureId(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
    if (!resource)
    {
        return ImTextureID_Invalid;
    }

    if (std::find(pendingImGuiTextureResources_.begin(),
            pendingImGuiTextureResources_.end(),
            resource) == pendingImGuiTextureResources_.end())
    {
        pendingImGuiTextureResources_.push_back(resource);
    }
    return imguiLayer_.CreateTextureIdForSrv(GetDevice(), resource, srvDesc, currentFrameIndex_);
}

#if WITH_EDITOR
void Renderer::ReleaseImGuiTextureDescriptors(ID3D12Resource* resource)
{
    imguiLayer_.ReleasePreviewDescriptorsForResource(resource);
    stateTracker_.ClearResourceState(resource);
}

void Renderer::MarkImGuiTextureShaderReadable(ID3D12Resource* resource)
{
    // Editor textures shown through CreateImGuiTextureId (icon atlas, asset
    // thumbnails, viewport billboards) are created in a pixel-shader-readable
    // state outside the state tracker. Register that so RenderImGui does not
    // record a COMMON->PSR barrier with a wrong before-state, which the D3D12
    // debug layer rejects. Call each frame before RenderImGui, since a resize
    // clears all known states. Engine render targets go through TextureDebugViewer
    // instead and keep their real tracked state, so they are unaffected.
    if (resource)
    {
        // Step 7: this now DECLARES the texture's canonical (it is created shader-readable and
        // stays there), rather than papering over the tracker's global map. That is why it
        // survives the tracker's deletion instead of going with it as D2 assumed.
        SetResourceState(resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}
#endif

void Renderer::ShutdownImGui()
{
    imguiLayer_.Shutdown();
}

bool Renderer::HandleImGuiWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return imguiLayer_.HandleWndProc(hwnd, message, wParam, lParam);
}

bool Renderer::ImGuiWantsMouse() const
{
    return imguiLayer_.WantsMouse();
}

bool Renderer::ImGuiWantsKeyboard() const
{
    return imguiLayer_.WantsKeyboard();
}

void Renderer::ReportLiveObjects()
{
    graphicsDevice_.ReportLiveObjects();
}

void Renderer::Tick(float dt)
{
    fontManager_.Tick();

    if (fps_ <= 0.0f)
    {
        fps_ = 1.0f / dt;
    }
    else
    {
        fps_ = fps_ * fpsAlpha_ + (1.0f - fpsAlpha_) / dt;
    }

    // Hot reload: accumulate elapsed time
    if (shaderHotReloadEnabled_) {
        shaderWatchAccumSec_ += dt;

        // 1) Every N seconds — launch a one-off background task to scan the filesystem
        if (shaderWatchAccumSec_ >= shaderWatchIntervalSec_) {
            // Prevent overlapping scans
            if (!materialManager_.IsProbeInFlight()) {
                (void)materialManager_.RequestFSProbeAsync();
            }
            // Keep the fractional remainder so we do not lose time
            shaderWatchAccumSec_ -= shaderWatchIntervalSec_;
            shaderWatchAccumSec_ = std::max(0.0f, shaderWatchAccumSec_);
        }

        // 2) Apply pending rebuilds (if the scan found changes and set the flag)
        if (materialManager_.ApplyPendingHotReloads(this, totalFrameNumber_, /*keepAliveFrames=*/render::kFrameCount + 1)) {
            materialsHotReloaded_ = true;
        }
    }
}

bool Renderer::ConsumeMaterialHotReloadFlag()
{
    bool wasReloaded = materialsHotReloaded_;
    materialsHotReloaded_ = false;
    return wasReloaded;
}

Renderer::ThreadCL Renderer::BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE type, ID3D12PipelineState* pso)
{
    CPU_SCOPE(ProfilerScopes::kRendererBeginThreadCommandList);
    ID3D12GraphicsCommandList* cl = 0;
    ID3D12CommandAllocator* alloc = 0;
    FrameResource* fr = currentFrameResource_;
    if (!fr) {
        fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
    }
    if (!fr) {
        return {};
    }

    //CPU_SCOPE(L"Renderer::BeginThreadCommandList.1");
    alloc = fr->AcquireCommandAllocator(GetDevice(), type);
    cl = fr->AcquireCommandList(GetDevice(), type, alloc, pso);

    if ((type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE || type == D3D12_COMMAND_LIST_TYPE_BUNDLE) &&
        currentFrameDescriptorHeapCount_ > 0)
    {
        cl->SetDescriptorHeaps(currentFrameDescriptorHeapCount_, currentFrameDescriptorHeaps_.data());
    }

    // Register the command list in the lock-free state tracker
    //RegisterCurrentThreadCL(cl);

    // Step 3: a fresh command list / bundle inherits no pipeline/root state, so reset
    // this thread's bind cache — the first draw recorded into it binds fully.
    render::g_clBindState.Reset();

    return ThreadCL{ alloc, cl, type };
}

void Renderer::BindDescriptorHeaps(ID3D12GraphicsCommandList* cl) const
{
    if (!cl)
    {
        return;
    }

    if (currentFrameDescriptorHeapCount_ > 0)
    {
        cl->SetDescriptorHeaps(currentFrameDescriptorHeapCount_, currentFrameDescriptorHeaps_.data());
    }
}

Renderer::ThreadCL Renderer::BeginThreadCommandBundle(ID3D12PipelineState* initialPSO)
{
    return BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_BUNDLE, initialPSO);
}

void Renderer::EndThreadCommandList(ThreadCL& t, size_t batchIndex, uint32_t localOrder) {
    CPU_SCOPE(ProfilerScopes::kRendererEndThreadCommandList);
    if (t.cl == nullptr) {
        RendererInvariantFailure("Renderer::EndThreadCommandList: null command list");
    }
    // Step 4: work lists close at End time (no longer at submit time, when the
    // fixup used to append the next list's acquire barriers to this one's tail).
    // Runs on worker tasks, which must not throw — check the HRESULT explicitly;
    // a list that failed to Close would lose its GPU work.
    const HRESULT hr = t.cl->Close();
    if (FAILED(hr)) {
        RendererInvariantFailure("Renderer::EndThreadCommandList: Close() failed");
    }
    submitTimeline_.RegisterDirect(t.cl, batchIndex, localOrder);

    // Clear the TLS binding for this command list
    //UnregisterCurrentThreadCL();

    t.cl = nullptr;
    t.alloc = nullptr;
}

void Renderer::BeginSubmitTimeline() {
    submitTimeline_.BeginTimeline();
}

size_t Renderer::BeginSubmitBatch() {
    return submitTimeline_.BeginBatch();
}

void Renderer::RegisterPassDriver(ID3D12GraphicsCommandList* cl, size_t batchIndex)
{
    submitTimeline_.RegisterDriver(cl, batchIndex);
}

void Renderer::EndThreadCommandBundle(ThreadCL& b, size_t batchIndex, uint32_t localOrder)
{
    CPU_SCOPE(ProfilerScopes::kRendererEndThreadCommandBundle);
    if (b.cl == nullptr) {
        RendererInvariantFailure("Renderer::EndThreadCommandBundle: null command list");
    }
    // Runs on worker tasks, which must not throw (the task system swallows task
    // exceptions) — check the HRESULT explicitly; a bundle that failed to Close
    // would lose its GPU work.
    const HRESULT hr = b.cl->Close();
    if (FAILED(hr)) {
        RendererInvariantFailure("Renderer::EndThreadCommandBundle: Close() failed");
    }
    submitTimeline_.RegisterBundle(b.cl, batchIndex, localOrder);
    b.cl = nullptr;
    b.alloc = nullptr;
}

void Renderer::ExecuteTimelineAndPresent() {
    CPU_SCOPE(ProfilerScopes::kRendererExecuteTimelineAndPresent);

    submitListsScratch_.clear();

    // Gather batches in order
    {
        CPU_SCOPE(ProfilerScopes::kService1);
        submitTimeline_.GatherFrameLists(submitListsScratch_, [this]() {
            // Fallback: a batch has bundles but no driver — create a temporary one
            FrameResource* fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
            ID3D12CommandAllocator* alloc =
                fr->AcquireCommandAllocator(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);
            ID3D12GraphicsCommandList* cl =
                fr->AcquireCommandList(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
            RecordBindDefaultsNoClear(cl);
            return cl;
        });
    }

    fixedSubmitScratch_.clear();
    fixedSubmitScratch_.reserve(submitListsScratch_.size() * 2 + 3);

    // Acquire a fresh, open DIRECT command list from this frame's pool.
    auto acquireDirectCL = [this]() -> ID3D12GraphicsCommandList* {
        FrameResource* fr = frameScheduler_.GetFrameResource(currentFrameIndex_);
        ID3D12CommandAllocator* alloc =
            fr->AcquireCommandAllocator(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT);
        return fr->AcquireCommandList(GetDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT, alloc);
    };

#if PROF_GPU_ENABLED
    {
        // GPU-profiler frame-begin: first list submitted this frame.
        ID3D12GraphicsCommandList* cl = acquireDirectCL();
        Profiler::Get().BeginGpuFrame(cl);
        ThrowIfFailed(cl->Close());
        fixedSubmitScratch_.push_back(cl);
    }
#endif

    {
        CPU_SCOPE(ProfilerScopes::kService2);

        // Step 4: work lists are already closed (directs at EndThreadCommandList,
        // drivers at gather time). For each in submission order, compute its
        // acquire barriers against the global state and, when non-empty, record
        // them into a small DEDICATED command list submitted immediately before
        // that work list. Per-list (never one prologue per batch): a later list
        // in a batch may transition out of an earlier list's final state, which
        // a single aggregated prologue could not represent.
        const ResourceStateTracker::CLState* previousState = nullptr;

        for (auto* cmd : submitListsScratch_) {
            const ResourceStateTracker::CLState* currentState = stateTracker_.FindCLStateForCmd(cmd);

            // Fold the previous list's final states into the global map first,
            // then resolve this list's acquire barriers against it.
            if (previousState) {
                stateTracker_.ApplyFinalStates(*previousState);
            }

            barrierScratch_.clear();
            // Step 7 — THE FLIP. Under the flip this runs for the UNMANAGED resources ONLY, and
            // that falls out of the routing rather than needing a filter: a compile-managed
            // resource returns from Renderer::Transition before ever reaching the tracker, so its
            // firstUse map now contains exactly the resources the graph does not model (the
            // backbuffer, uploads, ImGui). Those still need the tracker's stitching, because it
            // defers a command list's FIRST transition to submit time — disabling this wholesale
            // dropped that first barrier and left the pair half-applied.
            if (currentState) {
                stateTracker_.AppendAcquireBarriers(*currentState, barrierScratch_);
            }

            if (!barrierScratch_.empty()) {
                // THE cost this whole plan exists to remove: a command list allocated for nothing
                // but barriers. Count them under the flip — the number is what says whether the
                // tracker can go, and it is the Step 8 before/after baseline.
                if (render::g_barrierFlip && render::g_barrierFlipTrace) {
                    char m[128];
                    std::snprintf(m, sizeof(m), "[acquire-prologue] %zu barriers\n", barrierScratch_.size());
                    Renderer::DiagLog(m);
                }
                ID3D12GraphicsCommandList* acquire = acquireDirectCL();
                acquire->ResourceBarrier(static_cast<UINT>(barrierScratch_.size()), barrierScratch_.data());
                ThrowIfFailed(acquire->Close());
                fixedSubmitScratch_.push_back(acquire);
            }

            fixedSubmitScratch_.push_back(cmd);
            previousState = currentState;
        }

        if (previousState) {
            stateTracker_.ApplyFinalStates(*previousState);
        }

        // Dedicated epilogue list submitted last: present transition (+ GPU
        // profiler frame-end). Never reopen or append to the final work list.
        ID3D12GraphicsCommandList* epilogueCmd = acquireDirectCL();

        ID3D12Resource* backbuffer = swapchain_.Backbuffer(currentFrameIndex_);

        D3D12_RESOURCE_BARRIER presentBarrier{};
        presentBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        presentBarrier.Transition.pResource = backbuffer;
        presentBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        presentBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        presentBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        epilogueCmd->ResourceBarrier(1, &presentBarrier);
        // The engine's one existing return-to-canonical transition (D2's frame epilogue). Same
        // value the declaration carries, so it only needs to reach the tracker.
        SetTrackedStateOnly(backbuffer, D3D12_RESOURCE_STATE_PRESENT);
#if PROF_GPU_ENABLED
        Profiler::Get().EndGpuFrame(epilogueCmd);
#endif
        ThrowIfFailed(epilogueCmd->Close());
        fixedSubmitScratch_.push_back(epilogueCmd);

        stateTracker_.SetKnownStateDirect(backbuffer, D3D12_RESOURCE_STATE_PRESENT);
    }

    // Step 6: every command list's final states have been folded into the tracker by here, so
    // this is the frame's true end state. Default off.
    ReportOffCanonicalStates();

    {
        CPU_SCOPE(ProfilerScopes::kService3);
        if (!fixedSubmitScratch_.empty()) {
            GetCommandQueue()->ExecuteCommandLists(static_cast<UINT>(fixedSubmitScratch_.size()), fixedSubmitScratch_.data());
        }
    }

    stateTracker_.ResetLanesForFrame();

    {
        CPU_SCOPE(ProfilerScopes::kService4);
        lastPresentedIndex_ = currentFrameIndex_; // the buffer about to be shown (screenshots)
        swapchain_.Present();
    }
    SignalFrame(currentFrameIndex_);
    currentFrameIndex_ = swapchain_.CurrentBackBufferIndex();
    RefreshCurrentFrameCaches();
}

void Renderer::WaitForPreviousFrame() {
    // Fully wait for the GPU (for resize/destructor)
    frameScheduler_.WaitForGpuIdle(GetCommandQueue());
}

void Renderer::OnResize(UINT width, UINT height) {
    if (width == 0 || height == 0) {
        return;
    }
    width_ = width;
    height_ = height;
    if (dlssHandler_)
    {
        dlssHandler_->OnDisplaySizeChanged();
    }
    else
    {
        UpdateRenderResolutionFromScale();
    }

    // Important: wait for the GPU before replacing resources
    WaitForPreviousFrame();

    // Release the old RTV/DSV objects
    for (UINT i = 0; i < render::kFrameCount; ++i) {
        ClearResourceState(swapchain_.Backbuffer(i));
    }
    ClearResourceState(swapchain_.DepthBuffer());
    swapchain_.ReleaseBuffersForResize();

    // ResizeBuffers
    swapchain_.ResizeBuffers(width_, height_);

    // Recreate RTV and DSV
    CreateSwapChainAndRTVs(width_, height_);
    CreateDepthResources(width_, height_);
    CreateDeferredTargets(width_, height_);
    AllocateDlssResourcesIfNeeded();

    RefreshCurrentFrameCaches();
}

void Renderer::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES state) {
    // Step 6: this IS the canonical declaration (see the header).
    Declarations().Declare(res, state);
}

void Renderer::SetResourceState(ID3D12Resource* res, D3D12_RESOURCE_STATES creationState,
                                D3D12_RESOURCE_STATES canonicalState) {
    Declarations().Declare(res, creationState, canonicalState);
}

void Renderer::ClearResourceState(ID3D12Resource* res) {
    Declarations().Forget(res);
}

void Renderer::SetTrackedStateOnly(ID3D12Resource* res, D3D12_RESOURCE_STATES state) {
    stateTracker_.SetResourceState(res, state);
}

D3D12_RESOURCE_STATES Renderer::GetCanonicalState(ID3D12Resource* res) const {
    return canonicalStates_.Get(res);
}

void Renderer::ReportOffCanonicalStates() {
    if (!render::g_canonicalCheck) { return; }

    // Snapshot first: GetGlobalKnownState takes the tracker's lock, and holding the canonical
    // lock across it would nest two locks on the frame's critical path.
    canonicalStates_.Snapshot(canonicalScratch_);

    unsigned drifted = 0;
    for (const auto& [res, entry] : canonicalScratch_) {
        // NEVER dereference `res` here. Not every release path unregisters, so this table can
        // hold dangling keys; the name was captured at declaration time for exactly that reason,
        // and GetGlobalKnownState only uses the pointer as a hash key.
        //
        // Under the flip the tracker never sees a graph transition, so its global map is stale by
        // construction — read the compile's own ending state instead, or this whole report is
        // noise that reads like a result.
        const D3D12_RESOURCE_STATES actual = render::g_barrierFlip
            ? entry.predicted
            : stateTracker_.GetGlobalKnownState(res);
        if (actual == entry.state) { continue; }
        ++drifted;
        char msg[320];
        std::snprintf(msg, sizeof(msg), "[canonical] off-canonical res=%s canonical=0x%X actual=0x%X\n",
                      entry.name, static_cast<unsigned>(entry.state), static_cast<unsigned>(actual));
        Renderer::DiagLog(msg);
    }

    // Step 6b part 2: name the LEAK. Every resource has a unique debug name, so two live entries
    // sharing one name means an earlier resource was never unregistered — the dangling keys that
    // crashed the reporter and that let a recycled address inherit a stale canonical. Printing
    // the duplicated names says exactly which owners fail to unregister, instead of guessing at
    // ~60 declaration sites. Throttled: this walks the table twice.
    // Triggered by the table CHANGING SIZE, which is precisely when something was declared or
    // failed to be forgotten. A fixed frame-count throttle was wrong: the stress harness runs
    // only a handful of frames per churn op, so a 240-frame tick never fired.
    if (canonicalScratch_.size() != canonicalLastDeclared_) {
        // A steady count is NOT a leak: two GPU-instanced clouds, or one texture legitimately
        // loaded twice, sit at a constant 2 forever. Only a count that keeps CLIMBING past every
        // value it has held before means declares are outrunning forgets. Reporting bare
        // duplicates cannot tell those apart and twice made me call a steady x2 a leak.
        canonicalStates_.NetByName(canonicalNetScratch_);
        for (const auto& [name, net] : canonicalNetScratch_) {
            // Skip pointer-"named" entries. Their identity is an ADDRESS, which the allocator
            // reuses, so a per-name net count means nothing for them — the answer for an unnamed
            // resource is to give it a debug name, not to count it.
            if (name.find('.') == std::string::npos && name.find(':') == std::string::npos) { continue; }
            auto it = canonicalNetPeak_.find(name);
            const int peak = (it == canonicalNetPeak_.end()) ? 0 : it->second;
            if (net <= peak) { continue; }
            canonicalNetPeak_[name] = net;
            // Growing past a previous high-water mark. The first sighting of a name is not
            // interesting (every resource declares once), so only report from the second on.
            if (net < 2) { continue; }
            char dup[240];
            std::snprintf(dup, sizeof(dup), "[canonical] LEAK %s: live entries grew to %d\n",
                          name.c_str(), net);
            Renderer::DiagLog(dup);
        }

        unsigned anonymous = 0;
        for (const auto& [res, entry] : canonicalScratch_) {
            (void)res;
            // A resource with no debug name falls back to its address, unique by construction, so
            // it can never pair up with anything. Counted separately or the leak hides in them.
            if (std::strchr(entry.name, '.') == nullptr && std::strchr(entry.name, ':') == nullptr) { ++anonymous; }
        }
        char anon[160];
        std::snprintf(anon, sizeof(anon), "[canonical] %u named + %u UNNAMED entries declared\n",
                      static_cast<unsigned>(canonicalNetScratch_.size()), anonymous);
        Renderer::DiagLog(anon);
    }

    // One summary line so an empty log is distinguishable from a check that never ran — the same
    // mistake the comparator's SKIPPED line exists to prevent. Printed only when the numbers
    // CHANGE: at one line per frame it floods DBWIN's single shared buffer and the listener drops
    // the leak lines above, which is exactly how the first leak measurement came back empty.
    const unsigned declaredCount = static_cast<unsigned>(canonicalScratch_.size());
    if (drifted != canonicalLastDrift_ || declaredCount != canonicalLastDeclared_) {
        canonicalLastDrift_ = drifted;
        canonicalLastDeclared_ = declaredCount;
        char summary[160];
        std::snprintf(summary, sizeof(summary), "[canonical] frame end: %u of %u declared resources off-canonical\n",
                      drifted, declaredCount);
        Renderer::DiagLog(summary);
    }
}

// Barrier plan step 3: null unless a converted pass's body is running on this thread.
static thread_local Renderer::TransitionLog* tlTransitionLog = nullptr;

void Renderer::SetThreadTransitionLog(TransitionLog* log) {
    tlTransitionLog = log;
}

Renderer::TransitionLog* Renderer::CurrentThreadTransitionLog() {
    return tlTransitionLog;
}

// Step 7: the pass whose compiled barriers this thread should emit. Null outside a converted
// pass body, which is what keeps unconverted paths (uploads, editor, present) on the old route.
static thread_local Renderer::CompiledBarriers* tlCompiledBarriers = nullptr;

void Renderer::SetThreadCompiledBarriers(CompiledBarriers* cb) { tlCompiledBarriers = cb; }
Renderer::CompiledBarriers* Renderer::CurrentThreadCompiledBarriers() { return tlCompiledBarriers; }

// Barrier-diagnostics sink, shared by --barrier-flip-trace and --barrier-cmp. Goes to a FILE as
// well as the debugger: the --scene-stress runs that reproduce the residual mismatches have no
// debugger attached, so DBWIN output is simply lost (the same trap as the stress harness's own
// verdict). Flushed per line — a crash mid-frame must not take the last line with it, which is the
// one that matters.
void Renderer::DiagLog(const char* line) {
    static std::mutex mtx;
    static FILE* f = nullptr;
    std::lock_guard<std::mutex> lk(mtx);
    if (f == nullptr) { fopen_s(&f, "barrier_diag.log", "w"); }
    if (f != nullptr) { std::fputs(line, f); std::fflush(f); }
    OutputDebugStringA(line);
}

void Renderer::TransitionExplicit(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                                  D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (cl == nullptr || res == nullptr || before == after) { return; }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
}

void Renderer::Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES after) {
    //CPU_SCOPE(ProfilerScopes::kRendererTransition);
    if (tlTransitionLog != nullptr && res != nullptr) {
        TransitionLog& log = *tlTransitionLog;
        // Claim a slot atomically: a fan-out pass appends from several worker threads at once.
        const std::uint32_t slot = log.count->fetch_add(1, std::memory_order_relaxed);
        if (slot < log.capacity) {
            log.entries[slot] = ObservedTransition{ res, after };
        }
        else {
            // Truncating would make the comparator report a false "missing" entry.
            log.overflowed.store(true, std::memory_order_relaxed);
        }
    }

    // Step 7 — the flip. Emit the barrier the compile already produced for this pass instead of
    // handing the state to the tracker for submit-time stitching.
    // The flip owns a transition only for resources the compile actually models. An UNDECLARED
    // resource transitioned inside a converted pass body — Renderer::RenderImGui does exactly this
    // for editor preview textures, from Pass_Overlay — has no compiled entry, and treating that as
    // "no barrier needed" SILENTLY DROPPED it. Undeclared means "not ours": route to the tracker.
    if (render::g_barrierFlip && tlCompiledBarriers != nullptr && res != nullptr && cl != nullptr &&
        canonicalStates_.IsCompileManaged(res)) {
        CompiledBarriers& cb = *tlCompiledBarriers;
        // A request may only match the CURRENT point — the first not yet emitted. Never a later
        // one.
        //
        // Searching all points for (resource, state) is ambiguous, because the same transition
        // appears at several of them: Pass_Tonemap moves `tonemap` UAV -> COPY_SOURCE -> UAV, and
        // its very first request (ApplyDeclaredStates asking for UAV, which the compile turned
        // into nothing because UAV is already canonical) matched the RESTORE point at the end and
        // dragged the whole pass's barriers to the top of the command list. Measured with
        // --barrier-flip-trace: "point 3 emit 0x800->0x8" printed before "point 2 emit 0x8->0x800".
        //
        // Registrations are made in body order, so the body's requests arrive in point order and
        // the current point is the only one it can legitimately be asking for. Anything else is a
        // transition the compile decided needs no barrier.
        for (std::uint32_t p = 0; p < cb.pointCount; ++p) {
            CompiledBarriers::Point& pt = cb.points[p];
            if (pt.emitted.load(std::memory_order_relaxed)) { continue; }
            bool names = false;
            for (std::uint32_t i2 = 0; i2 < pt.count && !names; ++i2) {
                names = pt.entries[i2].Transition.pResource == res &&
                        pt.entries[i2].Transition.StateAfter == after;
            }
            if (!names) { break; } // current point is not this request -> nothing to emit
            bool notYet = false;
            if (pt.emitted.compare_exchange_strong(notYet, true, std::memory_order_relaxed)) {
                cl->ResourceBarrier(pt.count, pt.entries);
                if (render::g_barrierFlipTrace) {
                    char label[96];
                    canonicalStates_.NameOf(res, label, sizeof(label));
                    char m[280];
                    std::snprintf(m, sizeof(m), "[flip] pass=%d point %u/%u (%u barriers) asked %s 0x%X\n",
                                  cb.pass, p, cb.pointCount, pt.count, label, static_cast<unsigned>(after));
                    Renderer::DiagLog(m);
                }
            }
            return;
        }
        // Nothing to emit. Either the compile decided this transition needs no barrier (the
        // resource is already in `after`), or Prepare and the body disagree about what this frame
        // does — and only the second is a bug. The trace prints the CURRENT point so the two can
        // be told apart by eye: a benign miss sits at a point that names other resources, a real
        // one sits at a point that names THIS resource with a different state, or past the end.
        if (render::g_barrierFlipTrace) {
            std::uint32_t cur = cb.pointCount;
            for (std::uint32_t p = 0; p < cb.pointCount; ++p) {
                if (!cb.points[p].emitted.load(std::memory_order_relaxed)) { cur = p; break; }
            }
            char label[96];
            canonicalStates_.NameOf(res, label, sizeof(label));
            char detail[320] = {};
            int off = 0;
            if (cur < cb.pointCount) {
                const CompiledBarriers::Point& pt = cb.points[cur];
                for (std::uint32_t i2 = 0; i2 < pt.count && off >= 0 && off < static_cast<int>(sizeof(detail)) - 1; ++i2) {
                    char n2[96];
                    canonicalStates_.NameOf(pt.entries[i2].Transition.pResource, n2, sizeof(n2));
                    off += std::snprintf(detail + off, sizeof(detail) - static_cast<size_t>(off), "%s%s:0x%X->0x%X",
                                         (i2 == 0) ? "" : ", ", n2,
                                         static_cast<unsigned>(pt.entries[i2].Transition.StateBefore),
                                         static_cast<unsigned>(pt.entries[i2].Transition.StateAfter));
                }
            }
            char m[520];
            std::snprintf(m, sizeof(m), "[flip-miss] pass=%d res=%s want=0x%X cur=%u/%u [%s]\n",
                          cb.pass, label, static_cast<unsigned>(after), cur, cb.pointCount, detail);
            Renderer::DiagLog(m);
        }
        cb.unmatched.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Everything that reaches here is still the TRACKER's, and while anything does, the
    // submit-time acquire prologue (and its barrier-only command list) cannot be deleted. Under
    // --barrier-flip-trace, name them: this list IS the deletion worklist.
    if (render::g_barrierFlip && render::g_barrierFlipTrace && res != nullptr) {
        char label[96];
        canonicalStates_.NameOf(res, label, sizeof(label));
        char m[200];
        std::snprintf(m, sizeof(m), "[tracker-fallthrough] res=%s after=0x%X declared=%d\n",
                      label, static_cast<unsigned>(after),
                      canonicalStates_.IsCompileManaged(res) ? 1 : 0);
        Renderer::DiagLog(m);
    }
    stateTracker_.Transition(cl, res, after);
}

void Renderer::UAVBarrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* res) {
    if (cl == nullptr || res == nullptr) {
        return;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = res;
    cl->ResourceBarrier(1, &b);
}

void Renderer::EnsureVsmDummySrvs() {
    if (vsmDummyHeap_) { return; }
    ID3D12Device* dev = GetDevice();
    if (!dev) { return; }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only: these are copy SOURCES for StageSrvUavTable
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(vsmDummyHeap_.GetAddressOf()))) || !vsmDummyHeap_) {
        vsmDummyHeap_.Reset();
        return;
    }
    vsmDummyHeap_->SetName(L"VSM.DummySrvs");

    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE base = vsmDummyHeap_->GetCPUDescriptorHandleForHeapStart();

    // Slot 0: null StructuredBuffer<uint> — matches VsmPageTable (t7/t9). Inert (never sampled: the
    // shaders read it only under useVsm/vsmParams != 0, which is false whenever VSM isn't resident).
    vsmDummyBufferSrv_ = base;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        d.Format = DXGI_FORMAT_UNKNOWN;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Buffer.NumElements = 1;
        d.Buffer.StructureByteStride = 4;
        d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        dev->CreateShaderResourceView(nullptr, &d, vsmDummyBufferSrv_);
    }

    // Slot 1: null Texture2D (R16_UNORM) — matches VsmPool (t8/t10).
    vsmDummyTexSrv_ = base;
    vsmDummyTexSrv_.ptr += inc;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        d.Format = DXGI_FORMAT_R16_UNORM;
        d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        d.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(nullptr, &d, vsmDummyTexSrv_);
    }
}

ID3D12CommandSignature* Renderer::GetDrawIndexedCommandSignature() {
    if (drawIndexedCmdSig_) {
        return drawIndexedCmdSig_.Get();
    }
    ID3D12Device* device = GetDevice();
    if (!device) {
        return nullptr;
    }
    // One DRAW_INDEXED argument, no per-draw root-argument changes -> pRootSignature = nullptr.
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
    D3D12_COMMAND_SIGNATURE_DESC desc{};
    desc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    desc.NumArgumentDescs = 1;
    desc.pArgumentDescs = &arg;
    desc.NodeMask = 0;
    if (FAILED(device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(drawIndexedCmdSig_.GetAddressOf())))) {
        drawIndexedCmdSig_.Reset();
        return nullptr;
    }
    drawIndexedCmdSig_->SetName(L"Renderer.DrawIndexedIndirectSig");
    return drawIndexedCmdSig_.Get();
}

void Renderer::ExecuteIndirect(ID3D12GraphicsCommandList* cl, ID3D12CommandSignature* sig,
                               UINT maxCommandCount, ID3D12Resource* argBuffer, UINT64 argOffset,
                               ID3D12Resource* countBuffer, UINT64 countOffset) {
    if (cl == nullptr || sig == nullptr || argBuffer == nullptr) {
        return;
    }
    cl->ExecuteIndirect(sig, maxCommandCount, argBuffer, argOffset, countBuffer, countOffset);
}

void Renderer::RecordBindAndClear(ID3D12GraphicsCommandList* cl) {
    // Barrier: Present -> RenderTarget (for the current back buffer)
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = swapchain_.Backbuffer(currentFrameIndex_);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &b);
    // Step 6: a mid-frame flip, NOT the backbuffer's resting state — its canonical is PRESENT,
    // declared once in CreateSwapChainAndRTVs. Declaring RENDER_TARGET here would redefine
    // canonical every single frame and make the frame-end check vacuous.
    SetTrackedStateOnly(swapchain_.Backbuffer(currentFrameIndex_), D3D12_RESOURCE_STATE_RENDER_TARGET);

    // RTV/DSV
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_.BackbufferRTV(currentFrameIndex_);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = swapchain_.DepthDSV();
    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    // viewport/scissor
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;

    D3D12_RECT sr{ 0, 0, (LONG)width_, (LONG)height_ };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);

    // Clear
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    cl->ClearRenderTargetView(rtv, clear, 0, nullptr);
    cl->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
}

void Renderer::RecordBindDefaultsNoClear(ID3D12GraphicsCommandList* cl) {
    // Only bind RTV/DSV + viewport/scissor (no barrier or clear)
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = swapchain_.BackbufferRTV(currentFrameIndex_);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv = swapchain_.DepthDSV();
    cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv);

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(width_);
    vp.Height = static_cast<float>(height_);
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    D3D12_RECT sr{ 0, 0, (LONG)width_, (LONG)height_ };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sr);
}

void Renderer::CreateDeferredTargets(UINT width, UINT height)
{
    const UINT rtWidth = std::max(1u, renderWidth_);
    const UINT rtHeight = std::max(1u, renderHeight_);
    const UINT displayWidth = std::max(1u, width);
    const UINT displayHeight = std::max(1u, height);

    const auto reflectionSize = ComputeReflectionTextureSize(displayWidth, displayHeight);
    reflectionTextureWidth_ = reflectionSize.first > 0 ? reflectionSize.first : 1;
    reflectionTextureHeight_ = reflectionSize.second > 0 ? reflectionSize.second : 1;
    const auto oceanReflectionSize = ComputeScaledTextureSize(displayWidth, displayHeight, oceanReflectionTextureScale_);
    oceanReflectionTextureWidth_ = oceanReflectionSize.first > 0 ? oceanReflectionSize.first : 1;
    oceanReflectionTextureHeight_ = oceanReflectionSize.second > 0 ? oceanReflectionSize.second : 1;

    RenderTargetManager::Formats formats{};
    formats.gb0 = render::kGBuffer0Format;
    formats.gb1 = render::kGBuffer1Format;
    formats.gb2 = render::kGBuffer2Format;
    formats.gbAux = render::kGBufferAuxFormat;
    formats.velocity = render::kGBufferVelocityFormat;
#if WITH_EDITOR
    formats.objectID = render::kObjectIdFormat;
#endif
    formats.depth = render::kDeferredDepthFormat;
    formats.depthSrv = render::kDeferredDepthSrvFormat;
    formats.light = render::kLightTargetFormat;
    formats.sceneColor = render::kSceneColorFormat;
    formats.reflection = render::kReflectionFormat;
    formats.reflectionScratch = render::kReflectionScratchFormat;
    formats.oceanReflection = render::kReflectionFormat;
    formats.backbufferResource = render::kBackbufferResourceFormat;

    RenderTargetManager::Sizes sizes{};
    sizes.renderWidth = rtWidth;
    sizes.renderHeight = rtHeight;
    sizes.displayWidth = displayWidth;
    sizes.displayHeight = displayHeight;
    sizes.reflectionWidth = reflectionTextureWidth_;
    sizes.reflectionHeight = reflectionTextureHeight_;
    sizes.oceanReflectionWidth = oceanReflectionTextureWidth_;
    sizes.oceanReflectionHeight = oceanReflectionTextureHeight_;

    rtManager_.Create(GetDevice(), formats, sizes, Declarations());
}

void Renderer::DestroyDeferredTargets() {
    rtManager_.Destroy(Declarations());
#if WITH_EDITOR
    ResetObjectIdPickState();
#endif

    reflectionTextureWidth_ = 1;
    reflectionTextureHeight_ = 1;
    oceanReflectionTextureWidth_ = 1;
    oceanReflectionTextureHeight_ = 1;
}


std::pair<UINT, UINT> Renderer::ComputeScaledTextureSize(UINT referenceWidth, UINT referenceHeight, Math::float2 scale) const
{
    const UINT refWidth = std::max(referenceWidth, 1u);
    const UINT refHeight = std::max(referenceHeight, 1u);

    auto computeDim = [](UINT dim, float scale) -> UINT
    {
        if (scale <= 0.0f)
        {
            return 1u;
        }
        const float scaled = static_cast<float>(dim) * scale;
        const UINT value = static_cast<UINT>(scaled + 0.5f);
        return std::max(1u, value);
    };

    const UINT reflectionWidth = computeDim(refWidth, scale.x);
    const UINT reflectionHeight = computeDim(refHeight, scale.y);
    return { reflectionWidth, reflectionHeight };
}

std::pair<UINT, UINT> Renderer::ComputeReflectionTextureSize(UINT referenceWidth, UINT referenceHeight) const
{
    return ComputeScaledTextureSize(referenceWidth, referenceHeight, reflectionTextureScale_);
}

void Renderer::RecreateDeferredTargets()
{
    if (!GetDevice() || width_ == 0 || height_ == 0)
    {
        return;
    }

    WaitForPreviousFrame();
    DestroyDeferredTargets();
    CreateDeferredTargets(width_, height_);
    RefreshCurrentFrameCaches();
}

void Renderer::SetReflectionTextureScale(Math::float2 scale)
{
    Math::float2 sanitized{ scale.x, scale.y };
    if (sanitized.x < 0.0f) { sanitized.x = 0.0f; }
    if (sanitized.y < 0.0f) { sanitized.y = 0.0f; }

    if (sanitized.x == reflectionTextureScale_.x && sanitized.y == reflectionTextureScale_.y)
    {
        return;
    }

    reflectionTextureScale_ = sanitized;

    if (rtManager_.IsCreated())
    {
        RecreateDeferredTargets();
    }
}

UINT Renderer::GetReflectionTextureWidth() const
{
    return std::max(1u, reflectionTextureWidth_);
}

UINT Renderer::GetReflectionTextureHeight() const
{
    return std::max(1u, reflectionTextureHeight_);
}

void Renderer::SetOceanReflectionTextureScale(Math::float2 scale)
{
    Math::float2 sanitized{ scale.x, scale.y };
    if (sanitized.x < 0.0f) { sanitized.x = 0.0f; }
    if (sanitized.y < 0.0f) { sanitized.y = 0.0f; }

    if (sanitized.x == oceanReflectionTextureScale_.x && sanitized.y == oceanReflectionTextureScale_.y)
    {
        return;
    }

    oceanReflectionTextureScale_ = sanitized;

    if (rtManager_.IsCreated())
    {
        RecreateDeferredTargets();
    }
}

UINT Renderer::GetOceanReflectionTextureWidth() const
{
    return std::max(1u, oceanReflectionTextureWidth_);
}

UINT Renderer::GetOceanReflectionTextureHeight() const
{
    return std::max(1u, oceanReflectionTextureHeight_);
}

void Renderer::UpdateRenderResolutionFromScale()
{
    if (dlssHandler_)
    {
        dlssHandler_->RefreshRenderResolution();
        return;
    }

    const float clampedScale = std::clamp(renderResolutionScale_, 0.1f, 1.0f);
    renderResolutionScale_ = clampedScale;
    const float baseWidth = static_cast<float>(std::max(width_, 1u));
    const float baseHeight = static_cast<float>(std::max(height_, 1u));

    renderWidth_ = std::max(1u, static_cast<UINT>(baseWidth * clampedScale + 0.5f));
    renderHeight_ = std::max(1u, static_cast<UINT>(baseHeight * clampedScale + 0.5f));
}

void Renderer::SetRenderResolutionScale(float scale)
{
    float sanitized = scale;
    if (!std::isfinite(sanitized))
    {
        sanitized = 1.0f;
    }
    sanitized = std::clamp(sanitized, 0.1f, 1.0f);

    if (std::abs(sanitized - renderResolutionScale_) < 1e-4f)
    {
        return;
    }

    renderResolutionScale_ = sanitized;
    if (dlssHandler_)
    {
        dlssHandler_->OnRenderResolutionScaleChanged();
    }
    else
    {
        UpdateRenderResolutionFromScale();
        if (rtManager_.IsCreated())
        {
            RecreateDeferredTargets();
        }
    }
}

void Renderer::UpdateDlssSettings()
{
    if (dlssHandler_)
    {
        dlssHandler_->UpdateSettings();
    }
    else
    {
        UpdateRenderResolutionFromScale();
    }
}

void Renderer::AllocateDlssResourcesIfNeeded()
{
    if (dlssHandler_)
    {
        dlssHandler_->AllocateResourcesIfNeeded();
    }
}

void Renderer::UpdateDlssCameraData(const Camera& camera)
{
    if (dlssHandler_)
    {
        dlssHandler_->UpdateCameraData(camera);
    }
}

Math::float2 Renderer::GetCameraJitter() const
{
    if (dlssHandler_ && dlssHandler_->IsActive())
    {
        const Math::float2 jitterPixels = dlssHandler_->GetCurrentJitterPixels();
        const float renderWidth = static_cast<float>(std::max(renderWidth_, 1u));
        const float renderHeight = static_cast<float>(std::max(renderHeight_, 1u));
        return Math::float2(jitterPixels.x / renderWidth, jitterPixels.y / renderHeight);
    }

    return Math::float2(0.0f, 0.0f);
}

bool Renderer::EvaluateDLSS(ID3D12GraphicsCommandList* cl)
{
    if (!dlssHandler_)
    {
        return false;
    }

    return dlssHandler_->Evaluate(cl);
}

bool Renderer::IsDlssActive() const
{
    return dlssHandler_ && dlssHandler_->IsActive();
}

bool Renderer::IsDlssAvailable() const
{
    return dlssHandler_ && dlssHandler_->IsAvailable();
}

void Renderer::SetDlssActive(bool active)
{
    if (dlssHandler_)
    {
        dlssHandler_->SetActive(active);
        if (active && dlssMode_ != sl::DLSSMode::eOff)
        {
            UpdateDlssSettings();
            AllocateDlssResourcesIfNeeded();
        }
    }
}

void Renderer::SetDlssMode(sl::DLSSMode mode)
{
    if (dlssMode_ == mode)
    {
        return;
    }

    const UINT previousRenderWidth = renderWidth_;
    const UINT previousRenderHeight = renderHeight_;

    dlssMode_ = mode;

    if (mode == sl::DLSSMode::eOff)
    {
        if (dlssHandler_)
        {
            dlssHandler_->SetActive(false);
        }
        SetRenderResolutionScale(1.0f);
    }

    UpdateDlssSettings();

    if (mode != sl::DLSSMode::eOff)
    {
        AllocateDlssResourcesIfNeeded();

        if (rtManager_.IsCreated() &&
            (renderWidth_ != previousRenderWidth || renderHeight_ != previousRenderHeight))
        {
            RecreateDeferredTargets();
        }
    }
}

void Renderer::BindGBuffer(ID3D12GraphicsCommandList* cl, ClearMode mode) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
#if WITH_EDITOR
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[6] = {
        D.gbRTV[0], D.gbRTV[1], D.gbRTV[2], D.gbRTV[3], D.objectIDRTV, D.gbAuxRTV
    };
    cl->OMSetRenderTargets(6, rtvs, FALSE, &D.dsv);
#else
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[5] = {
        D.gbRTV[0], D.gbRTV[1], D.gbRTV[2], D.gbRTV[3], D.gbAuxRTV
    };
    cl->OMSetRenderTargets(5, rtvs, FALSE, &D.dsv);
#endif
    cl->OMSetStencilRef(0);

    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);

    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        for (int i = 0; i < 4; ++i) {
            cl->ClearRenderTargetView(rtvs[i], c, 0, nullptr);
        }
#if WITH_EDITOR
        cl->ClearRenderTargetView(rtvs[4], c, 0, nullptr);
        constexpr int kAuxRtvIndex = 5;
#else
        constexpr int kAuxRtvIndex = 4;
#endif
        const float auxNeutral[4]{ 1,1,0,0 };
        cl->ClearRenderTargetView(rtvs[kAuxRtvIndex], auxNeutral, 0, nullptr);
        if (mode == ClearMode::ColorDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindLightTarget(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    cl->OMSetRenderTargets(1, &D.lightRTV, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(D.lightRTV, c, 0, nullptr);
        if (mode == ClearMode::ColorDepth && withDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindSceneColor(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    cl->OMSetRenderTargets(1, &D.sceneRTV, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(D.sceneRTV, c, 0, nullptr);
        if (mode == ClearMode::ColorDepth && withDepth) {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindLightTargetWithVelocity(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { D.lightRTV, D.gbRTV[3] };
    cl->OMSetRenderTargets(2, rtvs, FALSE, withDepth ? &D.dsv : nullptr);
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(rtvs[0], c, 0, nullptr);
        cl->ClearRenderTargetView(rtvs[1], c, 0, nullptr);
        if (mode == ClearMode::ColorDepth && withDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindSceneColorWithVelocity(ID3D12GraphicsCommandList* cl, ClearMode mode, bool withDepth) {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
#if WITH_EDITOR
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = { D.sceneRTV, D.gbRTV[3], D.objectIDRTV };
    cl->OMSetRenderTargets(3, rtvs, FALSE, withDepth ? &D.dsv : nullptr);
#else
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { D.sceneRTV, D.gbRTV[3] };
    cl->OMSetRenderTargets(2, rtvs, FALSE, withDepth ? &D.dsv : nullptr);
#endif
    D3D12_VIEWPORT vp{ 0,0,float(renderWidth_),float(renderHeight_),0,1 };
    D3D12_RECT     sr{ 0,0,(LONG)renderWidth_,(LONG)renderHeight_ };
    cl->RSSetViewports(1, &vp); cl->RSSetScissorRects(1, &sr);
    if (mode != ClearMode::None) {
        const float c[4]{ 0,0,0,0 };
        cl->ClearRenderTargetView(rtvs[0], c, 0, nullptr);
        cl->ClearRenderTargetView(rtvs[1], c, 0, nullptr);
#if WITH_EDITOR
        cl->ClearRenderTargetView(rtvs[2], c, 0, nullptr);
#endif
        if (mode == ClearMode::ColorDepth && withDepth)
        {
            cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
        }
    }
}

void Renderer::BindShadowTarget(ID3D12GraphicsCommandList* cl, int cascadeIndex, bool clearDepth)
{
    auto& D = rtManager_.Deferred(currentFrameIndex_);

    // Single DSV for the entire atlas
    cl->OMSetRenderTargets(0, nullptr, FALSE, &D.shadowDSV);

    if (!clearDepth)
    {
	    const float tile = float(D.shadowRes) * 0.5f; // 2048
           // Layout: 0:(0,0)  1:(2048,0)  2:(0,2048)
    	float topLeftX = 0.0f;
    	float topLeftY = 0.0f;
    	if (cascadeIndex == 1) { topLeftX = tile; topLeftY = 0.0f; }
    	if (cascadeIndex == 2) { topLeftX = 0.0f; topLeftY = tile; }
    	if (cascadeIndex == 3) { topLeftX = tile; topLeftY = tile; }

    	D3D12_VIEWPORT vp{ topLeftX, topLeftY, tile, tile, 0.0f, 1.0f };
    	D3D12_RECT sc{ (LONG)topLeftX, (LONG)topLeftY, (LONG)(topLeftX + tile), (LONG)(topLeftY + tile) };
    	cl->RSSetViewports(1, &vp);
    	cl->RSSetScissorRects(1, &sc);
    }
	else
    {
        cl->ClearDepthStencilView(D.shadowDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

void Renderer::BindSpotShadowTarget(ID3D12GraphicsCommandList* cl, UINT lightIndex, bool clearDepth)
{
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    if (lightIndex >= LightManager::kMaxShadowedSpotLights)
    {
        lightIndex = LightManager::kMaxShadowedSpotLights - 1;
    }

    cl->OMSetRenderTargets(0, nullptr, FALSE, &D.spotShadowDSV[lightIndex]);

    const float res = static_cast<float>(std::max(D.spotShadowRes, 1u));
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, res, res, 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(res), static_cast<LONG>(res) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);

    if (clearDepth)
    {
        cl->ClearDepthStencilView(D.spotShadowDSV[lightIndex], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

void Renderer::BindPointShadowTarget(ID3D12GraphicsCommandList* cl, UINT cubeSlot, UINT face, bool clear)
{
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    if (cubeSlot >= LightManager::kMaxShadowedPointLights)
    {
        cubeSlot = LightManager::kMaxShadowedPointLights - 1;
    }
    if (face >= 6) { face = 5; }
    const UINT faceIndex = cubeSlot * 6u + face;

    // Depth-cube: this cube face IS a depth slice — bind it as the DSV (no color RT),
    // exactly like BindSpotShadowTarget binds a spot atlas slice.
    cl->OMSetRenderTargets(0, nullptr, FALSE, &D.pointShadowDSV[faceIndex]);

    const float res = static_cast<float>(std::max(D.pointShadowRes, 1u));
    D3D12_VIEWPORT vp{ 0.0f, 0.0f, res, res, 0.0f, 1.0f };
    D3D12_RECT sc{ 0, 0, static_cast<LONG>(res), static_cast<LONG>(res) };
    cl->RSSetViewports(1, &vp);
    cl->RSSetScissorRects(1, &sc);

    if (clear)
    {
        cl->ClearDepthStencilView(D.pointShadowDSV[faceIndex], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    }
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageGBufferSrvTable() {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    auto tbl = StageSrvUavTable({ D.gbSRV[0], D.gbSRV[1], D.gbSRV[2], D.depthSRV });
    return tbl.gpu; // shader key t0
}
D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageComposeSrvTable() {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    auto tbl = StageSrvUavTable({ D.lightSRV, D.gbSRV[2] }); // Light, Emissive
    return tbl.gpu;
}
D3D12_GPU_DESCRIPTOR_HANDLE Renderer::StageTonemapSrvTable() {
    auto& D = rtManager_.Deferred(currentFrameIndex_);
    D3D12_CPU_DESCRIPTOR_HANDLE src = D.sceneSRV;
    if (dlssHandler_ && dlssHandler_->ShouldUseUpscaledOutput() && D.dlssOutputSRV.ptr != 0)
    {
        src = D.dlssOutputSRV;
    }
    auto tbl = StageSrvUavTable({ src });
    return tbl.gpu;
}

D3D12_CPU_DESCRIPTOR_HANDLE Renderer::GetTonemapSourceSrvCPU() const
{
    const auto& D = rtManager_.Deferred(currentFrameIndex_);
    if (dlssHandler_ && dlssHandler_->ShouldUseUpscaledOutput() && D.dlssOutputSRV.ptr != 0)
    {
        return D.dlssOutputSRV;
    }
    return D.sceneSRV;
}

