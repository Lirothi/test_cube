# Dear ImGui Integration Plan for AI Agents

## Objective

Integrate Dear ImGui into this D3D12/Win32 project as a debug/tools UI layer.
The first usable result should show a simple ImGui window over the rendered scene
with:

- a text label
- a button
- an edit box
- a checkbox

The integration must not break the existing renderer, HUD text, camera controls,
resize handling, or shutdown path.

## Current Project Context

This is a native C++20 Visual Studio project using Win32 and D3D12.

Key files:

- `sources/app/App.cpp`: Win32 window creation, message pump, main loop.
- `sources/app/App.h`: app ownership and `WndProc` declaration.
- `sources/app/AppController.cpp`: app-level debug controls and HUD text.
- `sources/app/AppController.h`: controller state and public app control methods.
- `sources/app/scene/SceneRenderer.cpp`: render graph and overlay pass.
- `sources/rendering/core/Renderer.cpp`: D3D12 device, frame lifecycle, shutdown.
- `sources/rendering/core/Renderer.h`: renderer API and owned systems.
- `sources/input/InputManager.cpp`: Win32 keyboard/mouse message handling.
- `sources/input/InputManager.h`: input state queried by app/camera.
- `test_cube.vcxproj`: Visual Studio build file.
- `test_cube.vcxproj.filters`: Visual Studio filters file.

Existing overlay behavior:

- `AppController::BuildHud` writes text into `TextManager`.
- `SceneRenderer::Pass_Overlay` records the final overlay command list.
- The overlay pass currently binds the backbuffer and draws `TextManager`.

The best place to render ImGui is at the end of `SceneRenderer::Pass_Overlay`,
after `TextManager::Draw`.

## Guardrails

- Keep the first integration minimal and debug-oriented.
- Do not refactor the render graph or frame scheduler.
- Do not replace `TextManager`.
- Do not route engine textures into ImGui during the first pass.
- Do not use ImGui for gameplay UI yet.
- Keep ImGui rendering last in the overlay pass, because ImGui will bind its own
  descriptor heap.
- Avoid changing camera behavior except where needed to stop camera input while
  ImGui is actively using mouse or keyboard.

## Proposed Architecture

Add a small wrapper:

- `sources/ui/ImGuiLayer.h`
- `sources/ui/ImGuiLayer.cpp`

`Renderer` owns the `ImGuiLayer`.

`App::WndProc` forwards Win32 messages to the ImGui layer before the normal input
system consumes them.

`App` starts the ImGui frame after the message pump and before app/game UI is
built.

`AppController` builds the first debug ImGui window.

`SceneRenderer::Pass_Overlay` renders ImGui draw data after existing HUD text.

Frame shape:

```text
renderer.BeginFrame()
input.NewFrame()
process Win32 messages
renderer.BeginImGuiFrame()
renderer.Tick(deltaTime)
appController.Tick(...)
scene.Tick(...)
levelManager.Tick(...)
appController.BuildHud(...)
appController.BuildDebugUi(...)
scene.Render(...)
  main render graph
  overlay pass
    TextManager::Build
    TextManager::Draw
    Renderer::RenderImGui
renderer.EndFrame()
```

## Step 1: Vendor Dear ImGui

Add Dear ImGui source under:

```text
third_party/imgui/
```

Required files:

```text
third_party/imgui/imgui.cpp
third_party/imgui/imgui_draw.cpp
third_party/imgui/imgui_tables.cpp
third_party/imgui/imgui_widgets.cpp
third_party/imgui/backends/imgui_impl_win32.cpp
third_party/imgui/backends/imgui_impl_dx12.cpp
third_party/imgui/imgui.h
third_party/imgui/imconfig.h
third_party/imgui/imgui_internal.h
third_party/imgui/imstb_rectpack.h
third_party/imgui/imstb_textedit.h
third_party/imgui/imstb_truetype.h
third_party/imgui/backends/imgui_impl_win32.h
third_party/imgui/backends/imgui_impl_dx12.h
```

Optional during development:

```text
third_party/imgui/imgui_demo.cpp
```

Update:

- `test_cube.vcxproj`
- `test_cube.vcxproj.filters`

Add include paths if the project does not already pick them up through
`$(ProjectDir)third_party`:

```text
$(ProjectDir)third_party\imgui
$(ProjectDir)third_party\imgui\backends
```

