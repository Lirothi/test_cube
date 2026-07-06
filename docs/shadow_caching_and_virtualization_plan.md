# Shadow Caching → Virtualization Plan (Rung 0 + Rung 1 + Rung 2)

Executor plan to move this D3D12 deferred renderer's shadows toward UE5-style scalability, in
three milestones. Written FOR AN AI AGENT, separable steps, every step keeps both build
configs green and (until each marked "behavioral flip") leaves rendering pixel-identical.

- **Rung 0 — GPU-driven indirect shadow submission.** Move per-view shadow culling +
  instance marshalling off the CPU (GPU cull + `ExecuteIndirect`). Attacks the actual
  measured bottleneck (CPU submission) and is the foundation Rung 2 needs.
- **Rung 1 — Cached shadow maps.** Don't re-*rasterize* static casters every frame. After
  Rung 0 this is mostly a GPU-rasterization optimization (Rung 0 already removes the CPU win);
  its static/dynamic classification is still a shared foundation.
- **Rung 2 — Sparse/virtual shadow maps (VSM-lite).** Page pool + page table + on-demand page
  rendering. Reuses Rung 0's cull+indirect and Rung 1's classification.

**Recommended order: MEASURE first** (per-shadow-pass CPU split — see §Measure), then **Rung 0**
(highest leverage + unblocks Rung 2). Rung 1's Steps 10/11 (classification + dirty-tracking) are a
shared prerequisite for both Rung 0's incremental updates and Rung 2's invalidation — do them
regardless. Whether Rung 1's caching (Steps 12–15) or Rung 2 comes next is a measure-driven call
after Rung 0.

---

## Executor Guide (READ FIRST)

- **Build BOTH configs via PowerShell MSBuild, expect 0 warnings / 0 errors.** Not Git-Bash
  (it mangles `/p:` `/t:` switches). MSBuild path:
  `C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe`,
  target `test_cube.sln`, `/p:Configuration=Debug|Release /p:Platform=x64 /m /v:minimal /nologo`.
- **Line endings: `.cpp/.h/.hlsl` = CRLF, `.md` = LF.** Check after every edit
  (`grep -lU $'\r$' <file>`). `rt_lights.hlsli` is a pre-existing LF exception — preserve it.
- **Shaders compile at RUNTIME** (`Material` / `D3DCompileFromFile`, loaded by RELATIVE path
  `shaders/*.hlsl`, no copy-to-output). The C++ build will NOT catch shader / root-sig /
  cbuffer-stride / SRV-table errors. **You MUST run the app to validate any `.hlsl` or
  GPU-struct/descriptor-table change.**
- **Verification harness = `--scene-stress` on the DEBUG build** (the D3D12 debug/validation
  layer is `#ifdef _DEBUG`; a Release stress run has NO validation = false confidence).
  Exit 0 = clean; verdict text is in `scene_stress.log` + `OutputDebugString`, NOT stdout.
  `--scene-stress-gbv` (Debug only) adds GPU-Based Validation — run it whenever you add a new
  resource, barrier, or descriptor-table slot (that is its whole surface). Pre-existing GBV
  noise IDs: 939, 940, 1006, 1358 — anything else is yours.
- **Run the app with CWD = repo root** (`D:\Programming\test_cube`), never the exe folder —
  assets load by relative path (`data/…`, `shaders/…`) and a wrong CWD gives a blank
  "Loading…" window. Launching a SECOND instance while one is open conflicts on the GPU
  device; use `--scene-stress` for headless validation instead.
- **Do NOT commit.** The user commits incrementally, per step.
- **Follow existing conventions** (from the just-completed lights refactor): mirror the spot
  path rather than inventing constants; per-light per-frame decisions must use the
  NON-jittered selection frustum; ring-buffer or per-frame any GPU buffer read across frames
  in flight; free/realloc GPU resources only at GPU idle.

---

## Between-Step Safety (the "does it keep working?" invariant)

Every step is committable on its own: both configs build 0/0, the app runs, and rendering is
pixel-identical to the previous step — EXCEPT the explicit flip steps, which stay behind a
runtime toggle so the previous path remains the default until you've A/B-verified the new one.
The structure that guarantees this:

- **Add-dormant steps** (1–5, 10–13, 18–20): allocate a resource / add a compute pass / add a
  PSO that nothing samples yet. Output can't change because no consumer reads the new data.
  Cost is only wasted VRAM (Steps 1/12/18) + wasted GPU work until wired.
- **Flip steps** (6, 14, 21, 22, 24): change output BY DESIGN. Each keeps the old path behind a
  runtime toggle (F-key), defaulting OFF — so between steps the app still renders the old,
  correct image; you flip to A/B, verify, then make it default. **Never delete the old path in
  the same step that adds the new one.**
- **Refinement/verify steps** (7, 15, 23, 17, 26): tighten or measure; correctness preserved.

Caveats where the invariant is weaker — be careful:
- **A step that writes new UAVs/DSVs (4, 13, 19, 20) is "safe" only if its barriers are
  right.** A wrong state transition is a device-removal (crash), not a glitch — which is why
  `--scene-stress-gbv` (Debug) is MANDATORY on those steps, not optional.
- **Step 8 (mega-buffer) is NOT shadow-isolated** — it changes shared `Mesh` VB/IB storage the
  main GBuffer pass also uses. Treat it as a whole-renderer change; verify the main pass too.
- **Optimizations (7, 15, 23) can introduce STALE-data bugs** (a moved object's shadow not
  updating) that a green build + no-crash `--scene-stress` will NOT catch — they need
  behavioral verification (move something, watch its shadow).
- **Order matters:** Steps 10/11 (classification + dirty-tracking) must land before Step 7 and
  Steps 20/23 that depend on them; out of order, the dependent step has nothing to gate on.

---

## Current State (verified 2026-07-04, ground truth for the executor)

**Three shadow passes** (`sources/app/scene/SceneRenderer.cpp`), each depth-only
(`OMSetRenderTargets(0, nullptr, FALSE, &dsv)`), clear depth 1.0, rendered as PARALLEL
per-unit thread command lists (`TaskSystem::DispatchWait`):
- `Pass_CSM` (~:981) — 4 directional cascades into `D.shadow` (4096², 2×2 tiled; cascade 0
  clears the whole atlas, 1–3 just set viewports). Bind: `Renderer::BindShadowTarget(cl,
  cascadeIndex, clearDepth)`.
- `Pass_SpotShadows` (~:1098) — ≤8 lights into `D.spotShadow` (512²×8 array, one DSV per
  slice `D.spotShadowDSV[i]`, cleared per slice). Bind: `BindSpotShadowTarget(cl, lightIndex,
  clearDepth)`.
- `Pass_PointShadows` (~:1193) — ≤4 lights × 6 faces = 24 into `D.pointShadow` (256²×24 cube
  array, one DSV per face `D.pointShadowDSV[faceIndex]`, cleared per face; `faceIndex =
  cubeSlot*6 + face`). Bind: `BindPointShadowTarget(cl, cubeSlot, face, clear)`.

**Draw path**: `obj->RenderShadow(renderer, cl, view.view, view.proj, viewCB, lod)` using
`RenderableObject::shadowMaterial_` (built via `BuildShadowDesc` → `ConfigureShadowPipeline`).
`InstancedDrawBatch::RenderShadow` for collapsed instanced runs.

