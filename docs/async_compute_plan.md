# Async compute — architecture plan

**Status: NOT STARTED.**

**Goal: the renderer gains a second execution queue as a first-class architectural capability.**
The render graph learns to schedule a pass onto a `D3D12_COMMAND_LIST_TYPE_COMPUTE` queue,
cross-queue dependencies become fence edges the graph owns, and the barrier compile learns
per-queue resource ownership *and per-queue state legality*.

This is a capability, not an optimisation. Perf is a consequence and a regression check — **it is
not the acceptance criterion for any step.** The engine should be able to express "this work runs
on the async queue" because that is the shape a modern renderer needs; what it is worth on today's
scene is a separate question, answered later and per-pass. The honest ceiling on today's scene is
computed in "Reference points" below and it is **7.8 %** — write that number down now so nobody is
surprised by it at the end.

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

**Any step that claims a barrier array is unchanged must also run:**
```
x64\Debug\test_cube.exe --scene-stress-gbv=20 --barrier-cache-verify
```
See R9 — the barrier compile has a cross-frame cache, and a diff taken with the cache serving is a
diff of the cache against itself, i.e. no proof at all.

**Trace capture:**
```
x64\Release\test_cube.exe --trace=120 --shot-delay=8 --wind-freeze=3
```
writes `traces/trace_*.json` (Chrome-trace format) and exits. **Read it with a script, not with
your eyes** — group by name across ALL frames, take medians. A single frame proves nothing;
`--wind-freeze` makes the scene reproducible.

**Line endings:** `.cpp/.h/.hlsl` are CRLF. Editing tools rewrite whole files as LF — re-normalise
and verify (lone-LF count 0) after every scripted edit. (This document is LF; leave it that way.)

**Do not commit.** The user commits per step.

---

## Reference — what exists today

Everything below was read out of the code, not remembered. File:line is given so the executor can
re-check rather than trust.

**R1. One queue.** `GraphicsDevice::InitQueue` creates a single `D3D12_COMMAND_LIST_TYPE_DIRECT`
queue (`GraphicsDevice.h:78`); `Renderer::GetCommandQueue()` returns it and everything submits
there (`Renderer.cpp:845`).

**R2. Command list plumbing is type-aware, but the COMPUTE lane has NEVER RUN.** `FrameResource`
pools allocators and lists per `D3D12_COMMAND_LIST_TYPE` (`FrameResource.h:53`, `QueueIndex_`) and
`Renderer::BeginThreadCommandList(type)` takes the type (`Renderer.cpp:655`). But a grep for
`D3D12_COMMAND_LIST_TYPE_COMPUTE` across `sources/` finds exactly two hits, both of them the
plumbing itself — **no code has ever acquired a COMPUTE allocator or list.** So the plumbing needs
no design work, but it is unexercised: `CreateCommandList(COMPUTE)`, its `Reset`, and the
`SetDescriptorHeaps` at `Renderer.cpp:672` are all first-run-in-Step-1 code.

**R3. Submission is one flat batch, plus two fixed lists.** `SubmitTimeline::GatherFrameLists`
flattens the frame's pass batches into one array — per batch the driver list (bundles executed into
it), then the direct lists. `Renderer::ExecuteTimelineAndPresent` then wraps that array with a
**GPU-profiler frame-begin list at the front** and a **present/frame-end epilogue list at the back**
(`Renderer.cpp:790-836`) and submits the whole thing with a single `ExecuteCommandLists`, then
`SignalFrame`. Any "byte-identical submission" claim must dump `fixedSubmitScratch_`, not
`submitListsScratch_`.

**R4. The render graph has NO notion of a queue.** `AddPass`/`AddPassMT` schedule onto worker
THREADS; dependencies are recording-order constraints, not GPU sync. Nothing in `RenderGraph.h`
mentions a queue.

**R5. Barriers are compiled ahead of execution, on ONE linear order.**
`RenderGraph::CompileBarriers` (`RenderGraph.h:648`) walks the schedule carrying a single running
state per resource, seeded from `GetPredictedState`, and writes barriers into per-(pass, point)
slices. **This ahead-of-recording knowledge is the thing that makes cross-queue ownership tractable
at all** — the deleted `ResourceStateTracker` only knew states at record time, per command list, in
TLS, and could never have produced a release/acquire pair. Enhanced barriers are the default since
the barrier migration, so queue-scoped layouts (`LAYOUT_COMPUTE_QUEUE_*`, `LAYOUT_DIRECT_QUEUE_*`)
are available to express handoff without a round trip through COMMON.

