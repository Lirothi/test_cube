# Enhanced Barriers Migration — Executor Plan

This document is written to be executed by an AI coding agent. Read the **Executor
Guide** first, then do the steps in order. Each step must leave **both** build
configurations green and, unless a step is explicitly the behavioral flip
(**Step 6**), must leave runtime behavior unchanged.

**Goal:** migrate the engine's GPU synchronization from **legacy resource barriers**
(`ID3D12GraphicsCommandList::ResourceBarrier` with `D3D12_RESOURCE_BARRIER` transition
structs, one `D3D12_RESOURCE_STATES` per resource) to **enhanced barriers**
(`ID3D12GraphicsCommandList7::Barrier` with `D3D12_BARRIER_GROUP`, splitting each
transition into orthogonal **sync / access / layout**). Keep a legacy fallback for
drivers/OS without enhanced-barrier support.

**Core strategy (keep the blast radius tiny):** the `ResourceStateTracker` public API
already speaks legacy `D3D12_RESOURCE_STATES`, and ~26 files call it that way. **Do NOT
change those call sites.** Instead, keep the legacy `D3D12_RESOURCE_STATES` vocabulary
at the API surface and translate to `(sync, access, layout)` *inside* the tracker at
barrier-emission time, behind a runtime flag. The migration then lives almost entirely
in `ResourceStateTracker`, the barrier-submit path, and resource creation.

---

## Executor Guide (read first)

**Repo:** `D:\Programming\test_cube`. Shared conventions live in
`docs/level_editor_HANDOFF.md` — read it for gating/CRLF/build rules; the essentials
are repeated here.

**Build (run BOTH after every step):** use the PowerShell tool (not bash — bash mangles
MSBuild `/t:` `/p:` switches).
```
& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "D:\Programming\test_cube\test_cube.sln" /t:Build /p:Configuration=Debug   /p:Platform=x64 /m /nologo "/clp:ErrorsOnly;Summary"
& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "D:\Programming\test_cube\test_cube.sln" /t:Build /p:Configuration=Release /p:Platform=x64 /m /nologo "/clp:ErrorsOnly;Summary"
```
`Debug|x64` compiles with `/WX` (warnings = errors) and `WITH_EDITOR=1`. `Release|x64`
is the non-editor build. Both must report `0 Warning(s) 0 Error(s)`.

**This is all ENGINE code — NOT `WITH_EDITOR`-gated.** The renderer, `ResourceStateTracker`,
`GraphicsDevice`, `Renderer`, and resource-creation paths ship in Release. Behavior must
be identical in both configs until the flip. Do not wrap any of this in `#if WITH_EDITOR`.

**Line endings:** all `.cpp/.h` in this repo are **CRLF**; markdown is LF. The Write
tool creates new files as **LF**, so any new source/header must be converted to CRLF.
Verify every touched file with a lone-LF check (must be 0):
```powershell
$b=[IO.File]::ReadAllBytes($path); $n=0; for($i=0;$i -lt $b.Length;$i++){ if($b[$i]-eq 10 -and ($i-eq 0 -or $b[$i-1]-ne 13)){$n++} }; "$path loneLF=$n"
```

**Verification per step (this migration is unusually verification-sensitive):**
1. Both builds `0/0`.
2. CRLF check on every touched file.
3. **Run `x64\Debug\test_cube.exe --scene-stress-gbv` (GPU-Based Validation).** GBV is
   the PRIMARY signal here: enhanced barriers are all about correct layouts/sync, and GBV
   validates exactly that at GPU-execution time. Today, a legacy-barrier engine trips GBV
   message **`id=1358` "Incompatible texture barrier layout"** — that noise is the
   *target*: after the flip it must be **gone**, with **no new** GBV errors introduced
   (`id=1387` stride, `id=958` destroyed-resource, or any barrier/layout id). Pre-existing
   glass draw-path noise (`939/940/1006`) is out of scope — note it, don't fix it here.
