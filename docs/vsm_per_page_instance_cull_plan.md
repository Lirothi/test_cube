# VSM per-page instance culling plan

**Goal.** Kill the VSM per-page draw overdraw that folding GPU-instanced casters exposed. With GI
folded (`docs/gpu_instanced_vsm_integration_plan.md`), `Pass_VsmPageRender` jumped to **~4.49 ms GPU**
(~73% of a 6.2 ms frame) while `Pass_ShadowCull` (the cull + GI scatter) stayed at **0.05 ms** — so the
GPU-driven machinery is free and the cost is entirely wasted draw work downstream.

## Diagnosis (the smoking gun)

`shaders/vsm_page_setup_cs.hlsl` copies each page's **whole VIEW** cull args unchanged:

```
// per resident page p, per group g:
PageDrawArgs[p][g].InstanceCount        = Rung0Args[view][g].InstanceCount   // = casters in the WHOLE light frustum
PageDrawArgs[p][g].StartInstanceLocation = view's shared visible-list slice
```

So every resident page of a light re-draws **all casters visible in that light's entire frustum**, then
relies on the 128² viewport/scissor to discard everything outside the page's cell. The pixels are
clipped but the **vertex work is already spent**: `Σ_pages (view-visible casters)`. Dense GI (200 teapots)
× N resident pages × M views = the 4.49 ms. A page only needs the instances whose projection actually
lands in its cell.

## Design — per-page cull in the setup shader

Replace "copy the view's args" with "cull the casters to *this page's* frustum." The setup already runs
one thread per pool page and builds the page's off-center projection `pm`. Add:

1. Extract the page's 6 frustum planes from `pm` (Gribb–Hartmann, row-vector convention). The
   positive-vertex AABB test is scale-invariant, so unnormalized planes reuse the exact `Intersects`
   logic from `shaders/shadow_cull_cs.hlsl`.
2. Cull **all** `count_` casters against those planes using `boundsUnified_` (world AABBs the scatter
   already fills — static casters too). Two passes over the caster list (count per mesh-group →
   prefix-sum within the page's slice → scatter caster ids), all owned by the single page thread (no
   atomics).
3. Write per-page `InstanceCount = perGroupCount[g]` and `StartInstanceLocation = pageBase +
   perGroupBase[g]` into a **new per-page visible list** (`pageVisibleList_`, pool-pages × count_ uints).
   Keep `IndexCountPerInstance / StartIndex / BaseVertex` from the existing per-group constants.

The page render binds `pageVisibleList_` as the slot-1 per-instance stream instead of the shared
per-view list; everything else (mega VB/IB, ExecuteIndirect per page) is unchanged.

### Why this is the lever
- Draw work: `Σ_pages (view-visible casters)` → `Σ_pages (casters overlapping the page)`. For localized
  GI a ~10–30× cut in instances submitted; `Pass_VsmPageRender` should fall back toward its pre-GI cost.
- **Zero visual change** — a caster outside a page's cell is scissored out anyway; the conservative AABB
  test never drops a contributor. Pure waste removal. Helps *static* VSM casters as a bonus.
- The cull is cheap: ~resident-pages × count_ × a 6-plane test ≈ sub-ms. Makes the VSM draw independent
  of the Rung-0 per-view args (which stay for Legacy CSM/spot/point).
- It is exactly the "per-page/instance cull" future rung the GI plan deferred.

### Trade-off (accepted for v1)
First cut is **one thread per page** looping all casters — low GPU occupancy but tiny absolute work (all
threads hit the same L2-cached bounds). Escalate to a cooperative threadgroup-per-page cull only if
Step 3's measurement shows the cull itself on the profile.

## Steps (one per commit; build Debug+Release 0/0; `--scene-stress-gbv`; VSM visual A/B)

### Step 1 — plumbing (dormant, no-op)
Allocate `pageVisibleList_` (pool-pages × `count_` uints, DEFAULT/UAV) + its UAV in `renderHeap_` (grow
4→5 descriptors) + track `renderCasters_`; expose `ShadowGpuData::CasterGroupSrv()`. Not bound, not read
— pure allocation. **Verify:** both build 0/0; VSM unchanged (still ~4.49 ms), GBV clean.

