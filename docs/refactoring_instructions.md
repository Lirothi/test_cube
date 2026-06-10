# Prioritized Refactoring Instructions

Use this file as the working prompt for step-by-step architecture cleanup. Make one small refactor at a time, preserve behavior, and verify after each step. Do not combine unrelated phases in one change.

## Global Rules

- Preserve current runtime behavior unless a step explicitly says otherwise.
- Prefer small compatibility adapters over large call-site rewrites.
- Do not move code just to reduce file size. Move code only when it clarifies ownership or removes hidden coupling.
- Keep public APIs stable during extraction, then narrow them in later steps.
- Do not refactor third-party code.
- After each step, build the solution and run the app smoke path if possible.
- Watch especially for resize, level load/unload, material hot reload, DLSS toggle, SSR/FXAA toggles, ocean rendering, text overlay, and debug draw behavior.

## Priority 0: Baseline And Safety

Goal: establish confidence before changing architecture.

1. Record the current build command or Visual Studio configuration used for this repo.
2. Build without code changes.
3. Run the app and confirm:
   - window creation and resize work
   - demo level loads
   - basic camera input works
   - overlay text renders
   - scene renders with skybox, opaque objects, transparent objects, ocean, and debug grid
   - toggles for wireframe, profiler, SSR, DLSS, and FXAA do not crash
4. If build or runtime is already broken, document the existing issue before refactoring.

Acceptance: there is a known-good baseline or a clearly documented existing failure.

## Priority 1: Extract Scene Frame Data

Problem: `Scene` mixes world state, input handling, view preparation, render queue ownership, and pass execution.

Target shape:

- Add a `SceneFrameData` type near `sources/app/scene`.
- It should contain the data render passes need for one frame:
  - camera reference or camera matrices
  - main view
  - cascade views
  - spot shadow views
  - light manager data access
  - skybox pointer
  - render/debug settings such as `debugTexMode`, `showProfiler`, `doFxaa`, and SSR technique
- Keep ownership of objects and lights in `Scene`.
- `Scene::PrepareViews` fills `SceneFrameData` or updates fields that are then exposed through it.
- Do not move render passes yet.

Suggested files:

- `sources/app/scene/SceneFrameData.h`
- `sources/app/scene/Scene.h`
- `sources/app/scene/Scene.cpp`

Acceptance:

- `Scene::Render` can use `SceneFrameData` for pass inputs.
- Behavior is unchanged.
- No renderer internals are moved yet.

## Priority 2: Extract SceneRenderer / RenderPipeline

Problem: `Scene.cpp` owns the whole frame pipeline and all concrete render pass bodies.

Target shape:

- Introduce `SceneRenderer` or `RenderPipeline`.
- Move render graph construction and pass methods from `Scene` into this new class.
- `Scene` should call something like:
  - `PrepareViews(renderer)`
  - `SceneFrameData frame = BuildFrameData(...)`
  - `sceneRenderer.Render(renderer, frame)`
- Keep `SceneResourceBootstrapper` usable by the new renderer class initially.
- Do not redesign `RenderGraph` in this step.

Suggested files:

- `sources/app/scene/SceneRenderer.h`
- `sources/app/scene/SceneRenderer.cpp`
- `sources/app/scene/Scene.h`
- `sources/app/scene/Scene.cpp`

Acceptance:

- `Scene.cpp` no longer contains concrete `Pass_*` implementations.
- `Scene` remains responsible for world state and ticking objects.
- Render output remains unchanged.

## Priority 3: Replace Systems Access In App-Level Code

Problem: `Systems::Get*()` hides dependencies and makes lifecycle/order bugs easier.

Target shape:

- Introduce an explicit `AppContext` or `EngineContext`.
- Start with app-level call sites only:
  - `LevelManager`
  - `Level`
  - `DemoLevel`
  - `Camera`
  - `Scene`
- Keep `Systems` as a temporary adapter so the whole repo does not need to change at once.
- Prefer passing context through constructors or method parameters over adding new globals.

Suggested approach:

1. Add context type containing references to `Renderer`, `Scene`, `InputManager`, `LevelManager`, and optional `OceanSimulation`.
2. Change `LevelManager::LoadLevel` to receive or store context instead of calling `Systems::GetRenderer()` and `Systems::GetScene()`.
3. Change `Level::Unload` so it does not directly call `Systems::GetScene().Clear()`.
4. Change `Camera::UpdateFromInput` or related code to receive input explicitly.

