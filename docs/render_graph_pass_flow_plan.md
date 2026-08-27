# Render graph pass flow plan (kill the Prepare/Record mirroring)

## Problem

The barrier migration is DONE (docs/enhanced_barriers_migration_plan.md): declarations are
authoritative, `CompileBarriers` turns them into per-point barrier arrays before any body
records, and `Renderer::Transition` is just the emitter that matches "the body reached point N".
What remains is an AUTHORING problem: a pass is still written as two separate pieces of code —
a Prepare that declares points and a Record that must walk them in the same order with the same
conditions — kept in sync by discipline plus the comparator. Every conditional pass carries a
shared predicate (D1.1) or a prediction helper (`OceanSurfSim::CurrentAfterFrame`) whose only
job is to reproduce the record's control flow without running it. That is the friction this
plan removes: S1-S3 built the machinery and proved it on four pilots; S4-S9 carry it across every
remaining pass and then make the old shape unreachable. The engine behaviour must not change at
any step.

## Steps

- **S1 — `EmitPoint` marker + comparator default-on in Debug. (DONE, see Status)**
  The body stops repeating resources and states: `renderer->EmitPoint(cl)` emits the CURRENT
  compiled point wholesale and advances — one marker per declared point, nothing to get out of
  sync with. Mechanics this requires:
  - `BuildPassBarrierView` must KEEP empty points (a point whose barriers the compile ate
    because every resource was already in state) as `count = 0` entries — marker semantics need
    a 1:1 correspondence between declared points and view points. The `Transition` matcher
    learns to step over empty points transparently (CAS them emitted and continue), which
    preserves its behaviour exactly (an empty point previously did not exist in the view).
  - `EmitPoint` past the last point is an invariant failure (a stray marker = a conversion
    bug), and so is calling it with no compiled barriers installed.
  - A pass that used a marker is flagged (`CompiledBarriers::markerUsed`); the comparator skips
    the benign "INFO extra" direction and the SKIPPED heuristic for such passes (a marker body
    cannot diverge from the compile by construction, and it does not feed the observation log),
    while the FATAL "MISSING" direction keeps watching any remaining named `Transition` calls
    in the same pass (mixed passes are legal during migration).
  - `render::g_barrierComparator` defaults ON in Debug builds (`_DEBUG`), OFF in Release —
    mismatches surface on any editor run, not only under `--barrier-cmp`.
  - Pilot: `OceanSurfSim::RecordCompute` converts to two `EmitPoint` markers (its UAV batch
    point and its SRV handoff point; `UAVBarrier`s between dispatches are unaffected).
  GATE: both builds 0/0; the surf-sim debug checkerboard renders identically with the pilot;
  Release `--scene-stress=10` CLEAN; Debug `--scene-stress-gbv=10` CLEAN with the comparator
  now on by default — zero MISSING, no new noise.

- **S2 — `AddPass2`: setup returns execute.** The RDG-style authoring shape: one builder
  function per pass that (a) makes every frame decision as a LOCAL, (b) declares points/uses
  from those locals, (c) mutates cross-frame state (ping-pong indices) immediately, and
  (d) returns the record lambda capturing those locals BY VALUE. Prepare and Record cannot
  disagree because they are the same values. Implementation is a thin wrapper over the existing
  `AddPass` + `SetPassPrepare` two-phase machinery: the PrepareFn runs the builder and stores
  the returned lambda as the pass exec (heap-side PrepareState — both the C6262 stack budget
  and the DispatchTrack lifetime lesson already force that home). Cadence unchanged:
  Unroll → RunPrepares (builders, single-threaded) → CompileBarriers → record tasks.
  GATE: API in, zero passes converted yet, behaviour byte-identical, both stress gates green.