**R6. Per-frame resources are recycled per frame-in-flight SLOT.** `Renderer::BeginFrame`
(`Renderer.cpp:316`) calls `ResetPerFrame()` on the descriptor and sampler rings and resets command
allocators for the current slot, after `WaitForFrame(slot)`. Everything assumes **one** fence
decides when a slot is free (`FrameScheduler.cpp:40-58`). `kFrameCount == 3`.

**R7. GPU timestamps are single-queue, and the readback fence is worse than single-queue.**
`Profiler::InitGpu(device, queue, maxQueries=1024)` — one query heap, one `GetTimestampFrequency`,
one `GetClockCalibration`, one track in the trace (`kGpuTraceThreadIndex`). Beyond that:
- `nextGpuQuery_` is one global counter under `gpuMtx_` (`Profiler.cpp:1179`), and
  `gpuRecordingReadbackSlot_` one global slot rotation (`Profiler.cpp:637`);
- **`gpuDrainFence_` is signalled ONLY on the direct queue** (`Profiler.cpp:624-634`). A batch is
  declared readable when that fence passes. A `ResolveQueryData` recorded on the compute queue is
  not covered by it, so the collector would map a readback range the compute queue has not written
  yet and report fictitious numbers — silently.

**R8. `Pass_ObjectCompute` is glued into a CL group with a graphics pass.**
`SceneRenderer.cpp:496-505`: `BeginCLGroup()` wraps `Main_PrologueClear` and `Main_ObjectCompute`,
so the two share ONE DIRECT command list, one batch and one task. The group exists because both are
tiny and per-CL overhead dominated them. **A pass inside a CL group cannot change queue** — the
group's list is provisioned once, as DIRECT, by `RenderGraphPassContext::BeginCL`
(`RenderGraph.h:107-117`).

**R9. The barrier compile has a cross-frame CACHE, keyed per frame-in-flight slot.**
`RenderGraph.h:288-324`. Its key is an exact byte copy of (compile order + pass names + slices, the
whole `ResourceUse` arena) plus `renderer->DeclarationsGeneration()`, and it only stores a compile
whose output is a **fixed point** (every touched resource ends where it began). Consequences for
this plan:
- the queue a pass runs on becomes part of the cache key;
- the fixed-point test becomes per-queue (a resource must return to its incoming state *on its own
  queue*, not merely somewhere);
- any "the compiled barriers are byte-identical" proof must run with `--barrier-cache-verify`,
  which recompiles on a hit and diffs, or it proves nothing.

**R10. Legacy resource states the engine uses are NOT ALL LEGAL ON A COMPUTE QUEUE.** This is the
structural blocker for the first mover, and the original draft of this plan missed it entirely.
D3D12 restricts a compute command list to `COMMON`, `UNORDERED_ACCESS`,
`NON_PIXEL_SHADER_RESOURCE`, `COPY_SOURCE`, `COPY_DEST` (plus indirect-argument). Meanwhile:
- the ocean registers `NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE` for its displacement and
  foam maps (`OceanSimulation.cpp:1102-1133`) — the engine leans on that combined state everywhere;
- `barriers::LegacyStateToBarrier` is **queue-blind** (`BarrierTranslation.h:34`): it would hand a
  compute list `SYNC_PIXEL_SHADING` / `LAYOUT_GENERIC_READ`, which is equally illegal in the
  enhanced model.

So per-queue state is not only about *which* barrier is emitted (Step 7) but about *whether the
requested state can be expressed on that queue at all*. That gets its own checked rule (Step 5).

**R11. `Pass_ObjectCompute` is three unrelated workloads with three different consumers.** The body
is a loop over every renderable calling `obj->ExecuteCompute` (`SceneRenderer.cpp:1645-1653`):

