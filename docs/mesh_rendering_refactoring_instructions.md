# Mesh & rendering refactoring/improvement — batching, per-object cost, instancing, LOD (working prompt, rev 1)

Self-contained plan for improving the **mesh representation and per-object rendering path**
of this D3D12 deferred renderer. Companion to the architecture review done 2026-06-16
(in-chat); this file is the actionable, step-per-commit version. Do ONE step at a time,
build (`test_cube.sln`, x64, Debug + Release), verify, **stop**.

File:line refs are a STARTING MAP — some were verified directly (noted), others came from a
read-only exploration sweep; **verify against current code before relying** (line numbers
drift). Repo root `D:\Programming\test_cube`, C++ under `sources/`, shaders under
`shaders/`. Windows + PowerShell; MSBuild at
`C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe`.

## Why this matters (measured context)

This renderer draws **one independent draw call per renderable object per pass**, and every
draw re-binds full state. Verified: `Material::Bind` unconditionally issues
`SetGraphicsRootSignature` + `SetPipelineState` every call (`Material.cpp:509-518`), and
`RenderObjectBatch` walks the visible queue in order with **no sort by PSO/material**
(`SceneRenderer.cpp:292-370`); `mesh->Draw` re-binds VB/IB every draw (`Mesh.cpp:74-78`).
So 50 identical spheres (the `metalRoughGrid`) = 50 draws each re-setting root sig, PSO, IA,
and descriptor tables.

This is the **structural cause** of the shadow-perf finding (see
`docs/shadow_refactoring_instructions.md` Step 5/6): `Pass_CSM` measured **draw/vertex- and
fixed-overhead-bound, not fill- or draw-call-count-bound** — atlas shrink and caster culling
both moved it ~0%, because the cost is per-draw state + per-vertex work, and nothing in the
architecture collapses redundant draws, instances repeated meshes, or LODs distant geometry.

REALITY CHECK on scale: the demo is small (~65 objects) and GPU-bound at the shipping
settings (Scale 0.58 + DLSS: CPU ~0.45 ms, GPU ~1.37 ms/frame). On THIS scene the absolute
wins below are modest and may not move FPS; their value grows with object count and on
CPU-submission-bound configs (the renderer is CPU/submission-bound at 1600×900 native — see
project memory `renderer-submission-refactor`). **Step 1 gates the rest on a measurement of
which bound actually dominates.** Do not implement levers blind.

## Architecture map

Data flow: **scene objects → per-view cull/bucketize (Scene) → per-pass parallel draw
(SceneRenderer) → per-object Bind+Draw (RenderableObject/Material/Mesh)**.

1. **Mesh / geometry** — `Mesh` (`sources/rendering/meshes/Mesh.{h,cpp}`): one vertex format
   `VertexPNTUV` (pos/normal/tangent/uv, 32 B), 16- or 32-bit indices, ONE AABB, NO submeshes
   (single `DrawIndexedInstanced`, hardcoded triangle list, `Mesh.cpp:74-86`), immutable GPU
   buffers via two-stage default+upload copy. `MeshManager` caches by file path
   (`shared_ptr<Mesh>`) → N objects of a model share one GPU buffer (GOOD). OBJ + custom-text
   loaders; CPU tangent generation. NO LOD levels.

2. **Renderable hierarchy** — `RenderableObjectBase → RenderableObject →
   GBufferRenderable → StaticMesh` (+ `TransparentStaticMesh`, `GpuInstancedModels`).
   `RenderableObject` owns transform, world-AABB cache, a mesh, and TWO materials
   (`graphicsMaterial_`, `shadowMaterial_` = derived depth-only `_csm`,
   `RenderableObject.h:145-146`). A per-object `UniformBinder` fills CB fields from shader
   reflection (`UpdateMainCB`/`UpdateShadowCB`).

3. **Per-object draw path** (`RenderableObject.cpp` ~136-210, verify) — per object, per pass:
   `FrameResource::AllocDynamic` a CB → `RenderContextPool::Acquire` → `UniformBinder::Update*`
   (memcpy fields) → `Material::Bind` (SetRootSig + SetPSO + per-root-param binds) →
   `mesh->Draw`. The `UniformBinder`s copy PER-FRAME/PER-VIEW data into EACH object's CB —
   `TransparentStaticMesh` ~25 fields incl. cascade splits, light counts, camera
   (`TransparentStaticMesh.cpp`, verify); `GBufferRenderable` copies the view matrices per
   object (`GBufferRenderable.cpp`, verify).

