# Claude Refactoring Instructions (test_cube engine)

Working prompt for incremental architecture cleanup of the C++ DX12 engine in `sources/`.
Companion to `docs/refactoring_instructions.md` (Codex version). This file reflects the
2026-06 architecture review by Claude; where the two disagree, this one explains why.

## How to use

Pick ONE step, implement it fully, build, smoke-test, stop. Do not chain steps in one
change. Suggested prompt template:

```text
Use docs/claude_refactoring_instructions.md. Implement step <N.M> only.
Preserve behavior, build the solution (test_cube.sln, x64), and report results.
```

## Global rules

- Preserve runtime behavior unless the step says otherwise.
- Never refactor `third_party/`, `deps/`, or `docs/*.js` (the JS game is a separate project).
- Build via `test_cube.sln` (VS2022, x64). There are no automated tests — the smoke test is:
  run the app, demo level loads, resize works, toggles (wireframe / profiler / SSR / DLSS /
  FXAA / DebugTex) don't crash, ocean + transparents + text overlay render.
- Hot paths matter: pass bodies run on task-system worker threads every frame. Don't add
  locks, allocations, or `shared_ptr` copies inside per-frame code.
- Keep public APIs stable during extraction; narrow them in a later step.
- `Scene.cpp` (~2000 lines) and `Renderer.cpp` (~1900 lines) are the two god files. Almost
  every step below shrinks one of them.

## Known landmines (verified in review, fix opportunistically when touching the file)

- `Scene::Pass_Compose` (Scene.cpp ~line 1693): `skyBox_->GetTex()->GetSRVCPU()` is called
  unconditionally while line ~1680 guards `skyBox_ ? ...` — null skybox crashes the pass.
- `Systems::Get()` (Systems.cpp) locks a global mutex on EVERY access, and it is called from
  pass bodies on worker threads (`Pass_ShoreDepth`, `Scene::Tick`, ocean code). The lock is
  unnecessary after init: replace with an atomic pointer load or plain pointer + debug assert
  (lifecycle is already strictly App::Run-scoped). Do this no later than step 3.
- `Scene::PrepareViews` stomps camera config every frame (hfov=90°, zNear=0.01, zFar=10000
  hardcoded) — any level that sets camera FOV is silently overridden.
- `App::SetRunnig` typo; harmless but rename when touching App.
- RenderGraph::AddPassInternal: the `else` branch assert at the end is unreachable
  (`pendingSuccessors_` is a fixed `std::array<,MaxPasses>`); dead code, remove when touching.

## Step 1 — Extract frame data and shrink Scene (highest value)

Matches Codex Priority 1+2, with additions. Scene currently mixes: world state, input
handling, HUD text, view/cascade preparation, render-graph construction, and 17 concrete
pass bodies.

1.1 `SceneFrameData` struct in `sources/app/scene/` carrying per-frame pass inputs:
    camera matrices/jitter, main view ptr, cascade views + cached cascade matrices/biases
    (the `cachedLightView_`/`cachedScale_`... arrays move here), spot shadow views, light
    manager ref, skybox ptr, `ssrTechnique_`, `doFxaa_`, `debugTexMode_`, `showProfiler_`.
    `Scene::PrepareViews` fills it; pass bodies read only from it. No pass moves yet.

1.2 Extract a compute-pass helper BEFORE moving passes (Codex misses this; it is the
    biggest LOC win). Every fullscreen pass repeats the same ~40-line shape:
    Begin CL → SetName → N×Transition → AllocDynamic CB → write constants →
    RenderContext acquire → StageSrvUavTable → sampler table → material Bind →
    Dispatch(w/8, h/8) → UAVBarrier → End CL.
    Introduce `ComputePassRecorder` (or free function) taking {material, CB bytes,
    SRV list, UAV list, samplers, dispatch dims, transitions} so Lighting / SpotLights /
    PointLights / SSR / SSRBlur / Compose / Tonemap / FXAA shrink to ~10 lines each.

1.3 `SceneRenderer` (new files `sources/app/scene/SceneRenderer.h/.cpp`): move render-graph
    construction and all `Pass_*` bodies out of Scene. `Scene::Render` becomes:
    PrepareViews → build SceneFrameData → `sceneRenderer_.Render(renderer, frame)`.
    `SceneResourceBootstrapper` moves with it (it is pass-material state, not world state).