| workload | writes | consumed by | slack |
|---|---|---|---|
| `GpuInstancedModels::RecordCompute` (GI rotation) | `instanceBuffer` (UAV) | **`Main_ShadowCull`**, via the GI scatter (`ShadowGpuData.cpp:1491-1500`) | ~none: two passes later |
| `OceanSimulation::Update` (spectrum/FFT/mips/foam) | `Ocean.Displacement`, `Ocean.FoamTurbulence` | **`Main_Transparent`** (`SceneRenderer.cpp:1098-1105`); the ocean is `IsTransparent() == true`, `OceanRenderable.h:37` | the whole frame |
| `ParticleEmitterObject` sim/sort | particle/dead-list/sorted buffers (all UAV, plus COPY_SOURCE for the debug readback — `ParticleEmitterObject.cpp:385-408`) | **`Main_Transparent`**, same point | the whole frame |

Moving the pass whole means fencing `Main_ShadowCull` on it, which is near-zero overlap plus the
cost of the sync. Splitting it is therefore not a refinement, it is the difference between the
architecture demonstrating anything and not.

**R12. Out-of-band direct-queue submissions exist.** `UploadBatch::Submit` calls
`ExecuteCommandLists` on the direct queue outside the frame timeline (`UploadBatch.cpp:38-54`), and
the editor's `AssetThumbnailCache` signals the same queue directly
(`AssetThumbnailCache.cpp:1610`). Today one queue orders them against everything else for free.
A compute queue does not see them.

**R13. The `thread_local` bind cache is already safe.** `BeginThreadCommandList` calls
`render::g_clBindState.Reset()` for every list type it hands out (`Renderer.cpp:683`), so a compute
list cannot inherit graphics root state. This was on the original risk register; it is closed. No
work required, but do not delete the `Reset` while doing Step 1.

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
and it is Step 7.

**D4. Every step keeps the default behaviour until Step 9, and there is a permanent off switch.**
All passes stay on the graphics queue while the machinery is built. `--no-async-compute` forces
everything back and survives after the work lands, exactly as `--legacy-barriers` did — a suspected
async regression must be one flag away from being bisected, not a rebuild away.

**D5. Eligibility is a stated rule, not a per-pass hunch.** A pass may run on the async queue when
it (a) records only compute/copy work, (b) is not a member of a CL group (R8), (c) registers only
states legal on a compute queue (R10), (d) has no resource in common with a concurrently scheduled
graphics pass other than through a declared edge, and (e) does not touch the swapchain. Step 5
turns (a)-(c) into a build-time invariant; (d) is what the Step 7 compile computes; (e) is a
one-line check.

**D6. Queue legality is checked at REGISTRATION, not discovered at emission.** A pass marked
`AsyncCompute` that calls `ctx.Use(res, PIXEL_SHADER_RESOURCE)` must fail fast inside its Prepare,
naming the pass and the state. Waiting for the debug layer means the failure surfaces as a barrier
error on some other resource three passes later. The translation layer gets the same treatment:
`barriers::` learns the target queue and refuses to build an illegal enhanced barrier rather than
emitting one.

**D7. A cross-queue producer hands over in a state legal on BOTH queues.** The async pass leaves
`Ocean.Displacement` in `UNORDERED_ACCESS` or `NON_PIXEL_SHADER_RESOURCE`; the *graphics* consumer
performs the acquire into the full `NON_PIXEL | PIXEL` read state. This keeps the release side
trivially expressible and is why D3's release half is often a no-op — which is a feature, not a
shortcut: fewer half-barriers to get wrong.

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
| Pass_ObjectCompute (ocean FFT + particles + GI rotation) | 139 us | 7.8% |
| VsmPageRender.Setup | 107 us | 6.0% |
| Pass_VsmPageRequest | 88 us | 5.0% |
| ExecuteBundles | 75 us | 4.2% |
| Pass_ShadowCull | 52 us | 2.9% |

**The ceiling, stated up front.** `Pass_ObjectCompute` is 139 us of a 1777 us GPU frame. A perfect,
free, 100 % overlap is **7.8 %**, and that is before contention: both queues share the same shader
cores. The async part of it is smaller still, because the GI rotation stays on graphics (R11) — so
measure the split's share in Step 8 before believing any Step 9 number. Judge this work on the
two-track trace, not on the frame time.