### Step 2 — the flip
Rewrite the resident-page body of `vsm_page_setup_cs.hlsl` (extract page planes, two-pass cull, write
per-page list + args) with new `CasterBounds` (t2) + `CasterGroup` (t3) SRVs, `PageVisibleList` (u2) UAV,
and a `gNumCasters` CB field. In `RecordPageRender`: pass `count_` + stage `shadowGpu->UnifiedBoundsSrv(f)`
/ `CasterGroupSrv()` / the page-list UAV; swap the draw's slot-1 stream to `pageVisibleList_`; cycle it
UAV→VERTEX_AND_CONSTANT_BUFFER like `pageDrawArgs_`. **Verify:** both 0/0; `--scene-stress-gbv=120` (VSM)
CLEAN; **VsmPageRender collapses** (capture before/after with GI on); user A/B — VSM shadows byte-identical.

### Step 3 — measure, tune, docs — DONE
Measured in-engine (demo scene: 82 static + 200 GI casters, VSM local lights, DLSS Perf, 1024-page pool
~55% resident). No tuning needed and no threadgroup-per-page upgrade — the cull collapsed the GPU cost so
hard it no longer registers on the profile.

## Results (measured)

**Per-page cull (VSM + GI, before → after):**

| Pass | GPU before | GPU after | CPU before | CPU after |
|------|-----------:|----------:|-----------:|----------:|
| `Pass_VsmPageRender` | **4.49 ms** | **0.33 ms** (~13.6×) | 2.38 ms | 2.31 ms |
| `GPU.Frame` (whole)  | 6.18 ms | 2.04 ms | — | — |

The GPU draw overdraw is gone. `Pass_VsmPageRender` CPU barely moved (2.38 → 2.31 ms) because the cull
attacks per-instance *vertex* work, not the per-page CPU loop — see "New bottleneck" below.

**Legacy GI-fold bonus (GI folding off → on, VSM off), i.e. dropping the CPU RenderShadow tail:**

| Metric | GI off | GI on |
|--------|-------:|------:|
| `SpotShadow.PerLight` (CPU, aggregate ×32) | 8.08 ms | **6.96 ms** |
| `CSM.PerCascade` (CPU, ×4) | 0.77 ms | 0.59 ms |
| `Pass_CSM` (CPU) | 0.62 ms | 0.54 ms |
| `GPU.Frame` (whole) | 2.09 ms | 1.96 ms |

Overall FPS was unchanged (311 both) because the Legacy shadow passes record on parallel worker threads
and aren't the critical path there — the saving is real aggregate CPU work, just off the hot path.

## New bottleneck (follow-up rung, not in scope)
NOTE: these CPU numbers are **Debug config** — Release will cut CPU substantially and likely re-baseline
what's actually the bound (the GPU 4.49 → 0.33 ms win is config-independent — shaders compile the same at
runtime). With the GPU freed, in Debug **VSM is CPU-bound**: CPU.Frame 5.01 ms vs GPU.Frame 2.04 ms. The dominant VSM CPU
cost is `Pass_VsmPageRender` **2.31 ms** — the CPU loop over all 1024 pool pages (viewport + root-CBV +
one `ExecuteIndirect` per page), independent of instance count so the per-page cull didn't touch it.
Levers for a future rung: (a) the existing `g_residentIterOnly` toggle (skip the ~45% free pages via the
physOwner snapshot); (b) GPU-driven page iteration (one indirect dispatch/draw over the resident set
instead of a 1024-iteration CPU loop). Also `Scene::prepareQueue` / `SceneRenderQueue::Cull` scale with the
many VSM views — a separate CPU cost.

## Risks / notes
- **Per-page list state**: `pageVisibleList_` cycles UAV (setup write) → VERTEX_AND_CONSTANT_BUFFER (draw)
  each frame; single-buffered like `pageDrawArgs_` (GPU queue order + barriers cover WAR). GBV id flags a
  mismatch.
- **Local arrays** `perGroupCount/base[64]` per page thread — heavy registers, spills to local; fine at
  low page counts. The threadgroup variant (Step 3) moves these to groupshared if needed.
- **StartInstanceLocation range**: `p*count_ + base ≤ kPoolPageCount*count_` (≈288K) — well within UINT.
- **Rollback**: Step 1 is a dormant alloc; reverting Step 2 restores the copy-the-view-args behavior.
