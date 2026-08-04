# Barrier Architecture — Executor Plan

This document is written to be executed by an AI coding agent. Read the **Executor Guide**
first, then **Reference** (it is what the steps assume), then do the steps in order. Steps are
numbered **1..16 straight through**; there is one sequence, not two.

Each step must leave **both** build configurations green. Only two steps change runtime
behavior — **Step 7** and **Step 15** — and each is explicitly a flip with its own gate.

## The two goals, in order

**Goal 1 — delete `ResourceStateTracker` (Steps 1-8).** Make the render graph the single
author of barriers: passes register their state usage before recording, a compile step
resolves every transition ahead of execution, and no command list is ever allocated just to
carry barriers. This deletes the tracker class, its parallel-recording machinery, and the
acquire-prologue lists.

**Goal 2 — migrate to enhanced barriers (Steps 9-16).** Move GPU synchronization from legacy
`D3D12_RESOURCE_BARRIER` (one `D3D12_RESOURCE_STATES` per resource) to
`ID3D12GraphicsCommandList7::Barrier` (orthogonal **sync / access / layout**), with a mandatory
legacy fallback. This is a representation change, judged on validation signal, not CPU.

**Do Goal 1 first.** It touches ~120 call sites; Goal 2 rewrites how barriers are *emitted*.
The other order means touching every site twice.

## Progress

| Step | State |
|---|---|
| 1 — two-phase pass plumbing | **DONE** (committed `385eb9e`) |
| 2 — render graphs off the stack | **DONE** (committed `385eb9e`) |
| 3 — the comparator | **DONE** (committed `fed604f`) |
| 4 — lazy creation out of record bodies | **DONE** (committed `fed604f`) |
| 5 — convert passes | **DONE** (committed `58cde73`) — every pass converted, zero fatal mismatches in both shadow modes |
| 6 + 6b — canonical registry | **DONE (uncommitted)** — 0 off-canonical on a steady frame; declared-resource table no longer ratchets; every owner on `GpuResource`. **The measurement refuted D2's "canonical = creation state" — read Step 6 before Step 7.** |
| 7 — the flip | **DONE (uncommitted). `ResourceStateTracker` IS DELETED**, along with the per-list acquire prologue. Compiled barriers are the only barrier path; there is no flag any more. 0 non-noise GBV + 0 comparator findings over 20 GBV stress iterations in BOTH shadow modes; submission stress 0 failures; both configs 0/0. Net −777/+110. |
| 8..16 | not started |

---

## Executor Guide (read first)

**Repo:** `D:\Programming\test_cube`. Shared conventions live in
`docs/level_editor_HANDOFF.md`; the essentials are repeated here.

**Build (run BOTH after every step):** use the PowerShell tool (not bash — bash mangles
MSBuild `/t:` `/p:` switches).
```
& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "D:\Programming\test_cube\test_cube.sln" /t:Build /p:Configuration=Debug   /p:Platform=x64 /m /nologo "/clp:ErrorsOnly;Summary"
& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "D:\Programming\test_cube\test_cube.sln" /t:Build /p:Configuration=Release /p:Platform=x64 /m /nologo "/clp:ErrorsOnly;Summary"
```
Both must report `0 Warning(s) 0 Error(s)`. `Debug|x64` is `WITH_EDITOR=1`; `Release|x64` is
the non-editor build. **`"/clp:ErrorsOnly;Summary"` prints the warning COUNT but not the
text** — when a warning appears, rebuild with `/v:normal` and grep for `warning C` to see it.

**Stack budget is tight — `/analyze` C6262 fires at 16 KB.** `SceneRenderer::Render` sat at
~16.2 KB before Step 2 moved the main graph to the heap. Anything added to `RenderGraph::Pass`
is multiplied by `MaxPasses` (~31) and will trip it. If C6262 appears, do not raise the
threshold: move the data off the stack.

**This is all ENGINE code — NOT `WITH_EDITOR`-gated.** The renderer, the render graph,
`GraphicsDevice`, `Renderer` and resource-creation paths ship in Release. Do not wrap any of
this in `#if WITH_EDITOR`.

**Line endings:** all `.cpp/.h` in this repo are **CRLF**; markdown is LF. The Write tool
creates new files as LF, so any new source/header must be converted. Verify every touched file
(lone-LF count must be 0):
```powershell
$b=[IO.File]::ReadAllBytes($path); $n=0; for($i=0;$i -lt $b.Length;$i++){ if($b[$i]-eq 10 -and ($i-eq 0 -or $b[$i-1]-ne 13)){$n++} }; "$path loneLF=$n"
```

**Verification per step:**
1. Both builds `0/0`.
2. CRLF check on every touched file.
3. **`x64\Debug\test_cube.exe --scene-stress-gbv=20`** — verdict CLEAN, and the only
   `InfoQueue` message ids present are the known noise **939 / 940 / 1006 / 1358**. Anything
   else is a regression.
   **`--scene-stress-gbv` validates NOTHING in Release**: the debug layer and
   `SetEnableGPUBasedValidation` are inside `#ifdef _DEBUG` (`GraphicsDevice.cpp:35-54`), so a
   Release run only proves the app does not crash and logs `info queue: unavailable`.
4. **`--scene-stress`** (exit code is unreliable — trust `verdict:` in `scene_stress.log`)
   after any change to barrier emission, submission, or resource lifetime.
5. For Steps 7 and 15, and any step that changes visible transitions: run the app and compare
   a `--shot` capture against the previous step (`--wind-freeze=3.0` makes it deterministic).

**Measurement, when a step asks for it:** interleave A/B/A/B with ~35 s cooldowns and compare
frame counts first — this machine downclocks aggressively on back-to-back launches, and a run
with >5 % fewer frames than its pair is garbage regardless of what the ms say.

**Do NOT commit.** The user commits per step. Keep `memory/barrier-plan-progress.md` current.

---

## Reference

### R1. Current state (verified 2026-08-01)

- **Legacy barriers only.** No enhanced-barrier API anywhere.
- **Interfaces cap at `ID3D12Device5` + `ID3D12GraphicsCommandList4`.** `GraphicsDevice` holds
  `device_` and `device5_` (nullable, "null if DXR unsupported") — mirror that for `device10_`.
  Command lists are QueryInterface'd via `Renderer::AsCmdList4` (`Renderer.cpp:36`) — mirror
  that for `AsCmdList7`.
- **No Agility SDK** (no `D3D12SDKVersion` export, no `D3D12Core.dll`). The engine builds
  against the system Windows SDK; enhanced-barrier types need ≥ 10.0.20348. Step 9 confirms.
- **Barriers are emitted in exactly these places:**
  1. `ResourceStateTracker::Transition` (`ResourceStateTracker.cpp:119-125`) — intra-CL.
  2. `ResourceStateTracker::AppendAcquireBarriers` (`.cpp:230-236`) — the acquire barriers at
     CL boundaries.
  3. The submit path, `Renderer.cpp:777-800` — records the acquire output onto a dedicated CL
     and calls `ApplyFinalStates`.
  4. Present: `Renderer.cpp:812-819` (RT→PRESENT), `:994-999` (PRESENT→RT in
     `RecordBindAndClear`), plus `SetResourceState` seeds at `:253/:820/:827`.
  5. `Renderer::UAVBarrier`.
  6. Upload/creation paths: `Texture2D.cpp`, `TextureCube.cpp`, `UploadManager.h`, and
     `rt/AccelerationStructure.cpp` (special — AS bypasses the tracker, scratch is COMMON).

### R2. Why deleting the tracker is possible

- **Submission order is known before any recording starts.** `ExecuteParallel` calls
  `Unroll(renderer, /*executeInplace=*/false, &schedule)` **first** (`RenderGraph.h`), producing
  every node with its batch index, and only then creates tasks.
- **`SetResourceState` is not tracking — it is a creation-time seed.** Every call site is the
  line after `CreateCommittedResource`, restating the state the resource was just created in:
  `RenderTargetManager.cpp:195/221/258/322/374/519/583/645`,
  `VirtualShadowMap.cpp:50/75/98/279-283/562-614`, `LightManager.cpp:299/392`,
  `ShadowGpuData.cpp:139/202/1633`, `ParticleEmitterObject.cpp:165-174`,
  `GpuInstancedModels.cpp:83-94`, `OceanSimulation.cpp:553-626`, `Texture2D.cpp:384/701/790`,
  `TextureCube.cpp:205`. Every `ClearResourceState`/`ForgetResources` is destruction-time.
  **The global map is a shadow copy of what the owning object already knows.**
- **Nothing interleaves into the frame's submission behind the graph's back.** Uploads go
  through `UploadBatch`, whose `Submit`/`SubmitAndWait` closes, executes and **fully waits for
  the GPU** (`UploadBatch.h:12-20`); they never enter `SubmitTimeline`. A future
  streaming/async-upload path would break this and must either join the compile or stay behind
  a wait.
- **The declaration channel already exists as data**: `Pass::declares`, populated by the
  3-argument `AddPass` overloads; 20 of 34 `SceneRenderer` passes already use it.

### R3. Execution order is NOT pass order — the slot model

The compile must walk **gather order**, produced by `SubmitTimeline::GatherFrameLists`
(`SubmitTimeline.h:86-119`): **for each batch in index order → the driver first (with its
bundles executed inside it, sorted by `localOrder`) → then the directs, sorted by `localOrder`.**

Pass order in `schedule` is not that order, for three reasons:
- **Bundles execute before every direct in their batch**, regardless of when the pass that
  recorded them ran. (This engine has been bitten by exactly that once, in a transparent-pass
  blend-order misdiagnosis; the fix was `direct + localOrderBase`, not graph dependencies.)
- **Several passes can share one batch** — `RenderGraph`'s constructor takes a
  `submitBatchIndex` and `Unroll` reuses it when set. Their relative order is `localOrder`.
- **A fan-out pass occupies several slots** (per chunk / cascade / spot light).

So barriers are keyed by **slot**, never by pass and never by command-list pointer:

```
slot = (batchIndex, kind ∈ {driver-bundle, direct}, localOrder)
```

The pointer key is impossible anyway — command lists are created inside the pass body
(`ctx.BeginCL` → `Renderer::BeginThreadCommandList`), so they do not exist when Prepare runs.

This works because `localOrder` is assigned **before** the work is dispatched (chunk index,
cascade index, spot-light index; single-CL bodies use 0 — see the DETERMINISTIC ORDER comment
at `SubmitTimeline.h:16-25`), duplicates within a namespace fail fast, and
`RendererSubmissionStress` already pins the gather order with byte-for-byte tests.

### R4. Call-site census (2026-08-01)

122 tracked `Transition(...)` call sites — the workload map for Step 5.

| File | Sites | A | B | C | D | E |
|---|---:|---:|---:|---:|---:|---:|
| `app/scene/SceneRenderer.cpp` | 38 | 10 | 6 | 18 | 2 | 2 |
| `rendering/shadows/VirtualShadowMap.cpp` | 33 | 7 | 1 | 23 | 0 | 2 |
| `rendering/shadows/ShadowGpuData.cpp` | 13 | 3 | 3 | 6 | 1 | 0 |
| `ocean/OceanSimulation.cpp` | 13 | 4 | 1 | 7 | 1 | 0 |
| `vfx/ParticleEmitterObject.cpp` | 9 | 4 | 2 | 3 | 0 | 0 |
| `rendering/diagnostics/RendererSubmissionStress.cpp` | 8 | — | — | — | — | 8 |
| `rendering/core/DlssHandler.cpp` | 4 | 4 | 0 | 0 | 0 | 0 |
| `rendering/core/Renderer.cpp` | 3 | 0 | 0 | 2 | 0 | 1 |
| `rendering/meshes/GpuInstancedModels.cpp` | 3 | 3 | 0 | 0 | 0 | 0 |
| `ocean/OceanRenderable.cpp` | 2 | 0 | 0 | 0 | 2 | 0 |
| **Total** | **126** | **35** | **13** | **59** | **6** | **13** |