**First mover: the ocean + particle half of `Pass_ObjectCompute`** (Step 9). It is pure compute, its
states are compute-legal or made so by D7, and its consumer is `Main_Transparent` at the very end of
the frame — so the overlap window spans shadow cull, CSM/VSM page render, G-buffer, lighting,
compose and RT. The GI rotation does NOT move: `Main_ShadowCull` consumes its output two passes
later (R11), which is a fence, not an overlap.

**GPU.Frame stops meaning "all GPU work" at Step 9.** It is bracketed by two timestamps on the
direct queue (`Renderer.cpp:790-836`). Once work overlaps, the regression metric is **wall-clock**
(`CPU.Frame` / FPS) plus per-queue GPU spans, not `GPU.Frame`. Steps 1-8 may keep using GPU.Frame
because nothing overlaps yet.

---

## Steps

### Step 1 — the compute queue exists, is idle, and its command-list lane provably works

`GraphicsDevice` creates a second queue (`D3D12_COMMAND_LIST_TYPE_COMPUTE`) alongside the direct one
and exposes it. Nothing submits to it. Log it in `logs/device_caps.log` beside the other caps (that
file already exists — `GraphicsDevice.cpp:266`).

Because R2's COMPUTE lane has never run, this step also proves it: once, behind a temporary flag,
acquire a COMPUTE allocator + list from `FrameResource`, confirm `SetDescriptorHeaps`
(`Renderer.cpp:672`) and `Reset` succeed, close it, and **throw it away without submitting**.

**Acceptance:** both builds `0/0`; all three correctness gates unchanged; the COMPUTE acquire
succeeds and is logged; `--trace` GPU.Frame median within run-to-run noise of the pre-step number
(record both).

### Step 2 — cross-queue fences, a frame slot that waits for BOTH queues, and every idle path idling both

Extend the frame scheduler so a frame-in-flight slot is free only when **both** queues have passed
its fence value (R6 assumes one). Add the signal/wait helper the graph will use in Step 6.

**Every GPU-idle path idles both queues in this step, not in the hardening step.**
`FrameScheduler::WaitForGpuIdle` takes one queue (`FrameScheduler.cpp:60`) and
`Renderer::WaitForPreviousFrame` passes the direct one (`Renderer.cpp:861`); resize, level switch,
shutdown and `ResetFrameState` all route through them. The gates for this very step are
`--scene-stress`, which is nothing *but* reload/switch/resize churn — leaving this until the end
means every intervening step is validated by a harness that cannot be trusted.

Prove the machinery with a deliberately empty compute submission: each frame open a COMPUTE list,
close it, submit it, signal, and make the slot's release depend on that signal too.

**Acceptance:** all three gates CLEAN with the empty compute submission running; a Debug assert
fires if any per-frame ring or allocator is reused before both fences; 500+ frames of
`--scene-stress`; GPU.Frame unchanged within noise.

### Step 3 — the profiler sees the second queue

**This lands BEFORE any work moves, and that ordering is not negotiable.** In a single-track trace
an overlap is indistinguishable from a reordering — you would have no way to see whether async
compute is doing anything at all. This codebase has already paid for exactly that blindness: an
82 us hole in the GPU timeline that turned out to be unmeasured bundle work, invisible for as long
as nobody could measure it.

Three separate things, all of them required (R7):
1. **A second calibration.** Per-queue `GetTimestampFrequency` + `GetClockCalibration`; an
   uncalibrated second track shows fictitious overlap.
2. **A per-queue drain fence.** `gpuDrainFence_` is signalled only on the direct queue
   (`Profiler.cpp:624-634`). A batch containing compute-queue resolves must not be declared readable
   until the compute queue has passed its own signal, or the collector reads a range nobody wrote.
   Either signal both queues and require both, or keep one batch per queue.
3. **Two tracks in the trace.** GPU scopes tagged with the queue they were recorded on; the trace
   emits a second row (`kGpuTraceThreadIndex` is a single constant today).

The shared query heap and `nextGpuQuery_` counter can stay shared (a TIMESTAMP heap is valid on both
queue types) — but say so explicitly in a comment, because "shared heap, split fence" is exactly the
asymmetry a later reader will assume is a bug.

**Acceptance:** a `--trace` capture of Step 2's empty compute submission shows a second track; a
known-ordered pair (compute signal -> graphics wait) appears in the correct order on the shared
timebase; deliberately delaying the compute resolve does NOT produce garbage timings (it produces a
late batch); all gates CLEAN.

