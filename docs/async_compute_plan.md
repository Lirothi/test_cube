# Async compute — architecture plan

**Status: NOT STARTED.**

**Goal: the renderer gains a second execution queue as a first-class architectural capability.**
The render graph learns to schedule a pass onto a `D3D12_COMMAND_LIST_TYPE_COMPUTE` queue,
cross-queue dependencies become fence edges the graph owns, and the barrier compile learns
per-queue resource ownership.

This is a capability, not an optimisation. Perf is a consequence and a regression check — **it is
not the acceptance criterion for any step.** The engine should be able to express "this work runs
on the async queue" because that is the shape a modern renderer needs; what it is worth on today's
scene is a separate question, answered later and per-pass.

This plan is written for an AI executor. Every step is independently buildable, independently
verifiable, independently committable. **Do not merge steps.** Several exist purely to prove the
machinery they add is INERT — that proof is the deliverable, and skipping it is how this class of
change becomes undebuggable.

---

## Executor Guide (read first)

**Repo:** `D:\Programming\test_cube`. Conventions match
`docs/enhanced_barriers_migration_plan.md`; the essentials are repeated here.

**Build (run BOTH after every step):** use the PowerShell tool, not bash.
```
& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "D:\Programming\test_cube\test_cube.sln" /t:Build /p:Configuration=Debug   /p:Platform=x64 /m /nologo "/clp:ErrorsOnly;Summary"
& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    "D:\Programming\test_cube\test_cube.sln" /t:Build /p:Configuration=Release /p:Platform=x64 /m /nologo "/clp:ErrorsOnly;Summary"
```
Both must report `0 Warning(s) 0 Error(s)`. `Debug|x64` is `WITH_EDITOR=1` and runs `/analyze`;
`Release|x64` is the shipping build. Build Debug first — faster, and the stricter gate.

**Correctness gates (every step):**
```
x64\Debug\test_cube.exe --scene-stress-gbv=20 --barrier-cmp --canonical-check
x64\Debug\test_cube.exe --shadow-mode=legacy --scene-stress-gbv=20 --barrier-cmp --canonical-check
x64\Debug\test_cube.exe --scene-stress=12
```
The bar is `verdict: CLEAN`, **0 MISSING**, and **ZERO debug-layer messages** — that is this
engine's current state, not an aspiration, so any new id is a regression this step caused. The
no-GBV run matters because break-on-error is ACTIVE there: a debug-layer error kills the process.

**Trace capture:**
```
x64\Release\test_cube.exe --trace=120 --shot-delay=8 --wind-freeze=3
```
writes `traces/trace_*.json` (Chrome-trace format) and exits. **Read it with a script, not with
your eyes** — group by name across ALL frames, take medians. A single frame proves nothing;
`--wind-freeze` makes the scene reproducible.

**Line endings:** `.cpp/.h/.hlsl` are CRLF. Editing tools rewrite whole files as LF — re-normalise
and verify (lone-LF count 0) after every scripted edit.

**Do not commit.** The user commits per step.

---

## Reference — what exists today

**R1. One queue.** `GraphicsDevice::InitQueue` creates a single `D3D12_COMMAND_LIST_TYPE_DIRECT`
queue; `Renderer::GetCommandQueue()` returns it and everything submits there.

**R2. Command list plumbing is ALREADY type-aware.** `FrameResource` pools allocators and command
lists per `D3D12_COMMAND_LIST_TYPE` (Direct / Compute / Copy / Bundle — see `QueueIndex_`), and
`Renderer::BeginThreadCommandList(type)` takes the type. Acquiring a COMPUTE list needs no new
plumbing — this is a big part of why this is less work than it sounds.

**R3. Submission is one flat batch.** `SubmitTimeline::GatherFrameLists` flattens the frame's pass
batches into one array — per batch the driver list (bundles executed into it), then the direct
lists — and `Renderer` submits it with a single `ExecuteCommandLists`, then `SignalFrame`. Order is
deterministic (`localOrder`, assigned before dispatch).

