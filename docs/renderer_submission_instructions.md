# Renderer submission instructions — safety, determinism, CL groups, eager submit (rev 2)

Working prompt for command-list management work. Self-contained: read the recap before
touching anything. Do ONE step at a time, build (test_cube.sln, x64, Debug + Release),
verify, stop.

Rev 2 reconciles this plan with `docs/rendering_submission_refactoring_instructions.md`
(Codex). Adopted from it: three verified latent bugs (steps 1-3 here), the per-list
acquire-CL design and its batch-prologue warning (folded into step 5), the failure
semantics, and a more honest validation recipe. Its iterations 5-9 (graph-compiled
barrier plans, CommandContext, ResourceStateTracker removal) are a much larger opt-in
program — PARKED, see the end of this file. THIS file is the operative plan; the Codex
file's iterations 0-4 are superseded by steps 1-3/5 here, its 5-9 remain valid as the
parked program's spec.

## Architecture recap (as of writing — verify against code before relying on it)

- `SceneRenderer::Render` (sources/app/scene/SceneRenderer.cpp) builds a `RenderGraph`
  of ~17 passes. Pass prereqs control BATCH ORDER (= GPU execution order via submit
  ordering); `mtDeps` control RUNTIME recording order on the task system. Plain
  `AddPass` passes have NO runtime deps — their bodies record concurrently and
  correctness comes purely from submit-order barriers.
- Each pass body opens its own command list(s): `renderer->BeginThreadCommandList` /
  `EndThreadCommandList(batchIndex)`. CLs land in `submitTimeline_[batch]`
  (`PassBatch_ { driver, bundles, directs }` in Renderer.h). GBuffer/Transparent
  additionally spawn `DispatchTrack`ed jobs that append CLs to their batch AFTER the
  pass body returns (awaited by `kFrameAsyncWait` before the overlay epilogue).
- `Renderer::ExecuteTimelineAndPresent` (called once from `EndFrame`): walks batches
  in order; for each CL asks `ResourceStateTracker` for its first-use states and
  appends the resulting "acquire" barriers to the TAIL OF THE PREVIOUS CL (which is
  why pass bodies leave CLs open — the fixup closes them); then ONE
  `ExecuteCommandLists` for the whole frame, present barrier on the last CL, Present.
- Consequence: the GPU does not start until the entire frame is recorded, and every
  CL pays a full state-rebind prologue (D3D12 inherits no state across CLs).

Known latent bugs (verified; fixed by steps 1-3):

- `PassBatch_::directs`/`bundles` are `tc::inl_vector<,8>` — Debug-assert-only;
  a bucket with 256+ objects (one CL per 32-object chunk) overflows out of bounds in
  Release. Same footgun class as the task system's `dependents_` (already fixed there).
- CLs are stored in WORKER COMPLETION ORDER. Opaque passes tolerate this (depth test);
  transparent blending across chunk boundaries is nondeterministic run-to-run once a
  scene has 33+ transparent objects (today: 2, so latent).
- `ResourceStateTracker`: worker TLS caches `CLStateEntry*` into a robin_hood flat map
  whose entries `ResetLanesForFrame` destroys — the epoch check reads through a
  dangling pointer (works only because robin_hood retains its allocation). Threads
  64+ clamp onto lane 63 unsynchronized (today ~25 threads, so latent).

## Failure semantics (applies to all steps)

- Add one shared `[[noreturn]] RendererInvariantFailure(const char* msg)` helper
  (sources/rendering/core/): `OutputDebugStringA` + `std::abort()`. Reuse it.
- Invariant violations that would lose, duplicate, corrupt, or mis-order GPU work
  must fail through it in EVERY build config. Never `assert`-only, never silently
  drop a command list.
- Never throw from render-pass worker tasks — the lock-free task system swallows
  task exceptions (`RunTask` catch). `ThrowIfFailed` only from contexts where the
  exception cannot land in a worker; on workers check HRESULTs explicitly.

## Invariants (preserve throughout)