*(126 counts every textual site including the four `#if WITH_EDITOR` ones; 122 excludes the
tracker's own definition and the `Renderer::Transition` wrapper.)*

- **A — unconditional, one state for the whole pass (35).** Mechanical.
- **B — behind a predicate evaluated in the pass body (13):** `caching`, `scatterActive`,
  `singleDraw`, `compactArgs`, "is DLSS on", "is sorting enabled".
- **C — the same resource takes 2-3 states INSIDE one pass (59, 47 %).** Concentrated, not a
  tail: `VirtualShadowMap::RecordPageRender` alone moves **nine** resources through 2-3 states
  (`pageDrawArgs_` UAV→INDIRECT_ARGUMENT, `pageProj_` UAV→SRV, `pageVisibleList_` UAV→VB,
  `perPageDirty_` UAV→SRV, `pageGroupCount_`/`pageScatterDyn_` UAV→SRV, `physOwner_`
  SRV→COPY_SOURCE, `physOwnerPrev_` SRV→COPY_DEST→SRV, `pageArgCount_` UAV→INDIRECT_ARGUMENT,
  plus `pagePool_`→DEPTH_WRITE). `ShadowGpuData`'s `instancesUnified_` does
  COPY_DEST→UAV→NON_PIXEL_SRV (`:1386/1408/1470`).
- **D — resource identity or state value only known at execution time (6):** `resolveSource`
  (DLSS output vs tonemap, `SceneRenderer.cpp:3236`), `giBuf` per GI caster
  (`ShadowGpuData.cpp:1443`), the nullable `prevDisplacement` (`OceanRenderable.cpp:692/695`),
  `OceanSimulation`'s computed `srvState`.
- **E — out of model (13):** `RendererSubmissionStress.cpp` and the `#if WITH_EDITOR`
  object-id picking sites.

**A static `declares` list cannot express B, C or D** — it is written at graph-build time and
cannot see per-frame values. That is why the design is two-phase (D1), not bigger initializer
lists. Do not "simplify" it back.

### R5. Two findings that shape the work

1. **`declares` is not authoritative today.** `ApplyDeclaredStates` (`RenderGraph.h`) is a plain
   loop over `renderer->Transition(...)` run from the pass body on a worker. A pass may declare
   X, then transition to Y, and nothing complains. Compiled and recorded barriers could
   therefore disagree silently — the worst failure mode in this area. Step 5 must make
   declarations authoritative and Step 7 must assert it.
2. **20 of 34 `SceneRenderer` passes already declare**, yet the file still holds 38 imperative
   sites. Declaring is not currently exclusive of transitioning; category C is why.

### R6. Measured baseline (2026-08-01, Release, default level)

| Scope | avg |
|---|---|
| `Renderer::ExecuteTimelineAndPresent` | 0.269 ms |
| ├ `Service2` (acquire resolution **+ present epilogue**) | 0.102 ms |
| └ `Service4` (`Present`) | 0.090 ms |
| `CPU.Frame` | 1.964 ms |
| └ of which `WaitForFrame` | 1.310 ms |

Two things follow. **`Service2` shrinks but does not vanish** — its scope
(`Renderer.cpp:766-828`) also covers the present-epilogue CL, which survives; do not write
"0.102 ms saved" before Step 8 measures it. And **the CPU idles two thirds of every frame
today**, so Goal 1 is a capacity investment, not an FPS change: judge it on Step 7's acceptance
criteria, not on a frame-rate delta.

---

## Design

### D1. Two-phase passes (Prepare / Record)

A pass gains a second callback. `Prepare(ctx)` runs **per frame, serially, before any recording
begins**; `Record(ctx)` is today's body with every state mention removed.

```
for each pass in schedule order:   pass.Prepare(ctx)   // serial; registers via ctx.Use()
compile: resolve registrations into per-SLOT barrier arrays (gather order, see R3)
for each pass (parallel, as today): pass.Record(ctx)   // ctx.Barrier(cl, n) replays them
```

`Prepare` calls `ctx.Use(resource, state)` in the order the pass needs them — repeats for the
same resource are the normal case — and `ctx.NextPoint()` to close one barrier point and open
the next. `Record` marks the same boundaries with `ctx.Barrier(cl, n)`, which emits a
**precomputed** array: no lookup, no map, no tracker.

Because `Prepare` runs at frame time, it sees the same values `Record` does: **census category
B evaporates, most of D evaporates, and C becomes the natural shape.** Only E stays outside.

`Prepare` must also declare the pass's **fan-out shape** — how many slots it will occupy and
with which `localOrder`s — not just its state usage. That is computable: `localOrderBase` and
the job count are already decided before dispatch (`SceneRenderer.cpp:937/948/964/1000`).

**The one thing this does not solve: `Prepare` and `Record` must agree.** Two mandatory
defences:
1. **Single source of truth.** A predicate is computed once in `Prepare`, stored on the pass's
   frame state, and *read* by `Record`. `Record` never re-evaluates what `Prepare` used.
2. **Debug validation.** `Renderer::Transition` from inside a two-phase pass asserts against
   the compiled expectation (see R5.1 for why this is not optional).

### D2. Canonical states — what removes the live map

Every graph resource declares a canonical (resting) state once, at creation — exactly what
`SetResourceState` already passes (R2). Invariant: **every frame begins and ends with every
graph resource in its canonical state.** Therefore:

- the compile seeds from a static table each frame — no cross-frame carry, so no live map;
- the frame epilogue emits whatever transitions return resources to canonical (most already
  end there; the rest batch into the epilogue CL that already exists for the present barrier);
- it is robust against the graph changing shape frame to frame, which it does (DLSS on/off,
  VSM vs Legacy, editor). Carrying last frame's end state instead would be a live map again,
  and wrong on the first frame after a shape change.

**Non-graph paths keep local knowledge.** Uploads create a resource as `COPY_DEST` and
transition it themselves — the before-state is known by construction. AS already bypasses the
tracker. `Renderer::MarkImGuiTextureShaderReadable` (`Renderer.cpp:537-550`) exists *only* to
paper over the global map, and is deleted with it.

> **DECIDED at Step 6 (2026-08-03), both by measurement:**
> 1. **Canonical is the state a resource RESTS IN AT FRAME END, not the state it is created in.**
>    The paragraph above ("exactly what `SetResourceState` already passes") is wrong and is kept
>    only to show what was refuted: 85 of 160 resources drift under it, so its epilogue costs ~85
>    transitions per frame. 95 of the 97 drifting resources have a *stable* end state, so
>    declaring that instead makes the epilogue ~2 transitions.
> 2. **Lifetime goes to the resource wrapper** (the second bullet below), not to a hand-maintained
>    unregister at every release site. The registry leaked 143 → 169 entries over 20 churn
>    iterations and crashed the frame-end check on a released resource; a manual unregister leaves
>    that failure one new release path away, and it fails silently.

**Where canonical state lives — decide at Step 6, do not pre-commit:**
- *(cheaper)* a `resource → canonical` registry owned by the render graph, written at creation,
  read-only during the frame. The class dies; a flat map remains that never changes mid-frame
  and needs no locking.
- *(cleanest, bigger)* canonical state on a resource wrapper. There is no universal wrapper
  today — raw `ComPtr<ID3D12Resource>` in owner objects, with partial wrappers only
  (`ShadowGpuData::UavRing`, `Texture2D`, `TextureCube`, `InstanceBuffer`) — so this means
  introducing one and touching ~60 declaration sites. It pays for itself by making lifetime
  automatic: no unregister step, and no way for a recycled `ID3D12Resource*` to inherit a stale
  entry, a bug class this engine has already been bitten by.

### D3. How each tracker responsibility is discharged

| Responsibility | Replaced by |
|---|---|
| per-CL `firstUse` + acquire barriers | the compile step; nothing at runtime |
| parallel bookkeeping (lanes, TLS, epochs, mutex) | nothing — compile is serial, emission precomputed |
| `knownStates_` as *live* state | canonical states (D2) — no live map at all |
| `SetResourceState` / `ClearResourceState` | a write-once registry at create/destroy |

### D4. Enhanced-barrier translation (Goal 2)

A device-init flag `enhancedBarriers_` = `EnhancedBarriersSupported && CommandList7 available &&
!forceDisabled`. Everything branches on it; when false the engine emits exactly what it does
today. One translation layer `LegacyStateToBarrier(D3D12_RESOURCE_STATES, bool isBuffer)` →
`{ SYNC, ACCESS, LAYOUT }`; textures use all three, buffers omit layout.

| Legacy `D3D12_RESOURCE_STATES` | Sync | Access | Layout (textures) |
|---|---|---|---|
| `COMMON` / `PRESENT` | `SYNC_ALL` (or `NONE` at start) | `ACCESS_COMMON` | `LAYOUT_COMMON` (present: `LAYOUT_PRESENT`) |
| `RENDER_TARGET` | `SYNC_RENDER_TARGET` | `ACCESS_RENDER_TARGET` | `LAYOUT_RENDER_TARGET` |
| `DEPTH_WRITE` | `SYNC_DEPTH_STENCIL` | `ACCESS_DEPTH_STENCIL_WRITE` | `LAYOUT_DEPTH_STENCIL_WRITE` |
| `DEPTH_READ` | `SYNC_DEPTH_STENCIL` | `ACCESS_DEPTH_STENCIL_READ` | `LAYOUT_DEPTH_STENCIL_READ` |
| `PIXEL_SHADER_RESOURCE` | `SYNC_PIXEL_SHADING` | `ACCESS_SHADER_RESOURCE` | `LAYOUT_SHADER_RESOURCE` |
| `NON_PIXEL_SHADER_RESOURCE` | `SYNC_(COMPUTE/VS/etc)` | `ACCESS_SHADER_RESOURCE` | `LAYOUT_SHADER_RESOURCE` |
| `ALL_SHADER_RESOURCE` | `SYNC_ALL_SHADING` | `ACCESS_SHADER_RESOURCE` | `LAYOUT_SHADER_RESOURCE` |
| `UNORDERED_ACCESS` | `SYNC_(COMPUTE\|PIXEL)` | `ACCESS_UNORDERED_ACCESS` | `LAYOUT_UNORDERED_ACCESS` |
| `COPY_DEST` | `SYNC_COPY` | `ACCESS_COPY_DEST` | `LAYOUT_COPY_DEST` |
| `COPY_SOURCE` | `SYNC_COPY` | `ACCESS_COPY_SOURCE` | `LAYOUT_COPY_SOURCE` |
| `GENERIC_READ` (upload buffers) | `SYNC_ALL` | `ACCESS_(CONSTANT_BUFFER\|SHADER_RESOURCE\|...)` | — (buffer) |
| `RAYTRACING_ACCELERATION_STRUCTURE` | `SYNC_RAYTRACING` / `BUILD_RTAS` | `ACCESS_RTAS_READ/WRITE` | — (buffer) |

Conservative first cut: where a state maps to multiple stages use the widest correct `SYNC_*`;
Step 16 narrows for perf. `LAYOUT_UNDEFINED` as `LayoutBefore` is legal only where prior
contents are intentionally discarded. Texture-vs-buffer comes from `GetDesc().Dimension`
(cache it) or an `isBuffer` bit recorded at creation.

---

## Steps

### Step 1 — two-phase pass plumbing, dormant — **DONE (uncommitted)**

`sources/rendering/core/RenderGraph.h` only. Added `ResourceUse`, `PrepareFn`,
`SetPassPrepare`, the `ctx.Use` / `ctx.NextPoint` / `ctx.Barrier` API (`Barrier` a deliberate
no-op), and `RunPrepares` called from `ExecuteParallel` right after `Unroll` and before task
creation. Passes without a `Prepare` keep the tracker path verbatim — that mixed mode is what
makes Step 5 incremental.

**Result:** both configs `0/0`; Debug `--scene-stress-gbv=20` CLEAN (939/940/1006/1358 only).
`SetPassPrepare` call sites in the engine: **0** — provably inert.

Notes for whoever continues:
- Registrations go into **one arena per graph** with per-pass `{begin, count, points}` slices.
  A per-pass inline list is what first blew the stack budget (see Step 2).
- Everything two-phase lives in a heap-side `PrepareState` behind a `std::unique_ptr` that
  `SetPassPrepare` allocates lazily, so a graph with no Prepare costs one pointer.
- `ctx.Use` bounds-checks with `RendererInvariantFailure`, not `assert`: `tc::inl_vector`
  guards overflow with a bare assert, i.e. **unchecked in Release**, and a dropped `Use` is a
  missing barrier.
- `typename PrepareState::Slice& x = ...` inside the class template trips MSVC's dependent-name
  parse; `auto&` sidesteps it.
- The sequential `RenderGraph::Execute` runs pass bodies inline during `Unroll`, so it has no
  "all Prepares done, nothing recorded yet" moment. It now **fails fast** if it meets a graph
  with a Prepare rather than silently skipping registration. Wiring it is Step 3.

### Step 2 — render graphs off the stack — **DONE (uncommitted)**

`SceneRenderer::Render` built its `RenderGraph` as a local: ~16 KB (`MaxPasses` × `Pass`, each
with a `std::function`), leaving the function at ~16.2 KB against C6262's 16 KB threshold with
no headroom — Step 1 tripped it immediately.

- Added `RenderGraph::Reset(submitBatchIndex)`, which clears everything `AddPass` /
  `BeginCLGroup` build up **including the pass bodies** (they capture frame data; a reused
  graph must not hold stale captures), keeps the `PrepareState` allocation, and clears all of
  `pendingSuccessors_` / `groups_` (both are indexed independently of `passesNum_`).
- `SceneRenderer` owns `std::unique_ptr<RenderGraph<kMainRenderGraphPassCount>>
  mainRenderGraph_`, allocated once and `Reset()` at the top of `Render`.

Same semantics (a freshly empty graph each frame), no per-frame stack cost, no per-frame
allocation, and the pass array's `std::function`s are no longer reconstructed every frame.
The small sub-graphs (epilogue, gbuffer, transparent) stay local — they are a few passes each.

**Result:** both configs `0/0` (C6262 gone); Debug `--scene-stress-gbv=30` CLEAN, ids
939/940/1006/1358 only; `--shot` capture matches the previous build.

### Step 3 — the comparator, dormant — **DONE (uncommitted)**

**What was built** (not what the step originally described — see "scope correction" below):
- `Renderer::TransitionLog` + `SetThreadTransitionLog` (`Renderer.h`/`.cpp`): while installed on
  a thread, `Renderer::Transition` also appends `(resource, state)`. Thread-local, because pass
  bodies record on workers.
- `RenderGraph::RunPassBody` installs one around a converted pass's body; `ReportComparator`
  runs after every task has finished and diffs **registered (Prepare) vs performed (Record)**
  per pass, logging `[barrier-cmp]` lines to DBWIN.
- `render::g_barrierComparator`, default OFF, plus `--barrier-cmp`.
- **`Main_ReflectionBlur` converted** as the test subject — deliberately, because it exercises
  both hard census cases at once: `reflection` and `reflectionScratch` each take two states
  inside the body (category C) and the second pair sits behind a predicate (category B).
  Its body is unchanged; the comparator only watches.

**Compared as multisets, not sequences.** A pass may order its transitions differently from its
registrations as long as the same work happens; sequence equality only becomes required when the
compiled arrays go authoritative (Step 7), and that check belongs with the compile.

**Verified both ways** — a comparator that has never fired proves nothing:
- correct registration → **silent** across `--scene-stress=6`, stress verdict CLEAN;
- registration deliberately broken (one state changed to `COPY_SOURCE`, one `Use` deleted) →
  **39 lines**, correctly naming `REGISTERED but not performed state=0x800` and `PERFORMED but
  not registered state=0x40` for the right resources. Then reverted and re-verified silent.
- Debug `--scene-stress-gbv=20 --barrier-cmp` CLEAN, ids 939/940/1006/1358 only.

**Trap found while testing:** `--scene-stress` **returns from `wWinMain` before the rest of the
command line is parsed** (`main.cpp:188-206`). Any diagnostic flag meant to work during the churn
must be parsed *above* that branch — `--barrier-cmp` now is. The first negative-control run
looked like a working comparator staying silent; it was simply never enabled.

**Scope correction, deliberate.** The step originally said "build the per-slot barrier arrays in
gather order and compare them against what the tracker emits". That is not runnable yet: with no
pass converted, the compile would predict nothing and every real barrier would read as a
mismatch. Worse, a *global* prediction has to know the incoming state at each slot, which today
only the tracker knows — seeding the prediction from the tracker would make it compare the
tracker against itself.

So the check that actually unblocks Step 5 is the **local** one, and that is what exists:
does each pass DO what it REGISTERED. Ordering/slot-model validation (R3) is a global property
that can only be checked once every pass is converted; it moves to Step 7's acceptance, where the
compiled arrays first become authoritative. **Fan-out passes are still a gap**: work dispatched to
other threads is not observed, so those passes must be treated as unverified rather than clean —
`SceneRenderer.cpp:937/948/964/1000` is the shape to watch.

### Step 3 (original text, for reference)

Build the per-slot barrier arrays in **gather order** (R3) after `Unroll` and all `Prepare`
calls, and — behind a flag — *compare* them against what `ResourceStateTracker` actually emits.
Log mismatches; change nothing.

Also wire the sequential `RenderGraph::Execute` path (Step 1 left it failing fast), which needs
a two-phase Unroll there.

**This step decides whether the whole model is right**, and it is the cheapest possible place
to find out — nothing depends on the compiled arrays yet. Do not treat it as a formality, and
**run it on a scene that actually uses bundles** (transparent / ocean / particles): compiling in
`schedule` order instead of gather order is the highest-severity mistake available here,
because it is silently *almost* right — it only diverges where a batch mixes bundles and
directs, or where several passes share a batch.

**Acceptance:** both `0/0`; GBV CLEAN; with the flag on, the mismatch log is empty on a
bundle-using scene across a full `--scene-stress` churn.

### Step 4 — move lazy creation out of record bodies — **DONE (uncommitted)**

One creation point per frame, before the graph is built and therefore before anything records:
- `SceneRenderer::EnsureFrameResources(renderer)`, called from the top of `Render` right after
  `frame_` is set.
- `VirtualShadowMap::EnsureFrameResources(renderer, shadowGpu)` — wraps the former inline
  `EnsureShaderResources` + `EnsureRenderResources`; the three record bodies now only check
  whether the resources exist.
- Light buffers: growth moved to the same point. `LightManager` gained read-only
  `HasSpotLightBuffer` / `HasPointLightBuffer`, and `Pass_SpotLights` / `Pass_PointLights` use
  those instead of calling `Ensure*`. The counts are derived identically
  (`GetSpotLightCount()` / `PointLights().size()`), and the light set does not change during
  `Render`, so the hoist is exact rather than approximate.

**Result:** both configs `0/0`; Debug `--scene-stress-gbv=30 --barrier-cmp` CLEAN with ids
939/940/1358 only; comparator silent; `--shot` matches — spot cones, point lights and shadows
all present.

**Correction to this step's site list:** `DlssHandler.cpp:443` is **not** a live site. It sits
inside `if (kTagManualExposure)` where `constexpr bool kTagManualExposure = false`, i.e. dead
code left from the DLSS auto-exposure experiment. Left alone; removing dead code is not this
plan's business.

Why this matters beyond Prepare's requirement: growing a buffer inside a recording body frees
the previous allocation while earlier in-flight frames may still reference it. That is exactly
how the spot/point light buffers produced the `DXGI_DEVICE_HUNG` the `--scene-stress` harness
was written to catch, which had to be worked around by pre-growing under GPU idle. This removes
the shape of the bug instead of patching it per resource.

### Step 4 (original text, for reference)

`Prepare` can only register a resource that exists, but several passes create lazily inside the
record body: `VirtualShadowMap.cpp:401/440/783/786` (`EnsureShaderResources` /
`EnsureRenderResources`), `SceneRenderer.cpp:1982/2088` (`EnsureSpotLightBuffer` /
`EnsurePointLightBuffer`), `DlssHandler.cpp:443` (`EnsureExposureResources`). ~6-8 sites.

Independently correct: a lazy grow inside a record body is exactly the pattern that produced
the `DXGI_DEVICE_HUNG` use-after-free hunted down via `--scene-stress` (spot/point light
buffers freed mid-render), which had to be worked around by pre-growing under GPU idle.

**Acceptance:** both `0/0`; `--scene-stress` CLEAN (this is the use-after-free-prone area);
comparator still silent.

### Step 5 — convert passes to Prepare + Record — **DONE (uncommitted)**

**Final state: every pass in the main graph and the epilogue graph has a Prepare, and the
comparator reports zero `MISSING` lines.** Gate run twice, Debug `--scene-stress-gbv=20
--barrier-cmp`, once per shadow mode (the second is the only way `Main_CSM` runs at all):
`verdict: CLEAN after 20 iterations` both times, zero `MISSING`, zero observation overflow,
GBV ids only from the known-noise set {939, 940, 1006, 1358}. Both configs build `0/0`.

**The comparator has a third line kind: `SKIPPED (body performed nothing)`.** A pass that
early-outs registers everything and performs nothing, which otherwise logs one benign "extra"
per registration *per frame* — `Main_VsmPageRender` produced **31 000 lines** that way in a
still-camera run. One line instead, and it states the useful fact: that pass's registration is
**unverified**, which a pile of "extra" lines does not.

That distinction immediately corrected a wrong reading. `Main_VsmPageRender` looked like it never
ran — in a plain `--shot` run it does not, because a still camera + still content trips
`vsmSkipUpdate_` (`SceneRenderer.cpp:596-603`) and VSM freezes the pool. Under `--scene-stress`
the level churn keeps it alive: 1 `SKIPPED`, 70 benign extras, **zero fatal**. So **verify VSM
passes under the stress harness, not a static capture** — a silent still-camera run proves
nothing about them.

Converted in the first tranches — **12 passes, zero fatal mismatches**: `Main_ReflectionBlur` (Step 3),
`Main_Skybox`, `Main_Compose`, `Main_VsmPageRequest`, `Main_ReflectionSource`,
`Main_DebugDraw`, `Main_SelectionOutline`, `Main_Lighting`, `Main_SpotLights`,
`Main_PointLights`, `Main_ObjectIdReadback`, `Main_VsmPageRender`. Both configs `0/0`; Debug
`--scene-stress-gbv=20 --barrier-cmp` CLEAN (939/940/1006/1358 only); `--shot` unchanged.

The three lighting passes added **no** new comparator lines — their bodies do exactly what they
declare, so `UseDeclared()` alone converted them despite each having several AddPass variants
(VSM vs Legacy, with/without spot or point shadows).

**Watch for passes whose id can alias another pass.** `pObjectIdReadback` falls back to
`pTransp` when no pick is pending, and `pSelectionOutline` to `pDebugDraw`; a `SetPassPrepare`
placed after such an if/else would attach a second Prepare to the *other* pass and double-count
its registrations. Both are set inside their `if`. `Main_ObjectIdReadback` is also
`WITH_EDITOR`-only and needs a pending pick, so the stress harness never exercises it —
count it as written, not verified.

**The comparator now separates two severities — this is the bar, so read it carefully:**
- **`MISSING (performed, never registered)` is FATAL.** The body needs a transition nothing
  registered; once the compiled arrays are authoritative it simply would not be emitted.
  **Silence here is what "converted" means.** Currently: none.
- **`INFO extra (registered, pass did not perform)` is BENIGN.** Almost always the body
  early-outed before `ApplyDeclaredStates`. The compiled path emits the barrier anyway, so the
  running state stays consistent — it just costs one extra transition on frames the pass does
  nothing. Currently 6, all understood: `Main_VsmPageRequest` early-outs some frames
  (`Deferred[N].Depth`) and its three `COPY_SOURCE` moves belong to the conditional debug
  readback; `Main_DebugDraw` early-outs when there is nothing to draw.

**Subsystems register their own resources.** `Main_VsmPageRequest` transitions seven buffers
private to `VirtualShadowMap`, so VSM gained `PrepareRequestPass(RenderGraphPassContext&)`
rather than exposing every buffer for `SceneRenderer` to list. `RenderGraphPassContext` is
forward-declared in `VirtualShadowMap.h` and included only in the `.cpp` — the header already
pulls in `Renderer.h`, so including `RenderGraph.h` there would be circular. **Use this shape
for `ShadowGpuData`, `OceanSimulation` and the particle emitters too.**

**The workflow that makes this mechanical — use it, do not read every callee by hand:**
1. `ctx.UseDeclared()` registers exactly what the pass's `AddPass` declares, which is what
   its body's opening `ApplyDeclaredStates(cl)` performs. For a pass that transitions nothing
   beyond its declarations that is the whole conversion (`Main_Skybox` was silent first try).
2. Run `--barrier-cmp` and read the `PERFORMED but not registered` lines: those name exactly
   what the body does on top, including through callees. Add a `NextPoint()` + `Use()` for
   each, re-run, repeat until silent.
3. The log prints the resource's **debug name** (`WKPDID_D3DDebugObjectNameW`), so a line maps
   straight back to a `Use` call. Unnamed resources fall back to the address — swapchain
   backbuffers are the notable ones.

**Two things learned in the first tranche:**
- `kResourceUsesPerPassBudget` had to go **8 → 24**. `Main_Compose` declares 8 states by itself.
  The comparator reports `observation buffer overflowed` rather than silently truncating —
  truncation would have read as a false "registered but not performed". All of it is heap-side,
  so raising it costs no stack.
- **`Main_Tonemap` needed a hand-written Prepare** (done in the last tranche). It also drives
  the whole DLSS evaluate (`scene`/`gbVelocity`/`depth` → NPS, `dlssOutput` → UAV) *and* the
  backbuffer resolve (`tonemap` → COPY_SOURCE, backbuffer → COPY_DEST → RENDER_TARGET). Both
  the resolve source and the backbuffer are chosen at runtime — census category D — so a
  `UseDeclared` one-liner could never cover it.

Later the budget went **24 → 48**: `Main_Transparent`'s driver alone performs 15 and each
fan-out chunk adds two more.

### Step 5 worklist for the remaining ~13 passes

**Discovery result (measured, not guessed).** Temporarily giving an undeclared pass an *empty*
Prepare makes the comparator print everything it performs as `MISSING` — that is the exact list
to register. Run once, write the registrations, remove the empty Prepare. Do not leave empty
Prepares in the tree: they keep the log noisy and break the "no MISSING lines" bar.

What the probe found (states: `0x8` UAV, `0xC0` all-SRV, `0x40` non-pixel SRV, `0x10`
DEPTH_WRITE, `0x400` COPY_DEST, `0x800` COPY_SOURCE, `0x200` INDIRECT_ARGUMENT, `0x1` VB/CB):

| Pass | Performs | Owner that should register it |
|---|---|---|
| `Main_BuildAS` | nothing | — (AS bypasses the tracker) |
| `Main_PrologueClear` | nothing | — |
| `Main_ObjectCompute` | ~9 buffers: UAV / all-SRV / COPY_DEST / COPY_SOURCE | `GpuInstancedModels` + `ParticleEmitterObject`, per object |
| `Main_TerrainDepth` | 3 shore-depth textures: DEPTH_WRITE + all-SRV | `OceanSimulation` |
| `Main_ShadowCull` | `InstancesUnified`, `BoundsUnified`, `IndirectArgs`, `IndirectCounts`, `VisibleList` (UAV / SRV / COPY_DEST / INDIRECT_ARGUMENT / VB) + 5 GI instance buffers | `ShadowGpuData` |

Give each subsystem a `Prepare…Pass(RenderGraphPassContext&)` the way `VirtualShadowMap` has
(forward-declare the context in the header, include `RenderGraph.h` only in the `.cpp`).
`Main_ObjectCompute` and the GI half of `Main_ShadowCull` iterate per-object resources — census
category D — so they register by walking the same lists the bodies walk.

**Converted from that table:** `Main_BuildAS` and `Main_PrologueClear` (an *empty* Prepare is
their correct final registration — measured to perform no transitions), `Main_TerrainDepth`
(shore depth: DEPTH_WRITE then shader-readable), `Main_ShadowCull` (via the new
`ShadowGpuData::PrepareCullPass`, which walks `giCasters_` itself for the per-object GI buffers).

**That work found a Step 4 miss, and the comparator is what caught it.** `RecordCull` also called
`EnsureShaderResources` from inside the record body. `IsGiIndirectActive()` gates on the
GI-scatter PSO that call creates, so `PrepareCullPass` ran while it did not exist yet, read the GI
path as inactive, registered nothing for it — and then the body created the PSO and transitioned
the buffers anyway. Two `MISSING` lines, no other symptom. Fixed properly by hoisting
`ShadowGpuData::EnsureShaderResources` into `SceneRenderer::EnsureFrameResources` (it is now
public with that rationale on it), not by loosening the registration.

**How the rest were converted:**

| Pass | Registration |
|---|---|
| `Main_ObjectCompute` | `PrepareCompute(RenderGraphPassContext&)` on `RenderableObjectBase` (default no-op) mirroring `ExecuteCompute`, overridden in the three implementers of `RecordCompute`. `OceanRenderable` forwards to a new `OceanSimulation::PrepareUpdate`, which mirrors `Update`'s 5-point sequence over `displacement_` / `prevDisplacement_` / `foamTurbulence_`. The pass walks `frame_->objects` exactly as the body does. |
| `Main_GBuffer` | The RT/depth set the inner graph's driver declares, plus `PrepareOpaqueDrawStates`. Exact match, zero comparator lines. |
| `Main_CSM` / `Main_SpotShadows` / `Main_PointShadows` | Atlas → DEPTH_WRITE (declared for CSM, hand-registered for the other two) + `PrepareOpaqueDrawStates`. |
| `Main_RTDebug`, glass ×2 | `UseDeclared()` — the TLAS SRV is a plain descriptor and never transitions. |
| `Main_Transparent` | Hand-written, 5 points: depth/scene snapshot copies → `RecordOceanReflection`'s compute → `makePixelReadable` → forward-target rebind → per-object reads. Exact match. |
| `Main_Tonemap` | Hand-written; registers **both** branches of `ranDlss` and **both** resolve sources, since each is decided inside the body. |
| `Main_Debug`, `Epilogue_Overlay` | Empty Prepare — `RecordBindDefaultsNoClear` only sets targets and viewport, and ImGui/text perform no transitions. Comparator-verified, not assumed. |

**A second per-object hook, `PrepareRender`, mirrors `Render`.** `SceneRenderer::PrepareOpaqueDrawStates`
walks every **opaque** object calling it (G-buffer + all three shadow passes); `Main_Transparent`
walks the transparent half. Shadow passes share `PrepareRender` because `RecordShadow` reads the
same buffers in the same states as `Render` — and if that ever diverges the comparator says
`MISSING` rather than letting it pass. Walking every object instead of the visible buckets is
deliberate: a culled object's redundant barrier is free, a visible one's missing barrier is not.

**`--shadow-mode=` had to move up in `main.cpp`.** It was parsed *after* the `--scene-stress`
branch returns, so every stress run silently took the VSM path and `Main_CSM` was never even added
to the graph. Same fix as `--barrier-cmp` earlier. Without it the CSM/spot/point conversions could
not have been verified at all.

**Before Step 7, sweep for `Ensure*` still called from record bodies — DONE.** Swept: the only
ones left inside a record body are `ShadowGpuData::RecordCull` → `EnsureShaderResources` (now
idempotent — `SceneRenderer::EnsureFrameResources` runs first, so this is a guard for non-graph
callers) and `→ EnsureReadback` (debug-gated, creates a COPY_DEST readback and transitions
nothing), plus `TextManager::Draw` → `EnsureTextIndexCapacity` (`TextManager.cpp` contains no
`Transition` call at all). The `EnsureRing`/`EnsureUavRing` growth paths run from the caster-set
rebuild, not from a body. The 2 × 20 GBV iterations of reload/switch/resize/DLSS churn — which do
fire those growth paths — produced zero `MISSING`, which is the empirical form of the same check:
a resource freed and reallocated between Prepare and Record would show up as `MISSING` on the new
pointer.

**Fan-out observation — DONE (uncommitted).** The four passes that dispatch recording to worker
threads were invisible to the comparator, so a silent result there meant *unobserved*, not
correct. Now:
- `TransitionLog::count` is a `std::atomic<uint32_t>` and `Renderer::Transition` claims its slot
  with `fetch_add` — several workers append to one pass's buffer concurrently.
- `Renderer::CurrentThreadTransitionLog()` + the RAII `Renderer::TransitionLogScope` carry the
  pass thread's log onto a worker. Each of the four dispatch sites captures the log **on the pass
  thread** (`Renderer::TransitionLog* const cmpLog = Renderer::CurrentThreadTransitionLog();`)
  and the job installs it for its own duration: `SceneRenderer.cpp` object batches, CSM cascades,
  spot faces, point faces.
- **That last bullet used to say "every dispatch site joins its jobs before returning". It was
  wrong, and it was a use-after-free.** `RenderObjectBatch` uses `DispatchTrack`, which is
  fire-and-*track*: the jobs are joined only at the frame's `WaitForTrackedAsyncTasks`, long
  after the body returns. The `TransitionLog` was a local in `RunPassBody`, so the first time
  `Main_GBuffer` got a Prepare the run died with an access violation writing a dead stack frame
  (`GpuInstancedModels::Render`, `0xC0000005`). Two fixes, both required:
  - the log now lives in the heap-side `PrepareState::Observed`, so a straggler writes into a
    live object;
  - `ExecuteParallel` calls `tasks.WaitForTrackedAsyncTasks()` before `ReportComparator()` **when
    the comparator is on**, so the diff sees a complete buffer. Costs nothing with the flag off,
    and the frame's own wait then returns immediately.

  Worth noting how this failed: the comparator's own instrumentation was the bug, and it only
  fired on the one pass whose fan-out is not joined. Any future observation hook must assume the
  body can outlive its own stack frame.

**Proven, not assumed:** with a temporary empty Prepare, `Main_GBuffer` — pure fan-out — now
reports what its workers do: `GB0`/`GB1`/`GB2`/`GBAux`/`GBVelocity` → RENDER_TARGET, `Depth` →
DEPTH_WRITE, plus two unnamed all-SRV buffers (per-object GI instance buffers, census category D,
so `GpuInstancedModels` should register them). Before the change the same probe printed nothing.
`Main_SpotShadows` / `Main_PointShadows` still print nothing — their bodies genuinely perform no
transitions in the stress levels, which is a different fact from being unobserved and is now
distinguishable. (Cause found later: both return immediately under `render::VsmActive()`, so only
a `--shadow-mode=legacy` run exercises them. `demo.json` does have spot lights with
`shadowsEnabled: true`.)

**The sequential `Execute()` now supports Prepare too.** It used to hard-fail on a graph with one,
which left `Epilogue_Overlay` unconvertible. It now takes the same two-phase shape as
`ExecuteParallel` (`Unroll` → `RunPrepares` → run nodes → `ReportComparator`) when `prepare_` is
set, and keeps the old one-walk inline path when it is not — which is what the inner G-buffer and
transparent graphs use, so they are untouched. The epilogue graph moved to a member
(`epilogueRenderGraph_`) for the same reason the main one did: a local would heap-allocate its
`PrepareState` every frame.

**One `/analyze` trap:** `ReportComparator`'s loop over `passesNum_` fires C28020 on the array
index when instantiated for a one-pass graph (the epilogue), and `std::min` is not enough to
convince it — the bound has to be stated in the loop condition (`i < passesNum_ && i < MaxPasses`).
Debug is the only config that runs `/analyze`, so this is invisible in Release.

### Step 5 (original text, for reference)

Order: `DlssHandler`, `GpuInstancedModels`, `ParticleEmitterObject`, `OceanSimulation`,
`ShadowGpuData`, `VirtualShadowMap`, `SceneRenderer` — easiest first, worst last.

Make declarations authoritative as you go (R5.1). **The comparator must be silent after each
file before starting the next** — that is the whole point of Step 3.

**Metric:** imperative `Transition(...)` sites 122 → ~19 (categories D+E only).

**Acceptance per file:** both `0/0`; GBV CLEAN; comparator silent; `--shot` unchanged.

### Step 6 — canonical registry, logging mode — **MECHANISM DONE (uncommitted); RESULT REFUTES D2**

Turn every `SetResourceState` site into a canonical declaration (D2) and every
`ClearResourceState`/`ForgetResources` into an unregister. Behind the flag, have the compile
**assert at frame end that each graph resource is back at canonical — logging, not enforcing**.
Every failure is either a mis-declaration or a resource that genuinely needs an epilogue
transition; this step finds them all before the invariant is load-bearing.

**Registry-vs-wrapper: registry.** New `sources/rendering/core/ResourceDeclarations.h` holds
`CanonicalStateRegistry` (the `resource → {canonical state, debug name}` table) plus a
`ResourceDeclarations` sink with `Declare` / `Forget` / `ForgetMany`. Every creation site already
funnelled through `Renderer::SetResourceState`, so that method simply *became* the declaration —
zero call-site churn for the ~45 owners. The two exceptions are the backbuffer's mid-frame
`PRESENT ↔ RENDER_TARGET` flips, which moved to a new `SetTrackedStateOnly`; declaring
`RENDER_TARGET` there every frame would have redefined canonical continuously and made the check
vacuous. `RenderTargetManager` took the sink by value in place of its `ResourceStateTracker&`
(9 declaration sites), which is the shape Step 7 wants: the sink's `tracker` member disappears and
not one call site changes.

The wrapper option stays rejected for now — ~60 declaration sites — but see the lifetime finding
below, which is the strongest argument yet *for* it.

**Result (Release, `--canonical-check --scene-stress=4`): 85 of 160 declared resources end the
frame off-canonical.** Not a handful. Classified:

| Class | Example | Reading |
|---|---|---|
| declared `COMMON`, rests in a working state | `VSM.PageDrawArgs` COMMON → `INDIRECT_ARGUMENT`; `VSM.PhysOwner` COMMON → non-pixel SRV; all of `ShadowGpuData.*` | **mis-declaration** — the site passed the *creation* state |
| declared `RENDER_TARGET`/`UAV`/`DEPTH_WRITE`, rests shader-readable | every `Deferred[N].GB*`, `.Light`, `.Scene`, `.Depth`, `.Reflection*` | genuine end-of-frame drift: the frame ends with them consumed |

**So D2's premise — "canonical = what `SetResourceState` already passes, i.e. the creation
state" — is refuted by measurement.** Under D2 as written the epilogue would have to emit ~85
transitions every frame, which is precisely the cost Step 8 warns "could eat the win" (the whole
prize is 0.102 ms).

**But the invariant itself is nearly free if canonical is redefined as the frame-END resting
state: of the 97 resources that ever drift, 95 have a single stable end state.** Only two vary —
`VSM.PageRequest` and `VSM.AllocCounters`, `UNORDERED_ACCESS` on 14 frames of 15 and `COPY_SOURCE`
on the one frame the conditional debug readback runs. Declare the measured end state and the
steady-state epilogue is ~2 transitions, both behind a debug flag.

**Open question that redefinition creates, and Step 7 must answer:** `SetResourceState` currently
does double duty — declare canonical *and* seed the tracker with the true creation state. Split
them and frame 1 after a resource is created still needs a creation→canonical transition, because
the compile would seed from a state the resource is not yet in. Options: create resources directly
in their canonical state (`InitialResourceState` allows it for most), or emit a one-shot
transition at declaration. **Do not start Step 7 before choosing.**

**Lifetime finding — the check crashed, and that is a Step 7 blocker.** First run died with
`0xC0000005` on `ReloadLevel`, dereferencing a released resource inside the reporter. Not every
release path unregisters, so the table accumulates dangling keys; the tracker's own map has had
the same stale entries all along and never showed it, because it only ever uses the pointer as a
hash key. Two consequences:
- the reporter now captures the debug name at **declaration** time and never dereferences a key
  (`GetGlobalKnownState` is a hash lookup, which is safe);
- measured leak: the table ratchets **143 → 169 entries over 20 churn iterations** (with dips, so
  it is a leak, not level-to-level variation). A recycled `ID3D12Resource*` inheriting a stale
  canonical would seed a wrong before-state — silently. Either every release path gets an
  unregister, or D2's wrapper option (automatic lifetime) wins after all.

**Acceptance:** both `0/0` ✔; Debug `--scene-stress-gbv=20 --barrier-cmp --canonical-check`
CLEAN after 20 iterations, 0 `MISSING`, GBV ids only {939, 940, 1006, 1358} ✔; the off-canonical
log is **not** empty — but every entry is now classified and understood ✔.

### Step 6b — act on the two decisions — **PART 1 DONE (uncommitted): 85 → 0 on a steady frame**

Both forks were decided on Step 6's measurements; see the boxed note in D2.

**The split is done.** `SetResourceState` gained a three-argument form
`(res, creationState, canonicalState)`: the first still seeds the tracker (where the resource
actually *is*, so first-barrier before-states are unchanged), the second records the resting state.
`ResourceDeclarations` gained `SetCanonical` (canonical only, for owners whose shared creation
helper serves targets that rest in different states) and `RefreshName`.

**Deliberately NOT done here: aligning the creation state with canonical.** Changing
`InitialResourceState` alters real frame-1 GPU behaviour, and nothing before Step 7 reads the
canonical table for barrier generation — so it buys nothing yet and risks something now. See the
frames-in-flight finding below, which is the argument for doing it *at* Step 7.

**Converted so far, from the measured log (never guessed):**
- **Deferred targets** — the resting state now sits next to the debug name in one reviewable list
  in `RenderTargetManager::Create`, because the four generic creators serve targets that rest in
  five different states. That block also had to call `RefreshName`: it names the whole set *after*
  the creators declare, so every deferred target had been logging as a raw pointer.
- **All 16 VSM buffers** — every one was declared `COMMON` (the creation state) while resting in
  UAV / non-pixel-SRV / `INDIRECT_ARGUMENT` / `VERTEX_AND_CONSTANT_BUFFER`.

- **All 5 `ShadowGpuData` UAV rings** — `EnsureUavRing` gained a `canonical` parameter, because one
  helper creates buffers that rest in three different states (non-pixel SRV, `VERTEX_AND_CONSTANT_BUFFER`,
  UAV).
- **Ocean** (`Displacement`, `PrevDisplacement`, `FoamTurbulence`, `ShoreDepth`) and the
  **GPU-instanced instance buffer** — all created for writing, all resting shader-readable.
- **The three shadow atlases** — declared `DEPTH_WRITE`, but the light passes *sample* them, so the
  frame leaves them shader-readable (point shadows from both compute and pixel stages).

**Naming was a prerequisite, not a nicety.** `OceanSimulation` and `GpuInstancedModels` set no
debug names at all, and three owners (`RenderTargetManager`, `ShadowGpuData::EnsureRing` /
`EnsureUavRing`) named their resources *one line after* declaring them — so with the declaration-time
name capture every one of them logged as a raw pointer and could not be traced to a declaration
site. Names now precede declarations everywhere, and the shadow atlases are named inside
`Create*ShadowResource` as well, because `SetLocalShadowResidency` recreates them *after* `Create`'s
naming block has run.

**Result: 85 → 0 off-canonical on a steady frame** (`frame end: 0 of 140`). Everything still
reported comes from non-steady frames:

| Still reported | Reading |
|---|---|
| `VSM.PageRequest`, `VSM.AllocCounters` → `COPY_SOURCE` | the two genuinely-varying resources predicted at Step 6, only on the frame the debug readback runs |
| `Deferred[1].*`, `Deferred[2].*` at their **creation** state | untouched frames-in-flight — **see the finding below** |
| the shadow atlases at `DEPTH_WRITE` | the frame right after `SetLocalShadowResidency` recreates them, before anything samples them |

**Finding: per-frame-in-flight resource sets break the invariant as stated.** There are three
`Deferred[N]` sets and a frame touches exactly one, so the other two end the frame wherever they
were left. Once a set has been rendered into once it rests at canonical and stays there — so the
only mismatch is **the first frame after creation**, which is precisely the seeding question. The
report shows it as drift in the *opposite* direction (`canonical=0x40 actual=0x4`, i.e. still
RENDER_TARGET). This makes the answer concrete: **create resources directly in their canonical
state at Step 7**, and an untouched set is trivially at canonical too. A one-shot transition at
declaration would not fix it as cleanly, because the set is untouched for whole frames at a time.

### Step 6b Part 2 — the leak, measured before it is fixed

Rather than convert ~60 declaration sites blind, the registry was made to **name the leak**: every
resource has a unique debug name, so two live entries sharing one name means an earlier resource
was never unregistered. Two diagnostics, both on `--canonical-check`:
- duplicated-name lines (`[canonical] LEAK <n> live entries named <name>`), and
- a `<n> named + <n> UNNAMED entries declared` breakdown, because an unnamed entry falls back to
  its address, is unique by construction, and can therefore never show up as a duplicate — the
  first measurement came back empty for exactly that reason.

Both are triggered by the **table changing size**, not by a frame tick: the stress harness runs only
a handful of frames per churn op, so a 240-frame throttle never fired. The per-frame summary line
also had to become print-on-change — at one line per frame it floods DBWIN's single shared buffer
and the listener drops the lines that matter. (Two consecutive empty measurements were caused by
these two mistakes, not by the absence of a leak.)

**Result over 12 churn iterations:**

| | start | end | reading |
|---|---|---|---|
| named entries | 91 | 91 | **flat — every named owner unregisters correctly, except those below** |
| unnamed entries | 48 | 62 | **the bulk of the ratchet**; these have no debug name, so they cannot yet be attributed to an owner |
| duplicated names | — | `GpuInstanced.Instances` ×5, `Ocean.Displacement` / `PrevDisplacement` / `FoamTurbulence` / `ShoreDepth` ×2 each | named leakers |

Note `OceanSimulation::RetireGpuResources` *does* call `ClearResourceState` — but only for five of
its resources, and `shoreDepth_` is not one of them. That is the whole argument for the wrapper in
one example: the unregister list is hand-maintained and silently incomplete.

**The wrapper exists: `GpuResource` in `ResourceDeclarations.h`.** It owns the resource *and* its
registration — names, declares on `Attach`, forgets in the destructor. It holds a
`ResourceDeclarations` (two pointers), **not** a `Renderer*`, because `Renderer.h` includes this
header and the reverse dependency would be circular. Move-only. Shaped so the ~60 existing sites
barely change: create into a local `ComPtr` exactly as today, then `Attach`; everything else keeps
calling `Get()`.

**First owner converted, and it was picked for a reason: `OceanSimulation::shoreDepth_`** — the one
ocean resource `RetireGpuResources`' hand-written clear list forgets, which is why it leaked.
Verified: its `LEAK` lines are gone, `GpuInstanced.Instances` and `Ocean.PrevDisplacement` still
leak (not yet converted), and the steady-frame drift is still `0 of 140`. That is the wrapper's
shape proven end-to-end on a case the hand-written path provably got wrong.

**Second owner converted: `InstanceBuffer`.** Naming *and* declaring moved out of
`GpuInstancedModels::Init` and into `InstanceBuffer::Create` (which gained a `ResourceDeclarations`
parameter) — the declaration used to live in the caller while the resource died in the callee,
which is exactly why it was the worst leaker. `GpuInstanced.Instances` went ×5 → ×3.

**Caveat found while reading that number: the duplicate-name diagnostic assumes unique names, and
`InstanceBuffer` names every buffer identically.** The scene has two GPU-instanced objects, so ×2 is
legitimate and only ×3+ is a leak. Either give each instance buffer a distinct name or subtract the
expected count before trusting the line.

**Remaining:**
1. The ocean's retire-managed trio (`displacement_`, `prevDisplacement_`, `foamTurbulence_`). A
   first attempt was reverted rather than left half-applied: the header members had been switched to
   `GpuResource` but the matching `.cpp` edits silently no-ops'd — **the file is CRLF and the
   multi-line search patterns used `\n`**. Single-line edits in these files work; multi-line ones
   must use `\r\n` or the Edit tool.
2. Then the rest of the ~60 sites.

**A naming pass ran, and it immediately paid for itself.** Four owners were declaring *before*
naming, so the registry captured a pointer and their growth was unattributable:
`LightManager` (both light buffers), `ParticleEmitterObject` (all four sim buffers),
`ShadowGpuData::EnsureMegaBuffer`, and `TextureCube` — which set no debug name at all. Names now
precede declarations in every one. The particle buffers also got their measured resting states
while the block was open (`particles_`/`sorted_` rest shader-readable; the dead list and counter
never leave UAV).

**Result: two previously invisible leakers now have names — `ShadowGpuData.MegaVB` and
`MegaIB`, ×4 each.** That is exactly what the pass was for: growth that was a featureless
pointer count is now an owner and a line of code.

**Naming is now complete, and the unnamed ratchet is gone.** Two more gaps closed: the mesh
vertex/index buffers (`GpuInstancedModels::Init` declared them straight off
`mesh_->Get*BufferResource()` and never named them), and `Texture2D`, which called *every* texture
`Tex2D_RESOURCE` — it now carries a `debugName_` of `L"Tex2D:" + desc.path`, set at both public
entry points.

| | before | after |
|---|---|---|
| unnamed entries over 12 churn iterations | 45 → 61 (growing) | **flat 20** |
| named entries | ~90 | 112 → 111 (flat) |

### Step 6b Part 2 — result: the ratchet is closed

**The duplicate-name check had to be replaced before any of this could be judged.** Counting bare
duplicates cannot tell a leak from legitimate sharing — two GPU-instanced clouds, or one texture
used by four cached materials, sit at a steady count forever, and I called that a leak twice. The
registry now keeps a per-name **net** (declares minus forgets; `Declare` over an existing key
decrements the old name first) and the report prints only names whose net grows **past every value
it has held before**. No liveness knowledge is required, which is the point: a correct owner's net
returns to baseline after identical churn cycles, a leaking one's climbs. Pointer-"named" entries
are excluded — their identity is an address the allocator reuses, so a per-name count is
meaningless for them.

**Converted to `GpuResource`:** `OceanSimulation::shoreDepth_`, `InstanceBuffer`'s buffer,
`ShadowGpuData::megaVB_`/`megaIB_`, and the ocean's retire-managed trio (`displacement_`,
`prevDisplacement_`, `foamTurbulence_` — the retired copies hold wrappers too, so registration now
dies with the resource instead of at retire time, and the hand-written clear list that forgot
`shoreDepth_` is gone). `Texture2D` got a destructor-based unregister plus a forget-before-overwrite
at its three creation sites (creating over an existing `tex_` released the old resource while
leaving its entry behind).