**R4. The render graph has NO notion of a queue.** `AddPass`/`AddPassMT` schedule onto worker
THREADS; dependencies are recording-order constraints, not GPU sync. Nothing in `RenderGraph.h`
mentions a queue.

**R5. Barriers are compiled ahead of execution, on ONE linear order.** `RenderGraph::CompileBarriers`
walks the schedule carrying a single running state per resource, seeded from the canonical
registry, and writes barriers into per-(pass, point) slices. **This ahead-of-recording knowledge is
the thing that makes cross-queue ownership tractable at all** — the deleted `ResourceStateTracker`
only knew states at record time, per command list, in TLS, and could never have produced a
release/acquire pair. Enhanced barriers are the default since the barrier migration, so queue-scoped
layouts (`LAYOUT_COMPUTE_QUEUE_*`, `LAYOUT_DIRECT_QUEUE_*`) are available to express handoff without
a round trip through COMMON.

**R6. Per-frame resources are recycled per frame-in-flight SLOT.** `Renderer::BeginFrame` calls
`ResetPerFrame()` on the descriptor and sampler rings and resets command allocators for the current
slot, after `WaitForFrame(slot)`. Everything assumes **one** fence decides when a slot is free.

**R7. GPU timestamps are single-queue.** `Profiler::InitGpu(device, queue, maxQueries=1024)` — one
query heap, one calibration, one track in the trace.

---

## Design decisions

**D1. The queue is a property of the PASS, fixed when the graph is built.** Not of the command
list, not of the material, not decided at record time. `AddPass` gains an optional `RenderQueue`
(default `Graphics`); the pass body acquires a list of the matching type. Anything later than
graph-build time makes the barrier compile undecidable, because the compile runs before any body
records.

**D2. Cross-queue synchronisation is a graph EDGE, not a hand-written fence.** A dependency that
crosses queues compiles into `Signal` on the producer queue + `Wait` on the consumer queue at
submit time. Hand-placed fences inside pass bodies are forbidden: they are invisible to the
compile, which is precisely the class of bug the barrier migration spent sixteen steps deleting.

**D3. A resource that crosses queues gets a RELEASE on the source queue and an ACQUIRE on the
destination queue.** Two half-barriers, at two different points, in two different command lists,
with a fence between — not the single point the compile emits today. This is the structural change
and it is Step 6.

**D4. Every step keeps the default behaviour until Step 7, and there is a permanent off switch.**
All passes stay on the graphics queue while the machinery is built. `--no-async-compute` forces
everything back and survives after the work lands, exactly as `--legacy-barriers` did — a suspected
async regression must be one flag away from being bisected, not a rebuild away.

**D5. Eligibility is a stated rule, not a per-pass hunch.** A pass may move to the async queue when
it (a) records only compute work, (b) has no resource in common with a graphics pass scheduled
concurrently other than through a declared edge, and (c) does not touch the swapchain. Step 8 turns
this into a checked invariant rather than a review convention.

---

## Reference points — the current frame

Not a gate; these are the numbers a regression is measured against, and the basis for choosing
which pass moves first. Demo level, free-start camera, Release, medians over 123 frames:

| scope | median | share |
|---|---|---|
| **GPU.Frame** | **1777 us** | 100% |
| Pass_VsmPageRender | 367 us | 20.7% |
| Pass_Tonemap (NGX/DLSS inside our scope) | 309 us | 17.4% |
| Ocean.Surface | 288 us | 16.2% |
| Pass_ObjectCompute (ocean FFT + particles) | 139 us | 7.8% |
| VsmPageRender.Setup | 107 us | 6.0% |
| Pass_VsmPageRequest | 88 us | 5.0% |
| ExecuteBundles | 75 us | 4.2% |
| Pass_ShadowCull | 52 us | 2.9% |