1.4 Move input handling and HUD text out of Scene entirely. `Scene::Tick` maps keys to
    DLSS/SSR/FXAA settings and `Scene::Render` reads the Wireframe action and builds the
    FPS/camera HUD — rendering must not read input. Create an app-level
    `DebugHud`/`AppController` (sources/app/) that ticks before Scene and owns these
    toggles, writing them into SceneFrameData/Renderer.

Acceptance: Scene.h has no `Pass_*` declarations, no input includes; Scene.cpp < 600 lines;
identical visual output.

## Step 2 — Hide D3D12 upload plumbing (Codex Priority 4, promoted)

Promoted above context-passing because the same pattern is duplicated in `App::InitScene`
and the pending-level branch of `App::Run` (create allocator+CL, load, close, execute,
full-GPU wait), and it blocks Level.h from dropping `<d3d12.h>`.

2.1 `UploadBatch` RAII type (sources/rendering/core/): owns allocator + CL + keep-alive
    vector; `Begin(renderer)` / `SubmitAndWait(renderer)`. Replace both App call sites.
2.2 `LevelLoadContext` holds `UploadBatch&` instead of raw `ID3D12GraphicsCommandList*` +
    keep-alive vector. Bridge getters during migration; delete them at the end.

Acceptance: Level.h no longer includes `<d3d12.h>`; App.cpp creates no command lists.

## Step 3 — Explicit context instead of Systems::Get* (Codex Priority 3)

24 call sites across 9 files, app layer only — small job once Step 1 landed (most Scene
call sites disappear with the pass extraction).

3.1 Fix the `Systems::Get()` mutex (see landmines) first — independent, zero-risk.
32  `AppContext { Renderer&, Scene&, InputManager&, LevelManager&, OceanSimulation* }`
    passed to LevelManager/Level/DemoLevel/Camera. Keep `Systems::` as adapter for
    rendering/ocean/text modules until step 5.
3.3 `Camera::UpdateFromInput` receives `InputManager&` explicitly.

## Step 4 — Renderer facade split (Codex Priority 5, same order)

Extraction order: GraphicsDevice → SwapchainManager → FrameScheduler →
RenderTargetManager (DeferredTargets + CPU heaps) → ResourceStateTracker (knownStates_ +
CL lanes + TLS; keep the submit-time cross-CL barrier fixup in ExecuteTimelineAndPresent
intact — it is the heart of the engine's threading model, move it last and verbatim) →
DescriptorStaging. Renderer getters keep delegating until all call sites migrate.

## Step 5 — Resource-aware RenderGraph (Codex Priority 6)

Note before starting: the submit-time barrier resolution in `ExecuteTimelineAndPresent`
already infers cross-CL transitions from per-CL firstUse/current state maps. The graph
upgrade should DECLARE reads/writes per pass and feed the same machinery, not invent a
second tracker. Migrate one pass group at a time (GBuffer → Lighting → SSR → Compose →
Transparent → Tonemap), deleting manual `Transition` calls as each group migrates
(92 manual calls today, 74 of them in Scene.cpp/SceneRenderer.cpp after step 1).

## Step 6 — Data-driven demo level (Codex Priority 7, unchanged)

Only after ownership is clean. DemoLevel.cpp is 100% hardcoded constructors; start with
material presets (already registered by name in App::InitScene — move to JSON next to
`input/bindings.json`), then object descriptors. Keep C++ for ocean/debug-grid.

## Deliberate differences from the Codex file

- Added 1.2 (compute-pass helper): largest mechanical win, makes 1.3 a small diff.
- Added 1.4 (input/HUD out of Scene): Codex notes the mixing but never schedules removal.
- Promoted upload-batch work (their P4) above context-passing (their P3): it deletes
  duplicated App code and unblocks Level.h immediately, while context-passing is easier
  after Step 1 shrinks Scene anyway.
- Added the landmine list (Compose null-deref, Systems::Get() mutex-per-call on hot paths,
  PrepareViews camera stomp) — correctness items found during review.