## Step 2: Add `ImGuiLayer`

Create `sources/ui/ImGuiLayer.h`:

```cpp
#pragma once

#include <windows.h>
#include <wrl/client.h>
#include <d3d12.h>

class Renderer;

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
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    bool initialized_ = false;
};
```

Create `sources/ui/ImGuiLayer.cpp`.

Implementation requirements:

- Include `imgui.h`.
- Include `backends/imgui_impl_win32.h`.
- Include `backends/imgui_impl_dx12.h`.
- Create one shader-visible `D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV` heap.
- Use that heap for ImGui font texture descriptors.
- Use the renderer device, frame count, and backbuffer format.
- Call `ImGui_ImplWin32_Init(hwnd)`.
- Call `ImGui_ImplDX12_Init(...)`.
- In `BeginFrame`, call:

```cpp
ImGui_ImplDX12_NewFrame();
ImGui_ImplWin32_NewFrame();
ImGui::NewFrame();
```

- In `Render`, call:

```cpp
ImGui::Render();
ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
commandList->SetDescriptorHeaps(1, heaps);
ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
```

- In `Shutdown`, call:

```cpp
ImGui_ImplDX12_Shutdown();
ImGui_ImplWin32_Shutdown();
ImGui::DestroyContext();
srvHeap_.Reset();
```

Use `initialized_` guards so repeated shutdown is safe.

## Step 3: Expose ImGui Through `Renderer`

In `sources/rendering/core/Renderer.h`:

- Forward include or include `ui/ImGuiLayer.h`.
- Add a private member:

```cpp
ImGuiLayer imguiLayer_;
```

- Add public methods:

```cpp
void InitImGui();
void BeginImGuiFrame();
void RenderImGui(ID3D12GraphicsCommandList* commandList);
void ShutdownImGui();
bool HandleImGuiWndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
bool ImGuiWantsMouse() const;
bool ImGuiWantsKeyboard() const;
```

In `sources/rendering/core/Renderer.cpp`:

- Call `InitImGui()` near the end of `Renderer::InitD3D12`, after the device,
  swapchain, frame resources, and `RefreshCurrentFrameCaches()` are valid.
- Call `ShutdownImGui()` early in `Renderer::Shutdown`, before releasing D3D12
  resources.

Do not create or reset ImGui resources every frame.

## Step 4: Route Win32 Messages

In `App::WndProc` in `sources/app/App.cpp`:

1. Resolve the `App*` from `GWLP_USERDATA`.
2. If `app`, `systems_`, and renderer ImGui are available, call:

```cpp
const bool handledByImGui =
    app->systems_->renderer.HandleImGuiWndProc(hWnd, message, wParam, lParam);
```

3. If ImGui handles mouse or keyboard input, prevent the same message from
   reaching `InputManager` when appropriate.

Suggested helper logic:

```cpp
const bool mouseMessage =
    message >= WM_MOUSEFIRST && message <= WM_MOUSELAST;
const bool keyMessage =
    message >= WM_KEYFIRST && message <= WM_KEYLAST;

const bool consumedByImGui =
    (mouseMessage && app->systems_->renderer.ImGuiWantsMouse()) ||
    (keyMessage && app->systems_->renderer.ImGuiWantsKeyboard());
```

If `consumedByImGui` is true, skip:

```cpp
app->systems_->input.OnWndProc(hWnd, message, wParam, lParam);
```

Still let the normal `switch` handle `WM_SIZE`, `WM_DESTROY`, and other app
lifecycle messages.

## Step 5: Start the ImGui Frame

In `App::Run` in `sources/app/App.cpp`, after the Win32 message pump and before
`renderer.Tick(deltaTime)`, call:

```cpp
renderer.BeginImGuiFrame();
```

This matters because the current code calls `renderer.BeginFrame()` before
processing Windows messages. ImGui frame setup should happen after messages have
been processed for the frame.

## Step 6: Build the First UI

Add to `AppController.h`:

```cpp
void BuildDebugUi(Renderer& renderer);
```

Add to `AppController.cpp`:

```cpp
#include "imgui.h"
```

Example first window:

```cpp
void AppController::BuildDebugUi(Renderer& renderer)
{
    static int clickCount = 0;
    static char editText[128] = "edit me";

    ImGui::Begin("Debug UI");
    ImGui::Text("Dear ImGui is integrated");
    if (ImGui::Button("Button"))
    {
        ++clickCount;
    }
    ImGui::Text("Button clicks: %d", clickCount);
    ImGui::InputText("Edit box", editText, sizeof(editText));
    ImGui::Checkbox("FXAA", &settings_.doFxaa);
    ImGui::Checkbox("Profiler", &settings_.showProfiler);
    ImGui::Text("FPS: %.1f", renderer.GetFPS());
    ImGui::End();
}
```

In `App::Run`, call after `appController_.BuildHud(...)`:

```cpp
appController_.BuildDebugUi(renderer);
```

## Step 7: Prevent Camera/Input Conflicts

The camera captures the mouse while the look action is held. See
`sources/app/camera/Camera.cpp`.

In `AppController::Tick`, skip camera input when ImGui wants mouse or keyboard.

Example:

```cpp
const bool uiCapturingInput =
    renderer.ImGuiWantsMouse() || renderer.ImGuiWantsKeyboard();

if (!uiCapturingInput)
{
    scene.CameraRef().UpdateFromInput(input, deltaTime);
}
else if (input.IsMouseCaptured())
{
    input.SetMouseCapture(false);
}
```

Keep non-camera debug hotkeys working only if they are useful while UI is open.
If hotkeys interfere with typing, guard them with `!renderer.ImGuiWantsKeyboard()`.

## Step 8: Render ImGui in the Overlay Pass

In `SceneRenderer::Pass_Overlay` in `sources/app/scene/SceneRenderer.cpp`, after
existing text rendering:

```cpp
tm->Build(renderer, t.cl);
tm->Draw(renderer, t.cl);
renderer->RenderImGui(t.cl);
```

If `TextManager` is unavailable, ImGui should still render. A robust structure is:

```cpp
auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
renderer->RecordBindDefaultsNoClear(t.cl);

if (auto* tm = renderer->GetTextManager())
{
    if (frame_->settings.showProfiler)
    {
        Profiler::Get().EmitOverlay(tm, 16, 64, 20);
    }
    tm->Build(renderer, t.cl);
    tm->Draw(renderer, t.cl);
}

renderer->RenderImGui(t.cl);
renderer->EndThreadCommandList(t, ctx.batchIndex);
```

## Step 9: Build and Verify

Build:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe' test_cube.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
```

Manual verification:

- App launches.
- ImGui debug window appears over the scene.
- Text label is visible.
- Button increments a counter.
- Edit box accepts typed text.
- Checkbox toggles FXAA/profiler state.
- Camera does not rotate or capture mouse while interacting with ImGui.
- Existing HUD text still renders.
- Window resize still works.
- App exits cleanly.
- D3D12 debug layer does not report ImGui resource leaks.

## Common Failure Points

### Nothing Renders

Check:

- `BeginImGuiFrame()` is called exactly once before UI construction.
- UI construction happens before `RenderImGui`.
- `RenderImGui` is called inside an open command list.
- Backbuffer is in `D3D12_RESOURCE_STATE_RENDER_TARGET`.
- Overlay pass is still executed.

### Text Input Does Not Work

Check:

- `ImGui_ImplWin32_WndProcHandler` receives `WM_CHAR`.
- `InputManager` does not swallow keyboard messages before ImGui sees them.
- `BeginImGuiFrame()` runs after message processing.

### Camera Moves While Editing UI

Check:

- `AppController::Tick` skips `scene.CameraRef().UpdateFromInput` when ImGui
  wants mouse or keyboard.
- Mouse capture is released if UI starts consuming input.

### D3D12 Descriptor Heap Issues

ImGui sets its own shader-visible CBV/SRV/UAV heap during render. This is why it
should be rendered last in the overlay pass. If engine rendering is added after
ImGui, rebind the engine descriptor heaps with:

```cpp
renderer->BindDescriptorHeaps(commandList);
```

For the first implementation, keep ImGui last to avoid this.

## Acceptance Criteria

The task is complete when:

- Dear ImGui sources are part of the Visual Studio project.
- `ImGuiLayer` initializes and shuts down cleanly.
- Win32 input reaches ImGui.
- A debug ImGui window appears in the running app.
- Button, label, edit box, and checkbox work.
- Existing HUD text still renders.
- Camera input is blocked while ImGui captures mouse/keyboard.
- Debug x64 build succeeds.