**View build + cull** (`Scene::PrepareViews`, `sources/app/scene/Scene.cpp:533`): cascade
views via `UpdateCascades`; spot/point views from `LightManager::SelectShadowedSpots/Points`
(non-jittered frustum). Shadow casters bucketized ONCE via
`shadowCasterSource_.Bucketize(objects_, camMask, /*filterShadowCaster=*/true)`, then each
view's `queue` is culled in parallel (`view.queue.Cull(view.frustum, shadowCasterSource_)`).
`SceneView.queue` (`SceneRenderQueue`) holds `visibleBuckets_` (OpaqueSimple/Complex used by
shadows).

**Render graph** (SceneRenderer.cpp ~:392): `pShadow(CSM) → pSpotShadow → pPointShadow →
pGbuf`; lighting/spot/point-light passes MT-depend on the matching shadow pass and declare
the atlas as an SRV read state.

**Atlases** are allocated per-frame (`render::kFrameCount = 3`) in
`RenderTargetManager.cpp` (`CreateShadow`/`CreateSpotShadow`/`CreatePointShadow`), all
`R16_TYPELESS` → `D16` DSV + `R16_UNORM` SRV, recreated on resize. Current VRAM: CSM ~96 MB,
spot 12 MB, point 9 MB (×3 frames).

**Object model** (`RenderableObject`): `transformDirty_` (set by `SetPosition/Scale/Rotation`),
`modelMatrixChangedThisTick_` (set in `SetModelMatrix`/`SyncSceneState`, **cleared at end of
`SyncSceneState`**), `prevModelMatrix_`. One per-frame sync point:
`SyncSceneState(SceneObjectSyncReason)`. **No `isStatic()` exists.** `CastsShadow()` virtual
exists. `RenderLayerMask` is bit-based visibility, NOT static/dynamic.

**What actually moves**: `TransparentStaticMesh` (if `rotateSpeedDeg>0`, rotates in `Tick`),
`GpuInstancedModels` (per-instance rotation via `instance_anim.hlsl` compute every frame),
ocean (compute sim). Everything else is stationary after load. **Lights**: `SpotLight` has a
`dirty_` flag (position/dir/range/angles → set, cleared in `UpdateCachedData`); `PointLight`
and `DirectionalLight` have NONE.

---

## Measure First (decides the order)

Before committing to Rung 0's refactor, confirm where the shadow CPU cost actually is — the
engine ALREADY auto-instances shadow draws (`SceneRenderQueue::BuildInstancedBatches` runs per
view; `InstancedDrawBatch` collapses identical (mesh,material) runs into one
`DrawIndexedInstanced`), so the draw-CALL count is already ~O(mesh-groups × views), NOT
O(objects × views). The remaining per-view CPU cost is:
- **Cull + bucketize + sort** of the caster set (`view.queue.Cull` / `BuildInstancedBatches`),
  run for each of up to 4 cascades + 8 spots + 24 point-faces ≈ **36 shadow views/frame**.
- **Instance marshalling**: `InstancedDrawBatch::RecordInstanced` copies a
  `render::InstancePerObject` array (208 B × instances) into a fresh `FrameResource::Alloc
  Dynamic` ring CB **for every view every frame**.

Use the existing per-pass CPU scopes (`CPU_SCOPE(ProfilerScopes::kPassCSM / kPassSpotShadows /
kPassPointShadows)` vs `kPassGBuffer`) and `RenderGraph::ExecuteParallel`. Scale up shadowed
lights. If the shadow passes' CPU (cull+marshalling) dominates → Rung 0 is the win. If not
(few lights / heavily instanced already) → Rung 1's contained caching may suffice. **This
measurement is the whole justification for Rung 0 — do it first.**

---

# RUNG 0 — GPU-driven indirect shadow submission (Steps 1–9)

**Goal.** Move shadow culling + per-instance marshalling to the GPU: instance data lives in a
PERSISTENT structured buffer (written once, updated only for movers), a compute pass culls per
view and writes `ExecuteIndirect` argument buffers, and the shadow passes issue a handful of
`ExecuteIndirect` calls instead of per-view CPU cull + CB marshalling. This attacks the
measured CPU-submission bottleneck and is the exact foundation Rung 2's page rendering needs.

**Be precise about the win (the map corrected the naive story).** Auto-instancing already
collapsed the draw-CALL count, so Rung 0's CPU win is NOT "fewer draws" — it is: (1) no
per-view CPU **cull/bucketize/sort** (moved to a compute pass), (2) no per-view CPU
**instance-array copy** (data is persistent on the GPU; only movers re-upload), (3) GPU
**count buffers** skip culled/empty mesh-groups for free. The per-(mesh-group, view) *draw
binding* cost stays O(mesh-groups × views) unless you also do Step 8 (mega-buffer) — so if the
scene is mostly UNIQUE meshes (little instancing), Step 8 is where the big collapse is; if it's
instance-heavy, Step 1–Step 7 already capture most of it. Let the §Measure split decide how far to
go.

**What already exists (reuse it):** `render::InstancePerObject` (208 B SV_InstanceID struct,
`InstanceTypes.h:11`) — the instance-data format; `SV_InstanceID` indexing in the shadow VS
(`gbuffer_common.hlsl`); `RecordComputeDispatch` (`ComputeDispatch.h:22`) + `Renderer::UAV
Barrier` — the compute produce/consume pattern; `Frustum::planes_[6]` + `Intersects(AABB)`
(`Frustum.h:94`) — the cull test to port to HLSL; `RenderableObject::GetWorldBounds()` (cached
world AABB); `rt::BindlessTable` (`BindlessTable.h:44`) — the SM6.6 heap pattern IF Steps 8/9
need bindless. **No `ExecuteIndirect`/`CreateCommandSignature` exists anywhere — all new.**

---

### Step 1 — Persistent per-caster instance buffer

- **Goal:** all shadow-caster instance data in one persistent GPU buffer; still unused.
- **Changes:** allocate a persistent (default-heap) `StructuredBuffer<InstancePerObject>` sized
  to the shadow-caster count, indexed by a stable global caster id assigned in `Scene` (mirror
  how `shadowCasterSource_` enumerates casters). Populate at level load; per frame re-upload
  ONLY entries whose transform changed (reuse Rung 1 Steps 10/11 dirty tracking —
  `modelMatrixChangedThisTick_`). Keep the existing CPU marshalling path untouched.
- **Verify:** builds 0/0; `--scene-stress-gbv` clean (new buffer unused → no perturbation).
  Log: after frame 1 with a static scene, zero re-uploads.

### Step 2 — GPU cull inputs: frustum planes + caster bounds

- **Goal:** the cull pass's inputs exist on the GPU.
- **Changes:** per shadow view, upload its 6 frustum planes (`Frustum::planes_`, float4[6]
  inward `n·p+d>=0`) into a small buffer. Maintain a persistent `StructuredBuffer` of per-caster
  world bounds (center + half-extents, or center + radius from `GetWorldBounds()`), updated
  with transforms like Step 1. Unused yet.
- **Verify:** builds 0/0; GBV clean.

### Step 3 — Command signature + indirect/visible/count buffers

- **Goal:** the indirect execution machinery exists; unused.
- **Changes:** `CreateCommandSignature` for a single `D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED`
  (no per-draw root args — instance data comes from a bound SRV indexed by `SV_InstanceID` +
  the arg's `StartInstanceLocation`). Allocate, per (view, mesh-group): a
  `D3D12_DRAW_INDEXED_ARGUMENTS` arg buffer + a visible-caster-id list + a GPU count buffer
  (UAV, ring/per-frame). Add a thin `Renderer::ExecuteIndirect` wrapper. Unused yet.
- **Verify:** builds 0/0; `--scene-stress-gbv` clean.

### Step 4 — GPU cull compute pass

- **Goal:** produce the indirect args on the GPU; still not consumed by drawing.
- **Changes:** a compute shader (dispatched via `RecordComputeDispatch`) over all casters for a
  view: port `Frustum::Intersects(AABB)` (positive-vertex plane test) using Step 2's planes +
  bounds; for a visible caster, append its id to the (view, mesh-group) visible list and
  atomically bump that group's `DRAW_INDEXED_ARGUMENTS.InstanceCount` (+ maintain
  `StartInstanceLocation` bases). Group by the caster's `meshId`. `UAVBarrier` before draws.
  Do all views in one dispatch grid if practical (one thread per caster×view, or per caster
  looping views). Validate via a temporary readback of counts vs the CPU cull result.