### Step 4 — the graph learns the word "queue" (inert)

`RenderQueue { Graphics, AsyncCompute }` on pass registration, defaulting to `Graphics`. The pass
context exposes it so a body acquires the right list type through `BeginCL`. **Every existing pass
stays on Graphics.**

**Acceptance:** all gates CLEAN; GPU.Frame unchanged within noise; the enum is threaded through but
provably unused — grep shows no `AsyncCompute` at any call site.

### Step 5 — eligibility and queue legality as checked rules (inert)

D5 and D6 become invariants the graph checks at build/Prepare time, while nothing is marked async —
so every one of them is currently unreachable, and the deliverable is the *test* that each fires.

- a pass marked `AsyncCompute` that is a member of a CL group fails fast, naming the pass (R8);
- `ctx.Use(res, state)` inside an `AsyncCompute` pass's Prepare fails fast when `state` is not legal
  on a compute queue, naming the pass, the resource debug name and the state (R10, D6). Reuse
  `RenderGraph::ResourceLabel` (`RenderGraph.h:1023`) so the message is readable;
- `barriers::` learns the target queue and refuses to translate an illegal state instead of emitting
  `SYNC_PIXEL_SHADING` onto a compute list (`BarrierTranslation.h:34`);
- a pass marked `AsyncCompute` that touches the swapchain fails fast.

**Acceptance:** all gates CLEAN and unchanged (nothing is marked async, so nothing can fire);
**each rule is demonstrated by deliberately mis-marking a pass in a throwaway edit** and showing the
Debug message name the pass — then reverting. A rule that has never fired is not a rule.

### Step 6 — per-queue submission and fence edges (inert)

`SubmitTimeline` groups batches per queue and returns one list array per queue; `Renderer` submits
each to its own queue. Graph dependencies that cross queues compile into signal/wait pairs (D2).

**Acceptance:** the graphics queue's submitted array is **byte-identical** to today — dump
`fixedSubmitScratch_` (the WRAPPED array: profiler-begin list, work lists, epilogue — R3) in order
under a temporary flag, before and after, and diff them. The compute array is empty and no fence
edge exists yet because no pass is async. All gates CLEAN. If the array differs, the step is not
done, however plausible the difference looks.

### Step 7 — per-queue barrier state and ownership transfer (inert)

The structural step. `CompileBarriers` (`RenderGraph.h:648`) carries state per **(resource, queue)**
instead of one running state, and where a resource crosses queues it emits a RELEASE at the
producer's point and an ACQUIRE at the consumer's point (D3), using enhanced queue-scoped layouts
and honouring D7 (hand over in a state legal on both sides).

The compile's **cache** (R9) changes with it, and getting this wrong is silent corruption:
- the per-pass queue assignment joins the cache key alongside pass index and name
  (`CompileInputsUnchanged`, `RenderGraph.h:818`);
- the fixed-point test (`RenderGraph.h:760`) becomes per-queue — a resource must return to its
  incoming state *on the queue that will next read it*;
- `GetPredictedState`/`SetPredictedState` become per-queue, or gain an explicit "which queue last
  owned this" field. Whichever shape, state it in a comment: this is the one piece of cross-frame
  memory the barrier system keeps, and it is now two-dimensional.

**Acceptance:** with every pass still on Graphics, the compiled barrier arrays are **byte-identical
to today** — dump and diff them under a flag, **with `--barrier-cache-verify`** so the diff is
against a fresh compile and not against the cache (R9). `--barrier-cmp` stays at 0 MISSING and 0
extra. All gates CLEAN, zero debug-layer messages. **Do not move a pass in this step.** The entire
value here is a generalisation proven inert.

### Step 8 — split `Pass_ObjectCompute` by consumer, still all on Graphics (inert)

Graph surgery only, no queue change — so it is separately bisectable from the async flip.

- Dissolve the `BeginCLGroup` around `Main_PrologueClear` + `Main_ObjectCompute`
  (`SceneRenderer.cpp:496-505`), because a grouped pass cannot change queue (R8).
