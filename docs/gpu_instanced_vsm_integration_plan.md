# GpuInstancedModels → VSM (GPU-driven shadow) integration plan

**Goal.** Make `GpuInstancedModels` cast shadows through the GPU-driven indirect path
(`ShadowGpuData` cull → visible list → indirect args → mega-buffer → per-page
`ExecuteIndirect`) so they appear in **VSM** (they currently cast *nothing* in VSM) and, as a
bonus, drop their per-view **CPU tail** in Legacy. The approach folds each GI *instance* into
`ShadowGpuData`'s consolidated caster set so the existing cull + mega + per-page machinery
handles them uniformly — "for free" downstream.

Executor conventions (same as the VSM plan): one step per commit; build BOTH configs via
PowerShell MSBuild (`test_cube.sln`, Debug+Release, expect 0 warnings/0 errors); verify with
`--scene-stress` (DEBUG) and `--scene-stress-gbv` (adds GBV; known-noise ids {939,940,1006,1358});
shaders compile at RUNTIME (must run in the relevant mode); line endings `.cpp/.h/.hlsl` = CRLF,
`.md` = LF; do NOT commit (user commits per step). GI shadow correctness is partly VISUAL (Ctrl+V
VSM ↔ Legacy A/B) — the user signs off.

---

## Current state (why GI is excluded)

- `ShadowGpuData::IsCaster` rejects `IsGpuInstancedCaster()` — one object drives many GPU-side
  instances, so it can't be a single per-caster entry in the CPU-authored instance buffer.
- **Legacy** GI shadows: a CPU tail in `Pass_CSM` / `Pass_SpotShadows` / `Pass_PointShadows`
  (`SceneRenderer.cpp` ~1310/1441/1560) calls `obj->RenderShadow(...)` per atlas view for each
  `IsGpuInstancedCaster()` object → draws all instances via the object's own instance buffer.
- **VSM**: `VirtualShadowMap::RecordPageRender` only draws the consolidated `ShadowGpuData`
  casters → GI casts nothing.