1. A command allocator is not reset until the GPU completed every CL recorded on it
   (currently guaranteed by `WaitForFrame` in `BeginFrame` — don't weaken it).
2. Every submitted CL is closed exactly once before `ExecuteCommandLists`.
3. A transition executes before the first command requiring the destination state.
4. Intra-pass transitions, UAV barriers, and copies keep their recorded order.
5. Transparent draw order follows the sorted queue order, never completion order.
6. Debug and Release behave identically; capacity overflow never depends on assert.

## Verification recipe (every step)

- Build Debug + Release. The Debug D3D12 debug layer breaks on errors — the main
  safety net for barrier mistakes.
- `--tasksystem-stress` still exits 0 in both configs (guards the workers that
  parallel recording runs on).
- Once step 1 adds it: `--renderer-submission-stress` exits 0 in both configs.
- Runtime: run 60+ seconds, exercise toggles (F1, F3-F7, F9), resize twice, close via
  WM_CLOSE, exit code 0; repeat 3x. Window class `D3D12WindowClass`, title
  "D3D12 Multi-Mesh Renderer"; keys/close can be posted via PostMessage. Camera
  movement and hot-reload checks need interactive input — report honestly as
  NOT RUN if the environment cannot drive them; never claim an unrun check passed.
- Inspect debug output (OutputDebugString) for new D3D12 errors or state warnings.
- Visual output must be identical. If verifying via screen capture, present the
  analysis to the user for confirmation — do NOT conclude breakage from a capture
  alone (capture tooling on this machine is unreliable; see project memory).
- Performance (steps 4-5): warm up ~300 frames, then compare the F9 profiler's pass
  timings / FPS HUD across 3+ runs of baseline vs candidate at identical settings
  (window size, DLSS mode, camera start). Treat >3% median CPU-frame regression as a
  failure unless the user accepts it. Log `fixedSubmitScratch_.size()` per frame for
  CL counts. Report numbers, or report performance as NOT RUN — no vague claims.

## Step 1 — safe batch registration + submission stress harness

Replace `PassBatch_::directs`/`bundles` (`tc::inl_vector<,8>`, Release OOB on
overflow) with `std::vector` — BUT via a PERSISTENT BATCH POOL, not naively:

- `BeginSubmitTimeline` currently calls `submitTimeline_.clear()`, which DESTROYS the
  batches; with std::vector members that frees their buffers and re-pays ~20-40
  alloc/free pairs per frame — worse, `EndThreadCommandList` push_backs WHILE HOLDING
  `submitMtx_`, so a growth reallocation lands inside the lock all workers contend on.
  This is why inl_vector was chosen originally; preserve that property.
- Instead, never destroy batches: keep elements alive across frames and track an
  active-batch count. `BeginSubmitTimeline` resets in place (driver = nullptr,
  inner `.clear()` — which RETAINS heap capacity — count = 0); `BeginSubmitBatch`
  reuses an existing element and only push_backs when this frame has more batches
  than any previous frame. Steady state: zero allocations, push_back under the mutex
  is a store into warm capacity, and no capacity cliff — a big scene grows a batch's
  vector once, the frame it first happens.
- Do NOT keep a fixed cap with abort-on-overflow: unlike the task system's
  `dependents_` (a design bound), batch CL count is a scene-size bound — aborting on
  "user loaded a big scene" is wrong. (A small_vector with heap spill is an
  acceptable alternative if a new container is ever wanted; the pool needs none.)

Add invariant checks (null list, batch index out of range vs the ACTIVE count;
duplicate registration as a Debug-only scan) failing through
`RendererInvariantFailure`.

Add `test_cube.exe --renderer-submission-stress` (pattern: TaskSystemStress — exit
code = failures, log file, wired in main.cpp). CPU-only: drive the registration/
gathering layer with fake CL pointer values (registration only stores pointers; do
not run the real submit path, it calls Close()). Cover: >8 registrations retained in
order, invalid batch index death-test, duplicate detection. Death tests run via a
separate flag in a child process expecting abort (pattern:
`--tasksystem-stress --stress-overflow`).

Acceptance: stress exits 0 both configs; a synthetic >8-list batch is retained intact;
full verification recipe clean.

## Step 2 — ResourceStateTracker memory safety

- Remove `CLStateEntry*` caching from TLS. Pointers into a robin_hood flat map are
  invalidated by rehash (same epoch!) and by frame reset (destroyed entries). TLS may
  cache only stable values: the thread's `CLStateLane*` (heap-stable once created),
  the current CL key, and the observed lane epoch; resolve the entry by key lookup
  after validating the epoch.
- Remove the lane-63 clamp. Replace the fixed lane array with
  `std::vector<std::unique_ptr<CLStateLane>>`, creation guarded by one mutex; after
  assignment a lane is owned by exactly one recording thread, no lock on the record
  path. Lane allocation failure → `RendererInvariantFailure`, never lane sharing.
- Debug-only validation: a CL is recorded by one thread at a time; zero active
  recording scopes when `ResetLanesForFrame` runs.
- Extend `--renderer-submission-stress`: repeated lane reset + reuse, enough inserts
  to force a flat-map rehash, >64 participating threads, many CL keys per thread.
- Submit-phase access stays single-threaded by contract — state it in the header.

Acceptance: stress (incl. new cases) exits 0 both configs; rendering unchanged.

## Step 3 — deterministic submission order

Stop using worker completion order as GPU order. Assign a local order BEFORE work is
dispatched — never from registration timing or a completion counter:

- `EndThreadCommandList`/`EndThreadCommandBundle` registration gains a pre-assigned
  `localOrder` (carry it in `ThreadCL` from Begin, or pass at End).
- Natural sources: `RenderObjectBatch` chunk index, CSM cascade index, spot-shadow
  light index; single-CL pass bodies use 0/1. Driver = work-list order 0; ordinary
  directs from 1 up (matching today's driver-first behavior); bundles are a separate
  order namespace (they execute inside the driver, sorted by chunk index).
- Sort each batch's work lists and bundles by `localOrder` during gathering.
  Duplicate order within a namespace → `RendererInvariantFailure`.
- This makes transparent cross-chunk blend order follow the sorted queue (invariant
  5) and makes barrier placement reproducible run-to-run.
- Extend `--renderer-submission-stress`: register from randomized "completion" order,
  verify identical final ordering across runs; duplicate-order death test.

Acceptance: same scene → same CL order across runs; transparents/shadows correct;
parallel recording still enabled; stress exits 0 both configs.

## Step 4 — command-list groups in the render graph

Several passes record in microseconds but each pay a full CL: allocator acquire,
state-rebind prologue, submit overhead. Parallel recording buys nothing for them.
Make CL count follow RECORDING COST, not conceptual pass boundaries — via a generic
GROUP MECHANISM, not by hand-merging pass bodies and NOT by matching pass names.

4a. Mechanism — context-provided command lists.

    Invert CL ownership for simple passes. Today each body calls
    `renderer->BeginThreadCommandList()` / `EndThreadCommandList(batch)` itself.
    Add to `RenderGraphPassContext`:

        auto t = ctx.BeginCL();   // ungrouped: fresh CL; EndCL closes it into the batch
        ...record...              // grouped: the group's shared CL; EndCL is a no-op
        ctx.EndCL(t);             // (group closes the CL after its last member)

    Near-zero diff per pass body — same shape, different provider. Fan-out passes
    (CSM per-cascade workers, SpotShadows per-light, RenderObjectBatch chunks,
    GBuffer/Transparent drivers) keep calling the renderer directly; they create
    per-worker CLs by design and are NEVER groupable.

4b. Mechanism — group brackets at graph construction:

        rg.BeginCLGroup();
        auto pSSR  = rg.AddPass(Main_SSR, {pSky}, {...decls...}, ...);
        auto pBlur = rg.AddPass(Main_SSRBlur, {pSSR}, {...}, ...);
        auto pComp = rg.AddPass(Main_Compose, {pBlur}, {...}, ...);
        rg.EndCLGroup();

    A group collapses to ONE schedulable node: one batch slot, one task whose lambda
    runs member bodies in declaration order, `mtDeps` = UNION of members' (computed
    by the graph, not hand-merged). Members keep their identity for profiling —
    per-member CPU_SCOPE/GPU_SCOPE markers nest fine inside the shared CL. Groups
    must be contiguous chain segments; assert on grouping passes with external
    prereqs into the middle of a group. Empty groups (all members early-out) emit
    no CL — create the shared CL lazily on first BeginCL.

4c. Declarations need NO changes — this is why the mechanism beats manual merging.
    Each member's `ApplyDeclaredStates` runs at the member's own position WITHIN the
    shared CL, and the ResourceStateTracker already does the right thing per
    position: first touch of a resource registers a submit-time acquire barrier; a
    later member declaring a conflicting state gets a correctly-placed intra-CL
    barrier at exactly that recording position (e.g. SSR declares `ssr=UAV`, Compose
    later declares `ssr=NPS` — the tracker emits the UAV->NPS barrier between them).
    Do NOT concatenate decl lists or apply them all at CL start — that emits
    barriers before the work they guard.

4d. Policy — grouping decisions are DATA at the construction site, justified by the
    profiler's per-pass recording times (F9 overlay / CPU_SCOPE kPass*). Structure
    alone cannot decide: the main graph is one linear prereq chain, so "merge linear
    chains" would collapse the whole frame into one CL and kill parallel recording.
    Initial groups, in order of confidence:
    - `Main_SSR` + `Main_SSRBlur` + `Main_Compose` (sequential single-dispatch chain,
      no mtDeps; 3 CLs -> 1)
    - `Main_PrologueClear` + `Main_ObjectCompute` (2 -> 1)
    - `Main_Tonemap` + `Main_Debug` (conditional single draw after tonemap; 2 -> 1)
    - optional, measure first: `Main_Skybox` and/or the `Main_Lighting` +
      `Main_SpotLights` + `Main_PointLights` chain. CAUTION: Lighting/SpotLights
      carry mtDeps ({CSM}, {SpotShadows}); the group's unioned mtDeps delay its
      recording start to the latest dependency — measure whether that costs more
      than the saved CLs. Skip if unclear.
    Adaptive auto-grouping (graph reads last frame's per-pass record times,
    thresholds ~50us, with hysteresis) is a possible phase 2 — do NOT start with it:
    frame-to-frame grouping changes mean frame-to-frame barrier layout changes,
    which is miserable to debug. Deterministic brackets first.

LANDMINES:

- Tonemap calls `EvaluateDLSS` (Streamline records into the CL) and re-binds
  descriptor heaps afterwards — preserve that sequence inside its body wherever the
  CL comes from.
- Group = one batch slot; downstream prereq indices are unaffected if pass ids stay
  per-pass (preferred) — only the internal pass->batch mapping changes.
- `RegisterPassDriver` (GBuffer/Transparent) and bundle handling are tied to a
  batch's driver CL — those passes stay ungrouped; don't touch that path.

Acceptance: per-frame CL count drops from ~20+ to ~12-14 (log
`fixedSubmitScratch_.size()` to confirm); identical visuals; no debug layer errors;
all toggles work (incl. DLSS, FXAA, SSR cycling, DebugTex).

## Step 5 — eager batch submission (requires steps 1-3; do AFTER step 4)

Goal: submit ready prefixes of the batch timeline during recording, so the GPU starts
shadow/GBuffer work while the CPU still records later passes. Today everything waits
for the single end-of-frame `ExecuteCommandLists`.

5a. Per-LIST acquire command lists (adopted from the Codex plan; supersedes the
    per-batch prologue idea from rev 1). The fixup currently appends CL N's acquire
    barriers to CL N-1's tail, so a batch cannot close until the NEXT batch's first
    CL is analyzed — that's what forces end-of-frame submission. Change to: walk work
    lists in deterministic order (step 3); compute each list's acquire barriers from
    the tracker; when non-empty, record them into a small dedicated acquire CL
    submitted IMMEDIATELY BEFORE that work list; skip the acquire CL when empty.
    Ordinary work lists then close in `EndThreadCommandList` (check the Close()
    HRESULT — workers must not throw — fail via `RendererInvariantFailure`).
    Do NOT aggregate a batch's acquire barriers into one batch prologue: a later
    list in the same batch may need a transition out of an earlier list's final
    state, which a single prologue cannot represent. Bundle drivers keep their
    explicit lifecycle (bundles appended sorted, driver closed once, at seal time).
    The present transition + GPU-profiler frame-end go into a dedicated epilogue CL
    submitted last — never reopen or append to the final work list.
    This sub-step can ship alone with the single end-of-frame submit, as a low-risk
    intermediate: behavior identical, barriers just live in their own CLs. Measure
    the added tiny-CL cost (offset by step 4's merges).

5b. Submit ready prefixes. A batch is submittable when ALL its CLs exist. Pass-task
    completion is NOT sufficient: GBuffer/Transparent batches receive CLs from
    `DispatchTrack`ed jobs after the pass body returns. Mechanism:
    - per-batch contributor count: pass body registers itself at start;
      `RenderObjectBatch` pre-registers its job count BEFORE `DispatchTrack`;
      every `EndThreadCommandList`/`EndThreadCommandBundle` decrements;
      batch is sealed when its pass task is done AND contributors == 0.
    - a single submit path (mutex-guarded; can run on whichever thread seals the
      lowest pending batch): processes the contiguous sealed prefix — runs the
      barrier fixup for those batches (tracker submit-phase methods are unlocked by
      design and MUST stay single-threaded — the mutex provides that), orders lists
      by their step-3 tokens, calls `ExecuteCommandLists` for the prefix.
      `ID3D12CommandQueue` is free-threaded, so submitting from a worker is legal.
    - `EndFrame` flushes whatever remains, then the epilogue CL, Present, signal.
      Frame pacing, fence values, and allocator-reuse gating (`WaitForFrame` in
      `BeginFrame`) are unchanged — allocators were never reused within a frame.

LANDMINES:

- The tracker's `ApplyFinalStates`/`AppendAcquireBarriers`/`FindCLStateForCmd` are
  deliberately lock-free and single-threaded-by-contract. All eager-submit fixup work
  must be serialized under one mutex, and `ResetLanesForFrame` must only run after
  the final flush of the frame.
- The GPU profiler (`PROF_GPU_ENABLED`) brackets the frame with begin/end CLs in
  `ExecuteTimelineAndPresent` — the begin CL must go into the FIRST eager submission,
  the end CL into the epilogue.
- Streamline frame tagging: DLSS evaluate lives in the tonemap batch near the end;
  eager submission of earlier batches does not change its frame association, but
  verify DLSS toggling still works after the change.
- If a pass records zero CLs (early-out), its batch still seals — contributor count
  handles this naturally; do not special-case empty batches away, the acquire CL for
  the NEXT batch's lists may still be needed.

Acceptance: identical visuals, all toggles work, resize works, clean WM_CLOSE exit;
debug layer silent. Measure GPU-start latency via the GPU profiler (first batch
should begin executing while later passes still record); FPS not regressed on the
demo scene (a win may only appear on heavier scenes — that is fine, the goal is
removing the structural serialization).

## Parked — graph-compiled barrier program (opt-in, large)

`docs/rendering_submission_refactoring_instructions.md` iterations 5-9 describe the
full migration: entry/exit/access resource declarations, a graph compiler producing
barrier plans validated in shadow mode, a per-list `CommandContext` replacing TLS
lane tracking, and removal of `ResourceStateTracker`. The destination is sound and
its declaration model (explicit EXIT states — expressing what a pass with internal
flips leaves behind, which our first-use-only declarations cannot) is the part most
worth stealing early if needed. But it is a ~30+ session program with dual barrier
systems alive throughout; only start it as a deliberate decision, after steps 1-5
here, using that file as the spec. Its iterations 0-4 are superseded by this file.

## Explicitly out of scope

- The custom-command-stream / replay architecture (record into an engine-owned format,
  translate with precomputed barriers) — legitimate "engine v2" design, parked.
  If ever attempted: parallel translate into N CLs with barriers computed in a linear
  pre-scan, NOT single-threaded replay into one CL.
- D3D12 Enhanced Barriers migration.
- Per-subresource state tracking.
- Compute/copy queue execution.