**Measured over 12 churn iterations:**
- declared-resource count **140 → 135** — it no longer ratchets; that is the acceptance criterion.
- `ShadowGpuData.MegaVB`/`MegaIB`, which climbed 2 → 3 → 4 → 5, are gone from the report.
- every name still listed produces **exactly one** growth event and then stops:
  `GpuInstanced.Instances` → 2 (two GI objects), `Tex2D:textures/damaged_plaster_normal.dds` → 4
  (four cached materials share that normal map). Under the corrected criterion those are shared
  live copies, **not** leaks — `MaterialDataManager::cache_` deliberately outlives a level reload.

So the texture "leak" was a misreading of the old diagnostic, not a defect. The remaining
declaration sites can be moved onto `GpuResource` for tidiness, but the dangling-key class that
crashed the reporter and blocked Step 7 is closed.

**Acceptance:**
- ✔ `--canonical-check` reports **0 off-canonical** on a steady frame.
- ✔ both `0/0`; Debug `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN, 0 `MISSING`.
- ✘ the declared-resource count still ratchets instead of returning to baseline — that is Part 2.

One caveat to carry into Step 7: a few resting states are **configuration-dependent**, and the
values here were measured with the shipping defaults. `VSM.PageProj` rests in non-pixel-SRV only
while `g_pageDrawSingle` is on; `VSM.PageArgCount` would rest in `INDIRECT_ARGUMENT` if
`g_pageDrawCompact` were re-enabled; `ShadowGpuData.IndirectArgs` was measured under VSM. Re-run
`--canonical-check` after flipping any of those flags rather than trusting the constants.

### Step 7 — the flip: delete the tracker

**Three prerequisites from Step 6, all deliberate deferrals — settle them before writing code:**
1. **Align creation state with canonical.** The declaration is split today (`SetResourceState(res,
   creation, canonical)`): the tracker is still seeded with where the resource actually *is*, and
   nothing reads the canonical table for barrier generation yet. The moment the compile seeds from
   canonical, the first frame after a resource is created is wrong — the frames-in-flight finding
   makes this concrete (`Deferred[1]`/`[2]` sit at their creation state for whole frames). Answer:
   create resources directly in their canonical state (`InitialResourceState`).
2. **Config-dependent resting states — SWEPT, and it found more than predicted.** Three boot flags
   were added for this (`--vsm-page-multidraw`, `--vsm-page-compact`, plus the existing
   `--shadow-mode=legacy`), each parsed above the scene-stress branch, and `--canonical-check` re-run
   against each:

   | Config | Resource | default canonical | actual under that config |
   |---|---|---|---|
   | `--vsm-page-multidraw` | `VSM.PageProj` | non-pixel SRV | `VERTEX_AND_CONSTANT_BUFFER` |
   | `--vsm-page-multidraw` | `VSM.PhysOwner` | non-pixel SRV | `COPY_SOURCE` — **was not predicted** |
   | `--vsm-page-compact` | `VSM.PageArgCount` | UAV | `INDIRECT_ARGUMENT` |
   | `--shadow-mode=legacy` | `ShadowGpuData.IndirectArgs` | non-pixel SRV | `INDIRECT_ARGUMENT` |

   Four, not three — guessing which constants were config-dependent would have missed `PhysOwner`.

   **The harder half of this finding: `g_pageDrawSingle` and `g_pageDrawCompact` are toggleable at
   RUNTIME from the dev window.** A static canonical table is simply wrong the moment one flips
   mid-session. Step 7 has to pick one:
   - re-declare the affected resources when the flag changes (they already flip at GPU idle), or
   - declare a state valid for both paths and accept one redundant transition, or
   - demote those flags to boot-only.

   Whichever way, this is a *correctness* prerequisite, not a tuning detail: the compile seeds from
   this table.
3. **The two genuinely-varying resources — DONE.** `RecordDebugReadback` used to *park*
   `requestBuffer_`, `allocCounters_` and `physOwner_` in `COPY_SOURCE` and never restore them;
   that is why they were the only resources whose end-of-frame state changed frame to frame, which
   is exactly what a static canonical table cannot express. They now transition back to canonical
   at the end of the readback (D2's frame epilogue, in the one place that needed it), and the
   matching registration was extended with a second barrier point. Verified: no `COPY_SOURCE`
   drift left, 0 `MISSING`, steady-frame drift `0 of 140`. This also retires the "re-assert UAV at
   the top" workaround the parking had forced on `RecordPageRequest`.

Compiled barriers become authoritative.

**RESULT — the flip is CLEAN in both shadow modes (uncommitted, still default-OFF behind
`--barrier-flip`).** `--barrier-flip --barrier-cmp --canonical-check --scene-stress-gbv=20`, VSM
and `--shadow-mode=legacy`: `verdict: CLEAN after 20 iterations`, **0** non-noise GBV messages
(only the pre-existing {939, 940, 1006, 1358}), **0** comparator findings in either direction. The
tracker path re-measured under the same command with the flip off: also 0/0 — the 86 `INFO extra`
occurrences it used to report are gone, because every one of them was a real over-registration.

Getting there took five causes, and none of the last four were guessable — each was found by
instrumenting (`--barrier-flip-trace`, which now also carries the comparator into
`barrier_diag.log`, because the stress harness runs with no debugger and DBWIN output is lost).

1. **A request may only match the CURRENT un-emitted point.** Matching (resource, state) across
   all points is ambiguous — `Pass_Tonemap` moves `tonemap` UAV → COPY_SOURCE → UAV — and its
   first request matched the RESTORE point, dragging the whole pass's barriers to the top of the
   list. "Emit everything up to the match" was worse (it pulled other resources' points forward,
   D3D12 error 538).
2. **Every registration must be one the body will actually perform.** Under the tracker an
   over-registration was a benign `INFO extra`; under the flip a compiled barrier nobody asks for
   **stalls the rest of the pass** (rule 1) and leaves the model one transition ahead of the GPU.
   Two instances: `ShadowGpuData::PrepareCullPass` registered the GI scatter's COPY_DEST → UAV
   pair on `useUnified` alone when the body nests it inside `giOn`; and `PrepareOpaqueDrawStates`
   walked *every opaque object* on the old reasoning that "a culled object's redundant barrier is
   free" — true under the tracker, exactly backwards here. It now walks the pass's own views'
   VISIBLE buckets with the bodies' own GPU-driven-shadow gate. **Trap:** the spot/point view
   arrays are fixed-size and their tail entries hold queues whose objects a level switch has
   freed — reading past the active count crashed the harness inside the first `SwitchLevel`.
3. **Fan-out passes: the barrier must be recorded into the list submitted FIRST, not by whichever
   thread records first.** `Pass_SpotShadows`/`Pass_PointShadows` had every per-light list
   transition the atlas; the compiled point fired in whichever list won the race, which could be
   submitted after another list had already cleared the atlas (~500 errors, 527 and 538). Now only
   `lightIndex == 0` / `faceIndex == 0` transitions — `localOrder` makes that list first. This is
   what the tracker's submit-time stitching had been hiding.
4. **The compile and `Transition` must agree on WHICH resources are modelled.** The compile skipped
   only `IsResourceUnmanaged`; the flip gates on `IsCompileManaged` (declared **and** not
   unmanaged). An undeclared resource therefore got a compiled barrier that was never emitted.
5. **The compile seeds from the PREVIOUS COMPILE'S ENDING STATE, not from canonical.** This
   supersedes the design note above. Canonical seeding silently assumed *every frame ends with
   every resource at canonical* — false the moment the graph changes shape: `Pass_Tonemap` returns
   the depth and velocity buffers to their resting state as part of the DLSS branch, so a DLSS-off
   frame leaves them at DEPTH_WRITE / RENDER_TARGET and the next frame's GBuffer barrier claimed
   `NON_PIXEL_SHADER_RESOURCE`. The stress harness changes render resolution in three of its five
   churn ops, so this fired on essentially every one. Measured with the flip on:
   `[canonical] frame end: 7 of 122 declared resources off-canonical` — the invariant was simply
   not true. Fix: `CanonicalStateRegistry::Entry::predicted`, seeded from canonical at `Declare`
   (so a recycled address never inherits the previous owner's state), read by `CompileBarriers`
   and written back at the end of every compile. **This is not the tracker returning:** one value
   per resource, written once per frame by the single-threaded compile — no per-command-list
   lanes, no first-use bookkeeping, no TLS, no submit-time stitching. It also carries the main
   graph's ending state into the epilogue graph's compile, which canonical seeding could not do.
   Consequence to keep in mind: a missed emission is now *sticky* rather than self-correcting at
   the next frame's reseed — the comparator (silence in both directions) is the guard, so keep it
   in the acceptance run.

`--canonical-check` under the flip now reads `predicted` instead of the tracker's global map,
which the flip no longer updates — it was reporting a dead map that looked like a result.

**THE TRACKER IS DELETED (uncommitted).** `ResourceStateTracker.{h,cpp}` are gone, along with
their entries in `test_cube.vcxproj` **and** `.filters`. Net **−777/+110** lines.

What went, and what replaced it:

| deleted | replaced by |
|---|---|
| the per-list acquire-prologue loop in `ExecuteTimelineAndPresent` (Service2) | nothing — the loop is now a straight `push_back` of each work list |
| `firstUse` / `current` per command list, per-thread lanes, the TLS quartet, `knownStatesMtx_`, `ResetLanesForFrame`, `AppendAcquireBarriers`, `FindCLStateForCmd`, `ApplyFinalStates`, `SetKnownStateDirect` | `CanonicalStateRegistry::Entry::predicted` — ONE value per resource, written once per frame by the single-threaded compile |
| `Renderer::SetTrackedStateOnly`, `barrierScratch_`, `stateTracker_` | — |
| `ResourceDeclarations::tracker` | — (`creationState` kept: the upload-initialised sites legitimately state both) |
| `render::g_barrierFlip` / `--barrier-flip` | — the compiled barriers are the only barrier path, so there is nothing to flip |
| the four `ScenarioTracker*` cases in `RendererSubmissionStress` | — (the SubmitTimeline half is untouched and still passes, incl. the death tests) |

**The measurement that made deletion possible.** Before deleting, `[tracker-fallthrough]` and
`[acquire-prologue]` were added under `--barrier-flip-trace` and run in Release: the tracker's
entire remaining job was **the swapchain backbuffer, 2 transitions per frame**, delivered by
**one barrier-only command list per frame carrying exactly one barrier**. Converting
`Pass_Tonemap`'s backbuffer resolve to `TransitionExplicit` — legitimate, because
`RecordBindAndClear` and the present epilogue already bracket the backbuffer with hand-rolled
barriers, so its cycle PRESENT→RENDER_TARGET→(COPY_DEST→RENDER_TARGET)→PRESENT is fully
determined — emptied that set. Its registration was removed from Tonemap's Prepare in the same
edit, or the comparator would have reported it as an (now fatal) over-registration.

**`Renderer::Transition` no longer has a fallback: it is an invariant failure.** Reaching the end
of it means a resource the compile does not model, or a call from outside a pass body — which
under the old code fell through to the tracker and would now silently drop the barrier. Measured
empty first; loud rather than silent if that ever changes. Out-of-graph code uses
`TransitionExplicit` and supplies its own before-state.

**Acceptance met:** both configs `0/0`; `--barrier-cmp --canonical-check --scene-stress-gbv=20`
in BOTH shadow modes = `CLEAN after 20 iterations`, **0** non-noise GBV, **0** comparator
findings; `--renderer-submission-stress` = `failures: 0` with every death test still aborting.

Still open from the original list: `Renderer::MarkImGuiTextureShaderReadable` survives, but
REPURPOSED — it declares the editor texture's canonical state rather than patching a tracker map,
so it is no longer dead weight.

**Acceptance (this is Goal 1's payoff — be thorough):**
- Both `0/0`; GBV CLEAN; `--scene-stress` CLEAN across the full churn.
- `fixedSubmitScratch_` contains only the GPU-profiler begin list, the pass work lists, and the
  present epilogue — assert it in Debug.
- `firstUse`, `AppendAcquireBarriers`, `FindCLStateForCmd`, the per-thread lanes, the TLS
  quartet, `knownStatesMtx_` and `ResetLanesForFrame` no longer exist.
- Imperative `Transition` sites: ~19 (D+E only).
- Visual parity: `--shot` comparison at the same camera with `--wind-freeze`.

### Step 8 — measure — **DONE**

Interleaved A/B/A/B/A/B, Release, 45 s warmup each, ~23k frames per run (frame counts within 2 %
across all six, so no downclock artefact). "Before" = `2594b7d` built in a `git worktree`;
"after" = `0cce0c6`. Means of three paired runs, ms:

| CPU scope | before | after | delta |
|---|---|---|---|
| `Renderer::ExecuteTimelineAndPresent` | 0.248 | 0.151 | **−0.098 (−39 %)** |
| `Service2` | 0.093 | 0.007 | **−0.086 (13×)** |
| `Scene::Render` | 0.560 | 0.465 | −0.095 |
| `Renderer::BeginFrame` | 1.357 | 1.442 | **+0.085** |
| `Renderer::WaitForFrame` | 1.328 | 1.421 | **+0.093** |
| `CPU.Frame` | 1.962 | 1.950 | −0.012 |
| `GPU.Frame` | 1.941 | 1.932 | −0.009 |

**The target was hit almost exactly** — R6 predicted 0.102 ms for the acquire prologues and the
measurement says 0.098 ms — **and the frame did not get faster.** Every millisecond removed from
submission reappeared in `WaitForFrame`/`BeginFrame`: this scene is not CPU-submission-bound at
that point, so the saving becomes *idle*, not *throughput*. `CPU.Frame` moved 0.012 ms, which is
inside the run-to-run spread.

That is the plan's own prediction, now measured rather than asserted (see R6: "Goal 1 is a
capacity investment, not an FPS change"). What was actually bought:

- **0.098 ms/frame of CPU headroom** on the submit path, available to any future work that *is*
  submission-bound (GPU-driven passes, more lights, bigger scenes).
- **One fewer command list allocated, recorded, closed and submitted per frame**, and with it the
  whole per-thread lane / TLS / first-use bookkeeping that ran during recording.
- The `returnBarrierEstimate_` worry is settled: the epilogue's return-to-canonical cost never
  materialised, because `Entry::predicted` removed the need to return anything to canonical at
  all. `Service2` shrank to 0.007 ms — the present epilogue and nothing else, exactly as R6 said.

**Do not re-run this as an FPS test.** The honest headline is "the scope we targeted shrank 39 %
and the frame time is unchanged"; anyone measuring FPS here will conclude the work did nothing.

#### The two-phase prologue's own cost — measured, and it is NOT free

`RunPrepares` and `CompileBarriers` are now permanently instrumented
(`ProfilerScopes::kRenderGraphPrepares` / `kRenderGraphCompileBarriers`). Interleaved VSM/legacy,
Release, 45 s, ~23–24k frames, `usages:2` per frame (main graph + epilogue graph):

**`CompileBarriers` runs from the tail of `RunPrepares`, so the Prepares row is INCLUSIVE of the
CompileBarriers row.** Reading them as two costs and adding them turned a 0.012 ms prologue into a
reported 0.022 ms; the correction now sits on the scope declarations so it cannot happen twice.

| | Prepares (the whole prologue) | of which CompileBarriers | vs `CPU.Frame` |
|---|---|---|---|
| VSM | 0.012–0.013 | 0.010 | 0.6 % of 1.97 |
| legacy | 0.012 | 0.008–0.009 | 0.6 % of 1.86 |

Legacy is not more expensive despite `PrepareOpaqueDrawStates` now walking per-view visible
buckets for cascades + spots + point faces. **And this cost is NOT new:** `RunPrepares` already
called `CompileBarriers` unconditionally in the `2594b7d` baseline (verified in the committed
file), so the −0.098 ms above is already net of it.

**Both phases are SERIAL by construction, and deliberately so:**

- `RunPrepares` walks the schedule in order and hands out slices from ONE bump arena
  (`prepare_->arenaSize`), so it is not merely un-parallelised, it is not thread-safe as written.
- `CompileBarriers` is a sequential FOLD: `compileState_[res]` carries the running state from pass
  to pass, and that fold IS the derivation of every barrier's before-state.

**Could they be parallelised?** Yes, both, but differently:

- *Prepare* — per pass. Each pass registers only its own uses and the slices are already per-pass
  (`prepare_->slices[passIdx]`), so the arena would need per-pass regions — the same layout
  `barrierPoints[passIdx][point]` already uses. The real blocker is that Prepare callbacks WRITE
  subsystem state (`VirtualShadowMap::PrepareRenderPass` assigns `pageRenderDecisions_`), which is
  safe today only because it is serial.
- *Compile* — NOT per pass (the fold is inherently ordered), but per RESOURCE: each resource's
  chain of uses is independent of every other's. Build per-resource chains, resolve them in
  parallel, scatter the results back into the per-(pass, point) slices.

**Neither was worth doing, and the cheaper lever was taken instead.** Threading 0.012 ms would buy
idle, not frames (Step 8). The compile re-walked every use every frame for a graph shape that
rarely changes, so it is now **cached across frames** — see below. Revisit threading only if pass
count or per-pass use count grows several-fold; the scopes are permanent, so it is one
`--profdump` away.

### Step 8b — the cross-frame compile cache

`CompileBarriers` keeps its output and reuses it whenever every input is unchanged.

**The three inputs, and the guard on each:**

| input | guard |
|---|---|
| compile order + registered uses | an EXACT COPY compared byte for byte (`CompileInputsUnchanged`). Not a hash: a collision here serves barriers with a wrong before-state, which is silent corruption, and `arenaSize` is a few hundred 16-byte entries so comparing is cheaper than hashing anyway |
| which resources are declared / compile-managed | `CanonicalStateRegistry::Generation()`, bumped by Declare/Forget/ForgetMany/SetUnmanaged/Clear. This input cannot be seen in the use list at all |
| the incoming `predicted` of every resource first touched | the SAME generation — `SetPredicted` bumps it only when a value actually moves |
| (belt and braces) | a compile is stored only if it was a **fixed point**: every resource it touched ended where it began. Measured `not-fixed-point = 0`, so it never blocks; kept because a wrongly-refused cache costs 0.01 ms and a wrongly-accepted one corrupts |

**ONE SLOT PER FRAME IN FLIGHT — this is the whole difference between a cache and a decoration.**
The first version was single-slot and measured **0 hits out of 15** on the main graph. Cause:
almost every pass registers `GetDeferredForFrame()` targets and there are `kFrameCount` rotating
Deferred sets, so consecutive frames register different resource POINTERS. Frame N matches frame
N−kFrameCount, not N−1. Keyed per frame index, steady state gives:

```
main graph:     hits=6729  misses=439   not-fixed-point=0     (93.9 %)
epilogue graph: hits=3844  misses=252   not-fixed-point=0     (93.8 %)
```

That also means **hit rate can only be read in steady state.** A 15-frame `--scene-stress` reads
1/14, because level reload, resize and DLSS-mode churn each recreate resources and bump the
generation — correctly. Use `--profdump`, not the stress harness, and note `--barrier-compile-log`
is throttled to one line per 512 compiles for exactly this reason.

**Measured (Release, 45 s, ~23k frames, paired VSM/legacy):**

| | prologue before | prologue after | of which compile |
|---|---|---|---|
| VSM | 0.012–0.013 | **0.005** | 0.010 → **0.002** |
| legacy | 0.012 | **0.005** | 0.008–0.009 → **0.001** |

`CPU.Frame` 1.964/1.970 → 1.959/1.990: unchanged, as Step 8 predicts. The compile itself is 5–8×
cheaper; the prologue as a whole is 2.4× cheaper.

**Correctness gate — `--barrier-cache-verify`.** It recompiles every frame *even on a hit* and
diffs the fresh output against what the cache would have served, byte for byte, logging
`[barrier-cache] STALE` on any difference. Run over `--scene-stress-gbv=20` in BOTH shadow modes:
`CLEAN after 20 iterations`, **0** STALE lines, **0** comparator findings, **0** non-noise GBV.
Any future change to what the compile reads must re-run this — a cache on the barrier path is the
one place where "it looked fine" is not evidence.

### Step 9 — SDK / interface readiness + feature detection

- Confirm the toolchain sees `D3D12_BARRIER_GROUP` / `ID3D12GraphicsCommandList7` /
  `D3D12_FEATURE_D3D12_OPTIONS12`. **If not, adopt the Agility SDK** as its own sub-step
  (exports, ship `D3D12Core.dll`, re-verify both builds and a normal run) — the bump can itself
  change driver behavior.
- `GraphicsDevice`: QueryInterface `ID3D12Device10 device10_` (nullable, mirroring `device5_`);
  query `OPTIONS12.EnhancedBarriersSupported`.
- `Renderer`: add `AsCmdList7` mirroring `AsCmdList4`.
- Add `enhancedBarriers_` plus a force-off override (`--legacy-barriers`) so the legacy path
  stays testable and bisectable. Report it but treat it as OFF everywhere.

**Acceptance:** both `0/0`; identical behavior; the detected capability is logged.

### Step 10 — translation layer + resource-kind classification — **DONE (uncommitted)**

`sources/rendering/core/BarrierTranslation.{h,cpp}`: `LegacyStateToBarrier(state, isBuffer)` →
`{sync, access, layout}`, plus `IsTextureCompatible`. Buffer/texture classification is an
`isBuffer` bit on `CanonicalStateRegistry::Entry`, read from `GetDesc().Dimension` **at Declare
time** — the same reasoning as the debug name: that is the one moment the pointer is guaranteed
live, and the table can hold dangling keys.

**D4's table needed extending in the one place that matters: COMBINED states.** It is written one
row per legacy state, but this engine declares combinations deliberately — `0xC0`
(NON_PIXEL|PIXEL) for a texture read from both stages, `0x840` (NON_PIXEL|COPY_SOURCE) for
`VSM.PhysOwner`, `0x41` (NON_PIXEL|VERTEX_AND_CONSTANT_BUFFER) for `VSM.PageProj`. Sync and access
simply OR. **Layout cannot** — a texture has exactly one — so:

- one distinct layout (0xC0: both shader-resource bits share `LAYOUT_SHADER_RESOURCE`) → that one;
- several, all read-only (0x840) → `LAYOUT_GENERIC_READ`, which is what it exists for;
- several including a writable one → `LAYOUT_COMMON`: legal everywhere and slow enough to notice,
  because that combination is a mis-declaration rather than something to guess at.

An unknown bit widens to `SYNC_ALL` / `LAYOUT_COMMON` rather than being dropped — an
under-specified barrier is a race, a too-wide one is only slow.

**Gate.** A table nothing calls is a table nobody notices is wrong, so it is tested rather than
merely compiled: `ScenarioBarrierTranslation` in `--renderer-submission-stress` asserts D4's
single-state rows, the COMMON/PRESENT zero case, both combination rules, the buffer-only states
and the unknown-bit fallback. `failures: 0`.

**Acceptance met:** both `0/0`; nothing calls the translation yet; behaviour unchanged.

### Step 11 — create textures with an initial layout — **DONE (uncommitted)**

`sources/rendering/core/TextureCreate.{h,cpp}`: `render::CreateCommittedTexture(...)`, one place
that creates a texture. With the enhanced path off it is byte-for-byte the old
`CreateCommittedResource`; with it on, `CreateCommittedResource3` takes the `D3D12_BARRIER_LAYOUT`
that `barriers::LegacyStateToBarrier` derives from the resource's resting state.

Shaped to take a raw `ID3D12Device*` so each of the 14 converted sites changes by one identifier
instead of threading a new parameter through six `RenderTargetManager` signatures. The resolved
decision is published once by `GraphicsDevice::InitDevice`, in the same process-global style that
file already uses for DRED/GBV/`--legacy-barriers`.

Converted: `RenderTargetManager` x8 (every deferred target, the CSM/spot/point atlases),
`Texture2D` x3, `OceanSimulation` x4, `VirtualShadowMap` pool, `ReflectionHistory`. Swapchain
buffers come from the swapchain itself, not `CreateCommittedResource`.

**A separate opt-in was required, and the plan is amended here.** Step 9 says the capability must
be "treated as OFF everywhere", but detection reports `supported=yes` on this machine — so gating
creation on `AreEnhancedBarriersSupported()` alone would have changed behaviour by default at a
step whose whole point is that it must not. `GraphicsDevice::UseEnhancedBarriers()` =
supported AND opted in (`--enhanced-barriers`); the capability accessor stays a pure report.
Step 15 is where the default moves.

**Acceptance met, and the flag-on result is the honest one:**
- flag off — both builds `0/0`; `--barrier-cmp --canonical-check --scene-stress-gbv=12` =
  `CLEAN after 12 iterations`, 0 non-noise GBV. Unchanged.
- flag on — **every texture creates**: `CreateCommittedResource3` returned success for all of
  them, zero entries in the refusal log the helper writes on failure.
- flag on, then RUNNING — fails, by design. The debug layer raises at `ExecuteCommandLists`
  (`UploadBatch::Submit`) because the recorded barriers are still LEGACY, and a texture created
  with an explicit layout cannot take those. That is exactly what step 12 builds. The step's bar
  is "textures still create", not "the engine runs".

**Trap worth keeping: a BUFFER must never go through this helper.** The mechanical conversion
caught `Texture2D`'s upload heap (`DIMENSION_BUFFER`) along with the texture beside it, and
`CreateCommittedResource3` then raised *inside the call* for a layout on a bufferC — which read as
"enhanced creation is broken" until the stack showed it was one wrong call site. Buffers have no
layout; the site is now explicitly commented.

### (original spec) Step 11 — create textures with an initial layout (gated)

Enhanced barriers track texture layout, which must be declared at creation
(`CreateCommittedResource3` / `CreatePlacedResource2`). Route texture creation through a helper
that uses the new API **when `enhancedBarriers_`** and the existing path byte-for-byte
otherwise. Cover `RenderTargetManager` (all deferred targets, shadow atlases, the point cube
atlas), swapchain buffers/depth, `Texture2D`, `TextureCube`, RT targets. Buffers unchanged.

**Acceptance:** both `0/0`; unchanged with the flag off; with the flag *temporarily* forced on,
textures still create — then revert the force.

### Step 12 — enhanced emission — **DONE (uncommitted), NOT YET EXERCISED**

`barriers::EmitEnhanced(cl7, entries, count, isBuffer, ctx)` turns an already-COMPILED legacy
transition array into `D3D12_TEXTURE_BARRIER` / `D3D12_BUFFER_BARRIER` groups and calls
`ID3D12GraphicsCommandList7::Barrier`. Hooked into the ONE place compiled barriers reach the GPU —
`Renderer::Transition`'s point emission — which is exactly the payoff the plan predicted: steps 1-7
collapsed emission to a single site, so the enhanced branch is a dozen lines rather than a second
compiler. The compile is untouched and still produces `{resource, before, after}`.

Design points worth keeping:
- **Textures and buffers go into SEPARATE groups** — D3D12 requires a group to be homogeneous.
- **A refusal is never a lost barrier.** EmitEnhanced returns false (unexpressible state, a
  non-transition, more entries than its fixed scratch) and the caller falls straight through to
  `ResourceBarrier`. A gap in the translation degrades to legacy instead of vanishing.
- A texture transition must name BOTH layouts; if either side resolves to `LAYOUT_UNDEFINED` the
  state was buffer-only and mis-declared for that resource, so it refuses rather than inventing one.
- `Subresources.IndexOrFirstMipLevel = 0xffffffff` is the enhanced spelling of
  `D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES`.

**Honest status: the enhanced path still cannot run a frame, so this code has compiled but not
executed.** With `--enhanced-barriers` the app dies in `App::InitScene` at `UploadBatch::Submit` —
asset upload records DIRECT legacy barriers (`Texture2D`'s COPY_DEST → shader-readable), and those
are step 13's scope, not this one. The failure is one step further along than at step 11 (creation,
then upload), which is the expected progression.

To make step 13 provable rather than assumed, the emission now counts itself: the
`--barrier-compile-log` line carries `emit enhanced=N legacy=M`. `legacy` counting up is a
translation gap made visible.

**Acceptance met (flag off):** both `0/0`; `--barrier-cmp --canonical-check --scene-stress-gbv=12`
= `CLEAN after 12 iterations`, 0 non-noise GBV; `--renderer-submission-stress` `failures: 0`.

### (original spec) Step 12 — enhanced emission (gated)

Branch the barrier emission on `enhancedBarriers_`: build `D3D12_TEXTURE_BARRIER` /
`D3D12_BUFFER_BARRIER` from the translation, wrap in a `D3D12_BARRIER_GROUP`, call
`AsCmdList7(cl)->Barrier(...)`; else the current `ResourceBarrier`.

By this point Steps 1-7 have collapsed emission to **one place** — the compiled per-slot arrays
— so this is a much smaller surface than it would have been before Goal 1.

**Acceptance:** both `0/0`; `--scene-stress` CLEAN; behavior unchanged (flag off).

### Step 13 — present, UAV, and direct sites — **DONE (uncommitted)**

`barriers::EmitOne(cl, barrier)` takes an already-built `D3D12_RESOURCE_BARRIER` and re-expresses
it, enhanced when enabled and legacy otherwise. That shape was chosen because every direct site in
the engine already builds one and calls `ResourceBarrier(1, &b)` — so each site converts by
changing exactly one line, and buffer-vs-texture is derived from the resource itself (these are
uploads, present and screenshots: rare enough that one `GetDesc` costs nothing and spares every
caller from knowing).

Converted, 14 sites: `Texture2D` x3, `TextureCube`, `UploadManager` x2, `DlssHandler`,
`Screenshot`, `EditorPreviewRenderer` x2, `AssetThumbnailCache`, and `Renderer` x4 (present
epilogue, `RecordBindAndClear`, `TransitionExplicit`, `UAVBarrier`). Acceleration structures are
step 14. A blanket UAV barrier (null resource) becomes a `D3D12_GLOBAL_BARRIER`; a per-resource one
becomes a texture/buffer barrier with UAV access and layout on both sides.

**`--enhanced-barriers` now RUNS: `verdict: CLEAN after 1 iterations`.** Steps 11-13 are therefore
exercised, not merely compiled.

**And the residual is now a named, understood problem instead of a mystery — D3D12 error 1350:**
> `Deferred[0].GBVelocity` … last transitioned by **enhanced** barrier to
> `LAYOUT_SHADER_RESOURCE` **must be in common layout before switching to legacy barriers**

**Mixing the two models on one resource is illegal**, and the only remaining legacy emitter is the
compiled path's own fallback: when `EmitEnhanced` refuses a point it drops to `ResourceBarrier`,
which is safe in isolation but fatal once that resource has already taken an enhanced barrier. The
fallback that makes each step individually safe is exactly what step 15 must remove — the enhanced
path has to become EXCLUSIVE, not preferred. That is a design conclusion this step earned, and it
belongs in step 15's scope rather than being patched here.

**A diagnostic change that paid for itself immediately.** `SetupDebugBreaks` broke on
`D3D12_MESSAGE_SEVERITY_ERROR` unconditionally, so under `--scene-stress-gbv` — whose entire
contract is "errors logged, not fatal, so the harness can drain the info queue" — the process died
*inside* the offending call and the message was never read. Three debugging rounds across steps
11-13 got the call site from the stack and never the reason. The break is now suppressed when
`--scene-stress-gbv` is set, and the first run with it printed error 1350 verbatim.

**Acceptance met (flag off):** both `0/0`; `--barrier-cmp --canonical-check --scene-stress-gbv=12`
= `CLEAN after 12 iterations`, 0 non-noise GBV.

### (original spec) Step 13 — present, UAV, and direct sites (gated)

- Present (`Renderer.cpp:812-819` / `:994-999` + the canonical seeds): a `D3D12_TEXTURE_BARRIER`
  to/from `LAYOUT_PRESENT` when enhanced (verify flip-model present layout).
- `Renderer::UAVBarrier`: a buffer/texture barrier with `ACCESS_UNORDERED_ACCESS` before+after,
  or a `D3D12_GLOBAL_BARRIER` for a blanket one.
- Upload sites (`Texture2D`, `TextureCube`, `UploadManager`): give their COPY_DEST↔SHADER_RESOURCE
  transitions a gated enhanced path.

**Acceptance:** both `0/0`; `--scene-stress` CLEAN; behavior unchanged (flag off).

### Step 14 — acceleration structures & special states (gated)

Map AS/scratch states (`SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE`, `ACCESS_RTAS_READ/WRITE`;
AS is a buffer → no layout). Audit `AccelerationStructure.cpp` for direct barriers and give them
a gated enhanced path, preserving "scratch = COMMON not UAV" and AS-bypasses-the-graph.

**Acceptance:** both `0/0`; `--scene-stress` CLEAN with RT reflections on (F5); no device removal.

### Step 15 — the enhanced flip

Set `enhancedBarriers_` to its real value: enhanced on capable machines, legacy on the rest.

**Acceptance (Goal 2's payoff):**
- Both `0/0`.
- `--scene-stress-gbv`: verdict CLEAN and **`id=1358` layout warnings GONE** (209-322 hits per
  20-30 iteration run today), with **no new** GBV ids (`1387`, `958`, or any barrier/layout id).
  Pre-existing `939/940/1006` glass noise may remain.
- `--scene-stress` CLEAN across the full churn, **and again with barriers force-disabled** —
  the fallback must not be dead code.
- Visual parity: G-buffer, CSM + spot/point shadows, transparent/glass, SSR + RT reflections,
  tonemap. Compare `--shot` captures legacy vs enhanced at the same camera.

### Step 16 — tighten sync (optional, perf)

Replace the conservative `SYNC_ALL*` fallbacks with minimal correct scopes (a G-buffer→lighting
transition needs `SYNC_RENDER_TARGET`→`SYNC_(PIXEL/COMPUTE)_SHADING`, not `SYNC_ALL`). Measure;
each narrowing must keep GBV clean and `--scene-stress` CLEAN.

---

## Risk register

- **Compiling in `schedule` order instead of gather order (R3)** is the highest-severity
  mistake available, because it is silently *almost* right. Step 3's comparator on a
  bundle-using scene is the defence.
- **`Prepare` / `Record` divergence.** A body that does not follow what it registered produces
  wrong barriers with no complaint. Both defences in D1 are mandatory; dropping either turns
  this into invisible corruption rather than a crash.
- **The canonical-state invariant is the one genuinely new rule.** Everything else removes
  machinery; this adds a constraint the whole engine must obey. A resource that silently ends a
  frame off-canonical gives a wrong before-state on the *next* frame. Step 6 exists to find
  those in logging mode — do not merge Steps 6 and 7.
- **Return-to-canonical is not free.** If Step 8 shows the epilogue eating the win, the fallback
  is per-resource opt-out — but that reintroduces a small persistent map, so measure before
  conceding it.
- **This is a wide change** — ~120 call sites across 10 files, in the subsystem where a mistake
  is invisible until a specific frame on a specific machine. Half-done is worse than either end
  for maintainability; Step 1's mixed mode is what makes stopping between files safe.
- **The parallel-recording contract is being deleted, not adapted.** Anything relying on
  `Transition` being callable from a worker at any time must surface in Step 3.
  `RendererSubmissionStress.cpp` (8 sites) exercises exactly that path — decide early whether
  it is retargeted or retired.
- **Stack budget.** C6262 at 16 KB is a live constraint, not a nuisance (Step 2). Anything added
  to `Pass` is multiplied by `MaxPasses`.
- **Free/realloc only at GPU idle.** Unchanged rule; Step 11 changes creation only.

## Non-goals

- **Render-graph-native sync** — declaring `(sync, access, layout)` at `ctx.Use` instead of a
  legacy state. Natural once Goal 1 makes the graph the sole barrier author, and it would make
  D4's translation table unnecessary for graph passes. Still out of scope here: doing it earlier
  means designing the enhanced vocabulary and the graph rewrite simultaneously.
- **Split barriers / `BARRIER_SYNC_SPLIT`** — latency hiding across a begin/end pair.
- **Queue ownership / multi-queue** — enhanced barriers express cross-queue sync more cleanly;
  not relevant until async compute/copy queues exist.
- **Dropping the legacy path** — keep the fallback until enhanced barriers are proven across the
  target hardware range.