4. **Draw dispatch** — `SceneRenderer::RenderObjectBatch` (`SceneRenderer.cpp:292-370`,
   VERIFIED): visible objects (bucketed Opaque/Transparent × Simple/Complex by
   `SceneRenderQueue`) are chunked (default 32) and recorded in parallel, each `obj->Render()`
   doing its own full Bind. No PSO/material sort. Transparent buckets must keep sorted-queue
   order (blend correctness).

5. **Materials / PSO** — `MaterialManager` content-hash-caches PSOs+root-sigs
   (`Material.cpp` `BuildKey`), so objects sharing shader+defines+state share one PSO (GOOD,
   no PSO explosion). Each graphics material also eagerly builds a wireframe PSO. Runtime DXC
   (SM6) + D3DCompile (SM5) fallback + file-watch hot-reload. Input layouts pre-registered &
   cached by name (`InputLayoutManager`).

6. **Instancing (the model to emulate)** — `GpuInstancedModels`: structured buffer of
   per-instance transforms, compute-animated each frame, drawn as ONE
   `DrawIndexedInstanced(instanceCount)` per pass incl. one per shadow cascade
   (`GpuInstancedModels.cpp`, `Mesh.cpp:81-86`). One whole-cloud AABB (coarse culling).

## AI execution protocol

1. Execute only the step the user names. If none named, inspect code and RECOMMEND the next
   step; do not edit.
2. Stop after the requested step. Never start the next automatically.
3. Before editing, inspect current code and `git status`; work with pre-existing changes,
   never revert unrelated work.
4. Keep edits scoped to the requested step (and, for a multi-part step, the ONE sub-part).
5. No commits, branches, or flag removals unless the user explicitly asks.
6. Never claim a check passed unless it was run and observed: report `PASS`/`FAIL`/`NOT RUN`.
7. **Visual output must be pixel-identical** for the correctness-neutral steps (1–3); it
   CANNOT be self-verified (capture tooling unreliable; see project memory
   `screenshot-verification`). Present analysis to the USER; never conclude from a self-read
   capture.
8. When the code contradicts this doc, STOP and report the conflict; do not silently invent.
9. Perf changes are SEPARATELY MEASURED in RELEASE — never land two between measurements; a
   regression must be attributable to ONE change. (Lesson from shadow work: Debug GPU times
   mislead — the pass-time numbers differ ~3× Debug vs Release; always measure in Release.)

At the end of a step report: step, changed files & behavior, automated checks (exact
commands + results), manual/visual checks PASS/FAIL/NOT RUN, before/after numbers or
NOT RUN, remaining risks.

## Invariants (preserve throughout)

1. Rendered image is identical for Steps 1–3 (CB split, batching, auto-instancing are
   re-orderings/de-duplications of the SAME draws, not visual changes).
2. **Transparent draw order is never reordered** — blending depends on the sorted queue
   order. Batch/sort OPAQUE only; transparents keep queue order (and their determinism, per
   the renderer submission contract).
3. Preserve the command-list lifecycle / deterministic submit order owned by
   `docs/renderer_submission_instructions.md` — these steps change WHAT is recorded, not the
   CL/batch ordering mechanism.
