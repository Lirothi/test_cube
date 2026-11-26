#pragma once
// Architecture plan R4.3 (docs/architecture_review_plan.md, section 11.2, wave 3): the UI's renderer -- ImGui's frame,
// its textures and its draw -- out of Renderer. It sits over the renderer's device and frame (until R4.5 makes them
// rhi::Device / rhi::Frame) and the canonical-state registry: the textures it shows are borrowed from the frame's
// graph and given back (Render). Renderer owns one (Renderer::ImGui()).

#include <d3d12.h>
#include <windows.h>
#include <wrl/client.h>

#include <vector>

#include "imgui.h"
#include "ui/ImGuiLayer.h"

class Renderer;

namespace ui
{
class ImGuiRenderer
{
public:
    explicit ImGuiRenderer(Renderer& renderer) : renderer_(renderer) {}

    // Needs the renderer's window, device and queue; does nothing without them or when already initialized.
    void Init();
    void Shutdown();

    void BeginFrame();
    void EndFrame(); // game thread plan G1: the game half closes the UI frame; Render records it
    // Plan G8b, at the hand-over (after BeginFrame, nothing rendering): the UI's texture requests become descriptors in
    // this frame's slot and real IDs in its draw lists, the textures it marked readable are declared, and the atlas
    // textures ImGui changed are uploaded. The UI may have run while the render thread drew the previous frame.
    // (Renderer::HandOverImGui calls it, after copying the exposure readouts the UI shows.)
    void HandOver();
    void Render(ID3D12GraphicsCommandList* commandList);
    // Renderer::BeginFrame: the textures listed for a draw that did not happen are dropped with the frame.
    void BeginRenderFrame() { pendingTextureResources_.clear(); }

    ImTextureID CreateTextureId(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC& srvDesc);
#if WITH_EDITOR
    // Drop the cached ImGui preview descriptors for a resource (editor thumbnail
    // cache eviction). The caller idles the GPU before freeing the resource.
    void ReleaseTextureDescriptors(ID3D12Resource* resource);
#endif
    // Register an ImGui texture as pixel-shader-readable so Render does not emit a bogus
    // COMMON->PSR barrier. Call each frame the resource is displayed, before Render. Not
    // editor-only: the DeveloperWindow's reset-icon atlas (every config) uses it too, and the
    // Release build failed on the guard.
    void MarkTextureShaderReadable(ID3D12Resource* resource);

    bool HandleWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    // WM_DPICHANGED: the UI rescaled at the next UI frame (ImGuiLayer::RequestDpi).
    void RequestDpi(UINT dpi) { layer_.RequestDpi(dpi); }
    bool WantsMouse() const;
    bool WantsKeyboard() const;
    bool WantsTextInput() const;

private:
    Renderer& renderer_;
    ImGuiLayer layer_;
    std::vector<ID3D12Resource*> pendingTextureResources_;
    std::vector<ID3D12Resource*> gameReadableMarks_; // G8b: MarkTextureShaderReadable from the game half
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> keepAlive_; // G8b: the frame's shown textures, until drawn
};
} // namespace ui
