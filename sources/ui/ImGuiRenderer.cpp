#include "ui/ImGuiRenderer.h"

#include <algorithm>

#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "core/threading/ThreadRole.h"
#include "rendering/core/Renderer.h"

namespace ui
{
void ImGuiRenderer::Init()
{
    if (layer_.IsInitialized())
    {
        return;
    }

    if (!renderer_.GetHWND() || !renderer_.GetDevice() || !renderer_.GetCommandQueue())
    {
        return;
    }

    layer_.Init(renderer_.GetHWND(), renderer_);
}

void ImGuiRenderer::BeginFrame()
{
    layer_.BeginFrame();
}

void ImGuiRenderer::EndFrame()
{
    TC_CHECK_GAME("ImGuiRenderer::EndFrame");
    CPU_SCOPE(ProfilerScopes::kImGuiEndFrame);
    layer_.EndFrame();
}

void ImGuiRenderer::Render(ID3D12GraphicsCommandList* commandList)
{
    // Step 7 — out-of-graph, explicit before state. The list holds two kinds: editor-owned
    // textures (created shader-readable, so this is a no-op) and engine render targets shown by
    // TextureDebugViewer (left at their canonical by the frame's graph work, since the overlay
    // runs last). Both answers come from the canonical registry, so no tracker is involved.
    // G8b: what is drawn is the frame the hand-over gave the render half (HandOver) -- never ImGui's own state,
    // which the game half's next UI frame may be writing now. A loop that never hands over draws no UI.
    for (ID3D12Resource* resource : pendingTextureResources_)
    {
        Renderer::TransitionExplicit(commandList, resource, renderer_.GetCanonicalState(resource),
                                     D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    layer_.Render(commandList);
    // ...and put every one of them BACK. This used to be left undone, which was silently fine only
    // while every previewable target rested in a state sharing PIXEL_SHADER_RESOURCE's barrier
    // LAYOUT — NON_PIXEL_SHADER_RESOURCE does (both are D3D12_BARRIER_LAYOUT_SHADER_RESOURCE), so
    // the leftover state was invisible to the enhanced-barrier validator. It is NOT fine for a
    // target resting in UNORDERED_ACCESS (tonemap, fxaa, and briefly the GTAO output): the next
    // frame's compiled barrier claims a UAV before-state against a resource the inspector left in
    // the SHADER_RESOURCE layout, which is INCOMPATIBLE_BARRIER_LAYOUT and a debug-layer break.
    //
    // Restoring here rather than constraining what a target may rest in keeps the rule where it
    // belongs: the graph decides resting states, and this out-of-graph peek borrows and returns.
    // For editor-owned textures the canonical IS PixelShaderResource, so both calls are no-ops.
    for (ID3D12Resource* resource : pendingTextureResources_)
    {
        Renderer::TransitionExplicit(commandList, resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                     renderer_.GetCanonicalState(resource));
    }
    pendingTextureResources_.clear();
    keepAlive_.clear(); // recorded: the frame's list holds them now, and their owners idle the GPU to free them
}

ImTextureID ImGuiRenderer::CreateTextureId(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc)
{
    if (!resource)
    {
        return ImTextureID_Invalid;
    }
    // G8b: the game half gets a VIRTUAL ID, made real at the hand-over (HandOver) in the slot of the frame that
    // draws it -- the UI may run while the render thread draws the previous frame, before this frame's slot is free.
    if (threading::CurrentRole() == threading::ThreadRole::Game)
    {
        return layer_.RequestTexture(resource, srvDesc);
    }

    if (std::find(pendingTextureResources_.begin(),
            pendingTextureResources_.end(),
            resource) == pendingTextureResources_.end())
    {
        pendingTextureResources_.push_back(resource);
    }
    return layer_.CreateTextureIdForSrv(renderer_.GetDevice(), resource, srvDesc, renderer_.GetCurrentFrameIndex());
}

void ImGuiRenderer::HandOver()
{
    for (ID3D12Resource* resource : gameReadableMarks_)
    {
        renderer_.SetResourceState(resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    gameReadableMarks_.clear();
    if (layer_.HasTextureUpdates())
    {
        renderer_.Rhi().Flush(); // G10: the backend's upload submits on the queue itself -- after everything already handed over
    }
    std::vector<ID3D12Resource*> shown;
    layer_.HandOver(renderer_.GetDevice(), renderer_.GetCurrentFrameIndex(), shown, keepAlive_);
    for (ID3D12Resource* resource : shown)
    {
        if (std::find(pendingTextureResources_.begin(), pendingTextureResources_.end(), resource) ==
            pendingTextureResources_.end())
        {
            pendingTextureResources_.push_back(resource);
        }
    }
}

#if WITH_EDITOR
void ImGuiRenderer::ReleaseTextureDescriptors(ID3D12Resource* resource)
{
    threading::WaitRenderForGame("ImGuiRenderer::ReleaseTextureDescriptors");
    layer_.DropTextureRequests(resource); // a request of this frame draws nothing (G8b)
    gameReadableMarks_.erase(std::remove(gameReadableMarks_.begin(), gameReadableMarks_.end(), resource),
                             gameReadableMarks_.end());
    layer_.ReleasePreviewDescriptorsForResource(resource);
    renderer_.ClearResourceState(resource);
}
#endif

void ImGuiRenderer::MarkTextureShaderReadable(ID3D12Resource* resource)
{
    // Editor textures shown through CreateTextureId (icon atlas, asset
    // thumbnails, viewport billboards) are created in a pixel-shader-readable
    // state outside the state tracker. Register that so Render does not
    // record a COMMON->PSR barrier with a wrong before-state, which the D3D12
    // debug layer rejects. Call each frame before Render, since a resize
    // clears all known states. Engine render targets go through TextureDebugViewer
    // instead and keep their real tracked state, so they are unaffected.
    if (resource)
    {
        // G8b: from the game half it is declared at the hand-over (HandOver) -- the canonical registry is the
        // frame in flight's.
        if (threading::CurrentRole() == threading::ThreadRole::Game)
        {
            if (std::find(gameReadableMarks_.begin(), gameReadableMarks_.end(), resource) == gameReadableMarks_.end())
            {
                gameReadableMarks_.push_back(resource);
            }
            return;
        }
        // Step 7: this now DECLARES the texture's canonical (it is created shader-readable and
        // stays there), rather than papering over the tracker's global map. That is why it
        // survives the tracker's deletion instead of going with it as D2 assumed.
        renderer_.SetResourceState(resource, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

void ImGuiRenderer::Shutdown()
{
    layer_.Shutdown();
}

bool ImGuiRenderer::HandleWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    return layer_.HandleWndProc(hwnd, message, wParam, lParam);
}

bool ImGuiRenderer::WantsMouse() const
{
    return layer_.WantsMouse();
}

bool ImGuiRenderer::WantsKeyboard() const
{
    return layer_.WantsKeyboard();
}

bool ImGuiRenderer::WantsTextInput() const
{
    return layer_.WantsTextInput();
}
} // namespace ui