4. **Run `x64\Debug\test_cube.exe --scene-stress` (exit 0 = clean)** after any change to
   the barrier-emit / submit / resource-lifetime path, to catch device-removal regressions.
5. For the flip (Step 6) and any step that changes visible transitions, **run the app**
   and confirm the scene renders identically (G-buffer, shadows, transparent/glass,
   reflections, tonemap). A wrong barrier is often invisible until a specific frame — the
   scene-stress churn (resize / level switch / DLSS mode / render scale) is what shakes it
   out.

**Non-negotiable safety rule (learned the hard way):** any code that frees/reallocates a
GPU resource must run only when the GPU is idle (`Renderer::WaitForPreviousFrame()`). This
plan does not free resources on the hot path, but Step 2 changes *creation* — keep all
creation at load/resize/idle time as it is today.

**Do NOT commit.** The user commits. Leave the working tree with your changes for review.
Create a memory file `enhanced-barriers-progress.md` and update it as you complete steps.

**Driver/OS reality:** enhanced barriers require (a) an SDK whose `d3d12.h` declares the
enhanced-barrier types, (b) `ID3D12Device10` / `ID3D12GraphicsCommandList7` at runtime,
and (c) the driver reporting `D3D12_FEATURE_D3D12_OPTIONS12.EnhancedBarriersSupported`.
**A legacy fallback is mandatory** — do not drop support for machines without it. The
whole engine runs one model or the other per launch, chosen at device-init.

---

## Current State (verified)

- **Legacy only.** No enhanced-barrier API is used anywhere
  (`grep D3D12_BARRIER_GROUP / ::Barrier(` → none).
- **Interfaces cap at `ID3D12Device5` + `ID3D12GraphicsCommandList4`.**
  `GraphicsDevice` holds `device_` (`ID3D12Device`) and `device5_`
  (`ID3D12Device5`, "null if DXR unsupported") — mirror that nullable pattern for
  `device10_`. Command lists are used as base `ID3D12GraphicsCommandList` and
  QueryInterface'd to `4` via `Renderer::AsCmdList4(cl)` (`Renderer.cpp:36`) — mirror
  that for `AsCmdList7`.
- **No Agility SDK** is present (no `D3D12SDKVersion` export, no `D3D12Core.dll`, no
  `D3D12/` redist). The engine builds against the **system Windows SDK** `d3d12.h`.
  Enhanced-barrier types (`D3D12_BARRIER_GROUP`, `D3D12_TEXTURE_BARRIER`,
  `ID3D12GraphicsCommandList7`, `D3D12_FEATURE_D3D12_OPTIONS12`) exist in Windows SDK
  ≥ 10.0.20348. **Step 0 confirms the installed SDK has them; if not, adopt the Agility
  SDK** (that is the recommended path for a pinned version anyway).
