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
plan removes, in three separable steps. The engine behaviour must not change at any step.

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
  REJECTED as a candidate (see the finding above). Remaining: `Main_VsmPageRender`, its own
  session — the largest and highest-value conversion.