- **Verify:** builds 0/0; run app; readback counts match CPU cull within tolerance.
  `--scene-stress-gbv` clean (UAV writes are the surface).

### Step 5 — Indirect shadow vertex shader

- **Goal:** a depth-only shadow VS that fetches instance data GPU-side.
- **Changes:** shadow VS reads `world`/`prevWorld` from Step 1's buffer, indexed via
  `visibleList[StartInstanceLocation + SV_InstanceID]` → caster id → instance entry. Bind the
  instance buffer + visible list as SRVs (a normal descriptor table — NO new graphics bindless
  needed for R0). Mirror the existing `SV_InstanceID` gbuffer pattern; keep the `_csm`
  depth-only PSO. Runtime-compiled → **run to validate**.
- **Verify:** builds 0/0; app runs, shader compiles.

### Step 6 — Behavioral flip: ExecuteIndirect replaces the CPU loop

- **Goal:** the CPU submission win.
- **Changes:** in `Pass_CSM` / `Pass_SpotShadows` / `Pass_PointShadows`, replace the per-object
  `RenderShadow` / per-batch `RecordInstanced` loop with: (run Step 4 cull for the pass's views,
  ideally hoisted to one compute pass before the shadow passes), then per (view, mesh-group)
  bind VB/IB + the depth PSO + instance SRVs and `ExecuteIndirect(sig, maxCount, argBuf,
  offset, countBuf, offset)`. Route the cull compute in the render graph BEFORE the shadow
  passes (produce→consume, UAV→INDIRECT_ARGUMENT state). Keep the old CPU path behind a
  runtime toggle (F-key) for A/B + fallback.
- **Verify (the important one):** run — shadows IDENTICAL to the CPU path (toggle A/B). `--scene
  -stress` Debug CLEAN across level churn. `--scene-stress-gbv` clean — the cull-UAV →
  ExecuteIndirect-arg state transitions + the arg/count buffer barriers are the biggest GBV
  surface in this whole plan; run it. **Measure** the shadow-pass CPU scopes +
  `RenderGraph::ExecuteParallel` vs the toggle.

### Step 7 — Incremental instance/bounds upload (perf polish)

- Wire Steps 1/2 to re-upload only movers (Rung 1 Steps 10/11). Confirm a static scene does ~zero
  per-frame upload; a moving object updates only its entry. Measure.

### Step 8 — (Optional) Mesh mega-buffer → one ExecuteIndirect per view

- Consolidate shadow-caster geometry into a shared VB/IB (per-mesh baseVertex/baseIndex in a
  mesh-info buffer). Cull emits a multi-draw arg buffer; ONE `ExecuteIndirect` per view draws
  every visible caster regardless of mesh, collapsing the per-(view,mesh) CPU binding to
  O(views). **This is the big lever when meshes are mostly unique** (little instancing); skip
  it if §Measure shows instance-heavy scenes. Larger mesh-management change (touches
  `MeshManager`/`Mesh` buffer creation). **NOT shadow-isolated:** meshes are shared with the
  main GBuffer pass, so a consolidation bug affects normal rendering too — verify the main pass
  + both configs, not just shadows. Consider gating the mega-buffer behind a build/runtime flag.

### Step 9 — (Optional) Extend GPU-driven to the main GBuffer pass

- The main opaque pass has per-material texture variety → needs bindless material indexing per
  draw (extend the `rt::BindlessTable` pattern to graphics). Separate, larger milestone; the
  shadow path (uniform depth-only PSO) is deliberately first because it avoids this. Note only.

---

# RUNG 1 — Cached Shadow Maps (spot + point) (Steps 10–17)

**Goal.** Render STATIC shadow casters into a persistent atlas ONCE per light; each frame
copy that cache into the working atlas and re-render only DYNAMIC casters on top. Re-render
the static cache only when the light moves, a static caster changes, or the atlas is
recreated.

**⚠ How Rung 0 changes this (read before doing Steps 12–15).** Rung 1 was originally the CPU-
submission win. **Rung 0 already captures that** (GPU cull + persistent instance buffer means
re-submitting all casters every view is nearly free on the CPU). So AFTER Rung 0, Rung 1's
remaining benefit is purely **GPU rasterization** — not re-rasterizing static geometry into
every shadow view each frame — and it only matters if shadow rendering is GPU-bound after
Rung 0. Furthermore, that GPU-raster saving is realized more granularly by Rung 2's page
caching (Step 23). **Therefore:**
- **Step 10 + Step 11 are a shared, do-them-anyway foundation** — they feed Rung 0's incremental
  instance/bounds upload (Step 7) AND Rung 2's page invalidation (Step 23), independent of Steps 12–15.
- **Steps 12–15 (persistent static atlas + copy + dynamic-only render) are now OPTIONAL / measure-
  driven** — do them only if, after Rung 0, shadow rendering is GPU-bound AND you are not going
  straight to Rung 2 (whose Step 23 subsumes them). If you skip them, Steps 16/17 are moot.

**Scope (if pursued).** Spot + point lights only. CSM (directional) recenters every frame as
the camera moves, so whole-atlas caching is marginal there — it's the natural job of Rung 2's
clipmap; left as stretch Step 16.

**Approach (per shadow slot, each frame).**
1. Compute a cache tag for the light in this slot (light id + light-transform version +
   global static-set version).
2. If the tag ≠ the slot's stored tag → **cache miss**: render only STATIC casters into the
   persistent static atlas slice; store the new tag.
3. Copy static atlas slice → working atlas slice (GPU depth copy).
4. Render only DYNAMIC casters into the working atlas slice (depth test `LESS_EQUAL`, so a
   nearer dynamic occluder still wins).
5. Sampling is UNCHANGED — the light passes still read the working atlas.

Memory cost: +1 persistent (non-tripled) static atlas each = +4 MB (spot) + 3 MB (point).

---

### Step 10 — Static/dynamic caster classification  *(FOUNDATION — do regardless of Steps 12–15; feeds Rung 0 Step 7 + Rung 2 Step 23)*

- **Goal:** every shadow caster is classifiable static vs dynamic; the shadow queue can be
  split into two buckets. No behavior change.
- **Changes:**
  - Add `virtual bool IsDynamicCaster() const { return false; }` to `RenderableObjectBase`
    (default static). Override in the movers: `TransparentStaticMesh` (return
    `rotationSpeed_ != 0`), `GpuInstancedModels` (return true), `OceanRenderable` (true).
  - `#if WITH_EDITOR`: an object that is currently selected / gizmo-editable should count as
    dynamic (it can move any frame) — cheapest correct behavior. Encode via the editor
    marking the object dynamic while selected, OR just treat all editor objects as dynamic in
    editor builds (simplest; the cache still helps runtime/shipped scenes). Decide and note.
  - In `SceneRenderQueue`, add a static/dynamic split to the shadow bucketize/cull path:
    either a second set of `visibleBuckets_` (static vs dynamic) or a predicate on `Cull`.
    Reuse the existing `filterShadowCaster` machinery in `shadowCasterSource_.Bucketize`.