- **Barriers are emitted in exactly these places** (the entire migration surface):
  1. `ResourceStateTracker::Transition` (`ResourceStateTracker.cpp:119-125`) — the
     intra-command-list transition (`before → after` within one CL).
  2. `ResourceStateTracker::AppendAcquireBarriers` (`.cpp:230-236`) — the "acquire"
     barriers at CL boundaries (global known state → this CL's first-use), collected into
     a `std::vector<D3D12_RESOURCE_BARRIER>` and submitted by the caller.
  3. The submit path in `Renderer.cpp:736-756` — where `AppendAcquireBarriers` output is
     recorded onto a command list, plus `ApplyFinalStates`.
  4. The present transitions in `Renderer.cpp` (`:769` RENDER_TARGET→PRESENT, `:871`
     PRESENT→RENDER_TARGET, and the `SetResourceState(..., PRESENT/RENDER_TARGET)` at
     `:251/:773/:780/:875`).
  5. `Renderer::UAVBarrier` (`Renderer.cpp`) — `D3D12_RESOURCE_BARRIER_TYPE_UAV`.
  6. Direct barriers in upload/creation paths: `materials/Texture2D.cpp`,
     `materials/TextureCube.cpp`, `materials/UploadManager.h` (COPY_DEST↔SHADER_RESOURCE),
     and `rendering/rt/AccelerationStructure.cpp` (RT scratch/AS states — **special**,
     see Cross-Cutting).
- **The tracker is built for parallel recording** (per-thread lanes, submit-time barrier
  stitching; see the CONTRACT comment in `ResourceStateTracker.h`). The enhanced path must
  preserve that contract exactly — it only changes *what struct* is emitted, not *when* or
  *by which thread*.
- **`D3D12_RESOURCE_STATES` appears in ~26 files.** The strategy keeps all of them
  unchanged: they stay the tracker's vocabulary; translation happens inside the tracker.

---

## Design

Add a device-init runtime flag `enhancedBarriers_` = `EnhancedBarriersSupported &&
CommandList7 available && (not force-disabled)`. Everything below branches on it; when
false the engine emits the exact legacy barriers it does today.

Add one translation layer: `LegacyStateToBarrier(D3D12_RESOURCE_STATES, bool isBuffer)`
→ `{ D3D12_BARRIER_SYNC, D3D12_BARRIER_ACCESS, D3D12_BARRIER_LAYOUT }`. Textures use all
three; buffers omit layout (`D3D12_BARRIER_BUFFER_BARRIER` has no layout). A representative
mapping (validate each against the D3D12 enhanced-barriers spec + GBV — combined read
states and sync scopes are the subtle part):

| Legacy `D3D12_RESOURCE_STATES` | Sync | Access | Layout (textures) |
|---|---|---|---|
| `COMMON` / `PRESENT` | `SYNC_ALL` (or `NONE` at start) | `ACCESS_COMMON` | `LAYOUT_COMMON` (present: `LAYOUT_PRESENT`) |
| `RENDER_TARGET` | `SYNC_RENDER_TARGET` | `ACCESS_RENDER_TARGET` | `LAYOUT_RENDER_TARGET` |
| `DEPTH_WRITE` | `SYNC_DEPTH_STENCIL` | `ACCESS_DEPTH_STENCIL_WRITE` | `LAYOUT_DEPTH_STENCIL_WRITE` |
| `DEPTH_READ` | `SYNC_DEPTH_STENCIL` | `ACCESS_DEPTH_STENCIL_READ` | `LAYOUT_DEPTH_STENCIL_READ` |
| `PIXEL_SHADER_RESOURCE` | `SYNC_PIXEL_SHADING` | `ACCESS_SHADER_RESOURCE` | `LAYOUT_SHADER_RESOURCE` |
| `NON_PIXEL_SHADER_RESOURCE` | `SYNC_(COMPUTE/VS/etc)` | `ACCESS_SHADER_RESOURCE` | `LAYOUT_SHADER_RESOURCE` |
| `ALL_SHADER_RESOURCE` (both above) | `SYNC_ALL_SHADING` | `ACCESS_SHADER_RESOURCE` | `LAYOUT_SHADER_RESOURCE` |
| `UNORDERED_ACCESS` | `SYNC_(COMPUTE\|PIXEL)` | `ACCESS_UNORDERED_ACCESS` | `LAYOUT_UNORDERED_ACCESS` |
| `COPY_DEST` | `SYNC_COPY` | `ACCESS_COPY_DEST` | `LAYOUT_COPY_DEST` |
| `COPY_SOURCE` | `SYNC_COPY` | `ACCESS_COPY_SOURCE` | `LAYOUT_COPY_SOURCE` |
| `GENERIC_READ` (upload buffers) | `SYNC_ALL` | `ACCESS_(CONSTANT_BUFFER\|SHADER_RESOURCE\|...)` | — (buffer) |
| `RAYTRACING_ACCELERATION_STRUCTURE` | `SYNC_RAYTRACING` / `BUILD_RTAS` | `ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ/WRITE` | — (buffer) |

Conservative first cut: when a state maps to multiple stages, use the widest `SYNC_*`
that is still correct (e.g. `SYNC_ALL_SHADING`, or `SYNC_ALL`). Correctness first;
Step 7 narrows for perf. Enhanced barriers also allow `LAYOUT_UNDEFINED` as `LayoutBefore`
for discard semantics — use it only where the legacy code relied on common-state promotion
/ didn't care about prior contents; otherwise carry the real previous layout (the tracker
already knows the previous `D3D12_RESOURCE_STATES`).

