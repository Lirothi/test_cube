# VSM page caching plan (Rung 1)

**Goal.** Stop re-rendering VSM pages whose content didn't change. Today `RecordPageRender` clears the
**whole 4096² pool** and re-rasterizes casters into **every resident page** every frame, even pages that
hold only static casters. Only pages that are newly allocated or overlap a moving caster actually need
re-rendering. Cache the rest.

**Honest expected win (this scene).** Per-page culling already removed the overdraw (`Pass_VsmPageRender`
GPU 4.49 → 0.38 ms). The 200 teapots are *dynamic* (rotate every frame) so caching can't skip them — only
the ~82 static casters get cached. So the **average** drop is modest (~0.38 → ~0.25 ms). The real value:
(1) **spike smoothing** — `max 0.97` vs `avg 0.38` is re-allocation churn on camera moves; caching cuts it;
(2) **scalability** — VSM cost becomes ∝ *dynamic* content, not total, so it stays flat as static geometry
grows. This is the structurally correct rung; the single-scene number undersells it.

## What's already there (the foundation)

- `physOwner_` (physical → virtual owner), `physLastFrame_`, `needsRender_` (append list of pages
  allocated this frame), `allocCounters_` — all persistent, "this IS the cache" per the header comment.
- The pool + page table survive level switches. Alloc runs a full page-management pipeline already.
- `ShadowGpuData::IsDynamicCaster()` (GI = true, static = false), `MoverCount()` (static movers this frame).
- The per-page cull (just added) already iterates casters per page in `vsm_page_setup_cs.hlsl` — it can
  compute "does any dynamic caster overlap this page" for free during the cull.

## Design — GPU-computed per-page dirty, selective clear + draw

**Dirty determination (in the setup shader, per page p, no readback):**
`dirty[p] = isNew || dynamicOverlap || forceAll`, where
- `isNew = (physOwner[p] != physOwnerPrev[p])` — page newly allocated / re-owned. `physOwnerPrev_` is a
  copy of `physOwner_` from last frame (`PhysLastFrame` can't be used — the *touch* pass stamps it for
  still-resident pages too, so it means "requested" not "new").
- `dynamicOverlap` — during the per-page cull, any overlapping caster has `casterDynamic[c] != 0`
  (`IsDynamicCaster`, static per Rebuild: GI = 1, static = 0).
- `forceAll` — CB flag set when `MoverCount() > 0` (a static caster moved) or on a Rebuild. Conservative
  full re-render that frame — covers static movement without per-caster prev-bounds tracking.

**Selective render:**
- Setup writes `perPageDirty_[p]`. For a **clean** page it writes `InstanceCount = 0` for all groups (the
  caster draw becomes a no-op) and skips the list scatter; for a **dirty** page, the normal per-page cull.
- **Clear** becomes one gated `DrawInstanced(4, kPoolPageCount)` at the full-pool viewport: a depth-only
  clear pipeline (depth ALWAYS, write z=1.0) whose VS reads `perPageDirty_[instanceID]` and emits the
  page's pool-cell quad if dirty, else a degenerate (zero-area) quad. Replaces the whole-pool
  `ClearDepthStencilView` — clean pages keep last frame's depth (that's the cache); dirty pages get
  cleared then redrawn.

Every page is cleared+drawn at least once when allocated (`isNew`), so no page ever shows garbage.

### Accepted v1 limitation
A *translating* dynamic caster leaves a ghost in the page it vacated (we invalidate its current pages, not
its previous ones). The demo's teapots **rotate in place** (center fixed → same pages), so no ghost here.
Proper fix (prev-bounds invalidation, like UE5) is a follow-up. `forceAll` on any static move is the safety
net for editor/gameplay moves.

## Steps (one per commit; build Debug+Release 0/0; VSM `--scene-stress-gbv`; visual A/B)

### Step 1 — plumbing (dormant)
`ShadowGpuData`: `casterDynamic_` ring (per-caster `IsDynamicCaster`, filled at Rebuild alongside
`casterGroup_`) + `CasterDynamicSrv()`. `VirtualShadowMap`: allocate `physOwnerPrev_` + `perPageDirty_`
(DEFAULT/UAV) + descriptors. Nothing reads them yet. **Verify:** both 0/0; VSM unchanged, GBV clean.

### Step 2 — the flip
Setup shader reads `physOwnerPrev` + `casterDynamic` + `forceAll` (CB), computes `dirty`, writes
`perPageDirty_`, gates the caster args (clean → InstanceCount 0). New `vsm_page_clear` depth pipeline +
the gated `DrawInstanced(4, kPoolPageCount)` replaces the whole-pool clear. `RecordPageRender` copies
`physOwner_ → physOwnerPrev_` each frame (post-setup) and passes `forceAll = MoverCount()>0`. **Verify:**
both 0/0; VSM `--scene-stress-gbv` CLEAN; static shadows cached + dynamic re-render (visual A/B); capture
`Pass_VsmPageRender` GPU avg + max before/after.

### Step 3 — measure, optional refine, docs
Capture avg + max. Optional: coarse skip — test the page frustum against the union AABB of dynamic casters
to skip the cull entirely for pages far from any mover (makes the setup cheaper too, not just the draw).
Update memory + this doc.

## Risks / notes
- **physOwnerPrev copy timing**: frame N setup reads last frame's copy; copy `physOwner → physOwnerPrev`
  AFTER the setup reads it. State: physOwner COPY_SOURCE, physOwnerPrev COPY_DEST → SRV next frame.
- **First VSM frame / level load**: every resident page is new → full render (correct init). `forceAll` on
  Rebuild covers caster-set changes.
- **Clean-page preservation**: relies on NOT clearing clean cells — the pool must stay DEPTH_WRITE-bound
  only where drawn; the depth-clear pipeline must use ALWAYS (LESS_EQUAL wouldn't overwrite to far).
- **Rollback**: Step 1 dormant; reverting Step 2 restores whole-pool clear + draw-all.