**First mover: `Pass_ObjectCompute`.** It is pure compute, it is the largest compute pass, and the
ocean does not cast shadows (`OceanRenderable::CastsShadow() == false`), so it is the most likely
to be independent of the shadow rasterisation it would overlap. Step 7 verifies that independence
from the passes' own `Prepare` registrations rather than assuming it.

---

## Steps

### Step 1 — the compute queue exists and is idle

`GraphicsDevice` creates a second queue (`D3D12_COMMAND_LIST_TYPE_COMPUTE`) alongside the direct
one and exposes it. Nothing submits to it. Log it in `logs/device_caps.log` beside the other caps.

**Acceptance:** both builds `0/0`; all three correctness gates unchanged; `--trace` GPU.Frame median
within run-to-run noise of the pre-step number (record both).

### Step 2 — cross-queue fences, and a frame slot that waits for BOTH queues

Extend the frame scheduler so a frame-in-flight slot is free only when **both** queues have passed
its fence value (R6 assumes one). Add the signal/wait helper the graph will use in Step 5.

Prove it with a deliberately empty compute submission: each frame open a COMPUTE list, close it,
submit it, signal, and make the slot's release depend on that signal too.

**Acceptance:** all three gates CLEAN with the empty compute submission running; a Debug assert
fires if any per-frame ring or allocator is reused before both fences; 500+ frames of
`--scene-stress`; GPU.Frame unchanged within noise.

### Step 3 — the profiler sees the second queue

A second timestamp query heap with its own calibration (R7), GPU scopes tagged with the queue they
were recorded on, and the trace emitting two tracks (`pid`/`tid` split).

**This lands BEFORE any work moves, and that ordering is not negotiable.** In a single-track trace
an overlap is indistinguishable from a reordering — you would have no way to see whether async
compute is doing anything at all. This codebase has already paid for exactly that blindness: an
82 us hole in the GPU timeline that turned out to be unmeasured bundle work, invisible for as long
as nobody could measure it.

**Acceptance:** a `--trace` capture of Step 2's empty compute submission shows a second track; a
known-ordered pair (compute signal -> graphics wait) appears in the correct order on the shared
timebase; all gates CLEAN.

### Step 4 — the graph learns the word "queue" (inert)

`RenderQueue { Graphics, AsyncCompute }` on pass registration, defaulting to `Graphics`. The pass
context exposes it so a body acquires the right list type. **Every existing pass stays on Graphics.**

**Acceptance:** all gates CLEAN; GPU.Frame unchanged within noise; the enum is threaded through but
provably unused — grep shows no `AsyncCompute` at any call site.

### Step 5 — per-queue submission and fence edges (inert)

`SubmitTimeline` groups batches per queue and returns one list array per queue; `Renderer` submits
each to its own queue. Graph dependencies that cross queues compile into signal/wait pairs (D2).

**Acceptance:** the graphics queue's command-list array is **byte-identical** to today — dump the
pointers in order under a temporary flag, before and after, and diff them. The compute array is
empty and no fence edge exists yet because no pass is async. All gates CLEAN. If the array differs,
the step is not done, however plausible the difference looks.

### Step 6 — per-queue barrier state and ownership transfer (inert)

The structural step. `CompileBarriers` carries state per **(resource, queue)** instead of one
running state, and where a resource crosses queues it emits a RELEASE at the producer's point and
an ACQUIRE at the consumer's point (D3), using enhanced queue-scoped layouts.

**Acceptance:** with every pass still on Graphics, the compiled barrier arrays are **byte-identical
to today** — dump and diff them under a flag. `--barrier-cmp` stays at 0 MISSING and 0 extra. All
gates CLEAN, zero debug-layer messages. **Do not move a pass in this step.** The entire value here
is a generalisation proven inert.

### Step 7 — the first real user: `Pass_ObjectCompute` on the async queue

Move it to `AsyncCompute`, and add `--no-async-compute` (D4).