Texture-vs-buffer: the tracker must know each resource's kind to pick
`D3D12_TEXTURE_BARRIER` vs `D3D12_BUFFER_BARRIER`. Get it from
`ID3D12Resource::GetDesc().Dimension` (cache per resource to avoid a call per barrier), or
extend `SetResourceState`/creation to record a per-resource `isBuffer` bit.

---

### Step 0 — SDK / interface readiness + feature detection (no behavior change)

Establish the capability and a default-OFF flag; emit nothing enhanced yet.

- Confirm the toolchain sees enhanced-barrier types: a throwaway TU referencing
  `D3D12_BARRIER_GROUP` / `ID3D12GraphicsCommandList7` / `D3D12_FEATURE_D3D12_OPTIONS12`
  must compile. **If it does not, adopt the Agility SDK** (add the `D3D12SDKVersion` /
  `D3D12SDKPath` exports, ship `D3D12Core.dll` in the output dir, bump the referenced
  version) — do that as its own sub-step and re-verify both builds + a normal run before
  continuing. The Agility SDK bump can itself change driver/runtime behavior, so treat it
  as a real step.
- `GraphicsDevice`: after creating the device, QueryInterface `ID3D12Device10 device10_`
  (nullable, mirroring `device5_`). Query
  `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, ...)` →
  `options12.EnhancedBarriersSupported`.
- `Renderer`: add `ID3D12GraphicsCommandList7* AsCmdList7(cl)` mirroring `AsCmdList4`.
- Add `bool enhancedBarriers_` set at init to
  `EnhancedBarriersSupported && device10_ && AsCmdList7(probe) != nullptr`, AND a force-off
  override (a `--legacy-barriers` command-line flag or a compile constant) so the legacy
  path stays testable and is the diff-bisect baseline. **Leave `enhancedBarriers_`
  reported but treat it as OFF everywhere for now** (no consumer branches on it yet).

Acceptance: both builds `0/0`; run — identical behavior; log the detected capability.

### Step 1 — Translation layer + resource-kind classification (dormant)

Pure additions, not yet called on any hot path.

- Add `LegacyStateToBarrier(...)` (the mapping table above) as a free function or a
  `ResourceStateTracker` static. Unit-exercise it if the repo has a test hook; otherwise
  keep it self-contained.
- Add per-resource buffer/texture classification the tracker can consult (cached
  `GetDesc().Dimension`, or an `isBuffer` recorded at `SetResourceState`/creation time).

Acceptance: both builds `0/0`; behavior unchanged (nothing calls the new code yet).

### Step 2 — Create textures with an initial layout (gated; flag OFF ⇒ unchanged)

Enhanced barriers track texture **layout**; a texture's *initial* layout must be declared
at creation (`ID3D12Device10::CreateCommittedResource3` / `CreatePlacedResource2`, which
take `D3D12_BARRIER_LAYOUT InitialLayout`). Buffers have no layout and are created as today.

- Route texture creation through a helper that, **when `enhancedBarriers_`**, uses
  `CreateCommittedResource3(..., D3D12_BARRIER_LAYOUT_COMMON, ...)` (or the target initial
  layout) and seeds the tracker's known state accordingly; **when not**, uses the existing
  `CreateCommittedResource` path byte-for-byte.
- Cover every texture creation site: `RenderTargetManager` (all deferred targets, shadow
  atlases, the new point cube atlas), swapchain buffers/depth, `materials/Texture2D`,
  `materials/TextureCube`, and any RT targets.
- Keep the flag OFF ⇒ no path change.