4. Motion vectors stay correct: per-object `prevWorld` and the no-jitter view matrices must
   still reach the shader (don't drop them when splitting CBs).
5. Debug and Release render identically; the Debug D3D12 debug layer stays silent.

## Verification recipe (every step)

- Build Debug + Release (x64). Debug debug-layer must stay clean.
- **Perf is measured in RELEASE** (Debug GPU times are unrepresentative). Use the F9 profiler
  or a TEMP `OutputDebugStringA` probe in `Profiler::CollectGpuResults` summing the relevant
  GPU scope(s) (`GPU.Frame`, `Pass_GBuffer`, `Pass_CSM`), captured headlessly via a DBWIN
  listener — the pattern used in the shadow work (probe + DBWIN script under `%TEMP%`; remove
  the probe after). Also useful: per-frame DRAW-CALL and STATE-CHANGE counts, and CPU
  record-time per pass. Treat >3% Release GPU-frame regression as a failure. Numbers, or
  report NOT RUN.
- Headless smoke: drive `D3D12WindowClass` / title "D3D12 Multi-Mesh Renderer" (find by PID +
  class via EnumWindows; WASD/QE = move, look is mouse-only/not drivable), `WM_CLOSE` → exit
  0; scan ODS for D3D12 errors / shader-compile failures (Streamline NGX cache misses are
  benign). Visual correctness → USER.
- Profiler default-enabled (`enabled_{true}`); F9 only toggles the overlay, not GPU
  collection.

---

## Step 1 — Measurement gate: find the dominant bound (do this FIRST)

**Category: measurement. Risk: none (temp instrumentation, removed after).**

Before optimizing, determine — in RELEASE — whether the main render is **CPU-submission-
bound** (→ batching/instancing help), **GPU-state-change-bound** (→ batching helps), or
**GPU-vertex-bound** (→ LOD/instancing help), and whether the demo even has headroom.

- Instrument: per-frame draw-call count and state-change count (PSO/RS/IA set calls); CPU
  record time of `Pass_GBuffer`; GPU time of `Pass_GBuffer` (+ `GPU.Frame`, CPU.Frame).
- Stress it: temporarily scale the scene up (bump `metalRoughGrid` width/height or add objects
  in `data/levels/demo.json`) so the per-object cost is visible above noise, AND test a
  CPU-submission-bound config (e.g. 1600×900 native, DLSS off) since the demo's shipping
  config is GPU-bound.
- Decide the lever: if state-change/CPU-submission dominates → Step 3 (batching) first; if
  vertex throughput dominates → Step 5 (LOD) / Step 4 (instancing); if repeated identical
  meshes dominate → Step 4 (auto-instancing). Step 2 (CB split) is worth doing regardless.

**Acceptance:** a written before-table (draw calls, state changes, CPU record ms, GPU pass
ms, at demo + stressed + CPU-bound configs) and a chosen lever. Probe removed.

## Step 2 — Split per-frame/per-view data out of per-object constant buffers

**Category: structural cleanup + modest perf. Risk: medium (touches CBs + shaders; must stay
pixel-identical). Worth doing regardless of Step 1's bound.**

Today each object's `UniformBinder` copies per-frame/per-view data (view/proj matrices,
cascade splits, light counts, camera) into EVERY object's dynamic CB — e.g.
`TransparentStaticMesh` writes ~25 fields/object, most identical across all objects in the
pass. That's wasted CPU memcpy + CB bandwidth + larger per-object allocations.

**Fix:** introduce a shared FRAME/VIEW constant buffer, allocated once per pass and bound on
its own root-CBV slot (e.g. `b1`); move per-frame fields (viewProj, viewProjNoJitter,
prevViewProjNoJitter, cascade splits, light counts, camPos, exposure, …) there. Leave the
per-object CB (`b0`) with only per-object data: `world`, `prevWorld`, material params. Update
the affected shaders' cbuffers to match. `GpuInstancedModels` benefits too.

**Landmines:**
- Shader cbuffer layout MUST match the C++ field offsets (the reflection-driven `UpdateCBField`
  relies on names/offsets); update both sides together.
- Keep motion-vector fields intact (invariant 4).
- The frame CB must be bound before the object loop in EACH pass body (and for every command
  list, since D3D12 inherits no state across CLs).

**Acceptance:** pixel-identical output (USER confirm); per-object CB size + per-frame memcpy
field count drop (report); `Pass_GBuffer` CPU record time before/after; no Release GPU
regression.

## Step 3 — Draw batching / state-change minimization (sort opaque by PSO+material)

**Category: perf (the big CPU/state lever). Risk: medium-high (ordering + correctness). Gate
on Step 1 showing state-change/CPU-submission matters.**

Today every `obj->Render()` re-binds root sig + PSO + IA + tables even for back-to-back
identical objects. Reduce redundant state:
- Sort each OPAQUE bucket by (PSO, material, mesh) before recording. **Do NOT reorder
  transparent buckets** (invariant 2).