- **S3 — Pilot conversions.** Convert passes end-to-end to `AddPass2` + `EmitPoint`:
  `Ocean.SurfSim` (ping-pong + relocate branch — kills `CurrentAfterFrame`) and
  `Main_VsmPageRender` (`PageRenderDecisions` becomes lambda captures instead of a class member
  bridge). Each conversion is its own commit with the full gate. Wider conversion after that is
  optional, pass-by-pass, whenever a pass is touched anyway — both authoring styles coexist
  indefinitely.
  **Finding (S3 pilot #2 candidate rejected): `Main_Tonemap` is a COUNTEREXAMPLE, not a
  candidate.** Its body is legitimately nondeterministic — `ranDlss = EvaluateDLSS(cl)` can
  come back false only DURING recording, and `ranFxaa` hangs off material/size checks made in
  the body — so a builder cannot pre-commit those decisions without changing the DLSS-failure
  semantics. Its existing union registration ("declare both alternatives; a skipped state is
  one redundant barrier") is the CORRECT authoring form for such a pass, not debt. Rule of
  thumb this yields: AddPass2 fits passes whose frame decisions are knowable before recording;
  union-style Prepare stays the right tool for bodies that discover outcomes mid-record.

- **S4 — The conversion contract (documentation step, no code).** Everything after this converts
  passes; this step fixes what "converted" MEANS, so the tiers below can be judged mechanically
  instead of by taste, and so a conversion done six months from now looks like the S3 pilots.
  - **A pass is converted when** (a) it is authored with `AddPass2`; (b) every frame decision is a
    LOCAL in the builder, captured BY VALUE into the returned lambda; (c) cross-frame state it
    mutates — dirty flags, ping-pong indices, history counters — is committed IN THE BUILDER, which
    runs serially before any recording; (d) the body names no resource and no state: every
    transition is `renderer->EmitPoint(cl, point)` with the point index carried as a capture, and
    `ctx.ApplyDeclaredStates(cl)` (which is just N named `Transition` calls, RenderGraph.h:144) is
    gone with them. A pass that still calls a named `Transition` is MIXED: legal during migration,
    but not converted — the comparator's FATAL "MISSING" direction keeps watching it.
  - **Conversion is NOT a graph-shape change.** `if (rtBuildAS)`, the RT/SSR/clear variant chain,
    `pShadow = pShadowCull` in VSM mode, `pObjectIdReadback = pTransp` without a pending pick — those
    decide WHICH pass exists and what the prereq edges are. The pass name and the dependency list are
    fixed at Add time, so they stay outside the builder. `AddPass2` removes the Prepare/Record
    duplication INSIDE one pass and nothing else. (One free win: a builder is attached at Add time,
    so the "set the Prepare inside the `if`, or it lands on the pass this index aliases" trap —
    SceneRenderer.cpp:1673 — stops existing for converted passes.)
  - **Three rules S1–S3 paid for, in the order they bit:**
    1. A point is a POSITION in the pass's barrier program and a body request may only match the
       CURRENT point. Never gate a `NextPoint()`; gate what goes INSIDE it. Dropping one point
       stalled the tonemap's whole program and left the bloom chains in `UNORDERED_ACCESS` under the
       tonemap's read (P8C-2m).
    2. Declaring and then early-outing in the body is the FATAL case, not the benign one: the compile
       advances past barriers nobody emits and every later use of those resources gets a wrong
       before-state. The builder's single gate exists precisely to make that unrepresentable.
    3. Two states for the same resource in ONE point, with a copy recorded between them, is a silent
       wrong-state — the point is emitted wholesale at its first match (S3b's `physOwnerPrev`).
       Split the point around the copy.
  - **The standard gate** (every step below; steps only name their EXTRAS):
    ```
    both builds 0 Warning(s) 0 Error(s)   (Debug first: WITH_EDITOR + /analyze, the stricter one)
    x64\Debug\test_cube.exe --scene-stress-gbv=20 --barrier-cmp --canonical-check
    x64\Debug\test_cube.exe --shadow-mode=legacy --scene-stress-gbv=20 --barrier-cmp --canonical-check
    x64\Debug\test_cube.exe --scene-stress=12
    ```
    Bar: `verdict: CLEAN`, **0 MISSING**, zero debug-layer ids outside the known noise
    (939 / 940 / 1006 / 1358). The comparator is default-on in Debug since S1, so `--barrier-cmp`
    only adds the log file. `--scene-stress-gbv` proves NOTHING in Release (the debug layer is
    `#ifdef _DEBUG`).
  - **Any step claiming "the compiled barrier program is unchanged" must add
    `--scene-stress-gbv=20 --barrier-cache-verify`** — the compile is cached per frame-in-flight, and
    a diff taken while the cache serves is the cache against itself.
  - **Commit granularity:** one commit per pass in Tiers B and C. Tier A may go one commit per
    cluster: a declaration-only conversion registers the same `UseDeclared()` set in the same order,
    so it cannot change the barrier program by construction.
  - **Inventory at S4** (main + epilogue graphs; the reflection-source family is three registrations
    behind one Prepare, counted once):
    - CONVERTED (7): `Main_SurfSim`, `Main_ShoreWetness`, `Main_VsmPageRequest`,
      `Main_VsmPageRender`, `Main_Hzb`, `Main_Gtao`, `Main_DebugPreview`.
    - Tier A — declaration-only or empty Prepare (12): `Main_BuildAS`, `Main_PrologueClear`,
      `Epilogue_Overlay`, `Main_Lighting`, `Main_Skybox`, `Main_ReflectionSource` /
      `Main_RTReflections` / clear variant, `Main_ReflectionTemporal`, `Main_RTDebug`,
      `Main_GlassReflGbuffer`, `Main_GlassReflections`, `Main_ObjectIdReadback`,
      `Main_SelectionOutline`.
    - Tier B — the Prepare repeats a PREDICATE the body also evaluates (10): `Main_TerrainDepth`,
      `Main_SpotShadows`, `Main_PointShadows`, `Main_SpotLights`, `Main_PointLights`,
      `Main_ReflectionBlur`, `Main_Compose`, `Main_DebugDraw`, `Main_ExposureMetering`, `Main_Debug`.
    - Tier C — the Prepare repeats a WALK, or mirrors a nested graph (5): `Main_ObjectCompute`,
      `Main_ShadowCull`, `Main_CSM`, `Main_GBuffer`, `Main_Transparent`.
    - Counterexample (1): `Main_Tonemap` — see S8.

- **S5 — Tier A: the declaration-only passes.** Purely mechanical, no decision to hoist. Shape:
  ```cpp
  const size_t p = rg.AddPass2(RenderPass::X, { prev }, /*mtDeps*/ {}, { decls... },
      [this, renderer](RenderGraphPassContext& ctx) -> ExecFn {
          ctx.UseDeclared();
          const std::uint32_t point = ctx.usePoint ? *ctx.usePoint : 0u;
          return [this, renderer, point](RenderGraphPassContext c) {
              CPU_SCOPE(...);
              Pass_X(renderer, c, point);   // ApplyDeclaredStates -> EmitPoint(cl, point)
          };
      });
  ```
  Notes that decide the sub-order:
  - `Main_BuildAS`, `Main_PrologueClear`, `Epilogue_Overlay` register NOTHING. Their builders return
    the body unchanged and emit no marker at all (`EmitPoint` with no compiled barriers installed is
    an invariant failure, and these passes have none). The value here is uniformity, and it is what
    lets S9 close the door; do these three first, they are the cheapest possible exercise of the
    epilogue graph's Prepare path.
  - The reflection-source family keeps its `if/else` outside: three different pass NAMES, one shared
    `UseDeclared()` builder body. Same for the two glass-reflection variants.
  - `Main_ObjectIdReadback` and `Main_SelectionOutline` are `WITH_EDITOR`-only and only added under a
    runtime condition — that condition stays at Add time (see S4), the builder is unconditional.
  - Everything with declarations goes on the 5-argument overload
    `(name, prereqs, mtDeps, declares, builder)`: `Main_Lighting` (AddPassMT today) fills all five,
    `Main_Skybox` and the rest pass `{}` for mtDeps. No new overload is needed for the main graph —
    the runtime `DependencyList` form has no AddPass2 equivalent yet, but only the inner graphs use
    it, so that gap is S7d's, not this step's.
  GATE: standard, plus `--barrier-cache-verify` — this tier's entire claim is "byte-identical
  program". One commit per cluster (empty-Prepare trio / lighting+skybox / reflection+glass /
  editor pair).

- **S6 — Tier B: kill the shared predicates.** One commit per pass; each one deletes a predicate that
  is evaluated twice today. In every case the builder decides ONCE, the decision rides as a by-value
  capture, and the body loses its gate entirely (a second evaluation could only disagree — and under
  compiled barriers an early-out after declaring is the fatal case, not a benign skip).
  - `Main_TerrainDepth` (Prepare SceneRenderer.cpp:843, body :2476): `ShouldRenderShoreDepth()` and
    `ShouldBuildShoreSdf()` are read in both. The builder captures `drawDepth` / `buildSdf` + the
    three point indices. **And it must take over `MarkShoreSdfBuilt()`** (:2544): that is cross-frame
    state mutated DURING recording today — exactly what rule (c) moves into the builder. Watch the
    ordering with the SDF's own null checks (`GetShoreSdfSourceResource` / `Scratch` / `Sdf`): the
    resource pointers are part of the decision, so they are captured too, not re-fetched in the body.
  - `Main_SpotShadows` (:928) and `Main_PointShadows` (:954): both compute
    `n = min(views.size(), lightManager->GetShadowedSpotCount())` (resp. `* 6`) in the Prepare and
    the body recomputes the same clamp. The builder computes `n` once and `Pass_SpotShadows` /
    `Pass_PointShadows` take it as a parameter. The `render::VsmActive()` early-out becomes the
    builder's gate: in VSM mode the builder returns `{}` and the pass is a no-op with no declarations.
  - `Main_SpotLights` (:1338) and `Main_PointLights` (:1342): the light-count early-out is duplicated
    between Prepare and body. Builder gate; both are on the mtDeps+declares overload.
  - `Main_ReflectionBlur` (:1461-1483): **the original D1.1 debt named in this repo's own comment**
    ("the predicate is evaluated here AND in the body — that duplication is exactly what D1.1
    forbids; step 5 hoists it into pass state" — it never was). `GetBlurMaterial() &&
    GetBlurCBSizeBytes() != 0` decides whether the vertical dispatch's ping-pong point exists. The
    builder decides it and captures `{ hasVertical, point0, pointPingPong }`.
  - `Main_Compose` (:1504): `frame_->ocean && IsWetnessReady()` gates the wetness `Use`, and the
    trailing `scene -> RENDER_TARGET` point is unconditional on every path including the early-outs.
    Keep that asymmetry EXPLICIT in the builder (the second point is unconditional; only its content
    is gated) — this is rule (1) in miniature.
  - `Main_DebugDraw` (:1686): `dd->HasCommands()`, read in Prepare and again in the body.
  - `Main_ExposureMetering` (:1723): the trap is documented in place and must survive conversion —
    the FIRST point is unconditional because the body applies the declared `scene -> NPS` BEFORE it
    checks whether the camera is dormant. Converted shape: the builder decides `meter =
    cameraExposure.enabled && metering.IsReady()`, always declares point 0, and declares the
    histogram/exposure/baseLum points only when `meter`; the body always emits marker 0 and emits the
    rest only under the captured `meter`. This is the one Tier B pass where "just move the gate up"
    is wrong.
  - `Main_Debug` (:1937/:1949): `debugTexOn` / `debugPick` / `debugCanon` are already computed once
    outside and captured into BOTH lambdas — the mildest case in the tier. Converting makes the
    builder their single home and removes the three-way capture.
  GATE: standard. Extra per pass: exercise BOTH sides of the predicate in one session
  (`--scene-stress` already flips light counts, level content and editor state; for
  `Main_ReflectionBlur` toggle the blur material path, for `Main_ExposureMetering` use a level with
  NO `cameraExposure` block — `d_emissive_test` is the one that caught the original MISSING).
  `--barrier-cache-verify` on any pass whose point COUNT is claimed unchanged.

- **S7 — Tier C: the walks and the nested graphs.** These are not predicate duplication — the Prepare
  walks the same list the body walks, with the same filter, and the two can drift when the list or
  the filter changes. Each sub-step is its own commit.
  - **S7a — `Main_ShadowCull`.** The closest analogue to the S3 pilots and therefore FIRST:
    `ShadowGpuData::PrepareCullPass` (ShadowGpuData.cpp:490+) mirrors `RecordCull` (:1747) through two
    shared-predicate helpers, `WillUseUnifiedBuffers` and `WillRecordValidationReadback`, plus a walk
    of `giCasters_` that must skip exactly what the body skips. Convert exactly as `PrepareRenderPass`
    was: `PrepareCullPass` RETURNS a `CullDecisions { useUnified, giOn, readback, point indices, the
    filtered caster snapshot }`, `RecordCull` takes it as a parameter, both helpers become builder
    locals and stop being public API. The GI caster snapshot matters: the body must iterate the
    LIST THE BUILDER FILTERED, not re-filter, or the "walk twice" bug survives the conversion.
  - **S7b — `Main_ObjectCompute` and the per-object mirror.** `RenderableObjectBase::PrepareCompute` /
    `PrepareRender` (RenderableObjectBase.h:101/104) are the object-granularity version of the same
    problem — `OceanSimulation::WillCopyDisplacementHistory` (OceanSimulation.h:42) exists for no
    reason except keeping `PrepareUpdate` and `Update` from disagreeing. Two options; take the
    cheaper one first and only escalate if it fails to kill the helper:
    1. **Snapshot** — the builder walks once, records the objects that actually registered into a
       captured `tc::inl_vector`, and the body iterates THAT. Kills the double filter, keeps the
       object API as it is.
    2. **`BuildCompute(ctx) -> ExecFn` per object** — AddPass2 at object granularity, the pass builder
       collecting the returned lambdas. Strictly better (it kills `WillCopyDisplacementHistory` too)
       and strictly more churn: every renderable with a Prepare has to move.
    Recommendation: (1) for the pass, (2) for `OceanSimulation` alone, since it is the only object
    that carries a cross-frame decision.
  - **S7c — `Main_CSM` + `PrepareOpaqueDrawStates`.** `PrepareOpaqueDrawStates`
    (SceneRenderer.cpp:2395) reproduces the bodies' own indirect/GPU-instanced gate
    (`indirect && (!gpuInstanced || IsGiFoldedActive(obj))`) so it can register exactly what will be
    drawn. Same fix as S7b(1): the builder produces the registered-object list once and hands it to
    the body, so `Pass_CSM` / `Pass_SpotShadows` / `Pass_PointShadows` stop re-deriving it. This
    step is what makes S6's spot/point conversions complete rather than half-converted.
  - **S7d — `Main_GBuffer` and `Main_Transparent`, the nested graphs.** The outer Prepare mirrors an
    INNER graph: `Pass_GBuffer`'s `rgGB` driver declares the seven G-buffer targets
    (SceneRenderer.cpp:2978) and the outer Prepare re-lists them (:974); the transparent driver
    records a conditional copy sequence with raw `Transition` calls (:4746) that the outer Prepare
    re-states as four points (:1607). Inner graphs have NO Prepare and do not compile barriers —
    `ApplyDeclaredStates`/`Transition` are the real emitters there — so the fix is ONE declaration
    table owned by the outer builder and PASSED to the inner graph as its driver's `declares`,
    instead of two lists that happen to agree. The conditional copies (`depthCopy` / `sceneOpaque`
    null checks) become builder decisions captured into the driver body. API gap this step must
    fill: `AddPass2` has no runtime-`DependencyList` overload (the inner graphs use it for
    `selectedDeps`), and no `(prereqs, mtDeps, builder)` overload without declares. Add them here,
    where they are first needed, not speculatively.
  GATE: standard, in BOTH shadow modes, plus a visual check on `wind_test` (shadows + foliage) and
  on an ocean level for S7b/S7d. `--barrier-cache-verify` after each. These passes carry the fan-out
  paths, so also run `--scene-stress=12` twice: the first run's level switches are what killed the
  original spot/point registration (freed object pointers in stale view slots).

- **S8 — `Main_Tonemap`: the hybrid, or nothing.** The S3 finding stands — `ranDlss =
  EvaluateDLSS(cl)` can come back false only DURING recording, so its union registration ("declare
  both alternatives; a skipped state is one redundant barrier") is the CORRECT form and must survive
  any conversion. But the union is the DLSS branch only: `bloomActive_`, `bloomConvolution_`,
  `flaresGhosts_ || flaresStreak_`, the FXAA readiness conjunction and the tonemap-material /
  backbuffer gate are all knowable before recording, and every one of them is evaluated twice today
  (Prepare SceneRenderer.cpp:1764-1900, body `Pass_Tonemap`). The hybrid: `AddPass2` builder decides
  and captures all of those, keeps the DLSS union verbatim, body emits markers everywhere EXCEPT the
  DLSS branch, which keeps its named transitions (a legal mixed pass — the comparator still watches
  it, which is what you want here). **Do this only when something else already forces a change to
  this pass.** It carries the most hard-won barrier layout in the engine (P8C-2l/m: two separate
  incidents of a gated point shifting the program), the win is the smallest in the plan, and "convert
  it because the plan says all passes" is exactly the reasoning that produced those two incidents. If
  it is not converted, S9 records it as the one permitted exception rather than pretending otherwise.

- **S9 — Close the door.** Only after S5–S7 (S8 optional). The point of converting everything is not
  tidiness, it is that the OLD authoring shape stops being reachable:
  - `SetPassPrepare` becomes private — `AddPass2Internal` is its only caller, so the two-phase
    machinery survives as an implementation detail and no new pass can be authored as a mirror.
  - A `Pass::builtByBuilder` flag plus one Debug assert in `RunPrepares`: a pass with a non-empty
    exec that did not come from `AddPass2` is an authoring error, not a style choice. This is what
    turns the whole plan from discipline into a property.
  - `ApplyDeclaredStates` and the `declares` list can only die if S7d lands (inner graphs are their
    last real user). If S7d is skipped, they stay — say so in the Status rather than leaving a
    half-deleted API.
  - The comparator changes ROLE: with every body on markers, its INFO/SKIPPED directions are dead by
    construction and only the FATAL MISSING direction still has anything to say. Keep it default-on
    in Debug (it costs nothing and it is the net under any future hand-rolled `Transition`), but
    document that a MISSING now means someone wrote a raw transition in a marker body.
  - Record the exceptions explicitly: `Main_Tonemap`'s DLSS union (if S8 is not taken), and any pass
    whose body genuinely discovers an outcome mid-record. The rule of thumb from S3 is the criterion:
    **AddPass2 fits passes whose frame decisions are knowable before recording; union-style Prepare
    stays the right tool for bodies that discover outcomes mid-record.**
  GATE: standard, both shadow modes, `--barrier-cache-verify`, plus one full editor session (the
  editor-only passes are the ones a stress run never registers) and a `--shot` A/B against the
  pre-S5 binary on `wind_test` with `--wind-freeze=3.0`.

## Detachability

`EmitPoint` is additive (the named-`Transition` path stays fully supported); the comparator
default is one `#ifdef`; `AddPass2` is a wrapper the old API does not know exists. Any step can
be reverted independently.

## Status

- S1: DONE (uncommitted): EmitPoint + empty-point-preserving views + transparent empty-point
  skip in the Transition matcher + markerUsed comparator handling + comparator default-on in
  Debug + the OceanSurfSim pilot. Gates green (builds 0/0, stress CLEAN, Debug GBV stress CLEAN
  with default-on comparator, surf-sim debug view identical).
- S2: DONE (uncommitted): `AddPass2` (two overloads: plain, and mtDeps+declares) +
  `AddPass2Internal` — the pass is created with a placeholder body (RunPrepareOne skips an
  empty exec, that was the one trap), the installed Prepare runs the builder and replaces the
  body with its return; an empty return becomes a no-op body ("nothing this frame").
  Gates green (builds 0/0, Release stress CLEAN, Debug GBV stress CLEAN, comparator silent).
  The API is deliberately unexercised — zero conversions per the plan; its end-to-end
  semantics are proven by the FIRST S3 pilot, which is the very next step.
- S3: pilot #1 DONE (uncommitted) — **the surf sim is its own render-graph pass now**
  (`RenderPass::Main_SurfSim`, third member of the compute CL group, right after the FFT
  dispatches): `OceanSurfSim::BuildPass` is the single builder (decisions as locals, cross-frame
  ping-pong state committed at Prepare time, declarations from the same locals, record lambda
  with by-value captures + two EmitPoint markers); `CurrentAfterFrame` and the
  PrepareCompute/RecordCompute pair are DELETED, as are the surf-sim injection points in
  OceanRenderable::PrepareCompute/RecordCompute (replaced by one `BuildSurfSimPass` bridge).
  Gates green: builds 0/0, Release stress CLEAN, Debug GBV stress CLEAN, comparator silent,
  surf-sim checkerboard identical in Debug with everything armed. Tonemap examined and
  REJECTED as a candidate (see the finding above).
- S3 pilot #2 DONE (uncommitted) — **`Main_VsmPageRender` converted.** `PrepareRenderPass`
  now RETURNS the `PageRenderDecisions` it declared from; the class-member bridge
  (`pageRenderDecisions_`, `CurrentPageRenderDecisions`, the `valid` flag) is DELETED;
  `ComputePageRenderDecisions` is pure/const; `RecordPageRender` takes `dec` as a parameter.
  SceneRenderer authors the pass with AddPass2: one gate (`vsmSkipUpdate_`/shadowGpu, plus the
  outer vsmActive) decides declarations AND record together, the decisions ride as a by-value
  capture, and `Pass_VsmPageRender` lost its duplicated gates entirely (a second decision could
  only disagree — under compiled barriers an early-out after declaring is the fatal case).
  Gates: builds 0/0; Release stress CLEAN; Debug GBV stress CLEAN in BOTH shadow modes
  (`--shadow-mode=legacy` too — the stress runs VSM by default and legacy exercises the
  no-pass path); comparator silent in all runs; VSM shadows visually intact on wind_test.
- S3b DONE (uncommitted) — **RecordPageRender converted to EmitPoint markers.**
  `PageRenderDecisions` now also carries the ABSOLUTE declaration indices of the pass's points
  (base / scatterWrite / scatterRead / cacheCopy / cacheRead / consume), captured by
  PrepareRenderPass as it declares. The record body names NO resource and NO state: ~15 named
  `Transition` calls became 6 markers (plus the untouched `UAVBarrier`s); the idempotent
  physOwner re-assert on the resident-readback path is deleted outright.
  **Latent bug found AND fixed by the conversion:** the caching snapshot declared
  `physOwnerPrev → COPY_DEST` and `→ NPS` in ONE point while the copy records between them — a
  point is emitted wholesale at its first match, so the copy would have seen NPS. Dormant only
  because `g_pageCaching` defaults off. The declaration is now split into pointCacheCopy /
  pointCacheRead around the copy. Gates re-run green (both builds, both stress modes, comparator
  silent, shadows intact).
- S3c DONE (uncommitted) — **`Main_VsmPageRequest` converted the same way.** New
  `PageRequestPoints` (base / alloc / readback flag / readbackCopy / readbackRestore) returned
  by `PrepareRequestPass`; the pass is authored with AddPass2 (one gate; the builder registers
  the camera-depth read itself — the GBV id=1358 depth rationale carries over); the bodies
  (`Pass_VsmPageRequest`, `RecordPageRequest`, `RecordPageAllocate`, `RecordDebugReadback`)
  emit 4 markers and name no states; the one-shot readback decision travels as `pts.readback`
  instead of re-evaluating `WillRecordDebugReadback` in the record. The readback's
  interleaved copy/transition sequence was already point-safe (all three sources reach
  COPY_SOURCE before the first copy) — the markers just make that explicit. Gates green (both
  builds, both stress shadow modes, comparator silent, shadows intact). S3 COMPLETE.
- Since S3, four MORE passes carry the AddPass2 shape in the tree today (written new in it, or
  converted when they were touched anyway, which is the clause S3 left open): `Main_ShoreWetness`
  (`OceanRenderable::BuildWetnessPass`), `Main_Hzb` and `Main_Gtao` (P6C/P6B — the GTAO builder is
  the reference example of cross-frame state committed at Prepare time: history size, history
  frame counter, frame index), and `Main_DebugPreview`. Seven converted in total; the S4 inventory
  counts them.
- S4 DONE (committed in 0d44274): the conversion contract + the standard gate + the inventory.
- S5 DONE (uncommitted) — **all 12 Tier A registrations converted**: Main_BuildAS,
  Main_PrologueClear, Epilogue_Overlay (declare nothing: builder returns the body, no marker),
  Main_Lighting (both variants share ONE builder), Main_Skybox, Main_ReflectionSource /
  Main_RTReflections / the clear variant, Main_ReflectionTemporal, Main_RTDebug,
  Main_GlassReflGbuffer, Main_GlassReflections (RT + SSR variants), Main_ObjectIdReadback,
  Main_SelectionOutline. Six `SetPassPrepare` lambdas deleted; every converted body's
  `ctx.ApplyDeclaredStates(cl)` became one `renderer->EmitPoint(cl, point)`. The only
  `ApplyDeclaredStates` calls left in SceneRenderer are Tier B/C passes and the inner G-buffer
  driver, which is exactly the expected residue.
  **BUG FOUND AND FIXED by the conversion: `Main_RTDebug`'s trailing `reflection -> NPS` was
  never registered at all.** Its Prepare was `UseDeclared()` — one point — while the body
  performed a SECOND transition at the end. A request may only match the CURRENT point, and
  there was none left, so the barrier was silently dropped and `reflection` ended the frame in
  UNORDERED_ACCESS instead of its canonical read state on every frame the RT debug view was on.
  It is now a second declared point, gated on the same `trace` decision the body uses.
  **Latent class closed: four passes early-outed AFTER declaring** — Main_Lighting (material /
  CB size / staged SRVs), Main_Skybox (no skybox), Main_GlassReflGbuffer (no prepass material),
  Main_SelectionOutline (material / CB size / handles). Each returned without emitting a single
  one of the barriers it had already declared, which is the fatal direction under compiled
  barriers. The gates now live in the builders; the bodies lost them entirely.
  Gates: both builds 0/0 (artefacts verified fresh); Debug `--scene-stress-gbv=20 --barrier-cmp
  --canonical-check` CLEAN in BOTH shadow modes, comparator showing only the two documented
  benign INFO extras (Ocean.Wetness*, Exposure.Value) and zero MISSING; `--scene-stress=12`
  CLEAN; `--barrier-cache-verify` silent. Visual A/B against a baseline binary built from the
  SAME commit: on `wind_test` and on `ssr_bronze_palms` the A/B sits INSIDE the same-binary
  noise floor (that floor is large on both levels — 2.8k-16.8k pixels differing by >16 between
  two runs of one binary, DLSS jitter plus auto-exposure — so the first single-sample diff LOOKED
  like a regression until the floor was measured).
  **Coverage gap, stated rather than papered over:** Main_SelectionOutline and
  Main_ObjectIdReadback need an editor selection / a pick, and Main_RTDebug needs its UI toggle,
  so none of the three is exercised by the automated gates. A wrong marker there is a loud
  RendererInvariantFailure (EmitPoint validates its index), not silent corruption.
- S6 DONE (uncommitted) — **all 10 Tier B passes converted**: Main_TerrainDepth,
  Main_SpotShadows, Main_PointShadows, Main_SpotLights, Main_PointLights, Main_ReflectionBlur,
  Main_Compose, Main_DebugDraw, Main_ExposureMetering, Main_Debug. Every duplicated predicate
  named in the step above is gone, including the D1.1 debt in Main_ReflectionBlur that this
  repo's own comment said step 5 would hoist and never did. The only `SetPassPrepare` calls left
  in SceneRenderer are the five Tier C passes and Main_Tonemap, exactly as the inventory said.
  **BUG FOUND AND FIXED, confirmed by measurement: Main_ExposureMetering emitted its readback
  barriers a dispatch too early.** `baseLum -> read` shared ONE point with
  `exposure/histogram -> COPY_SOURCE`, and a point is emitted wholesale at its first match, so
  the copy-source pair fired at the end of the base-luminance dispatch — before the solve wrote
  the exposure record and read the histogram, leaving the readback copy that follows the solve
  with no barrier of its own between the write and the read. `--barrier-flip-trace` printed it:
  `point 1/3 (3 barriers) asked Exposure.BaseLogLum 0x40`, then two flip-misses. It never showed
  up as a debug-layer error because all three are BUFFERS and the engine emits enhanced
  barriers, where buffers have no layout to validate. Now four points in the body's real order;
  the same trace prints `point 0/4 (2) / 1/4 (1) / 2/4 (2) / 3/4 (2) marker`, in order, no misses.
  **Second fix, same tier:** `MarkShoreSdfBuilt()` was called from the RECORD body of
  Main_TerrainDepth, after `BuildShoreSdf` — including on the runs where the flood bailed out on
  its own materials, which cleared the dirty flag and cost that level its one SDF rebuild. The
  flag is now committed in the builder, gated on a new `OceanSimulation::CanBuildShoreSdf()`
  that asks the flood what it needs BEFORE anything is declared. Ordering checked: the surf sim
  reads the same flag and its builder runs earlier in the schedule, so it still sees the frame
  the maps are rebuilt on.
  **Latent declare-then-skip closed in four more passes:** Main_SpotLights and Main_PointLights
  each had FOUR early-outs the Prepare never mirrored (light buffer, its CPU pointer and SRV,
  the staged G-buffer handles, VSM readiness) — every one returned with ten states declared;
  Main_ExposureMetering's three materials; Main_TerrainDepth's null-DSV skip inside renderCascade.
  Gates: both builds 0/0; Debug `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN in
  BOTH shadow modes; `--scene-stress=12` CLEAN; `--barrier-cache-verify` silent; zero
  flip-miss / MISSING / EmitPoint invariant lines in any run. The shore-SDF path was checked
  separately with `--ocean-surf-sim --barrier-flip-trace` (the stress harness never reaches it):
  the pass fires ONCE per level with its five points in order, which is also the proof that
  moving the dirty-flag clear did not turn it into a per-frame rebuild. Visual A/B against a
  HEAD-built binary on `wind_test` and `ssr_bronze_palms`: inside the same-binary noise floor on
  both, and identical by eye.
  Side effect worth knowing: the comparator's two long-standing benign INFO extras on Compose
  (Ocean.WetnessA/B) are GONE from the log — a marker pass cannot diverge from its compile, so
  the benign direction is suppressed for it by design.
- S7 DONE (uncommitted) — **all four Tier C sub-steps**, so every pass in the main and epilogue
  graphs is now authored with `AddPass2` except `Main_Tonemap`.
  - **S7a `Main_ShadowCull`:** `PrepareCullPass` RETURNS `ShadowGpuData::CullDecisions` (active /
    useUnified / giOn / readback, eight point indices, and the FILTERED GI-caster index list) and
    `RecordCull` takes it. `WillUseUnifiedBuffers` and `WillRecordValidationReadback` are
    DELETED — they existed only to keep the two sides from drifting. The record body lost every
    gate and all nine named transitions; the GI scatter walks the list the declaration produced
    instead of re-filtering. The one-shot validation snapshot (`valBounds_`/`valFrame_`/
    `valState_`) and its readback allocation moved into the builder: they are cross-frame state,
    and `EnsureReadback` failing mid-record could skip a point that had already been declared.
  - **S7b `Main_ObjectCompute`:** `RenderableObjectBase::PrepareCompute` now RETURNS whether the
    object's compute will record anything. The builder collects exactly those objects and the
    body runs that list — the pass no longer walks the whole scene twice with two copies of the
    same filter, and objects with no compute are skipped by both sides. Three overriders updated
    (ocean, GPU-instanced models, particles).
  - **S7c the `indirect` decision:** `render::g_indirectShadowsEnabled && shadowGpu &&
    IndirectDrawReady()` was derived independently in FOUR places (PrepareOpaqueDrawStates,
    Pass_CSM, Pass_SpotShadows, Pass_PointShadows). It decides WHICH OBJECTS DRAW, so a
    disagreement between the registration and the draw is a GPU-instanced caster transitioning
    its instance buffer with nothing declared behind it. One `IndirectShadowDrawsActive()`,
    called by the builders, passed into both the walk and the bodies. `Main_CSM` converted with
    it.
  - **S7d the nested graphs:** `Main_GBuffer`'s seven target states were written twice (outer
    Prepare + the inner driver's `declares`) with a comment asking the two to stay in step; the
    inner driver now declares NOTHING and emits the outer pass's point as a marker. Same for
    `Main_Transparent`: its driver's copy/read/pixel/rebind sequence is four markers, and
    `RecordOceanReflection`'s two early-outs became ONE builder decision (`oceanReflect` covers
    the targets, the material, the CB size and all three descriptors), so the read point is
    declared only on the frames the compute runs. **The API gap this step predicted did not
    materialise:** no `DependencyList` overload of `AddPass2` was needed, because the fix was to
    delete the inner declarations rather than to pass them down.
  Gates: both builds 0/0; Debug `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN in
  BOTH shadow modes; `--scene-stress=12` CLEAN; `--barrier-cache-verify` silent; zero
  flip-miss / MISSING / EmitPoint invariant lines across all of them. The ocean + shore-SDF path
  re-checked with `--ocean-surf-sim --gbv --barrier-flip-trace`: TerrainDepth still fires ONCE
  per level with five points in order, and the trace's benign misses now come from exactly four
  passes — ObjectCompute and Transparent (the ocean's own still-named transitions), TerrainDepth
  (the flood's two, documented) and Tonemap (its union, by design). The comparator log is down to
  Tonemap's union alone: every other INFO extra disappeared as its pass became a marker pass.
  Visual A/B against a HEAD-built binary: `ssr_bronze_palms` inside the same-binary noise floor;
  `wind_test` is too noisy to resolve anything (six same-binary pairs span 0-21 955 pixels >16,
  eight A/B pairs span 6 291-32 604), so the verdict there rests on the gates and on the frames
  being identical by eye.
- S8-S9: NOT STARTED. S8 is deliberately conditional; S9 is what makes the whole thing structural
  rather than a habit, and after S7 its precondition is met — the only `SetPassPrepare` call left
  in the engine is `Main_Tonemap`'s.