Acceptance: both builds `0/0`; run — unchanged (flag off). With the flag *temporarily*
forced on for a smoke test, textures must still create (no `CreateCommittedResource3`
failures); revert the force before finishing.

### Step 3 — Enhanced emission inside the tracker (gated; flag OFF ⇒ unchanged)

Branch the two emit points on `enhancedBarriers_`.

- `Transition`: when enhanced, build a `D3D12_TEXTURE_BARRIER` or `D3D12_BUFFER_BARRIER`
  from `LegacyStateToBarrier(before)` → `LegacyStateToBarrier(after)`, wrap in a
  `D3D12_BARRIER_GROUP`, call `AsCmdList7(cl)->Barrier(1, &group)`. Else the current
  `ResourceBarrier` path.
- `AppendAcquireBarriers`: when enhanced, accumulate `D3D12_TEXTURE_BARRIER` /
  `D3D12_BUFFER_BARRIER` lists (grouped by type) instead of `D3D12_RESOURCE_BARRIER`. This
  changes the *output container's* type — introduce a small `AcquireBarrierBatch` that
  holds both representations (or templatize) so the submit site (Step 4) can consume
  whichever is active. Preserve the exact ordering/`knownStates_` update semantics.
- Keep the `firstUse`/`current`/`knownStates_` bookkeeping in `D3D12_RESOURCE_STATES`
  (unchanged) — only the emitted struct differs. This is what keeps the parallel-recording
  contract and all 26 call sites intact.

Acceptance: both builds `0/0`; `--scene-stress` exit 0; behavior unchanged (flag off).

### Step 4 — Submit path, present, UAV, and direct sites (gated; flag OFF ⇒ unchanged)

- `Renderer.cpp:736-756`: record the acquire barriers via `Barrier()` when enhanced, else
  `ResourceBarrier`. Consume the `AcquireBarrierBatch` from Step 3.
- Present (`Renderer.cpp:769/871` + the `SetResourceState` present/RT seeds): when enhanced,
  transition the backbuffer with a `D3D12_TEXTURE_BARRIER` to/from `LAYOUT_PRESENT`
  (verify: flip-model present layout). Keep the legacy `STATE_PRESENT` path otherwise.
- `Renderer::UAVBarrier`: when enhanced, emit a `D3D12_BUFFER_BARRIER`/`TEXTURE_BARRIER`
  with `ACCESS_UNORDERED_ACCESS` before+after and appropriate sync (or a
  `D3D12_GLOBAL_BARRIER` for a blanket UAV barrier). Else the legacy UAV barrier.
- Direct upload sites (`Texture2D`, `TextureCube`, `UploadManager`): route their
  COPY_DEST↔SHADER_RESOURCE transitions through the tracker (preferred) or add a gated
  enhanced path.

Acceptance: both builds `0/0`; `--scene-stress` exit 0; behavior unchanged (flag off).

### Step 5 — Acceleration-structure & special states (gated)

RT is special: AS resources live in `RAYTRACING_ACCELERATION_STRUCTURE`, scratch is
`UNORDERED_ACCESS`/`COMMON`, and (per the RT notes) some AS work bypasses the state tracker.

- Map AS/scratch states in the table (`SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE`,
  `ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ/WRITE`; AS is a buffer → no layout).
- Audit `AccelerationStructure.cpp` for direct barriers and give them a gated enhanced path.
  Preserve the existing "scratch = COMMON not UAV" and AS-bypasses-tracker behavior.

Acceptance: both builds `0/0`; `--scene-stress` exit 0 (RT paths exercised); with RT
reflections on (F5), no device removal.

### Step 6 — The flip (behavioral change) + validation

Set `enhancedBarriers_` to its real value (supported & available) — the engine now uses
enhanced barriers on capable machines, legacy on the rest.

Acceptance (this is the payoff — be thorough):
- Both builds `0/0`.
- **`--scene-stress-gbv`:** verdict CLEAN; **`id=1358` layout warnings GONE**; **no new**
  GBV errors (no barrier/layout/`id=1387`/`id=958`). Pre-existing `939/940/1006` glass
  noise may remain (out of scope).