- Track last-bound root-sig/PSO/mesh in the command-list recording scope and skip
  `SetGraphicsRootSignature`/`SetPipelineState`/`IASetVertexBuffers`+`IASetIndexBuffer` when
  unchanged. This needs `Material::Bind` (or a wrapper) to become change-aware — currently it
  sets unconditionally (`Material.cpp:509-518`).
- Skip restaging descriptor tables that don't change between same-material objects.

**Landmines:**
- Parallel chunked recording (`RenderObjectBatch`, chunk=32) means "last-bound" state is
  per-CHUNK, not global — sort so same-PSO runs land in the same chunk, or accept per-chunk
  redundancy. Each command list starts with no inherited state, so the first draw in each CL
  must bind fully.
- Transparent correctness + the deterministic submit order (renderer submission contract)
  must be preserved.
- Measure: on the demo this may be ~0 (GPU-bound); the win shows on heavier/CPU-bound scenes.

**Acceptance:** identical visuals (USER); state-change count drops (report); CPU record time
and Release GPU/CPU frame before/after at demo + stressed configs; transparents unchanged.

## Step 4 — Auto-instancing of identical (mesh, material) object runs

**Category: perf (draw-call + shadow). Risk: medium. Reuses the proven `GpuInstancedModels`
path.**

Collect runs of objects sharing the same (mesh, graphics material) — e.g. the 50-sphere grid
— and draw them as ONE instanced call per pass (and per shadow cascade), feeding per-instance
transforms via a structured buffer, exactly like `GpuInstancedModels`. This is the lever that
would have cut the shadow draw load (shadow doc's deferred per-instance idea).

**Landmines:**
- Per-instance culling is then coarse (one bound for the run) — acceptable for clustered
  groups; keep small/spread groups un-instanced or bound per-instance.
- Material params that vary per object (the grid sweeps metal/rough per sphere) must move into
  the per-instance buffer, not the material.
- Transparent instancing breaks blend order — opaque only.

**Acceptance:** identical visuals (USER); draw-call count drops for the grid; `Pass_GBuffer`
and `Pass_CSM` Release ms before/after; no popping.

## Step 5 — Mesh LOD

**Category: perf (vertex lever). Risk: medium (visual). Ties to shadow doc 6b.**

Add per-mesh LOD levels (lower-poly proxies) and select by screen-size / distance / cascade
index. Biggest value: low-poly proxies in shadow far cascades and for distant gbuffer draws
(where detail is invisible) — directly attacks the vertex-bound cost the shadow work found.

**Landmines:** LOD pop / silhouette shift on shadows; keep near cascades + near gbuffer at
full detail. Needs LOD assets or runtime decimation (asset-pipeline work).

**Acceptance:** triangle count drops for distant/shadow draws; `Pass_GBuffer`/`Pass_CSM` ms
down; no visible LOD pop (USER); near detail unchanged.

## Step 6 — Submesh / multi-material support

**Category: feature/architecture. Risk: medium. Do only if assets need it.**

`Mesh` is one buffer = one draw = one material; the OBJ loader ignores `usemtl` groups, so
multi-material models must be split into multiple objects (more draws → feeds back into
Step 3). Add submesh sections (index ranges + per-section material) drawn in a loop, with
per-section bounds. Enables richer assets without object explosion.

**Acceptance:** a multi-material model renders correctly as one object with N sections;
per-section culling/bounds correct; draw count is N sections (not N objects).

---

## Parked / minor cleanups (low priority, order-independent)

- **Lazy wireframe PSO** — every graphics material eagerly builds a second wireframe PSO it
  may never use (`Material.cpp`); build on first wireframe toggle or behind a flag.
- **Configurable topology** — `Mesh::Draw` hardcodes triangle list (`Mesh.cpp:77`).
- **Content-hash mesh dedup** — dedup is path-based; identical geometry under two paths =
  two buffers.
- **Pre-cache active root params** — `Material::Bind` iterates all root params with a non-zero
  check each draw; precompute the active set.

## Explicitly out of scope

- Replacing the command-list / submission model (owned by
  `docs/renderer_submission_instructions.md`).
- A full render-graph/material-sort GPU-driven pipeline rewrite (engine-v2 scope).
- Skeletal animation / dynamic (CPU-mutable) geometry — the mesh layer is intentionally
  static/immutable.