Acceptance:

- App-level code no longer requires `Systems::Get*()` for normal control flow.
- `Systems` may still exist for compatibility with lower-priority modules.

## Priority 4: Hide D3D12 Upload Details From Level Loading

Problem: `LevelLoadContext` exposes `ID3D12GraphicsCommandList` and upload keep-alive resources to level/domain code.

Target shape:

- Replace raw D3D12 upload fields with a higher-level `ResourceUploadContext` or `UploadBatch`.
- Levels request resource initialization through renderer/resource APIs instead of directly carrying command-list details.
- Keep an internal bridge to the old fields while migrating resource loaders.

Suggested files:

- `sources/app/levels/Level.h`
- `sources/app/levels/LevelManager.cpp`
- resource loaders in `sources/rendering`, `sources/materials`, `sources/text`, and `sources/ocean`

Acceptance:

- `Level.h` no longer includes `<d3d12.h>`.
- Level implementations do not directly mention `ID3D12GraphicsCommandList`.
- Upload behavior remains unchanged.

## Priority 5: Split Renderer Internals Behind Facades

Problem: `Renderer` owns device/swapchain/fences, deferred targets, submit timeline, resource-state tracking, DLSS, descriptor staging, and many high-level managers.

Target shape:

- Keep `Renderer` as the public facade during extraction.
- Extract internals in this order:
  1. `GraphicsDevice` or `D3D12DeviceContext`: device, queue, factory/debug setup.
  2. `SwapchainManager`: swapchain, backbuffers, resize handling.
  3. `FrameScheduler`: frame resources, fences, begin/end frame.
  4. `RenderTargetManager`: deferred targets, RTV/DSV/SRV CPU heaps.
  5. `ResourceStateTracker`: known states and transitions.
  6. `DescriptorStaging`: per-frame shader-visible descriptor table staging.
- Keep the existing `Renderer` getter methods delegating to extracted components until call sites are migrated.

Suggested files:

- `sources/rendering/core/GraphicsDevice.h/.cpp`
- `sources/rendering/core/SwapchainManager.h/.cpp`
- `sources/rendering/core/FrameScheduler.h/.cpp`
- `sources/rendering/core/RenderTargetManager.h/.cpp`
- `sources/rendering/core/ResourceStateTracker.h/.cpp`
- `sources/rendering/core/DescriptorStaging.h/.cpp`

Acceptance:

- `Renderer.h` becomes meaningfully smaller.
- Existing render call sites still compile through the `Renderer` facade.
- Resize, shutdown, and frame synchronization behavior are unchanged.

## Priority 6: Make RenderGraph Resource-Aware

Problem: `RenderGraph` schedules pass lambdas, but resource reads/writes and barriers are still manually encoded inside pass bodies.

Target shape:

- Add explicit pass declarations for resource reads and writes.
- Start with symbolic resources, not a full allocator rewrite.
- Teach graph execution to issue transitions for common deferred resources.
- Migrate one pass group at a time:
  1. GBuffer
  2. Lighting
  3. SSR/SSR blur
  4. Compose
  5. Transparent
  6. Tonemap/copy to backbuffer

Acceptance:

- Pass code has fewer manual `renderer->Transition(...)` calls.
- Resource hazards are represented in graph declarations.
- Parallel execution still respects dependencies.

## Priority 7: Move Hardcoded Demo Data To Configuration

Problem: demo level setup hardcodes materials, shader paths, object placement, lights, and camera defaults.

Target shape:

- Do this only after ownership boundaries are cleaner.
- Start with material presets and level object descriptors.
- Keep C++ constructors available for special objects such as ocean/debug grid until a data model exists for them.

Acceptance:

- Demo level becomes mostly data assembly.
- Engine/rendering code no longer needs edits for simple scene content changes.

## Recommended First Prompt For Later Work

Start with Priority 1 only:

```text
Use docs/refactoring_instructions.md. Implement Priority 1 only: introduce SceneFrameData and route Scene::Render pass inputs through it. Do not move render passes yet. Preserve behavior and build afterward.
```