- **`--scene-stress` exit 0** across the full churn (resize / level switch / DLSS / render
  scale / editor spawn), and again with barriers *force-disabled* (the legacy path must
  still pass — the fallback is not dead code).
- **Run the app:** G-buffer, CSM + spot shadows, transparent/glass, SSR + RT reflections,
  tonemap all render identically to the legacy build. Compare screenshots legacy vs
  enhanced at the same camera.

### Step 7 — Tighten sync (optional, perf)

Now that sync/access/layout are explicit, replace the conservative `SYNC_ALL*` fallbacks
with the minimal correct scopes (e.g. a G-buffer→lighting transition needs only
`SYNC_RENDER_TARGET`→`SYNC_(PIXEL/COMPUTE)_SHADING`, not `SYNC_ALL`). Optionally add a
tracker API to express sync/access directly where the legacy-state mapping is too coarse.
Measure with the profiler; each narrowing must keep GBV clean + `--scene-stress` exit 0.

---

## Cross-Cutting Notes

- **Mandatory legacy fallback.** Everything is gated on `enhancedBarriers_`. Machines
  without `EnhancedBarriersSupported` / `CommandList7` run the untouched legacy path. Keep
  a force-off switch so the legacy path is always testable and bisectable.
- **Do not change the tracker's threading contract.** Enhanced emission changes only the
  struct type at the two emit points and the submit site; the per-thread lanes, first-use
  stitching, and submit-time single-threaded resolution stay exactly as documented in
  `ResourceStateTracker.h`. Re-read that CONTRACT before touching `Transition` /
  `AppendAcquireBarriers` / `ResetLanesForFrame`.
- **Layout tracking is the new failure mode.** Legacy conflated layout into state; enhanced
  makes it explicit, so a wrong `LayoutBefore` (not matching the resource's real layout)
  is a GPU hazard GBV will flag (that is literally what `id=1358` checks). The tracker
  already knows the previous `D3D12_RESOURCE_STATES` per resource — derive `LayoutBefore`
  from it; only use `LAYOUT_UNDEFINED` where contents are intentionally discarded.
- **Mixing models is legal but avoid it.** A resource may be transitioned by legacy OR
  enhanced barriers, but not tracked inconsistently. Because the engine picks ONE model per
  launch via `enhancedBarriers_`, there is no mixing within a run — keep it that way.
- **Subresources.** Legacy uses `D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES`; enhanced uses
  `D3D12_BARRIER_SUBRESOURCE_RANGE` (all-subresources = `{0, 0xffffffff, ...}` or
  `NumMipLevels=0`). The spot/point shadow atlases are arrays with per-slice DSVs/RTVs —
  mirror the existing per-subresource intent.
- **Verification bias.** A wrong barrier frequently renders correctly for many frames and
  only corrupts under specific timing — exactly the class of Release-only bug the
  `--scene-stress`(-gbv) harness exists to catch. Prefer it over eyeballing a static frame.
- **No new resource frees on the hot path.** Step 2 changes creation only (load/resize/idle
  time). Keep the "free/realloc only at GPU idle" rule for anything you add.

## Non-Goals / Future Refinements

- **Render-graph-native sync.** The render graph declares per-pass `D3D12_RESOURCE_STATES`;
  a deeper refactor could declare sync/access directly for tighter, pass-aware barriers.
  Out of scope — keep the legacy-state declarations and translate.
- **Split barriers / `BARRIER_SYNC_SPLIT`.** Overlapping work across a begin/end barrier
  pair for latency hiding — a perf refinement, not part of the migration.
- **Queue-ownership / multi-queue.** If async compute/copy queues are added later, enhanced
  barriers express cross-queue sync more cleanly; not in scope now.
- **Dropping the legacy path.** Only after enhanced barriers are proven on the full range of
  target hardware — keep the fallback indefinitely otherwise.