- GI instance transforms live in each object's own `InstanceBuffer` (`InstanceData { float4x4
  world; float4x4 prevWorld; float rotationY; ... }`, stride 144), rewritten every frame by the
  object's rotation compute (`GpuInstancedModels::RecordCompute`). `world` is directly usable.

## Design decision — unified default-heap buffer + GI scatter compute

**Fold each GI instance into `ShadowGpuData` as an individual caster.** Reserve a GI caster-id
sub-range after the static casters; a per-frame GPU **scatter** compute writes each GI instance's
`world` + world-space `CasterBounds` into that range. Then the *unchanged* cull processes them,
and the *unchanged* mega + per-page draw renders them.

The blocker: `ShadowGpuData::instances_` / `bounds_` are **upload-heap rings** (CPU-mapped,
GENERIC_READ) — a compute shader can't UAV-write them. Fix: introduce **unified DEFAULT-heap**
mirrors that the GPU writes and the cull/VS read:

- `instancesUnified_` : DEFAULT-heap, kFrameCount regions × `count_` × `sizeof(InstancePerObject)`,
  UAV + per-region SRV. Region f = static region copied from `instances_` ring + GI region scattered.
- `boundsUnified_`    : DEFAULT-heap, kFrameCount regions × `count_` × `sizeof(CasterBounds)`, same.

Per frame (in `RecordCull`, before the clear/cull dispatches):
1. `CopyBufferRegion` the **static** slice `[0, Nstatic)` from the `instances_`/`bounds_` upload
   ring region f → the unified region f (Nstatic small ≈ tens of KB; always copy, simple).
2. **Scatter**: one dispatch per GI object writing `[giBase, giBase+count)` of the unified region.
3. Cull reads `boundsUnified_` (region f) instead of `bounds_`; the indirect VS reads
   `instancesUnified_` (region f) instead of `instances_` (t0). `casterGroup_` / `perGroup_` /
   `viewFrustums_` are unchanged rings.

The **only two descriptor swaps** are the cull's bounds SRV (`RecordCull`) and the indirect VS's
t0 (`RecordIndirectShadowDraws` + `RecordPageRender`). **No edits to `shadow_cull_cs.hlsl` or
`shadow_indirect_csm.hlsl`** — same struct layouts, different resource. The only new shader is the
scatter. Downstream (visible list stores global caster ids; VS reads `instancesUnified_[id].world`)
lines up because GI global ids `[giBase, giBase+count)` are exactly where the scatter writes.

### Runtime kill-switch (A/B the cost)
Gate the GI folding behind a runtime flag `render::g_giIndirectShadowsEnabled` (InstanceTypes.h,
**default ON**) + a Ctrl-key toggle ("ToggleGiIndirectShadows" in `input/bindings.json` +
AppController + a dev-window "VSM" tab checkbox), mirroring `g_indirectShadowsEnabled` (Ctrl+I):
- **ON** (default): GI folded → cull count includes GI, scatter runs, GI casts in VSM + Legacy via
  indirect, and the Legacy CPU tail is **skipped**.
- **OFF**: scatter skipped, cull count = `Nstatic` (GI ids never visited), and the Legacy CPU tail
  **runs** — i.e. exactly today's behavior (GI casts in Legacy only, nothing in VSM).

So the CPU tail is **retained but gated**, not deleted. The precise per-object rule: the tail draws
a GI object when `!render::g_giIndirectShadowsEnabled || !giFolded[obj]`, where `giFolded[obj]` is
set at Rebuild (true only for objects that fit under the group cap). Flag OFF ⇒ tail draws all GI;
flag ON ⇒ folded objects go indirect (tail skips them) and any over-cap objects tail-draw. This
gives the live A/B for the per-page overdraw cost and doubles as the group-cap safety fallback.

### Why unified over the alternatives
- **CPU tail in the page loop** (quick): per-page × per-GI draws with no per-page cull → defeats
  VSM's submission savings. Rejected.
- **Two buffers + a per-caster branch** in the cull + VS (static ring vs GI default, split at
  Nstatic): avoids the tiny static copy but edits the hot cull + VS shaders and threads a
  threshold — more risk for no real gain. Kept as a fallback note, not the plan.
- **Unified**: one buffer, uniform downstream, shaders untouched. The static copy is trivial.

### What GI instances inherit (accepted trade-offs)
- **Base-LOD shadows** — the mega group carries the base mesh index count; GI's per-instance LOD
  tiers are dropped for shadows (matches every other indirect caster; far cascades already do this).
- **Per-view (not per-page) culling** — like all casters, a GI instance visible in a view is
  re-drawn (scissored) into every resident page of that view. Dense GI fields in the directional
  clipmap's coarse levels ⇒ vertex overdraw. This is the existing VSM cost model, amplified by
  instance count — the plan delivers correctness; a per-page/instance cull is a separate future rung.

---

## Data model

- Caster-id space: `[0, Nstatic)` CPU static casters, `[Nstatic, count_)` GI instances (contiguous
  per GI object). `count_ = Nstatic + Σ giCount`.
- Mesh groups: `[0, staticGroups)` static + one group per GI object (all its instances share one
  mesh). `numMeshGroups_ = staticGroups + #GIobjects`. Each GI group: `perGroup_[g] = { base =
  giBase, indexCount = giMesh base index count }`, `groupMesh_[g] = giMesh`, `casterGroup_[c] = g`
  for `c ∈ [giBase, giBase+giCount)`.
- New CPU list captured at Rebuild for the scatter: `giCasters_ = [{ RenderableObjectBase* obj,
  uint giBase, uint count, float4 aabbCenter, float4 aabbExtent }]` (mesh-local AABB from
  `GetMesh()->GetBoundingBox()`; obj* refreshed every Rebuild).

---

## Steps

### Step 1 — GI caster accessors (interface, add-dormant)
`RenderableObjectBase`: add non-`dynamic_cast` virtual accessors (internal-RTTI pattern):
`virtual D3D12_CPU_DESCRIPTOR_HANDLE GetInstanceCasterSrv() const { return {}; }` and
`virtual UINT GetInstanceCasterCount() const { return 0; }`. `GpuInstancedModels` overrides:
return `instanceBuffer_.GetSRVCPU()` and `instanceCount_`. (Mesh AABB reuses the existing
`GetMesh()->GetBoundingBox()`.) No behavior change. **Verify:** both build 0/0.

### Step 2 — Unified instance+bounds buffers + static copy + descriptor swap (parity)
Add `instancesUnified_` / `boundsUnified_` DEFAULT-heap UavRings (kFrameCount regions × `count_`,
strides `sizeof(InstancePerObject)` / `sizeof(CasterBounds)`) + a per-region SRV heap
(`UnifiedInstanceSrv(f)` / `UnifiedBoundsSrv(f)`). Allocate in Rebuild sized to `count_`. In
`RecordCull`, before the clear dispatch: transition unified → COPY_DEST, `CopyBufferRegion` the
`[0, count_)` slice from the `instances_`/`bounds_` ring region f (at this step GI region is empty,
so copy the whole thing), transition → NON_PIXEL. Swap the cull's `bounds_.Srv(f)` → `UnifiedBoundsSrv(f)`,
and `InstanceSrv(f)` (used by `RecordIndirectShadowDraws` + `VirtualShadowMap::RecordPageRender`
t0) → `UnifiedInstanceSrv(f)`. **No GI yet — this must be a pure no-op refactor** (unified is a
verbatim copy of the ring). **Verify:** both 0/0; `--scene-stress-gbv=120` CLEAN; Legacy + VSM
shadows visually unchanged (user A/B). This de-risks the buffer swap in isolation.

### Step 3 — GI reservation at Rebuild (sizing/groups, still dormant)
In `Rebuild`, after enumerating static casters, enumerate `IsGpuInstancedCaster()` objects with a
valid instance-caster buffer + mesh: assign each a `giBase` + a mesh group; extend `count_`,
`numMeshGroups_`, `casterGroup_`, `perGroup_`, `groupMesh_`, and build `giCasters_`. Size
`instancesUnified_`/`boundsUnified_`/`visibleList_`/`indirectArgs_` for the new `count_`/groups.
`EnsureMegaBuffer` now concatenates GI meshes too. **Keep the cull count at `Nstatic` for now**
(pass `numCasters = Nstatic` to the cull CB, not `count_`) so GI ids are never visited → the
GI region stays untouched and nothing changes on screen. **Verify:** both 0/0; scene-stress CLEAN;
buffers are larger (log `count_`/groups) but shadows unchanged. Confirm `staticGroups + #GI ≤
VSM_MAX_SETUP_GROUPS (64)`; if exceeded, log + clamp (GI beyond the cap falls back to the CPU tail —
see Step 5 guard).

### Step 4 — GI scatter compute + enable in cull + gate the Legacy CPU tail (the flip)
- New `shaders/shadow_gi_scatter_cs.hlsl` (`[numthreads(64,1,1)]`): CB `{ giBase, count,
  aabbCenter, aabbExtent }`; `t0 = StructuredBuffer<InstanceData>` (the object's instance buffer);
  `u0 = RWStructuredBuffer<InstancePerObject> instancesUnified`, `u1 = RWStructuredBuffer<CasterBounds>
  boundsUnified`. Thread i (< count): read `world = t0[i].world`; write `u0[giBase+i].world = world`;
  compute world AABB from the mesh-local AABB (`center' = mul(world, center)`, `extent' = abs(world)·extent`,
  abs on the upper 3×3), write `u1[giBase+i] = { center', radius = length(extent'), extent' }`.