Before moving it, verify independence from the passes it will overlap: list every resource
`Pass_ObjectCompute` writes (its `Prepare` registrations are the authoritative list — read them,
do not guess) and every resource the concurrent graphics passes read, and intersect. A non-empty
intersection is not a blocker — it is a required fence edge (D2) — but it must be **declared**, not
discovered by the debug layer.

**Acceptance:**
- All three gates CLEAN, zero debug-layer messages, 0 MISSING.
- **The two-track trace shows the pass genuinely overlapping**, not merely relocated. This is the
  step's real acceptance criterion: the architecture works.
- Visual parity: `--shot` at a frozen wind clock against a same-config control pair — the
  cross-config difference must sit inside the run-to-run noise band (this scene's floor is ~0.2% of
  pixels differing by >8; measure it, do not assume it).
- **No perf REGRESSION.** Interleave A/B/A/B and then repeat in reversed order — successive runs
  drift downward from thermal downclock, which alone will manufacture a result for whichever arm
  ran first. Report mean and standard deviation. A win is welcome; the bar is "not slower outside
  the noise", because both queues contend for the same shader cores and an overlapped pass can cost
  more than it saves.

### Step 8 — eligibility as a checked rule, and the remaining passes

Turn D5 into an invariant the graph checks at build time (a pass marked `AsyncCompute` that records
a graphics command, or touches the swapchain, fails fast). Then move the remaining eligible
compute passes one at a time, each with Step 7's acceptance.

Candidates in the current frame, in order: `Pass_ShadowCull`, `Pass_VsmPageRequest`,
`VsmPageRender.Setup`. Each moves only if its own trace shows overlap and no regression.

**Acceptance per pass:** as Step 7. **Acceptance for the rule:** a deliberately mis-marked pass
fails fast in Debug with a message naming the pass.

### Step 9 — hardening

Device removal, window resize, and level switch with work in flight on both queues. All three idle
the GPU and rebuild resources, and all three currently assume one queue.

**Acceptance:** `--scene-stress-gbv=30` (nothing but reload/switch/resize churn) CLEAN in both
shadow modes; a forced device removal is handled without a hang; `--no-async-compute` still
produces a byte-identical graphics submission to the pre-Step-7 build.

---

## Risk register

- **The barrier compile's single-linear-order assumption is THE structural blocker (Step 6).**
  Everything else is plumbing that already half exists. Budget accordingly.
- **Per-frame ring lifetime.** Descriptor, sampler and upload rings are reset per frame-in-flight
  slot on the assumption that one fence decides when the slot is free. A compute queue running
  ahead or behind breaks that. **This engine has already shipped one bug of exactly this shape** — a
  staging cache keyed on the frame-in-flight SLOT instead of the frame NUMBER, handing out
  descriptor addresses the ring had already reused, visible only under GBV as wrong descriptor
  types. Any "is this frame done" test must consider both queues.
- **The bind cache is `thread_local` and reset per command-list acquire.** A compute list recording
  concurrently on another queue must not inherit graphics root-signature state.
- **Deadlock.** A wait ordered before its signal hangs the GPU with no message. Every cross-queue
  edge must be signalled from a queue submitted earlier in the same frame; the graph should reject
  a cycle at build time rather than at 3 am.
- **Async compute can be slower.** Both queues contend for the same shader cores; a compute pass
  overlapped with an occupancy-bound raster pass can slow both. Hence Step 7's "no regression" bar
  and the permanent `--no-async-compute`.
- **Timestamp calibration is per queue.** Two queues' timestamps are not comparable without
  calibrating each; an uncalibrated second track shows fictitious overlap (Step 3).

## Non-goals

- **A copy queue** for uploads. The same architecture makes it straightforward afterwards, but it
  has its own lifetime rules — separate piece of work.
- **Splitting rasterisation** across queues. Two raster workloads contend for the same units.
- **Reworking what the passes themselves do.** This plan changes where work runs, not what it is.