- Split the body's object loop (`SceneRenderer.cpp:1645-1653`) into two passes by consumer (R11):
  `Main_GpuInstanceCompute` (the GI rotation, stays on Graphics forever — `Main_ShadowCull` consumes
  it) and `Main_ObjectCompute` (ocean + particles, the future async pass). Split each object's
  `PrepareCompute` registration to the matching pass at the same time; the two Prepares must
  partition the old one exactly, or the compile silently loses a barrier.
- **Measure the split**: the trace must now show two scopes summing to roughly the old 139 us. That
  number is the real ceiling for Step 9, and it is not 139.

Do not "improve" anything else while in there. The CL-group dissolution costs one extra command list
per frame by construction — expected, report it, do not chase it.

**Acceptance:** all gates CLEAN, 0 MISSING; visual parity via `--shot` at a frozen wind clock; the
two new scopes appear in the trace with the expected split; the frame's CPU cost grows by at most
the one dissolved group's worth of per-CL overhead (report the number, do not hide it).

### Step 9 — the first real user: the ocean/particle compute on the async queue

Move `Main_ObjectCompute` (post-split: ocean + particles) to `AsyncCompute`, and add
`--no-async-compute` (D4).

Before moving it, verify independence from the passes it will overlap: list every resource it writes
(**its `Prepare` registrations are the authoritative list — read them, do not guess; this plan names
`Main_Transparent` as the consumer from `SceneRenderer.cpp:1098-1105`, and that claim is a starting
point, not a substitute for the check**) and every resource the concurrent graphics passes read, and
intersect. A non-empty intersection is not a blocker — it is a required fence edge (D2) — but it must
be **declared**, not discovered by the debug layer.

Expect D7 to bite here: the ocean's own registrations use `NON_PIXEL | PIXEL_SHADER_RESOURCE`
(`OceanSimulation.cpp:1102-1133`). Those registrations move to the *consumer* side; the async pass
hands the maps over in a compute-legal state. Step 5's rule will tell you exactly which lines to
change, by name.

**Acceptance:**
- All three gates CLEAN, zero debug-layer messages, 0 MISSING.
- **The two-track trace shows the pass genuinely overlapping**, not merely relocated, and the
  overlapping partner is one of the passes named above. This is the step's real acceptance
  criterion: the architecture works.
- Visual parity: `--shot` at a frozen wind clock against a same-config control pair — the
  cross-config difference must sit inside the run-to-run noise band (this scene's floor is ~0.2% of
  pixels differing by >8; measure it, do not assume it).
- **No perf REGRESSION, measured on wall-clock, not GPU.Frame** (see Reference points). Interleave
  A/B/A/B and then repeat in reversed order — successive runs drift downward from thermal downclock,
  which alone will manufacture a result for whichever arm ran first. Report mean and standard
  deviation. A win is welcome; the bar is "not slower outside the noise", because both queues
  contend for the same shader cores and an overlapped pass can cost more than it saves.
- `--no-async-compute` reproduces Step 8's graphics submission byte for byte.

### Step 10 — the remaining candidates, judged honestly

Move remaining eligible compute passes one at a time, each with Step 9's acceptance.

The original candidate list was ordered by GPU cost, which is the wrong order: **`Pass_ShadowCull`,
`Pass_VsmPageRequest` and `VsmPageRender.Setup` are each consumed by the immediately following pass**
(cull -> indirect shadow draws; page request -> page render; setup -> page render). Fencing them
buys a stall, not an overlap. Do not move them on the strength of their us count.

For each candidate, the go/no-go is computed BEFORE the edit: from its `Prepare` registrations, how
many passes separate it from its first consumer, and what runs in between. A pass whose consumer is
the next node moves only if there is genuinely independent graphics work scheduled alongside it —
and then the trace must show that work, or the move is reverted.

If no candidate passes that test, **that is a legitimate outcome of this step**: the capability is
built, the first user is real, and the rest of the frame is honestly serial. Write the negative
result into this document rather than moving a pass to make the list look finished.

### Step 11 — hardening

Device removal, window resize, and level switch **with work in flight on both queues** — the idle
paths themselves were fixed in Step 2, but they were only ever exercised with an empty compute
submission until now.