- `ShadowGpuData::RecordCull`: when `g_giIndirectShadowsEnabled`, after the static copy, for each
  `giCasters_` entry stage its `GetInstanceCasterSrv()` as t0 + the unified UAVs and dispatch
  `ceil(count/64)`; UAV-barrier the unified buffers; then transition them to NON_PIXEL and run the
  clear/cull with `numCasters = count_` (includes GI). When the flag is OFF, skip the scatter and
  pass `numCasters = Nstatic`. Ordering: the GI rotation compute (`RecordCompute`) must run
  **before** the scatter — verify the object-compute pass precedes `Main_ShadowCull` (else GI
  shadows lag one frame; add a dep or move the scatter after gbuffer if needed). State: the GI
  instance buffer is read as NON_PIXEL by the scatter (transition at the call site, like the other
  cull inputs).
- Gate the `IsGpuInstancedCaster()` `RenderShadow` CPU-tail loops in `Pass_CSM` /
  `Pass_SpotShadows` / `Pass_PointShadows` to draw an object only when
  `!render::g_giIndirectShadowsEnabled || !giFolded[obj]` (retained as the OFF path + over-cap
  fallback, not deleted). Leave a comment.
- **Verify:** both 0/0; flag ON — `--scene-stress-gbv=120` (Legacy) + steady-VSM
  `--scene-stress-gbv=120` CLEAN, only {939,940,1006,1358}; DBWIN shows the enlarged caster/group
  counts. Flag OFF — behavior identical to today (regression check). **User A/B** (Ctrl-toggle):
  GI casts shadows in VSM (Ctrl+V) with the flag ON, reverts to Legacy-only with it OFF; no
  double-draw, no gap.

### Step 5 — Guards, measurement, docs
- Group-cap guard: if `staticGroups + #GI > 64`, the over-cap GI objects are not folded (excluded
  from `giCasters_` / the reservation) and fall back to the retained CPU tail — correctness over a
  hard cap. (Realistically #GI ≪ 64, so this is a safety net.)
- Mega uniform-layout: GI meshes must share the caster stride + R32 index for the mega path; if
  not, the existing per-group fallback in `RecordPageRender` already covers them (verify).
- **Measure**: Legacy CPU should drop (GI tail gone); watch VSM per-page GPU (clipmap overdraw with
  dense GI). Capture VsmPageRender CPU/GPU before/after; note if a per-page cull rung is warranted.
- Update memory `shadow-caching-virtualization-plan.md` + this doc with results.

---

## Risks / notes
- **Upload→default copy state**: unified buffers cycle COPY_DEST → UAV (scatter) → NON_PIXEL
  (cull/VS) each frame; the fiddly part (mirror the existing per-pass transitions; GBV id would
  flag a mismatch). WAR-safe via kFrameCount regions (region f written+read in frame f).
- **Scatter ordering** vs GI rotation compute (Step 4) — 1-frame lag if mis-ordered; visually minor
  for slow rotation but fix it for correctness.
- **Per-page overdraw** for dense GI in coarse clipmap levels — accepted for v1; the honest
  follow-up is per-page instance culling, out of scope here.
- **Rollback**: Steps are independent commits; reverting Step 4 restores the CPU tail (GI back to
  Legacy-only), Step 2 is a pure refactor that can stand alone.