- **Verify:** builds 0/0. Temporarily log the static/dynamic counts per shadow view for demo
  (mostly static) vs demo1; confirm the movers land in "dynamic". Revert the log.

### Step 11 — Change-detection that survives to shadow-render time  *(FOUNDATION — do regardless of Steps 12–15; Step 7 uses the caster move-signal, Steps 20/23 use the light dirty-versions)*

- **Goal:** at shadow-render time we can answer "did this caster move this frame?" and "did
  this light move?" — the invalidation inputs. No behavior change (nothing consumes them yet).
- **Changes:**
  - `RenderableObject`: stop losing the move signal. Either don't clear
    `modelMatrixChangedThisTick_` until after the shadow passes, or snapshot it into a
    render-visible `movedThisFrame_` during `SyncSceneState`. Expose a getter.
  - Add a global **static-set version counter** on `Scene` (or `LightManager`): bump it on
    level load, editor spawn/delete, visibility toggle, or when any STATIC caster reports a
    move this frame (rare — editor only). Dynamic movers do NOT bump it.
  - Add a `dirty_`/transform-version to `PointLight` and `DirectionalLight` mirroring
    `SpotLight::dirty_` (set on `SetDesc`/`SetPosition`/`SetDirection`, expose a monotonic
    "transform version" the cache can compare). For spot, expose its existing `dirty_` as a
    version too.
- **Verify:** builds 0/0. Unit-log: move a light / a static object in the editor and confirm
  the version bumps; a rotating `TransparentStaticMesh` does NOT bump the static-set version.

### Step 12 — Persistent static atlases + per-slot cache tags  *(OPTIONAL after Rung 0 — GPU-raster-bound scenes only; Steps 13–15 subsumed by Rung 2 Step 23)*

- **Goal:** the storage for the cache exists; still unused.
- **Changes:**
  - In `RenderTargetManager`, allocate ONE persistent (NOT per-frame) static spot atlas
    (512²×8, same format/DSV-per-slice/SRV as `CreateSpotShadow`) and static point atlas
    (256²×24, mirror `CreatePointShadow`). Name them `SpotShadowStatic` / `PointShadowStatic`.
    Recreate only on resize/atlas-resize; keep the "free only at GPU idle" rule.
  - Add per-slot cache-tag arrays (CPU side, on `LightManager` or `SceneRenderer`):
    `struct ShadowCacheTag { uint64_t lightId; uint32_t lightVersion; uint32_t staticSetVersion; bool valid; }`
    sized `kMaxShadowedSpotLights` and `6*kMaxShadowedPointLights` (or per cube = 4). Invalidate
    all tags on resize.
- **Verify:** builds 0/0; `--scene-stress` exit 0 + `--scene-stress-gbv` clean (new resources
  allocated but unused → must not perturb GBV). Confirm the +7 MB VRAM in a memory log.

### Step 13 — Populate the static cache on cache-miss (still redundant)

- **Goal:** render static casters into the static atlas on a tag mismatch — but STILL render
  everything into the working atlas as today, so rendering stays identical. This isolates the
  cache-fill path for verification before the flip.
- **Changes:** in `Pass_SpotShadows` / `Pass_PointShadows`, before the existing per-slot work:
  compute the light's cache tag (Step 11 inputs); if it ≠ the stored tag, bind the STATIC
  atlas slice (`BindSpotShadowTargetStatic`/point equivalent — clear + depth-write), render
  only the STATIC bucket (Step 10) with the same `RenderShadow` path, then store the tag.
  Add render-graph declared states for the static atlases (DEPTH_WRITE during these passes).
- **Verify:** builds 0/0; run the app — shadows unchanged (working atlas still authoritative).
  `--scene-stress` Debug CLEAN + `--scene-stress-gbv` clean (the new DSVs/targets are the GBV
  surface). Log cache hit/miss: after the first frame a static light should be a permanent HIT.

### Step 14 — Behavioral flip: working = copy(static) + dynamic-only

- **Goal:** the actual win. Working atlas slice = copy of static slice, then only dynamic
  casters rendered on top.
- **Changes:** in both passes, replace "clear + render ALL casters into the working slice"
  with:
  1. `CopyTextureRegion` (or `CopyResource` per subresource) static slice → working slice.
     Manage states: static slice `DEPTH_WRITE→COPY_SOURCE`, working slice
     `DEPTH_WRITE→COPY_DEST→DEPTH_WRITE`. Declare these in the render graph; the copy must be
     ordered after the cache-fill (Step 13) and before the dynamic draws.
  2. Render only the DYNAMIC bucket into the working slice (no clear — the copy IS the
     initial depth), depth test `LESS_EQUAL` (already the shadow depth func).
  - Slots with a valid static cache + zero dynamic casters + unmoved light: the working slice
    is just the copy — correct, or optimize in Step 15.
- **Safety:** gate the new copy+dynamic path behind a runtime toggle (as Step 6 does for
  indirect), defaulting to the Step-13 render-all path, so the committed build always renders
  correctly and you can A/B before making it the default. Don't remove the render-all path in
  this step.
- **Verify (this is the important one):** run the app — shadows must look IDENTICAL to before
  (static + moving casters both shadow correctly; move a `TransparentStaticMesh` and its
  shadow tracks it). `--scene-stress` Debug CLEAN across the level churn (SwitchLevel changes
  the caster set → cache misses → must not crash). `--scene-stress-gbv` clean (the COPY
  barriers are new GBV surface). **Measure**: CPU submission time (`RenderGraph::ExecuteParallel`)
  and GPU shadow-pass time on demo (mostly static) before/after — expect the CPU drop.

### Step 15 — Edge cases + the zero-dynamic fast path

- **Goal:** correctness under churn + skip the copy when possible.
- **Changes:**
  - Shadowed-set churn: a light entering the set (new slot) → tag mismatch → Step 13 rebuilds it
    once (already handled if tags key on lightId). A light leaving frees its slot; ensure the
    next occupant's tag mismatches (different lightId) → rebuild. Verify no stale cross-light
    cache (this is the analog of the WAR wrong-RT bug — key tags by lightId, not slot alone).
  - Zero-dynamic fast path: if a slot's cache is valid, the light is unmoved, AND there are no
    dynamic casters in that view, skip the copy and point the light sampler at the STATIC
    atlas slice for that slot (needs the light passes to select static-vs-working per slot —
    a per-slot "source" flag in the light structured buffer, or just always-copy and defer
    this optimization). Decide based on measured copy cost.
  - Editor moves of a STATIC object → static-set version bump (Step 11) → all caches rebuild next
    frame (conservative, fine — editing is not perf-critical).
  - Resize → invalidate all tags (Step 12).
- **Verify:** `--scene-stress` Debug CLEAN including the `EditorSpawnDelete` op; move a static
  object in the editor and confirm its shadow updates within a frame; toggle a light's
  position and confirm its shadow rebuilds. GBV clean.

### Step 16 — (Stretch) CSM caching

- Directional recenters as the camera moves (texel-snapped in `UpdateCascades`). Whole-atlas
  caching only pays while the snapped center is unchanged (camera still / slow). Option:
  cache per cascade, invalidate a cascade when its snapped center or the sun direction
  changed; distant cascades (which barely move) benefit most. LOWER ROI than spot/point and
  cleanly subsumed by Rung 2's clipmap — implement only if measured worthwhile, else defer.

### Step 17 — Verify + measure + document