Also: R12's out-of-band direct-queue submissions (`UploadBatch::Submit`, `UploadBatch.cpp:38-54`,
and `AssetThumbnailCache.cpp:1610`). They are ordered against frame work today purely because there
is one queue. Decide and document which of the two holds: either the async queue never reads a
resource those paths write, or they gain a fence the compute queue waits on. "Probably fine" is not
one of the options — this is the same shape as the staging-cache bug in the risk register.

**Acceptance:** `--scene-stress-gbv=30` (nothing but reload/switch/resize churn) CLEAN in both
shadow modes; a forced device removal is handled without a hang; an upload issued during a frame
that has async work in flight is proven ordered (test it deliberately); `--no-async-compute` still
produces a byte-identical graphics submission to the pre-Step-9 build.

---

## Risk register

- **Queue-illegal states (R10) are the blocker the first draft missed.** `PIXEL_SHADER_RESOURCE`,
  `RENDER_TARGET`, `DEPTH_*` cannot appear in a barrier on a compute list, the engine's combined
  read states contain them everywhere, and the enhanced translation table is queue-blind. Steps 5
  and 7 exist for this; do not defer it into Step 9 and discover it as a debug-layer error.
- **The barrier compile's single-linear-order assumption is the structural work (Step 7),** and its
  **cross-frame cache (R9) is the part that fails silently.** A cache hit serving barriers whose
  before-state the other queue already moved past is GPU corruption with no message. Every claim of
  "unchanged barriers" runs under `--barrier-cache-verify`.
- **Per-frame ring lifetime.** Descriptor, sampler and upload rings are reset per frame-in-flight
  slot on the assumption that one fence decides when the slot is free. A compute queue running
  ahead or behind breaks that. **This engine has already shipped one bug of exactly this shape** — a
  staging cache keyed on the frame-in-flight SLOT instead of the frame NUMBER, handing out
  descriptor addresses the ring had already reused, visible only under GBV as wrong descriptor
  types. Any "is this frame done" test must consider both queues (Step 2).
- **Profiler readback (R7).** A drain fence signalled on one queue declaring another queue's
  resolves readable produces plausible-looking wrong numbers — the worst failure mode available to a
  measurement tool, and this whole plan is judged by that tool.
- **Out-of-band submissions (R12).** Uploads and editor thumbnail work bypass the timeline entirely.
- **Deadlock.** A wait ordered before its signal hangs the GPU with no message. Every cross-queue
  edge must be signalled from a queue submitted earlier in the same frame; the graph should reject
  a cycle at build time rather than at 3 am.
- **Async compute can be slower.** Both queues contend for the same shader cores; a compute pass
  overlapped with an occupancy-bound raster pass can slow both. Hence Step 9's "no regression" bar
  and the permanent `--no-async-compute`.
- **The COMPUTE command-list lane has never executed (R2).** Cheap to prove in Step 1; expensive to
  debug if it first runs concurrently with everything else in Step 9.
- ~~The bind cache is `thread_local` and reset per command-list acquire.~~ **Closed** — R13:
  `BeginThreadCommandList` already resets it for every list type (`Renderer.cpp:683`).

## Non-goals

- **A copy queue** for uploads. The same architecture makes it straightforward afterwards, but it
  has its own lifetime rules — separate piece of work. (Step 11 still has to *decide* what the
  existing direct-queue uploads mean for the compute queue; that is not the same as building one.)
- **Splitting rasterisation** across queues. Two raster workloads contend for the same units.
- **Reworking what the passes themselves do.** This plan changes where work runs, not what it is.
  The one exception is Step 8, which splits a pass along a line that already exists inside its body.

## After this plan — where the second queue is actually worth more

Recorded here so it is not re-derived later, and explicitly out of scope for the eleven steps above.
The 7.8 % ceiling applies to *overlapping work that must finish this frame*. The larger prize is
work that does **not** have to:

- **Wind-deformed BLAS refit** (step RW of `docs/rt_shadows_integration_plan.md`) — a per-frame
  rebuild that nothing in the same frame needs at full freshness.
- **Editor thumbnail rendering** (`AssetThumbnailCache`), which today contends with the frame.
- **Ocean mip chain / foam**, if the split in Step 8 shows them separable from the FFT.

Those are "second queue as a background lane", not "second queue as an overlap", and they are worth
a separate plan once the machinery here exists.