- Both builds 0/0; CRLF/LF clean; `--scene-stress` Debug CLEAN; `--scene-stress-gbv` clean
  (no new IDs beyond 939/940/1006/1358). Profile CPU submission + GPU shadow time on a
  mostly-static scene (demo) and a churny one (demo1), before vs after. Record the win + the
  copy cost in this doc and the memory file. Confirm the user visually (static shadows stable,
  dynamic shadows track).

---

# RUNG 2 — Sparse / Virtual Shadow Maps (VSM-lite) (Steps 18–26)

**Goal.** Virtual addressing: conceptually huge shadow resolution, but only the 128²-ish
pages that on-screen pixels actually need are allocated in a shared physical pool and
rendered; combined with page caching, per-frame rendering shrinks to newly-visible or changed
pages. Gets crisp shadows at any distance and scales to many lights.

**Prerequisite: Rung 0 (now satisfied by this plan).** Full VSM efficiency needs a GPU-DRIVEN
pipeline — per-page GPU culling + issuing casters into physical pages via `ExecuteIndirect`.
The engine had none of that (no `ExecuteIndirect`/command signatures/mesh shaders, CPU-side
cull) — which is exactly why **Rung 0 exists and must land first.** With Rung 0 done, Rung 2
REUSES its foundation: the persistent instance buffer (Step 1), GPU cull compute (Step 4), command
signature + indirect buffers (Step 3), and indirect shadow VS (Step 5). Step 22 (rendering into pages)
is then "cull per page + ExecuteIndirect into the page's viewport", not a from-scratch
CPU-driven raster. This is still **VSM-lite** (no Nanite/mesh-shader software raster, so no
sub-triangle page granularity), but it is genuinely viable on Rung 0's back. Step 26's gate becomes
"is virtual addressing a net win over plain Rung 0 indirect shadows?" — not "was GPU-driven
even possible?". Keep the Rung 0 (non-virtual) path behind a runtime toggle as the fallback.

**Scope + ordering correction (added after implementing Steps 18/19).** The VSM page pipeline
(Steps 18–23) targets the **LOCAL lights first** — spot lights + point-light cube faces. The
**directional shadow stays on the existing CSM** (`Pass_CSM`, 4 cascades) UNCHANGED through
Steps 18–23, and is only virtualized in **Step 24**, where the clipmap REPLACES the cascades
(so the cascades are eliminated by Step 24, not before — do not treat them as VSM "virtual
views" in Steps 19–23). **Two things a naïve single-level request gets wrong (measured in
Step 19: ~57k pages requested vs a 1024-page pool, 56× over):** (1) no mip/LOD means every
receiver — near or far — requests a page at the finest `kVirtualRes` level, so far geometry
requests orders of magnitude more page-area than it needs; (2) a uniform `kVirtualRes=8192` is
~32× the old spot/point maps (512²/256²), so local lights over-subscribe massively. **Both are
fixed by Step 19b BEFORE Step 20** — a bounded request is the prerequisite for allocation to be
meaningful.

---

### Step 18 — Physical page pool + page-table resources

- **Goal:** the addressing storage exists; unused.
- **Changes:** allocate a physical page-pool depth texture (e.g. a single D16 atlas of
  `PAGES_X × PAGES_Y` pages of 128²; budget the pool, e.g. 4k×4k = 1024 pages ≈ 32 MB) and a
  page-table resource (a `Texture2D`/`StructuredBuffer` per light-or-clipmap-level mapping
  virtual page → physical page index + resident/flags). Define constants: `kPageSize=128`,
  `kVirtualRes` (e.g. 8192 per spot / per cube face / per clipmap level), pool dimensions.
  Add SRVs/UAVs/DSVs. These are PERSISTENT (cross-frame), not per-frame-tripled — the pool IS
  the cache.
- **Verify:** builds 0/0; `--scene-stress-gbv` clean (unused resources must not perturb).
- **DONE (uncommitted):** `VirtualShadowMap` class — 4096² D16 pool (1024 pages, 32 MB) + page
  table `StructuredBuffer<uint>`. **Sizing note:** the initial flat level-0-only page table +
  `kVirtualRes=8192` is REVISED by Step 19b (per-mip page-pyramid layout + local-light-only
  view set + right-sized `kVirtualRes` per light type).

### Step 19 — Screen-space page-request pass

- **Goal:** determine which virtual pages the visible frame needs.
- **Changes:** a compute pass over the depth/GBuffer (after GBuffer, before shadow render):
  for each visible pixel and each relevant shadowed light, reconstruct world pos, project into
  that light's virtual shadow space, pick the mip level from screen-space density (ddx/ddy of
  the shadow UV, or distance heuristic), and mark the needed `(light, level, page)` in a
  page-request buffer (atomic-OR into a bitfield, or append). Add a debug visualization mode
  (F-key) that colors marked pages.
- **Verify:** run the app; debug-viz shows a sane, camera-following set of requested pages
  (dense near, sparse far). `--scene-stress` Debug CLEAN; GBV clean.
- **DONE (uncommitted) — SINGLE-LEVEL, transitional:** request bitfield UAV + clear/mark
  compute shaders (`vsm_page_request_clear_cs.hlsl`, `vsm_page_request_cs.hlsl`); reconstruct
  world from camera depth, project into each shadow view, mark the finest-level page. Verified
  via a readback count-log (F-key screen-color viz deferred). **Two deliberate gaps handled by
  Step 19b:** (a) NO mip selection — every pixel marks a finest-level page → ~57k requested;
  (b) it transitionally projected into the 4 CSM cascades too — WRONG (directional stays on CSM
  until Step 24). The mark math (reconstruct → project → page id) is the reusable core.

### Step 19b — Mip-level page requests + local-light scope + right-sized virtual res  *(PREREQUISITE FOR STEP 20 — do before it)*

- **Goal:** bound the page-request set so it fits the pool (target: total requested pages ≲ pool
  size with headroom). Without this, Step 20's allocation has nothing sane to do (56× over).
- **Changes:**
  - **Local-light scope.** Restrict the VSM virtual views to spot lights + point-light cube
    faces (drop the 4 CSM cascade slots from Step 19's request). Directional keeps rendering via
    the existing `Pass_CSM` until Step 24. `kMaxVirtualViews` → `kMaxShadowedSpotLights` +
    `kMaxShadowedPointLights*6` (= 8 + 24 = 32), no cascades.
  - **Per-mip addressing.** Give each virtual view a mip pyramid: level L has
    `(kVirtualRes>>L)/kPageSize` pages per axis, down to 1×1. Per-view page count becomes the
    pyramid sum (e.g. 2048²: 16²+8²+4²+2²+1 = 341). Re-lay-out the page table + request bitfield
    with per-(view,level) base offsets. Encode the level in the `(view, level, page)` request.
  - **Level selection.** Per pixel per view, pick the mip level so shadow-texel density ≈
    screen-pixel density — a distance heuristic first (`level = clamp(log2(dist / refDist), 0,
    maxLevel)`), refine to `ddx/ddy` of the shadow UV later. Mark the page at THAT level.
  - **Right-size `kVirtualRes` per light type.** 8192² is ~32× the old spot/point maps; use a
    lower base for local lights (e.g. spot 2048², point-face 1024² — tune against the request
    count log). Directional keeps its (future) clipmap res in Step 24.
- **Perf (MEASURED: the single-level Step-19 pass costs ~1.2 ms GPU — unacceptable, and it's
  currently add-dormant).** The cost is dominated by (a) `InterlockedOr` contention (≈1.44M
  pixels × up to 36 views hammering a 4608-word bitfield → threads serialize on shared words)
  and (b) full-res × all-views projection. Fixes, in priority order:
  1. **Run the request pass at reduced resolution** (e.g. 1/8 per axis ≈ 1/64 the threads) — the
     biggest win (pages are coarse 128², so sub-sampled screen coverage still finds them). Add a
     ~half-page margin / 1-page dilation so page edges aren't missed.
  2. **Mip levels** (above) collapse distinct-page count → less contention.
  3. **Per-pixel/tile light cull** — only project into views whose light influence reaches the
     pixel (reuse the spot-cone / point-radius test), not all 24 point faces.
  4. **Groupshared aggregation** — coalesce a tile's marks in LDS, one atomic per unique page.
  5. **Gate the pass** — do not run it every frame while unused (until Step 21 consumes the
     request); tie it to the VSM-active toggle. (Quick immediate win.)
- **Verify:** the request-count log drops from ~57k to ≲ pool with headroom; per-view counts
  are dense-near / sparse-far as the camera moves (mip levels working); **request pass GPU cost
  ≪ 1.2 ms** (reduced-res + gating). `--scene-stress` + `--scene-stress-gbv` CLEAN. This
  bounded, mip-aware, local-light request is the input Step 20 allocates from.
- **DONE (uncommitted) 2026-07-05:** all four changes landed. `kVirtualRes` 8192→**2048**; mip
  pyramid (`kNumMipLevels=5`, per-view `kPagesPerView=341` = 16²+8²+4²+2²+1, `LevelPageOffset`
  prefix sums); local-light scope `kMaxVirtualViews`=**32** (spots + point faces, cascades
  dropped — `Pass_VsmPageRequest` no longer adds the 4 cascade views); per-pixel camera-distance
  level selection (`level = clamp(log2(distCam/kLodRefDist=10), 0, kMaxMipLevel)`); reduced-res
  dispatch (`kRequestDownscale=8` → 1/64 the threads); and a runtime **gate**
  `render::g_vsmPageRequestEnabled` (default **OFF** — add-dormant; **Ctrl+V** =
  "ToggleVsmPageRequest"). Page table shrank 147456→10912 entries; request bitfield 4608→341
  words. **Measured (demo, gate temporarily ON): 872 pages requested** (was ~57279 — 66× drop,
  fits the 1024-page pool), mip histogram L0=20/L1=723/L2=129/L3=0/L4=0 (multiple levels →
  selection works). Both configs 0/0; `--scene-stress-gbv=120` + `--scene-stress=300` CLEAN
  (only known-noise IDs {939,940,1006,1358}); shaders compile. DEFERRED (documented, not
  needed for the count/cost goal): reduced-res **dilation** for edge-accurate pages (a Step-21
  sampling concern — nothing samples yet), **per-pixel/tile light cull** + **groupshared
  aggregation** (further perf), and **per-light-type** virtual res (unified 2048 used). Request
  GPU cost not separately re-measured headless — expected ≪1.2 ms from 1/64 threads + mips;
  confirm in a live capture if it matters. NEXT = **Step 20** (allocate physical pages from this
  bounded request + persistent page table / free-list / LRU).

### Step 20 — Page allocation / free + persistent page table

- **Goal:** turn requests into physical-page assignments, with cross-frame caching.
- **Changes:** a compute pass consuming Step 19b's bounded mip-aware local-light requests: for
  each requested page not already resident → allocate a free physical page (from a free-list);
  mark newly-allocated or invalidated pages into a "needs-render" list; free (LRU) pages not
  requested for N frames.
  Maintain the page table + free-list + LRU state PERSISTENTLY across frames (this is the
  cross-frame GPU state VSM hinges on). Invalidation hooks come from Rung 1 (light moved →
  invalidate that light's pages; static caster changed → invalidate overlapping pages).
- **Verify:** debug-viz shows pages persisting frame-to-frame when the camera is still
  (cache), and only edges churning as it moves. GBV clean (cross-frame UAV state is a common
  GBV/state-tracker pitfall — this is a key run).
- **DONE (uncommitted) 2026-07-05:** GPU-driven allocation with cross-frame caching. Added 5
  PERSISTENT DEFAULT-heap `RWStructuredBuffer<uint>` (kept in UNORDERED_ACCESS, survive level
  switches with the pool): `physOwner` (physical→virtual, or INVALID), `physLastFrame` (LRU
  clock), `freeList`, `needsRender` (Step-22 input), `allocCounters[4]` (free/needs/fail/
  resident). Four compute shaders run each frame in `RecordPageAllocate` right after
  `RecordPageRequest` (same CL — request buffer stays UAV between them): **init** (one-shot:
  clear page table + mark all pages free), **touch** (per virtual page: keep resident+requested
  pages alive via `physLastFrame=curFrame`, + reset counters), **build-free** (per physical
  page: evict pages unrequested for ≥`kLruFrameThreshold=16` frames — clear their page-table
  entry + release — and append free pages to the free list), **allocate** (per virtual page:
  requested-but-not-resident → atomic-pop a free page, write `pageTable[v]=resident|phys`, claim
  owner/last-frame, append to needs-render; pool-full → fail counter). Page-table entry packing
  unchanged (bit31 resident | bits0-15 physical). UAV barriers between the 4 ordered passes. Gate
  is still `g_vsmPageRequestEnabled` (default OFF; Ctrl+V) — add-dormant (nothing samples the page
  table or renders pages until Steps 21/22). Debug readback now reports request mip-histogram +
  `resident`/`newThisFrame`/`fail`. **VERIFIED (gate temp-ON, reverted): both configs 0/0;
  `--scene-stress-gbv=120` + `--scene-stress=300` CLEAN (only known-noise {939,940,1006,1358} —
  the cross-frame UAV alloc state added NO new GBV surface, the doc's flagged pitfall); all 6 VSM
  shaders compile.** Caching confirmed across churn samples: demo (872 requested) →
  `resident=879 newThisFrame=4 fail=0` (nearly all cached — only 4 new allocs, fits pool); another
  level → `request=235 resident=421 newThisFrame=0` (stable request set fully served from cache).
  DEFERRED: **Rung-1 invalidation hooks** (light-moved / static-caster-changed → invalidate that
  light's/overlapping pages) belong to **Step 23** (they need Steps 21/22 rendering to matter);
  Step 20 is pure request→resident LRU allocation. Screen-space debug-viz (camera-follow visual)
  still deferred — verified via the resident/new/fail counters instead. NEXT = **Step 21**
  (virtual→physical sampling in the local-light shadow samplers, behind a toggle, parity-checked).

### Step 21 — Virtual→physical sampling in the shadow samplers

- **Goal:** shading reads through the page table. Do this behind a runtime toggle and validate
  parity against the Rung-1 path before rendering into pages.
- **Changes:** update the shadow sampling for the LOCAL lights — `spotlight_cs.hlsl`,
  `pointlight_cs.hlsl`, `glass.hlsl` — to: project into virtual space → look up the page table →
  sample the physical page (with the same depth-compare + PCF). Handle not-resident (fall back
  to lit, or to a coarser resident level). Mirror the existing
  `PointShadowFactor`/`ComputeSpotShadow` structure; pass page-table SRV + pool SRV + page
  constants like the spot `invShadowSize` plumbing. **Directional/CSM sampling in the lighting
  pass is NOT touched here — it stays on the current cascade path until Step 24 moves directional
  onto the clipmap + page table.**
- **Verify (runtime-compiled → must run):** with the pool pre-filled from the Rung-1 renderer
  (temporary bridge), the virtual sampler must match the Rung-1 image. `--scene-stress` Debug
  CLEAN; GBV clean (new SRV-table slots).
- **DONE for SPOT (uncommitted) 2026-07-05 — point + glass DEFERRED.** No Rung-1 bridge exists
  (Rung 1 skipped), so Step 22 fills the pages and Step 21 samples them (done together). Shared
  `shaders/vsm_sample.hlsli`: `VsmSpotShadow` / `VsmPointShadow` / `VsmSampleNDC` — project into
  the light's virtual space, mip-select by camera distance (mirrors request/setup), page-table
  lookup, and `SampleCmpLevelZero` the physical pool page with a 3×3 PCF **clamped to the page**
  (no neighbour bleed); not-resident → lit fallback. `shaders/vsm_addressing.hlsli` holds the
  shared addressing (`VsmPageId`/`VsmDecodePage`/`VsmSelectLevel`). **SPOT wired end-to-end:**
  `spotlight_cs.hlsl` root-sig SRV table 7→9 (+t7 `StructuredBuffer<uint>` page table, +t8
  `Texture2D` pool), CB gained `useVsm` + `vsmRefDist` (reflection-based `UpdateCBField`, so no
  manual offsets), and `ComputeSpotShadow` branches to `VsmSpotShadow` when `useVsm`. `Pass_
  SpotLights` binds the pool + page-table SRVs (persistent, valid post-load) and sets `useVsm =
  g_vsmPageRequestEnabled`. Render-graph state: the spot pass **declares pool + page-table SRV**
  (both branches: active adds a `pVsmPageRender` prereq for fresh content) so they are readable on
  entry; `ResourceStateDeclList` capacity 8→**10** (spot now has 9 decls). **VERIFIED (gate temp
  ON): both configs 0/0; `--scene-stress-gbv=40` + `--scene-stress=200` CLEAN — only known-noise
  {939,940,1006,1358}; spot shader compiles.** Two bugs fixed en route: setup shader `b0`
  collision with the alloc-common cbuffer (dropped the include, inlined `VSM_INVALID`); GBV id=538
  (pool cleared while in SRV — `RecordPageRender` now transitions the pool → DEPTH_WRITE itself
  rather than via a graph decl the non-MT pass never applied). **DEFERRED (documented):** POINT +
  GLASS sampling (point needs the 6-face viewProj buffer + a different depth scheme; glass is the
  transparent pass — both still sample the atlas, which is still rendered, so they stay correct),
  and the automated **parity readback** (atlas-vs-VSM diff) — spot VSM parity is the user's visual
  A/B via **Ctrl+V**. Visual sign-off REQUIRED (I cannot verify the image headless).
- **POINT + GLASS DONE (uncommitted) 2026-07-06.** `VsmPointShadow` reconstructs the cube face's
  `LookAtLH*PerspectiveFovLH(90)` view-proj in-shader (major-axis face select — the 6 faces tile
  the sphere exactly), so no per-face matrix buffer is needed; it feeds `VsmSampleNDC` at local view
  `8+slot*6+face`. `pointlight_cs.hlsl` gains t7/t8 (page table + pool) + `useVsm`/`vsmRefDist` and
  branches `PointShadowFactor` (world-space bias = pull toward the light before projecting). Glass
  (`glass.hlsl`, the transparent VS+PS): root-sig SRV table 9→11 (+t9/t10), `vsmParams` in the
  `GlassView` CB, and both `SampleSpotShadow` + `PointShadowFactor` branch to the VSM helpers; the
  VSM SRVs are plumbed to the frame-less glass draw via `Renderer::SetVsmShadowSrvs` (set in
  `Pass_Transparent`, read in `TransparentStaticMesh::Render`). Point/spot/glass all reuse the same
  `vsm_sample.hlsli`. **VERIFIED: both configs 0/0; shaders compile; `--scene-stress-gbv=40` +
  `--scene-stress=200` CLEAN (only {939,940,1006,1358} — the new SRV-table slots on point + glass
  added no new surface).** Fixed a declaration-order crash (point CB `PointLightFrame` was below
  `PointShadowFactor`; moved it above). All three lighting paths now sample VSM when Ctrl+V is on.
- **USER-TESTED + TUNED 2026-07-06.** Core spot path renders correctly; Release CPU ~0.5 ms even
  near a dense spot grid. Post-test fixes: **mip chain** (request marks the selected level + all
  coarser; sampler `VsmSampleNDC` walks to a coarser resident level) — killed the distance
  flicker; **`kLodRefDist` 10→5** + **`kRequestDownscale` 8→4** — killed the checkerboard +
  right-sized pages (872→~130 resident); **resident-only render iteration** (a `physOwner`
  readback ring so `RecordPageRender` skips ~free pool pages — the 8 ms→~0.5 ms Release win; small
  ~3-frame edge pop-in of freshly-revealed pages while moving). **Two limitations the user
  ACCEPTED (2026-07-06):** (1) **8-spot cap** — a scene with >`kMaxShadowedSpotLights` spots
  (demo.json has 9) toggles a whole spot's shadow as the camera moves and `SelectShadowedSpots`
  re-picks the top 8; PRE-EXISTING (identical on the atlas path), not a VSM bug — proper fix is
  the Rung-2 uncap. (2) **`GpuInstancedModels` don't cast VSM shadows** (excluded from the indirect
  caster path since Step 6) — the atlas still shadows them when VSM is off.

### Step 22 — Render casters into allocated physical pages (reuses Rung 0)

- **Goal:** fill the "needs-render" pages from Step 20 — using Rung 0's cull + indirect machinery,
  NOT a from-scratch CPU raster.
- **Changes:** extend Rung 0's GPU cull (Step 4) to cull **per page**: for each page in the
  needs-render list, the compute pass tests caster bounds against that page's virtual rect and
  emits `ExecuteIndirect` args for it (reuse Step 3's command signature + Step 1's instance
  buffer + Step 5's indirect VS). Draw with viewport/scissor set to the physical page's location
  in the pool and a projection mapping the page's virtual sub-rect → the physical page (an
  off-center projection per page, in a per-page constants buffer). Because Step 23 makes almost all
  pages cached, the per-frame needs-render set is tiny — so even the O(pages) `ExecuteIndirect`
  binding stays small. (Without Rung 0 this step was the CPU-bound "hard part"; on Rung 0 it is
  cull-per-page + indirect.)
- **Verify:** switch sampling (Step 21) to read the pages this pass rendered (drop the Rung-0
  bridge); image must still match. `--scene-stress` Debug CLEAN; GBV clean (per-page DSV /
  viewport + pool DEPTH_WRITE↔read barriers + the cull-UAV→indirect-arg transitions are the
  surface).
- **DONE (uncommitted) 2026-07-05 — full GPU-driven, race-free.** New `Main_VsmPageRender` pass
  (after `Main_VsmPageRequest`, only wired when the gate is on). `shaders/vsm_page_setup_cs.hlsl`:
  one thread per PHYSICAL page — resident pages decode their virtual page → (view, level, px, py),
  build an **off-center projection** `pageProj = lightVP[view] × S` (S maps the page's NDC sub-rect
  to full [-1,1], z preserved so the stored depth == the sampler's), and **copy that view's Rung-0
  cull args** (VSM local view v → Rung-0 slot v+4) into a per-page `DRAW_INDEXED_ARGUMENTS` slot;
  free pages get InstanceCount 0. `VirtualShadowMap::RecordPageRender`: setup dispatch (reads Rung-0
  args as an SRV — transitioned from INDIRECT_ARGUMENT, safe post-shadow-pass), then transition
  args→INDIRECT_ARGUMENT + pageProj→VERTEX_AND_CONSTANT_BUFFER, clear the whole pool, and loop the
  1024 pool pages setting each cell's viewport/scissor + its `pageProj` as the b1 root CBV
  (256-byte stride) and one `ExecuteIndirect` per mesh-group — **reusing the proven
  `RecordIndirectShadowDraws` machinery** (instance buffer t0, Rung-0 visible list as the
  per-instance stream, `shadow_indirect_csm` PSO). Correctness-first: the whole pool is cleared +
  every resident page re-rendered each frame (Step 23 will gate to changed pages only, and reduce
  the ~O(1024×groups) `ExecuteIndirect` CPU cost). Exposed `ShadowGpuData::GroupMeshes()`. **VERIFIED
  (gate temp ON): both configs 0/0; `--scene-stress-gbv=40` + `--scene-stress=200` CLEAN — the new
  DSV pass, per-page viewport, pool DEPTH_WRITE↔SRV round-trip, and the Rung-0-arg cross-pass read
  added NO new GBV IDs (only known noise).** Visual sign-off (via Step 21 spot sampling) REQUIRED.

### Step 23 — Page-level caching (the payoff)

- **Goal:** a page re-renders ONLY when newly allocated, or a caster overlapping it moved, or
  the light moved — reusing Rung 1's classification at page granularity.
- **Changes:** wire Rung 1's static/dynamic + light dirty-versions into Step 20's invalidation:
  static pages persist untouched; a dynamic caster's movement invalidates only the pages its
  bounds overlap (this frame + last frame's bounds). Per-frame render list becomes tiny for
  mostly-static scenes.
- **Verify:** debug counter of "pages rendered this frame" → near-zero when camera + scene are
  static, small when a dynamic object moves. `--scene-stress` Debug CLEAN. Measure vs Rung 1.
- **NOT DONE — superseded for now by the MEGA-BUFFER (2026-07-06, user's pick over Step 23).**
  Rationale: with hardware raster, "render only changed pages" needs the CPU to iterate a
  GPU-computed changed-page list, which requires a GPU→CPU readback (the virtual→physical map is
  GPU-side) → the same ~3-frame latency as `g_residentIterOnly` (mover ghosting). The clean
  zero-latency version is a software rasterizer (Nanite), out of scope. So instead we attacked the
  **per-page CPU submission cost** directly, latency-free: **`ShadowGpuData::EnsureMegaBuffer`**
  concatenates all caster mesh groups into ONE VB + ONE IB (built once on the GPU-idle level-load
  upload CL; uniform-stride + R32 only, else per-group fallback), so `RecordPageRender` binds
  geometry ONCE and issues a single `ExecuteIndirect(maxCount=groups)` per page instead of a
  bind+draw per (page, mesh-group) — ~4x fewer D3D calls, **same-frame, no flicker/latency**. The
  VSM setup shader (`vsm_page_setup_cs`) bakes each group's mega vertex/index offset into the copied
  draw args (`gGroupMega`), so the args self-describe into the mega buffer. Enabler: **mesh VB/IB
  are now created in COMMON** (they decay to COMMON anyway; every reader — IA, RT BLAS, the mega
  copy — implicitly promotes), which is what lets the load-CL copy use implicit COMMON→COPY_SOURCE
  promotion robustly for both fresh and cross-level-cached meshes (no barriers, no state guessing).
  Verified: both configs 0/0, `--scene-stress-gbv=40` + `--scene-stress=200` CLEAN. The real Step 23
  (page-level caching) remains the eventual CPU win once a same-frame changed-set mechanism exists.

### Step 24 — Directional clipmap (REPLACES CSM — this is where cascades are eliminated)

- **Goal:** crisp directional shadows at distance + camera-move caching (only edge pages
  rebuild on recenter, vs. a whole cascade). **This is the step that removes cascades:** through
  Steps 18–23 directional ran on the untouched CSM (`Pass_CSM`); here it moves onto the clipmap +
  the shared page pool, and `UpdateCascades`/`Pass_CSM` are deleted. End state = no cascades.
- **Changes:** replace the 4-cascade `UpdateCascades`/`Pass_CSM` with a virtual clipmap: N
  nested levels centered on the camera, each a virtual shadow map addressed through the same
  page pool + table (add directional as clipmap-level "views" to the request/alloc/sample/render
  steps, mip-selected by distance like the local lights), page-cached across camera motion.
  Sampling picks the clipmap level by distance. Large; a standalone sub-milestone.
- **Verify:** directional shadow parity/quality vs CSM at near range, better at distance; page
  churn only at level edges when the camera moves. Full stress + GBV.

### Step 25 — (Optional) SMRT-style soft shadows

- Replace PCF with a shadow-space ray march for contact-hardening penumbra (UE's SMRT).
  Independent polish, orthogonal to addressing. Note as future.

### Step 26 — Verify, measure, gate

- Both builds 0/0; CRLF/LF clean; `--scene-stress` Debug CLEAN; `--scene-stress-gbv` clean.
  Keep the Rung-0 (non-virtual indirect) path behind a runtime toggle (F-key) for A/B.
  **Gate:** virtual addressing must be a net win over plain Rung 0 indirect shadows on a
  representative scene (crisper distant shadows and/or lower cost from page caching). If it's
  not — e.g. the page-management overhead exceeds the savings at this scene scale — record the
  finding and stop at Rung 0; VSM pays off most at large scale / many lights. Document results
  in this doc + the memory file.

---

## Cross-Cutting Notes

- **Rung ordering is load-bearing.** Do the Steps 10/11 foundation first (both Rung 0 Step 7 and
  Rung 2 Steps 20/23 depend on it). Then Rung 0 (GPU-driven indirect) — Rung 2 reuses its instance
  buffer, cull compute, command signature, and indirect VS. Rung 1's Steps 12–15 caching is an
  optional GPU-raster branch, not a prerequisite for Rung 2.
- **The WAR lesson applies.** Any cache/tag/instance/page-table state read across frames in
  flight must be keyed by stable identity (lightId/casterId, not slot) or per-frame/
  ring-buffered — the same trap as the spot "wrong shadow RT" bug. The persistent instance
  buffer (Step 1), indirect arg/count buffers (Step 3), copy sources (Step 14), and the physical pool
  (Rung 2's page steps) are single resources read while N frames are in flight; ring-buffer or guard
  their state transitions in the render graph. The cull-UAV → `ExecuteIndirect`-arg
  produce/consume (UAV → INDIRECT_ARGUMENT) is a new, must-get-right transition.
- **GBV is the acceptance gate for every new resource/barrier/descriptor.** New DSVs, the
  static atlases, the COPY barriers, the pool UAV cross-frame state, and every new SRV-table
  slot are exactly what GBV catches. Run `--scene-stress-gbv` (Debug) on the steps that add
  them.
- **Idle-only realloc.** Static atlases (Step 12) and the physical pool (Step 18) follow the existing
  "free/recreate only at GPU idle" rule (mirror `JsonLevel::Load` pre-grow / `RebuildLights`).

## Non-Goals

- Nanite-equivalent software rasterization / sub-triangle page granularity. Rung 0 DOES build
  GPU-driven *submission* (GPU cull + `ExecuteIndirect`), but not a software rasterizer — so
  Rung 2 stays VSM-*lite* (page granularity, hardware raster).
- GPU-driven the MAIN gbuffer pass (Step 9) — noted, not built here; it needs bindless material
  indexing the shadow path deliberately avoids.
- Ray-traced shadows (the DXR path is a separate track — see `rt_reflections` docs).
- Removing the fixed shadowed-light caps in Rung 1 (caching makes each light cheap, but the
  atlas slot counts `kMaxShadowedSpotLights`/`kMaxShadowedPointLights` stay; uncapping is a
  Rung 2 clipmap/pool consequence, not a Rung 1 goal).
