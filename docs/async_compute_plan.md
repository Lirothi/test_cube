# Async compute — architecture plan

**Status: PLAN COMPLETE. Steps 1-8 committed (through `2be588e`); steps 9-11 done, uncommitted. The renderer has a second queue, two passes use it, and it is worth -3.0 % wall-clock.**

**Goal: the renderer gains a second execution queue as a first-class architectural capability.**
The render graph learns to schedule a pass onto a `D3D12_COMMAND_LIST_TYPE_COMPUTE` queue,
cross-queue dependencies become fence edges the graph owns, and the barrier compile learns
per-queue resource ownership *and per-queue state legality*.

This is a capability, not an optimisation. Perf is a consequence and a regression check — **it is
not the acceptance criterion for any step.** The engine should be able to express "this work runs
on the async queue" because that is the shape a modern renderer needs; what it is worth on today's
scene is a separate question, answered later and per-pass. The honest numbers on today's scene are
computed in "Reference points" below — **4.2 % for the first mover alone, ~14.8 % theoretical across
all six passes that can move, ~7-10 % realistically after contention** (measured 2026-08-28, second
capture). Write those down now so nobody is surprised by them at the end, and note two things: the
budget only converts to frame time because the frame is measurably GPU-bound, and **these
percentages have moved on every single capture — three captures, three answers** because the frame
moves around them. The structural claims come from the
render graph and are stable; the numbers are a dated snapshot and must be re-measured, never
inherited.

This plan is written for an AI executor. Every step is independently buildable, independently
verifiable, independently committable. **Do not merge steps.** Several exist purely to prove the
machinery they add is INERT — that proof is the deliverable, and skipping it is how this class of
change becomes undebuggable.

---

## Executor Guide (read first)

**Repo:** `D:\Programming\test_cube`. Conventions match
`docs/enhanced_barriers_migration_plan.md`; the essentials are repeated here.

**Citations were verified against the tree on 2026-08-28 at HEAD `9d35d8c` ("rt passes separation"),
working tree clean; the numbers come from a capture taken at that HEAD on `data/levels/demo.json`.**
Those dates matter. This
file was first written against a tree that changed underneath it within the hour —
`SceneRenderer.cpp` was split into `SceneRenderer_{Graph,Geometry,Lighting,Post,Reflections,Shadows}.cpp`,
the render graph moved to `AddPass2`, and four passes were added to `RenderPass`. A day later five
more commits landed (`RT upgrades`, `rt wind reaction`, denoiser/SSR tuning), `Main_RTDenoise` was
deleted and folded into `Main_ReflectionTemporal`, and the measured frame changed by 38 %. Hours
after that, `9d35d8c` split `Main_RTReflections` into `Main_RTTrace` + `Main_RTResolve` — which
changed this plan's conclusions, not just its numbers.
**Re-check any `file:line` here before trusting it**, re-capture before any step that quotes a
number, and if a symbol has moved, fix the citation as part of the step rather than working from the
prose alone.

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
diff of the cache against itself, i.e. no proof at all. (Flag parsed in `main.cpp:284`.)

**Trace capture:**
```
x64\Release\test_cube.exe --trace=120 --shot-delay=8 --wind-freeze=3
```
writes `traces/trace_*.json` (Chrome-trace format) and exits. **Read it with a script, not with
your eyes** — group by name across ALL frames. A single frame proves nothing; `--wind-freeze` makes
the scene reproducible. The GPU track is `tid == 0` ("GPU Queue" in the `thread_name` metadata);
`GPU.Frame` events carry a per-frame suffix, so strip it before grouping.

**For a REGRESSION verdict on `GPU.Frame`, use the MEAN, not the median.** Learned the expensive way
in Step 1: the median said +2.9 % and the mean said +0.2 % for a change that cannot touch GPU work
at all. The cause is `Pass_BuildAS`, which is **bimodal with the two modes almost equally populated**
(~83 us and ~272 us, about 50/50 since the wind-deform refit landed). Its median therefore lands
wherever the 50th percentile happens to fall *between* the modes, and a three-frame shift in the
cheap/expensive ratio moves it by 170 us — which propagates straight into the frame median. Its
mean, p25, p75 and p95 are all stable across runs to within 2 %.

So: **mean for the verdict, and always diff PER SCOPE before believing a frame-level number.** The
per-scope diff is what identified this in one step — every other scope moved by ±2 % while BuildAS
moved by +122 %, which is the signature of a statistic misbehaving, not of a regression. Take two
post-change captures to establish the run-to-run floor (0.6 % on the mean here) before comparing
against anything.

**Line endings:** `.cpp/.h/.hlsl` are CRLF. Editing tools rewrite whole files as LF — re-normalise
and verify (lone-LF count 0) after every scripted edit. (This document is LF; leave it that way.)

**Do not commit.** The user commits per step.

---

## Reference — what exists today

Everything below was read out of the code, not remembered. File:line is given so the executor can
re-check rather than trust — see the staleness warning above.

**R1. One queue.** `GraphicsDevice::InitQueue` creates a single `D3D12_COMMAND_LIST_TYPE_DIRECT`
queue (`GraphicsDevice.h:78`); `Renderer::GetCommandQueue()` returns it and everything submits
there (`Renderer.cpp:913`).

**R2. Command list plumbing is type-aware, but the COMPUTE lane has NEVER RUN.** `FrameResource`
pools allocators and lists per `D3D12_COMMAND_LIST_TYPE` (`FrameResource.h:53`, `QueueIndex_`) and
`Renderer::BeginThreadCommandList(type)` takes the type (`Renderer.cpp:723`). But
`D3D12_COMMAND_LIST_TYPE_COMPUTE` appears in exactly **two files across `sources/`** — `FrameResource.h`
and `Renderer.cpp` — and both are the plumbing itself: **no code has ever acquired a COMPUTE
allocator or list.** (Re-verified 2026-08-27.) So the plumbing needs no design work, but it is
unexercised: `CreateCommandList(COMPUTE)`, its `Reset`, and the `SetDescriptorHeaps` at
`Renderer.cpp:743` are all first-run-in-Step-1 code.

**R3. Submission is one flat batch, plus two fixed lists.** `SubmitTimeline::GatherFrameLists`
flattens the frame's pass batches into one array — per batch the driver list (bundles executed into
it), then the direct lists. `Renderer::ExecuteTimelineAndPresent` (`Renderer.cpp:827`) then wraps
that array with a **GPU-profiler frame-begin list at the front** (`Renderer.cpp:862`) and a
**present/frame-end epilogue list at the back** (`Renderer.cpp:885`) and submits the whole thing
with a single `ExecuteCommandLists` (`Renderer.cpp:913`), then `SignalFrame`. Any "byte-identical
submission" claim must dump `fixedSubmitScratch_`, not `submitListsScratch_`.

**R4. The render graph has NO notion of a queue.** Passes schedule onto worker THREADS;
dependencies are recording-order constraints, not GPU sync. Nothing in `RenderGraph.h` mentions a
queue.

**R5. Barriers are compiled ahead of execution, on ONE linear order.**
`RenderGraph::CompileBarriers` (`RenderGraph.h:713`) walks the schedule carrying a single running
state per resource, seeded from `GetPredictedState` (`RenderGraph.h:782`), and writes barriers into
per-(pass, point) slices. **This ahead-of-recording knowledge is the thing that makes cross-queue
ownership tractable at all** — the deleted `ResourceStateTracker` only knew states at record time,
per command list, in TLS, and could never have produced a release/acquire pair. Enhanced barriers
are the default since the barrier migration, so queue-scoped layouts (`LAYOUT_COMPUTE_QUEUE_*`,
`LAYOUT_DIRECT_QUEUE_*`) are available to express handoff without a round trip through COMMON.

**R6. Per-frame resources are recycled per frame-in-flight SLOT.** `Renderer::BeginFrame`
(`Renderer.cpp:359`) calls `ResetPerFrame()` on the descriptor and sampler rings and resets command
allocators for the current slot, after `WaitForFrame(slot)`. Everything assumes **one** fence
decides when a slot is free (`FrameScheduler.cpp:40-58`). `kFrameCount == 3`.

**R7. GPU timestamps are single-queue, and the readback fence is worse than single-queue.**
`Profiler::InitGpu(device, queue, maxQueries=1024)` (`Profiler.cpp:1089`) — one query heap, one
`GetTimestampFrequency`, one `GetClockCalibration`, one track in the trace. A capture taken today
confirms it: the trace's `thread_name` metadata lists exactly one GPU row, `tid 0 = "GPU Queue"`.
Beyond that:
- `nextGpuQuery_` is one global counter under `gpuMtx_` (`Profiler.cpp:1179`), and
  `gpuRecordingReadbackSlot_` one global slot rotation (`Profiler.cpp:637`);
- **`gpuDrainFence_` is signalled ONLY on the direct queue** (`Profiler.cpp:624-634`). A batch is
  declared readable when that fence passes. A `ResolveQueryData` recorded on the compute queue is
  not covered by it, so the collector would map a readback range the compute queue has not written
  yet and report fictitious numbers — silently.

**R8. `Main_ObjectCompute` is one of FOUR passes glued into a single CL group.**
`SceneRenderer_Graph.cpp:81-132`: `BeginCLGroup()` wraps `Main_PrologueClear`,
`Main_ObjectCompute`, `Main_SurfSim` and `Main_ShoreWetness`, so all four share ONE DIRECT command
list, one batch and one task. The group exists because each is tiny and per-CL overhead dominated
them. **A pass inside a CL group cannot change queue** — the group's list is provisioned once, as
DIRECT, by `RenderGraphPassContext::BeginCL` (`RenderGraph.h:131`). Dissolving it costs three extra
command lists, not one.

**R9. The barrier compile has a cross-frame CACHE, keyed per frame-in-flight slot.**
`RenderGraph.h:333-343`. Its key is an exact byte copy of (compile order + pass names + slices, the
whole `ResourceUse` arena) plus `renderer->DeclarationsGeneration()` (`CompileInputsUnchanged`,
`RenderGraph.h:883`), and it only stores a compile whose output is a **fixed point** — every touched
resource ends where it began (`RenderGraph.h:825`). Consequences for this plan:
- the queue a pass runs on becomes part of the cache key;
- the fixed-point test becomes per-queue (a resource must return to its incoming state *on its own
  queue*, not merely somewhere);
- any "the compiled barriers are byte-identical" proof must run with `--barrier-cache-verify`,
  which recompiles on a hit and diffs, or it proves nothing.

**R10. Legacy resource states the engine uses are NOT ALL LEGAL ON A COMPUTE QUEUE.** This is the
structural blocker for the first mover, and the original draft of this plan missed it entirely.
D3D12 restricts a compute command list to `COMMON`, `UNORDERED_ACCESS`,
`NON_PIXEL_SHADER_RESOURCE`, `COPY_SOURCE`, `COPY_DEST` (plus indirect-argument /
acceleration-structure). Meanwhile the combined read state is used everywhere the ocean touches:
- `OceanSimulation::PrepareUpdate` registers `NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE`
  for displacement and foam (`OceanSimulation.cpp:1102-1133`);
- `OceanSurfSim`'s pass builder does the same for its wave/foam ping-pong pair and the shore map
  (`OceanSurfSim.cpp:299-340`) — and that pass is a CL-group sibling (R8), so it is on any list of
  things that might move;
- `barriers::LegacyStateToBarrier` is **queue-blind** (`BarrierTranslation.h:34`): it would hand a
  compute list `SYNC_PIXEL_SHADING` / `LAYOUT_GENERIC_READ`, which is equally illegal in the
  enhanced model.

**The engine's DEFAULT read state is compute-illegal.** `SceneRenderer_Graph.cpp:49` defines
`kSrvAll = PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE` — "one combined state is one barrier
instead of a flip between them" — and uses it at **32 sites**. So this is not a quirk of the ocean:
most passes that read a G-buffer target declare a state no compute queue accepts. Of the Step 10
movers, `Main_RTTrace` (depth, gb1) and `Pass_Hzb` (depth) declare `kSrvAll`; `Pass_Gtao` uses a
NON_PIXEL-only constant (`kAoRead`) and is clean, as is the wetness pass.

So per-queue state is not only about *which* barrier is emitted (Step 7) but about *whether the
requested state can be expressed on that queue at all*. That gets its own checked rule (Step 5), and
D7 is the standard remedy: the async pass declares NON_PIXEL only, the graphics consumers keep their
own PIXEL half.

**R11. `Main_ObjectCompute` is three unrelated workloads with three different consumers.** The
builder collects the objects whose `PrepareCompute` returns true and the body runs exactly that list
(`SceneRenderer_Graph.cpp:95-117`, `SceneRenderer_Geometry.cpp:193-209`):

| workload | writes | consumed by | slack |
|---|---|---|---|
| `GpuInstancedModels::RecordCompute` (GI rotation) | `instanceBuffer_` (UAV) | **`Main_ShadowCull`**, via the GI scatter (`ShadowGpuData.cpp:1791`) — and the scatter's source really is the same buffer: `GetInstanceCasterResource()` returns `instanceBuffer_.GetResource()` (`GpuInstancedModels.h:44`) | ~none: two passes later |
| `OceanSimulation::Update` (spectrum/FFT/mips/foam) | `Ocean.Displacement`, `Ocean.FoamTurbulence` | **`Main_Transparent`** — its builder walks transparent objects calling `PrepareRender` (`SceneRenderer_Graph.cpp:1124-1128`), and the ocean's registers displacement + prevDisplacement there (`OceanRenderable.cpp:777-781`); the ocean is `IsTransparent() == true` | the whole frame |
| `ParticleEmitterObject` sim/sort | particle/dead-list/sorted buffers (all UAV, plus COPY_SOURCE for the debug readback) | **`Main_Transparent`**, same point | the whole frame |

Moving the pass whole means fencing `Main_ShadowCull` on it, which is near-zero overlap plus the
cost of the sync. Splitting it is therefore not a refinement, it is the difference between the
architecture demonstrating anything and not.

**R12. Out-of-band direct-queue submissions exist.** `UploadBatch::Submit` calls
`ExecuteCommandLists` on the direct queue outside the frame timeline (`UploadBatch.cpp:38-54`), and
the editor's `AssetThumbnailCache` signals the same queue directly
(`AssetThumbnailCache.cpp:1610`). Today one queue orders them against everything else for free.
A compute queue does not see them.

**R13. The `thread_local` bind cache is already safe.** `BeginThreadCommandList` calls
`render::g_clBindState.Reset()` for every list type it hands out (`Renderer.cpp:751`), so a compute
list cannot inherit graphics root state. This was on the original risk register; it is closed. No
work required, but do not delete the `Reset` while doing Step 1.

**R14. Some passes read resources UNDECLARED, and the compile cannot see those reads.**
`OceanRenderable::BuildSurfSimPass` states it outright: "both shore maps are read UNDECLARED at
their post-build resting states, so the sim must not run on a frame that (re)builds them"
(`OceanRenderable.cpp:696-702`) — the ordering is enforced by *graph position*, not by a
declaration. D2 derives cross-queue fence edges from declarations, so **an undeclared read produces
no edge**: a pass moved to the async queue keeps its graph position but loses the implicit ordering
that position gave it. Any pass considered for the async queue must first be checked for undeclared
reads, and they must be declared (or the pass must not move).

**R15. The graph's authoring API is `AddPass2`, and `SetPassPrepare` is PRIVATE.** A pass is one
builder — `BuildFn = std::function<ExecFn(PassContext&)>` — that makes the frame's decisions,
declares from them via `ctx.Use()/ctx.NextPoint()`, and returns the record lambda (an empty return
means "does nothing this frame"): `RenderGraph.h:433`. `SetPassPrepare` survives only as an
implementation detail of `AddPass2Internal` and is private with an assert (`RenderGraph.h:1295`),
precisely so no new pass can be written as a hand-mirrored Prepare/Record pair. **Every "AddPass
gains X" instruction in this plan means `AddPass2`.** Builders also read `ctx.usePoint` directly to
record which point a declaration landed on (see `SceneRenderer_Graph.cpp:165-183`), so a queue
change that shifts points is visible to pass authors.

---

## Design decisions

**D1. The queue is a property of the PASS, fixed when the graph is built.** Not of the command
list, not of the material, not decided at record time. `AddPass2` gains an optional `RenderQueue`
(default `Graphics`); the pass body acquires a list of the matching type. Anything later than
graph-build time makes the barrier compile undecidable, because the compile runs before any body
records. Note that with R15's shape, "graph build time" and "declaration time" are the same moment —
the builder is both — which is strictly better for this plan than the old split Prepare/Record.

**D2. Cross-queue synchronisation is a graph EDGE, not a hand-written fence.** A dependency that
crosses queues compiles into `Signal` on the producer queue + `Wait` on the consumer queue at
submit time. Hand-placed fences inside pass bodies are forbidden: they are invisible to the
compile, which is precisely the class of bug the barrier migration spent sixteen steps deleting.
**Edges are derived from declarations, so R14's undeclared reads are invisible to this** — that is a
precondition to check, not an edge case to hope about.

**D3. A resource that crosses queues gets a RELEASE on the source queue and an ACQUIRE on the
destination queue.** Two half-barriers, at two different points, in two different command lists,
with a fence between — not the single point the compile emits today. This is the structural change
and it is Step 7.

**D4. Every step keeps the default behaviour until Step 8, and there is a permanent off switch.**
All passes stay on the graphics queue while the machinery is built. `--no-async-compute` forces
everything back and survives after the work lands, exactly as `--legacy-barriers` did — a suspected
async regression must be one flag away from being bisected, not a rebuild away.

**D5. Eligibility is a stated rule, not a per-pass hunch.** A pass may run on the async queue when
it (a) records only compute/copy work, (b) is not a member of a CL group (R8), (c) registers only
states legal on a compute queue (R10), (d) reads nothing undeclared (R14), (e) has no resource in
common with a concurrently scheduled graphics pass other than through a declared edge, and (f) does
not touch the swapchain. Step 5 turns (a)-(c) into a build-time invariant; (d) is a review check
the step also automates where it can; (e) is what the Step 7 compile computes; (f) is one line.

**D6. Queue legality is checked at REGISTRATION, not discovered at emission.** A pass marked
`AsyncCompute` whose builder calls `ctx.Use(res, PIXEL_SHADER_RESOURCE)` must fail fast right there,
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

**Re-measured 2026-08-28 (second capture of the day)** from
`traces/trace_20260828_172645_release_000.json`, 123 frames, medians of the GPU track (`tid 0`), at
HEAD `9d35d8c` ("rt passes separation"), level `data/levels/demo.json` — which is also what boots by
default in Release, so every capture in this document is the same level.

**READ THIS BEFORE USING ANY NUMBER BELOW.** This table is not a property of the engine. It is a
property of the camera, the level's contents and settings, and the code at the moment of capture,
and it moves FAST. Three captures over two days:

| scope | 08-27 | 08-28 a | 08-28 b |
|---|---|---|---|
| GPU.Frame | 3476 us | 2157 us | **3234 us** |
| Pass_VsmPageRender | 1237 us | 312 us | **488 us** |
| Pass_Tonemap (incl. bloom) | 372 us | 67 us | **664 us** |
| &nbsp;&nbsp;Pass_BloomConv | 303 us | absent | **595 us** |
| Pass_BuildAS | 20 us | 20 us | **86 us (mean 206)** |
| RT reflections total | 176 us | 87 us | **131 us** (118 trace + 13 resolve) |
| Pass_Gtao | 73 us | absent | **60 us** |
| Ocean.Surface | 60 us | 292 us | **241 us** |

Passes have appeared, vanished and returned; the frame has swung 3476 -> 2157 -> 3234. **The
structural claims in this plan come from the render graph's prereq lists and are stable. Every
percentage is a dated snapshot.** Re-capture before Step 8 and again before Step 10, and treat any
inherited number as wrong until re-measured. The first version of this plan was wrecked exactly this
way, twice.

| scope | median | mean | share |
|---|---|---|---|
| **GPU.Frame** | **3234 us** | 3266 | 100 % |
| Pass_Tonemap | 664 us | 727 | 20.5 % |
| &nbsp;&nbsp;Pass_BloomConv | 595 us | — | 18.4 % |
| Pass_VsmPageRender | 488 us | 542 | 15.1 % |
| Pass_DLSS | 262 us | 305 | 8.1 % |
| RenderObjectBatch (transparent) | 252 us | — | 7.8 % |
| &nbsp;&nbsp;Ocean.Surface | 241 us | — | 7.5 % |
| **Pass_ObjectCompute** | **135 us** | 145 | 4.2 % |
| **Pass_RTTrace** | **118 us** | 131 | 3.6 % |
| ExecuteBundles | 116 us | 120 | 3.6 % |
| &nbsp;&nbsp;VsmPageRender.Scatter | 95 us | — | 2.9 % |
| **Pass_BuildAS** | **86 us** | **206** | 2.7 % |
| Pass_VsmPageRequest | 85 us | 91 | 2.6 % |
| Transparent.Driver | 71 us | 71 | 2.2 % |
| **Pass_Gtao** | **60 us** | 65 | 1.9 % |
| Pass_GlassReflections | 51 us | 52 | 1.6 % |
| **Pass_ShadowCull** | **50 us** | 50 | 1.5 % |
| Pass_Lighting | 43 us | 49 | 1.3 % |
| Pass_ExposureMetering | 36 us | 41 | 1.1 % |
| **Pass_Hzb** | **28 us** | 36 | 0.9 % |
| Pass_Compose | 26 us | 26 | 0.8 % |
| VsmPageRender.Setup | 14 us | — | 0.4 % |
| **Pass_RTResolve** | **13 us** | 13 | 0.4 % |

Bold rows are the movable set (Step 10). **The scopes NEST — do not add the shares up.**
`Pass_BloomConv`, `Tonemap.Curve` and `Tonemap.Resolve` sit inside `Pass_Tonemap`;
`VsmPageRender.Scatter` and `.Setup` sit inside `Pass_VsmPageRender`; `Ocean.Surface` sits inside the
transparent `RenderObjectBatch`.

**`Pass_BuildAS` is BIMODAL — quote its mean, not its median.** Since the wind-deformation refit
landed its distribution is min 80 / med 86 / **p75 273 / p95 625** / max 723. A quarter of frames pay
three times the median. For a frame-time budget that makes its mean (206 us) the honest figure, and
it makes the pass a better async candidate than its median suggests, not a worse one. (`Pass_Hzb`
also shows a 407 us max against a 28 us median, but that is a single outlier frame — p95 is 29.)

**The frame is hard GPU-BOUND again.** From the same capture:

| | 08-27 | 08-28 a | 08-28 b |
|---|---|---|---|
| CPU frame | 3468 us | 2140 us | **3270 us** |
| GPU frame | 3476 us | 2157 us | **3234 us** |
| `Renderer::WaitForFrame` | 2814 us | 1226 us | **2544 us** |
| actual CPU work per frame | ~654 us | ~915 us | **~726 us** |

The CPU idles 78 % of the frame on the fence, so GPU savings convert to frame time ~1:1. **Re-check
this ratio before Step 8** — it was down to 57 % one capture ago, and if the GPU side ever drops
near the CPU side the whole budget stops converting.

**The frame is still almost perfectly SERIAL** — one median-duration frame, 28 top-level scopes,
**55 us of total gaps** out of 3234. Typical layout (median start offset / median duration; the two
`RenderObjectBatch` entries are the opaque draws at ~432 and the transparent/ocean draws at ~1986):

```
Pass_BuildAS             0 ..   86      Pass_Gtao             1377 .. 1437
Pass_PrologueClear      90 ..  107      Pass_Lighting         1441 .. 1484
Pass_ObjectCompute     111 ..  246      Pass_SpotLights       1489 .. 1523
Pass_ShadowCull        278 ..  328      Pass_PointLights      1525 .. 1549
GBuffer.Driver         331 ..  339      Pass_Skybox           1554 .. 1561
ExecuteBundles         342 ..  458      Pass_RTResolve        1564 .. 1577
RenderObjectBatch (op) 432 ..  453      Pass_Reflection.Temporal 1581 .. 1596
Pass_VsmPageRequest    630 ..  715      Pass_Reflection.Blur  1599 .. 1619
Pass_Hzb               720 ..  748      Pass_Compose          1623 .. 1649
Pass_RTTrace           753 ..  871      Glass (Gbuf + Refl)   1652 .. 1712
Pass_VsmPageRender     878 .. 1366      Transparent.Driver    1723 .. 1794
                                        RenderObjectBatch (tr) 1986 .. 2242
                                        Pass_ExposureMetering 2205 .. 2241
                                        Pass_DLSS             2249 .. 2511
                                        Pass_Tonemap          2528 .. 3192
```

**The first-mover ceiling: 3.6 %.** The first mover is `Main_RTTrace` (Step 8) at 118 us of a
3234 us GPU frame. The ocean/particle compute that used to hold that slot is 135 us before its split
and less after it, and it now moves in Step 10 — measure the split in Step 9 before believing its
number.

**The full analytic budget: ~477 us median / ~583 us mean, i.e. 14.8 % / 18 %.** Six passes, derived
in Step 10 from the graph's own prereq lists. Realistically **7-10 %** after contention. That is
roughly double the previous capture's budget, and the reason is `9d35d8c`: splitting the RT
reflection into trace + resolve converted 118 us of previously wedged work into movable work, and
the wind-deform refit made `Main_BuildAS` a real cost.

**The overlap partner is `Pass_VsmPageRender` (488 us, 15.1 %)**, still the longest raster block and
still the best kind of partner — depth-only rasterisation of many instances is geometry- and
fixed-function-bound with low ALU occupancy. The graphics work available in the window before the
last mover's consumer (`Pass_RTResolve` at ~1564) totals ~840 us against ~477 us of compute to hide,
so the budget fits with room to spare. **If the two-track trace does not show the async passes
running under `Pass_VsmPageRender`, the move did not work.**

**Context, so the budget is not oversold.** The single biggest item in the frame is now
`Pass_Tonemap` at 664 us (20.5 %), almost all of it `Pass_BloomConv` (595 us), followed by
`Pass_DLSS` (262 us). Async compute touches neither — they are a serial tail chain. This plan is
worth doing for the capability, and 7-10 % is a real number, but the frame's biggest line item is
elsewhere.
**First mover: `Main_RTTrace`** (Step 8). It is pure compute, it is already a standalone pass so it
needs no graph surgery first, and its only consumer is `Main_RTResolve` at ~1564 — so the overlap
window spans the VSM page request, the page render, lighting and the small light passes. The
ocean/particle half of `Main_ObjectCompute` follows in Step 10 once Step 9 has split it; the GI
rotation never moves, because `Main_ShadowCull` consumes its output two passes later (R11), which is
a fence, not an overlap.

**GPU.Frame stops meaning "all GPU work" at Step 8.** It is bracketed by two timestamps on the
direct queue (`Renderer.cpp:862` / `:885`). Once work overlaps, the regression metric is
**wall-clock** (`CPU.Frame` / FPS) plus per-queue GPU spans, not `GPU.Frame`. Steps 1-8 may keep
using GPU.Frame because nothing overlaps yet.

---

## Steps

### Step 1 — the compute queue exists, is idle, and its command-list lane provably works

`GraphicsDevice` creates a second queue (`D3D12_COMMAND_LIST_TYPE_COMPUTE`) alongside the direct one
and exposes it. Nothing submits to it. Log it in `logs/device_caps.log` beside the other caps (that
file already exists — `GraphicsDevice.cpp:266`).

Because R2's COMPUTE lane has never run, this step also proves it: once, behind a temporary flag,
acquire a COMPUTE allocator + list from `FrameResource`, confirm `SetDescriptorHeaps`
(`Renderer.cpp:743`) and `Reset` succeed, close it, and **throw it away without submitting**.

**Acceptance:** both builds `0/0`; all three correctness gates unchanged; the COMPUTE acquire
succeeds and is logged; `--trace` GPU.Frame within run-to-run noise of the pre-step number (record
both, and read the mean — see the Executor Guide).

**DONE 2026-08-28, uncommitted.** 152 insertions, 0 deletions, across five files — purely additive,
which is what an inert step should look like.

- `GraphicsDevice::InitQueue` now creates `Queue.AsyncCompute` beside `Queue.Direct` and **appends**
  its verdict to `device_caps.log` (InitDevice opens that file with `"w"`, this with `"a"`, because
  the queue does not exist yet when the caps line is written). Creation failure is **non-fatal** and
  leaves the pointer null: a compute queue is a capability, and every later step must read null as
  "async unavailable" rather than as an error — which is also what makes `--no-async-compute` a real
  fallback. `ReleaseQueue` drops the compute queue first, keeping the original teardown order.
- `Renderer::GetComputeQueue()` / `HasComputeQueue()` expose it. Nothing calls them yet.
- `Renderer::ProbeComputeLaneOnce()` runs at the end of the first `BeginFrame` under
  `--compute-lane-probe`, acquires one COMPUTE allocator + list, binds the frame's descriptor heaps,
  closes it and drops it. **Submits nothing.** One bool test per frame with the flag off.

**Results.** Both builds `0/0` (verified by object timestamps, not just the log). All three gates
`verdict: CLEAN`, 0 MISSING, no `barrier_diag.log` produced at all, `emit legacy=0` in every run.
The probe:

```
[caps] async compute queue: created (idle)
[caps] compute-lane probe: OK (allocator+list acquired, 2 heaps bound, closed) | compute queue: present
```

That is the first time `CreateCommandList(D3D12_COMMAND_LIST_TYPE_COMPUTE)` has ever executed in
this engine (R2), and it works on the first try — allocator, list, `Reset`, and both descriptor
heaps.

**Perf: inert.** `GPU.Frame` mean 3252 us pre -> 3258 us post (**+0.2 %**), against a 0.6 %
run-to-run floor measured from two post-change captures. The median said +2.9 %, which was false —
see the Executor Guide's note on `Pass_BuildAS` bimodality, written from this step.

### Step 2 — cross-queue fences, a frame slot that waits for BOTH queues, and every idle path idling both

Extend the frame scheduler so a frame-in-flight slot is free only when **both** queues have passed
its fence value (R6 assumes one). Add the signal/wait helper the graph will use in Step 6.

**Every GPU-idle path idles both queues in this step, not in the hardening step.**
`FrameScheduler::WaitForGpuIdle` takes one queue (`FrameScheduler.cpp:60`) and
`Renderer::WaitForPreviousFrame` passes the direct one (`Renderer.cpp:929`); resize, level switch,
shutdown and `ResetFrameState` all route through them. The gates for this very step are
`--scene-stress`, which is nothing *but* reload/switch/resize churn — leaving this until the end
means every intervening step is validated by a harness that cannot be trusted.

Prove the machinery with a deliberately empty compute submission: each frame open a COMPUTE list,
close it, submit it, signal, and make the slot's release depend on that signal too.

**Acceptance:** all three gates CLEAN with the empty compute submission running; a Debug assert
fires if any per-frame ring or allocator is reused before both fences; 500+ frames of
`--scene-stress`; GPU.Frame unchanged within noise.

**DONE 2026-08-28, uncommitted.** 240 insertions / 16 deletions across `FrameScheduler.{h,cpp}`,
`Renderer.{h,cpp}`, `main.cpp`.

- **Two fences, one value.** `Fence.Frame.Direct` + `Fence.Frame.AsyncCompute`, both signalled with
  the same value per slot, so `frameFenceValues_` stays one array and "free" is one expression.
  NOT one fence signalled by both queues: a shared fence completes out of order, so
  `GetCompletedValue() >= V` would stop meaning "both queues passed V" — which is the only question
  this class answers.
- **`WaitForFrame`** waits on both (direct first — it is almost always the later of the two, so the
  second wait usually returns immediately). **`IsFrameComplete`** is the non-blocking form.
- **`SignalFrame(direct, compute, slot)`** signals both. When the device refused a compute queue the
  compute fence is advanced **from the CPU**, so the "both passed V" rule needs no null special case
  anywhere else.
- **`WaitForGpuIdle(direct, compute)`** idles both. This is the whole reason the step is here and
  not in hardening: every resize, level switch, editor command, thumbnail eviction, screenshot and
  shutdown path in the engine — ~50 call sites — funnels through `Renderer::WaitForPreviousFrame`
  into this one function, so one edit makes all of them queue-correct.
- **`SignalCrossQueue` / `WaitCrossQueue`** on a third fence (`Fence.CrossQueue`), dormant, for
  step 6's D2 edges. `WaitCrossQueue` is a GPU-side `ID3D12CommandQueue::Wait` — a cross-queue
  dependency must cost the CPU nothing.
- **`--async-empty-submit`** puts one empty COMPUTE list on the async queue every frame. It is the
  step's proof device: without it the compute fence is signalled on an idle queue and completes
  instantly, so every wait added here would be vacuously true and would ship untested.
- **The Debug assert** sits in `BeginFrame` immediately before the per-frame pools are reset — the
  exact point where reuse would happen.

**Results.** Both builds `0/0` (object timestamps checked, not just the log). All three gates
`verdict: CLEAN` **with `--async-empty-submit` on**, plus a 45-iteration run (well past 500 frames)
CLEAN. `0 MISSING`, and `--canonical-check` reported `frame end: 0 of 257 declared resources
off-canonical`.

**The assert was PROVEN to fire, not just written.** A throwaway build stranded the compute half
(never signal its fence, skip its wait) and logged the predicate:

```
[step2-selftest] slot=0 frameValue=5 directCompleted=6 computeCompleted=4 -> IsFrameComplete=FALSE
```

The direct fence had already passed (6 >= 5) — under the old single-queue rule that slot would have
been declared free and its rings recycled — while the compute fence sat at 4. The guard catches
exactly the case the old code could not see. The self-test was then **removed entirely** rather than
left behind a `#if 0`: a disabled switch that silently corrupts frames when flipped is a landmine,
not documentation. Confirmed removed by the rebuilt `FrameScheduler.obj` returning to its exact
pre-self-test byte size.

**Perf: inert.** `GPU.Frame` mean 3258 us -> 3251 us (**-0.2 %**) with the empty submission running,
against a 0.2 % run-to-run floor. Per-scope deltas all <=18 us and mixed in sign — noise, no
systematic shift.

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
   emits a second row. Today there is exactly one (`tid 0 = "GPU Queue"`), so the trace-reading
   scripts key on that — update them in the same step.

The shared query heap and `nextGpuQuery_` counter can stay shared (a TIMESTAMP heap is valid on both
queue types) — but say so explicitly in a comment, because "shared heap, split fence" is exactly the
asymmetry a later reader will assume is a bug.

**Acceptance:** a `--trace` capture of Step 2's empty compute submission shows a second track; a
known-ordered pair (compute signal -> graphics wait) appears in the correct order on the shared
timebase; deliberately delaying the compute resolve does NOT produce garbage timings (it produces a
late batch); all gates CLEAN.

**DONE 2026-08-28, uncommitted.**

- **Per-queue calibration and frequency.** `GetTimestampFrequency` and `GetClockCalibration` are
  queue methods; two queues are not one clock. Mapping compute timestamps through the direct queue's
  calibration yields plausible numbers in the wrong place — the exact failure that would invent an
  overlap or hide a real one.
- **Per-queue drain fence**, one value signalled on both, batch readable only when both have passed.
  The old single fence declared compute resolves readable before the compute queue had run them.
- **Two trace tracks.** GPU rows are tid 0/1, CPU threads shift to tid 2+. The compute row is named
  **unconditionally**, so "the async queue did nothing" is visible as an empty named row rather than
  as an absent one.
- **The queue label is read off the recording command list** (`ID3D12GraphicsCommandList::GetType()`)
  instead of being plumbed through ~100 `GPU_SCOPE` call sites. Zero churn, and a scope cannot be
  mislabelled because the label comes from the thing doing the recording.
- **`Async.EmptySubmit` scope** added to step 2's empty compute list: an empty list emits no
  timestamps, so without it the second track would exist and be empty, proving nothing.
- **`--async-order-probe`**: the compute queue signals the cross-queue fence, the next frame's
  graphics submission waits on it. Gives step 2's dormant `SignalCrossQueue`/`WaitCrossQueue` their
  first exercise, and produces a pair whose order is true by construction.

**A REAL DEFECT THIS STEP SURFACED — and the reason the no-GBV gate is not redundant.** The first
cut shared the query heap and readback buffer between queues, reasoning that a TIMESTAMP heap is
valid on both queue types. True, and beside the point: a GPU scope on a compute list ends in
`ResolveQueryData`, which **writes** the readback buffer — so two queues were writing one resource
with no synchronisation. D3D12 said so exactly:

```
d3d12 ERROR (id=1047): ExecuteCommandLists: Buffer Resource is still referenced by write GPU
operations in-flight on another Command Queue ('Queue.Direct')
```

It fired only in the **no-GBV** gate, where break-on-error is active — the two GBV gates passed.
Fixed with **disjoint resources per queue** (own query heap + own readback buffer) rather than
fences: the profiler must not add synchronisation between the queues it is measuring. Cost: one
extra 8 KB heap and one extra readback buffer.

**Results (final build).** Both builds `0/0`; all three gates `verdict: CLEAN`, 0 MISSING, including
the no-GBV gate under `--async-order-probe`. Trace: `tid0 "GPU Queue"` 3341 events, `tid1 "GPU Queue
(async compute)"` 54 events, **0 garbage durations**, and **54 ordered pairs checked, 0 violations** —
every `Async.EmptySubmit` ends before the following `GPU.Frame` begins, across two independently
calibrated clocks.

**Late-resolve behaviour proven, not assumed.** A throwaway build signalled the compute drain fence
one value BEHIND (deadlock-free by construction: the value always arrives next frame). Result: same
63 frames captured, same compute-track population, **0 garbage durations** — a late batch, not
corruption. Reverted.

**A mistake worth recording:** the first attempt at that test STRANDED the compute drain fence
instead of delaying it, which deadlocked `WaitForGpuProfilerIdle` at capture-finish and hung the
app. Then the analysis script read the newest trace on disk — which was the PREVIOUS run's file,
because the hung run never produced one — and reported a confident, wrong verdict. Two lessons, both
already rules here and both re-learned the hard way: a test that can block must be deadlock-free by
construction, and **every run must be checked for a NEW artefact before its output is analysed**.

### Step 4 — the graph learns the word "queue" (inert)

`RenderQueue { Graphics, AsyncCompute }` on pass registration, defaulting to `Graphics`. **The API
is `AddPass2` (R15)** — one builder per pass, `SetPassPrepare` is private and must stay that way.
The pass context exposes the queue so `BeginCL` (`RenderGraph.h:131`) acquires the right list type.
**Every existing pass stays on Graphics.**

**Acceptance:** all gates CLEAN; GPU.Frame unchanged within noise; the enum is threaded through but
provably unused — grep shows no `AsyncCompute` at any call site.

**DONE 2026-08-28, uncommitted.** 77 insertions / 7 deletions in `RenderPass.h` + `RenderGraph.h`.
Two files, because the queue is a property of pass IDENTITY and of the GRAPH, and of nothing else.

- **`RenderQueue { Graphics, AsyncCompute }`** in `RenderPass.h`, next to `RenderPass` — the header
  that already carries pass identity and is cheap to include.
- **`Pass::queue`**, set in `AddPass2Internal` and cleared to `Graphics` by `Reset()` so a reused
  graph cannot inherit last frame's queue.
- **Two new `AddPass2` overloads** taking the queue as the SECOND argument, beside the pass name —
  not as a trailing default. `builder` is always the last argument and is always a multi-line
  lambda, so a trailing queue parameter would sit after twenty lines of lambda at every call site
  that used it, where it is both unreadable and easy to attach to the wrong pass.
- **`PassContext::queue` + `CommandListType()`**, and `BeginCL` now opens a list of the pass's own
  type. The mapping queue -> `D3D12_COMMAND_LIST_TYPE` lives in exactly one function, because step 6
  (per-queue submission) and step 8 (the first move) must agree with whatever a body already opened.
- **The CL-group branch stays DIRECT** and says why: a group shares one list, so its members share
  one queue. Step 5 turns "an AsyncCompute pass inside a CL group" into a fail-fast rather than a
  silent recording onto the wrong queue.
- **The 8 remaining v1 `AddPass` call sites were left alone.** All of them are the inner
  GBuffer/Transparent sub-graphs — raster fan-out that will never be an async candidate — so
  threading a queue through v1 would add API surface nothing can use.

**Results.** Both builds `0/0`; all three gates `verdict: CLEAN`, 0 MISSING.
**The inertness proof:** `RenderQueue::AsyncCompute` appears in exactly two places in the entire
tree — the `CommandListType()` mapping and a comment — and in **zero call sites**. Every pass is
Graphics, `BeginCL` therefore still asks for `TYPE_DIRECT` everywhere, and the frame is unchanged by
construction.

**PERF VERDICT DEFERRED — the machine was compiling Unreal Engine during this step, and the
measurement is not attributable.** Recorded so it is re-done, not quietly skipped:
`GPU.Frame` mean 3328 us (step 3) -> 3653 us (step 4), i.e. +9.8 %, with a 1.5 % run-to-run spread
(vs 0.2-0.6 % on a quiet machine). Three reasons not to read that as a regression, none of them
"it can't be, I only added an enum":
1. the **CPU frame moved in lockstep** (3383 -> 3658, +8.1 %) — a renderer change that added GPU
   work would show up in the fence wait, not in CPU work itself;
2. the per-scope deltas are concentrated in the heaviest passes (`Pass_Tonemap` +154 us,
   `Pass_BloomConv` +135 us) while others went DOWN (`Pass_Gtao` -29 us, `Ocean.Surface` -24 us) —
   broad inflation, not a located cost;
3. nothing in the diff reaches the GPU: an enum, a field, two assignments per pass per frame, and a
   `BeginCL` that still resolves to `TYPE_DIRECT`.
**Re-measure on a quiet machine before treating step 4 as perf-clean**, and prefer an interleaved
A/B/A/B against the previous commit's binary over a comparison across sessions.

### Step 5 — eligibility and queue legality as checked rules (inert)

D5 and D6 become invariants the graph checks at build time, while nothing is marked async — so every
one of them is currently unreachable, and the deliverable is the *test* that each fires.

- a pass marked `AsyncCompute` that is a member of a CL group fails fast, naming the pass (R8);
- `ctx.Use(res, state)` inside an `AsyncCompute` pass's builder fails fast when `state` is not legal
  on a compute queue, naming the pass, the resource debug name and the state (R10, D6). Reuse
  `RenderGraph::ResourceLabel` (`RenderGraph.h:1108`) so the message is readable;
- `barriers::` learns the target queue and refuses to translate an illegal state instead of emitting
  `SYNC_PIXEL_SHADING` onto a compute list (`BarrierTranslation.h:34`);
- a pass marked `AsyncCompute` that touches the swapchain fails fast.

R14's undeclared reads cannot be caught mechanically — nothing observes a read that never reaches
the graph. Write the check into the step as a REVIEW item with a named procedure: for each candidate
pass, list the resources its record body binds and diff that against what its builder declared.

**Acceptance:** all gates CLEAN and unchanged (nothing is marked async, so nothing can fire);
**each mechanical rule is demonstrated by deliberately mis-marking a pass in a throwaway edit** and
showing the Debug message name the pass — then reverting. A rule that has never fired is not a rule.

**DONE 2026-08-28, uncommitted.**

**The legality predicate was TRANSCRIBED, not derived.** `barriers::IsDirectQueueExclusiveState` is
UE's `IsDirectQueueExclusiveD3D12State` (`Source/Runtime/D3D12RHI/Private/D3D12Util.h:271`), and
reading it first changed the answer: from first principles the natural shape is an ALLOW-list of the
five states a compute queue accepts, and UE ships a **DENY-list of four bits** tested with ANY —
`RENDER_TARGET | DEPTH_WRITE | DEPTH_READ | PIXEL_SHADER_RESOURCE`. The deny-list is the correct
shape here, because what makes this engine's `kSrvAll` (`PIXEL|NON_PIXEL`, 32 sites) illegal is the
PIXEL bit being PRESENT, not an allowed bit being absent — an allow-list containing NON_PIXEL would
have waved it straight through. **One definition**, shared by the registration rule and the emission
backstop, so the two can never disagree about what "legal" means.

**Rules, and the evidence each one actually fires.** All three reachable ones were demonstrated by
mis-marking a real pass in a throwaway edit, then reverted:

| rule | demonstration | message |
|---|---|---|
| queue-illegal state at registration | `Main_Hzb` marked AsyncCompute | `pass 'Hzb' ... registers res=Deferred[0].Depth in state 0xC0, which only the DIRECT queue can carry` |
| AsyncCompute inside a CL group | `Main_ObjectCompute` (a group member) marked AsyncCompute | `pass 'ObjectCompute' is marked AsyncCompute inside a CL group` |
| async pass touching the swapchain | `Main_Hzb` async + a temporary `Use(backbuffer, UAV)` | `pass 'Hzb' ... registers the SWAPCHAIN backbuffer` |

Each names the pass, and where a resource is involved, the resource and the state.

**The fourth rule could NOT be demonstrated, and the reason is step ordering, not the rule.** The
emission-side backstop in `Renderer::Transition` reads the queue off the recording list
(`cl->GetType()`, the same trick as step 3) and fails if a compiled barrier carrying a
direct-exclusive state reaches a COMPUTE list. To reach it, the registration rule has to be disabled
AND a pass has to actually record on a compute list — and at step 5 a compute list cannot be
SUBMITTED at all: `EndThreadCommandList` registers it into the graphics timeline, and
`ExecuteCommandLists` on the direct queue rejects a list of the wrong type. The run died before its
first frame, exactly as it should. **Carry this into Step 6's acceptance**: once per-queue
submission exists, re-run this demonstration.

**A diagnostic gap fixed on the way.** `RendererInvariantFailure` wrote its message only to
`OutputDebugStringA` and then aborted — so on any headless run (the stress harness, a `--shot`
capture) the one message explaining why the process died went nowhere. It now also appends to
`logs/invariant_failure.log`. Without that, this step's acceptance was literally unobservable.

**Results.** Both builds `0/0`; all three gates `verdict: CLEAN`, 0 MISSING, and no
`invariant_failure.log` produced — which is the inertness proof: nothing is marked async, so no rule
can fire in normal operation.

**R14's undeclared reads remain a REVIEW item**, as the step says — nothing observes a read that
never reaches the graph. Procedure for each candidate pass, to be run at Step 8: list the resources
its record body binds, diff against what its builder declared.

### Step 6 — per-queue submission and fence edges (inert)

`SubmitTimeline` groups batches per queue and returns one list array per queue; `Renderer` submits
each to its own queue. Graph dependencies that cross queues compile into signal/wait pairs (D2).

**Acceptance:** re-run Step 5's fourth-rule demonstration, which was unreachable until this step
(mark a pass AsyncCompute with the registration rule disabled, and confirm the emission-side
backstop in `Renderer::Transition` fires instead of a debug-layer error); the graphics queue's
submitted array is **byte-identical** to today — dump
`fixedSubmitScratch_` (the WRAPPED array: profiler-begin list, work lists, epilogue — R3) in order
under a temporary flag, before and after, and diff them. The compute array is empty and no fence
edge exists yet because no pass is async. All gates CLEAN. If the array differs, the step is not
done, however plausible the difference looks.

**DONE 2026-08-29, uncommitted.**

- **`SubmitTimeline` returns SEGMENTS, not two flat arrays.** A segment is a contiguous run of
  batches on one queue plus the cross-queue synchronisation that brackets it. Two flat arrays cannot
  express a fence edge, because an edge lands *between* submissions — the producer must be told to
  signal after the work its consumer waits on, and a flat array has no "here". With every pass on
  Graphics there is exactly ONE segment, and it is today's array.
- **`PassBatch` carries its queue and its cross-queue wait.** The wait is derived by the graph from
  the pass's **prereqs/mtDeps**, not from batch order — "later in the list" is not a dependency, and
  treating it as one would serialise the queues and delete the overlap the whole plan exists to
  create. Latest producer wins, since waiting for it subsumes the earlier ones.
- **Submission walks segments in batch order**, emitting `SignalCrossQueue`/`WaitCrossQueue` (step
  2's dormant helpers) only where the graph recorded an edge. A segment without an edge is submitted
  with no synchronisation at all — that is the point.
- **The wrapping lists moved into the segments**: the profiler-begin list is inserted at the front
  of the FIRST graphics segment and the epilogue appended to the LAST, so `GPU.Frame` still brackets
  the same span of the direct queue's timeline.
- `fixedSubmitScratch_` is gone — the work lists now live in the segments, already in order.

**The inertness proof.** `--dump-submit-order` writes the submitted arrays by DEBUG NAME (pointers
differ between runs and prove nothing; the names are the pass identities, which is what "unchanged"
is a statement about). Captured before the change and after it:

```
before: 27 lines   after: 27 lines
diff (pointers normalised): NO DIFFERENCES
== queue=graphics count=26 ==
```

26 lists, same names, same order, ONE segment, and **no compute segment at all** — exactly what the
acceptance asks for. The `--renderer-submission-stress` harness (which had to be adapted: it now
flattens the segments locally, because a production helper that merged two queues' arrays would be a
trap) passes with **0 failures**, death tests included. All three gates CLEAN, 0 MISSING.

**A REAL BUG THIS STEP FOUND IN STEP 5'S WORK.** Compiled barriers reach the GPU from **two** places
— `Renderer::Transition` and `Renderer::EmitPoint` — and step 5 guarded only the first. `EmitPoint`
is the path every `AddPass2` body actually takes, so the emission-side queue-legality backstop was
unreachable in practice. It is now ONE function called from both sites. This is the engine's
standing rule paid for again: when the same thing is assembled in more than one place, a change to
it must go through one helper.

**The backstop still has not been observed FIRING, and here is exactly why.** Two attempts:
1. mis-mark `Main_Hzb` async with the registration rule disabled -> the run dies at
   `EndThreadCommandList: Close() failed`, i.e. the pass records something else that is illegal on a
   compute list, and D3D12 reports it at Close;
2. the barrier the demonstration was aiming at is not there anyway — earlier passes already read
   `Deferred[].Depth` as `kSrvAll`, so by Hzb's turn the compile emits nothing for it.

So constructing a reachable case needs a pass that is otherwise compute-legal AND whose compiled
point genuinely contains a direct-exclusive transition. **Do not fake one.** Step 8 moves
`Main_RTTrace`, which declares `kSrvAll` on depth and gb1 and therefore hits this naturally: if the
D7 rework is forgotten the registration rule fires, and if a stale point survives the rework the
backstop does. Verify it there. Recorded as an undemonstrated guard rather than a passed one — the
second attempt is also mildly reassuring in its own right: a mis-marked pass fails loudly on its own
recording, so the backstop is not the only thing between a mistake and silence.

### Step 7 — per-queue barrier state and ownership transfer (inert)

The structural step. `CompileBarriers` (`RenderGraph.h:713`) carries state per **(resource, queue)**
instead of one running state, and where a resource crosses queues it emits a RELEASE at the
producer's point and an ACQUIRE at the consumer's point (D3), using enhanced queue-scoped layouts
and honouring D7 (hand over in a state legal on both sides).

The compile's **cache** (R9) changes with it, and getting this wrong is silent corruption:
- the per-pass queue assignment joins the cache key alongside pass index and name
  (`CompileInputsUnchanged`, `RenderGraph.h:883`);
- the fixed-point test (`RenderGraph.h:825`) becomes per-queue — a resource must return to its
  incoming state *on the queue that will next read it*;
- `GetPredictedState`/`SetPredictedState` become per-queue, or gain an explicit "which queue last
  owned this" field. Whichever shape, state it in a comment: this is the one piece of cross-frame
  memory the barrier system keeps, and it is now two-dimensional.

**Acceptance:** with every pass still on Graphics, the compiled barrier arrays are **byte-identical
to today** — dump and diff them under a flag, **with `--barrier-cache-verify`** so the diff is
against a fresh compile and not against the cache (R9). `--barrier-cmp` stays at 0 MISSING and 0
extra. All gates CLEAN, zero debug-layer messages. **Do not move a pass in this step.** The entire
value here is a generalisation proven inert.

**DONE 2026-08-29, uncommitted.**

- **The compile's running value is now `(state, owning queue)`.** One value per resource still — a
  resource HAS one state — but the owner is what says whether the next consumer's queue may legally
  take it, so the two travel together everywhere.
- **`Entry::predictedOwner`** joins `predicted` in the canonical registry. This is the one piece of
  cross-frame memory the barrier system keeps, and it is now two-dimensional. `SetPredicted` bumps
  the generation when EITHER moves: same state left by the other queue is a different starting
  point, and a cache reused across an ownership change is exactly the silent corruption the
  generation exists to prevent.
- **The cache key carries the queue** (`CompiledPass::queue`), and the **fixed-point test compares
  owner as well as state**.
- **Ownership transfer (D3/D7)**: crossing queues is legal only if the state the producer left is
  legal on the consumer's queue too. When it is not, the compile **fails fast naming the resource,
  the state and the consuming pass**, and says the fix belongs in the PRODUCING pass — one more
  `ctx.Use` handing the resource over in a both-legal state, which the compile then emits on the
  producer's own list like any other barrier. It deliberately does not invent that barrier itself:
  it would have to be inserted into a pass the walk has already passed, i.e. a whole second compile
  pass, to fix something a one-line declaration fixes at the source.

**Results — inert, measured against a CONTROL rather than against remembered numbers.** The barrier
emission counters are a direct function of the compiled barriers, so the S6 commit was rebuilt and
run to produce them:

| gate | S6 control | S7 |
|---|---|---|
| VSM, GBV | 7892 | **7892** |
| Legacy, GBV | 6772 | **6772** |
| no-GBV | 5228 | **5228** |

Identical, to the barrier, across **52 iterations of churn**. `--barrier-cache-verify` over the full
GBV churn reports **0 STALE** — the cache with the queue in its key and the per-queue fixed-point
test serves exactly what a fresh compile produces. `--barrier-cmp` 0 MISSING, all gates CLEAN, no
invariant failures, both builds 0/0. `--dump-barriers` writes the compiled arrays (139 barriers by
resource name) for inspection.

**Taking the control was not ceremony.** The first comparison was against 7846/6742/5182 — numbers
recorded back at Step 2 — and showed +46/+30/+46, which looks exactly like a regression. The S6
control produced 7892/6772/5228, i.e. the drift happened somewhere in Steps 3-6 and the RT commits
(the AS counter moved 147 -> 208 too), not here. **A baseline from four steps ago is not a control.**

### Step 8 — the first real user: `Main_RTTrace` on the async queue

Move `Main_RTTrace` to `AsyncCompute`, and add `--no-async-compute` (D4).

**Why this pass and not `Main_ObjectCompute`** (the choice was made deliberately; if you reverse it,
write down why):
- It is **already a standalone pass** and not a CL-group member, so it needs no graph surgery first.
  `Main_ObjectCompute` needs its group dissolved and itself split by consumer before it can move at
  all — a whole step of precondition (now Step 9).
- It was **purpose-built for this move** by `9d35d8c`, whose enum comment says so outright: "RTTrace
  needs only TLAS/depth/gb1 and is the pass that later moves to the compute queue."
- At 118 us median / 131 mean it is the largest mover with a single, declared consumer.
- It **exists in every gate**: `reflectionSource` defaults to `ReflectionSource::RT`
  (`SceneFrameData.h:506`) and none of the three levels the stress harness cycles
  (`demo.json`, `demo1.json`, `new1.json` — `SceneStress.cpp:1029`) overrides it. So the async path
  is genuinely exercised by the correctness gates rather than merely compiled.
- Both candidates need D7 either way, so that is not a tiebreaker.

**Two caveats to state in the commit, not to discover later.**
1. On **non-RT hardware** `decisions_.rtReflect` is false (`SceneRenderer.cpp:218`) and this pass does
   not exist, so the async queue is never exercised there. That is acceptable for a first mover — it
   is not acceptable as the permanent only user, which Step 10 fixes.
2. RT **disables itself stickily** after an AS/bindless allocation failure (`SceneRenderer.cpp:210`),
   so the pass can vanish mid-session. That is a graph-shape change the design already handles, and
   it doubles as a free test of Step 2's "a frame slot waits for both queues even when the compute
   queue has nothing".

Before moving it, verify independence from the passes it will overlap: its builder's declarations
are the authoritative list — depth and gb1 read, `rtPayload` / `rtPayloadUv` written
(`SceneRenderer_Graph.cpp:556-560`) — and its only consumer is `Main_RTResolve`
(`:835-840`). Intersect that against what the concurrent graphics passes touch. A non-empty
intersection is not a blocker, it is a required fence edge (D2), but it must be **declared**.

**D7 applies here**: `depth` and `gb1` are declared with `kSrvAll`, which carries the compute-illegal
`PIXEL_SHADER_RESOURCE` bit (R10). The async form declares NON_PIXEL only; the graphics readers keep
their own PIXEL half. Step 5's rule names the exact lines.

**The incoming edge is the interesting one.** `Main_RTTrace` depends on `Main_BuildAS`, which stays on
the graphics queue in this step and **declares no resource states at all** (R14). The fence must
therefore be derived from the explicit **mtDep** `{ gb.pBuildAS }` (`:556`), not from resource
declarations — this step is where D2's ability to compile an edge from a graph dependency alone gets
proven. If it cannot, the TLAS is unordered against its only reader and nothing will say so.

**Acceptance:**
- All three gates CLEAN, zero debug-layer messages, 0 MISSING.
- **The two-track trace shows `Pass_RTTrace` genuinely overlapping `Pass_VsmPageRender`**, not merely
  relocated. In the reference timeline RTTrace sits at ~753 and VsmPageRender spans 878..1366, so the
  overlap is what the move is FOR. This is the step's real acceptance criterion: the architecture
  works.
- Visual parity: `--shot` at a frozen wind clock against a same-config control pair — the
  cross-config difference must sit inside the run-to-run noise band (this scene's floor is ~0.2 % of
  pixels differing by >8; measure it, do not assume it).
- **No perf REGRESSION, measured on wall-clock, not GPU.Frame** (see Reference points). Interleave
  A/B/A/B and then repeat in reversed order — successive runs drift downward from thermal downclock,
  which alone will manufacture a result for whichever arm ran first. Report mean and standard
  deviation. A win is welcome; the bar is "not slower outside the noise", because both queues
  contend for the same shader cores and an overlapped pass can cost more than it saves.
- `--no-async-compute` reproduces Step 7's graphics submission byte for byte.

**DONE 2026-08-29, uncommitted. `Main_RTTrace` runs on the async compute queue.**

**The acceptance criterion, met exactly:** the two-track trace shows `Pass_RTTrace` **100 % overlapped**
(mean and median, 123 frames), and its partner is **`Pass_VsmPageRender`** — the pass this plan named
in advance. 25 393 us of overlap with it across the capture.

**Two real fixes, both found by measurement rather than review:**

1. **The D7 hand-over, discovered by the Step 7 guard.** The first run failed with
   `pass 'RTTrace' (ASYNC COMPUTE) takes res=Deferred[0].GB1 ... left it in 0x4` — gb1 was still a
   RENDER_TARGET. A consumer normally acquires for itself, and that is exactly what cannot work
   here: the acquire's BEFORE state would be direct-queue-only, on a compute list. So the graphics
   side hands over, in a pass RTTrace explicitly depends on. `Main_Hzb` now declares gb1 NON_PIXEL
   (its only declaration for a resource it does not use — a hand-over, exactly as the guard's
   message prescribes), `Main_VsmPageRequest` and `Main_Hzb` declare depth NON_PIXEL, and RTTrace
   gained a **prereq on `pHzb`**. Before the move RTTrace acquired both itself and worked only
   because it happened to be scheduled after the G-buffer's other readers — incidental ordering, not
   a dependency. The async move turned that latent fragility into a named error.
2. **The fence was correct but COARSE, and the trace said so.** The first submit loop signalled the
   producer queue at the moment the *consumer* segment was submitted, which means "everything
   submitted to that queue so far" — far more than the graph asked for. Result: 52 % overlap, with
   `Pass_PointLights`/`Pass_SpotLights` as partners instead of `Pass_VsmPageRender`. The fix: cut a
   segment boundary after any batch that someone waits FOR, and signal there. 52 % -> **100 %**, and
   the partner became the right one. **A fence edge is not "signal before the wait" — it is "signal
   immediately after the producer".**

**`barriers::` finally learned the queue (R10 predicted this).** `NON_PIXEL_SHADER_RESOURCE`
translates to `SYNC_VERTEX_SHADING | SYNC_COMPUTE_SHADING`, and the vertex stage does not exist on a
compute queue — D3D12 reports that not at the barrier but as a bare `Close() failed` naming nothing.
`EmitEnhanced` now narrows the sync mask when the target list is COMPUTE (read off `cl7->GetType()`,
no plumbing) and REFUSES — falling back to the legacy barrier — if narrowing would empty a non-empty
scope, because a barrier that waits for nothing is a race, not a saving.

**Results.** All three gates `verdict: CLEAN`, 0 MISSING, no invariant failures, `legacy=0` (the
enhanced path handled the compute-queue barriers; nothing fell back). The `--no-async-compute` gate
is CLEAN too. Barrier count 7892, unchanged from the Step 6/7 control.

**Perf: no regression, and no win either — the predicted outcome, not a disappointment.** A/B
**inside one binary** via `--no-async-compute`, interleaved A/B/A/B:

| | async OFF | async ON |
|---|---|---|
| wall-clock (CPU frame) | 3256 us | 3281 us (**+0.8 %**) |
| run-to-run spread | 0.9 % | 0.1 % |
| `Pass_RTTrace` | 128 us | **212 us (+84)** |
| `Pass_VsmPageRender` | 550 us | **588 us (+38)** |

+0.8 % against a 0.9 % spread is inside the noise, which is the bar. Why there is no win is visible
in the table: 128 us left the serial chain, and contention put +84 back into RTTrace and +38 into the
pass it overlaps. **Both queues share the same shader cores** — the risk this plan registered, now
measured rather than assumed.

**Visual parity holds, and the FLOOR had to be measured twice to say so.** One same-config control
pair gave 0.881 % of pixels differing by >8 against 2.060 % across configs, which reads as a
regression. A second floor sample (OFF vs OFF) gave **1.853 %**, and the three cross-config pairs
span 0.812-2.060 %. The cross differences sit inside the same-config range: this scene's RT dither
and DLSS history make run-to-run variation large, and **one control pair is not a floor**.

**One honest deviation from the acceptance text.** `--no-async-compute` restores a 26-list graphics
submission with no compute segment, but the pass ORDER differs from Step 6's dump: RTTrace sits two
places later. That is the new `pHzb` prereq — a deliberate correctness fix — not the flag failing.
Byte-identity to Step 6 is not claimable and is not claimed.

**Still not observed firing: the emission-side backstop** (Step 5's fourth rule). It is now on the
live path — RTTrace really does emit compiled barriers on a compute list every frame — and it stayed
silent, which is the correct outcome for a correct configuration.

**A BUILD TRAP worth the rule.** Adding a member to `SubmitTimeline` and to `Renderer` (both widely
included headers, so both a class-LAYOUT change) and then building INCREMENTALLY produced a Release
binary that failed `RegisterDirect: batch index outside the active range` on every run, while Debug
was clean. A full `/t:Rebuild` made it disappear (10 consecutive clean runs). The bad binary is gone
so this cannot be proven retroactively, but the shape is a stale-object layout mismatch, not a race.
**After adding a member to a widely-included header, do a full Rebuild before trusting a Release
run** — and note that the first three failures looked perfectly deterministic.

### Step 9 — split `Main_ObjectCompute` by consumer, still all on Graphics (inert)

Graph surgery only, no queue change — so it is separately bisectable from the async flip. It is the
precondition for moving the ocean/particle compute in Step 10, and it is no longer on the critical
path to proving the architecture, because Step 8 already did that.

- Dissolve the `BeginCLGroup` around `Main_PrologueClear` + `Main_ObjectCompute` + `Main_SurfSim` +
  `Main_ShoreWetness` (`SceneRenderer_Graph.cpp:81-132`), because a grouped pass cannot change queue
  (R8). Four members means **three** extra command lists per frame, not one — expected, report it,
  do not chase it. If the CPU cost is unacceptable, the alternative is to keep a group *per queue*
  rather than dissolving; decide with the measurement, not in advance.
- Split the object-compute builder (`SceneRenderer_Graph.cpp:95-117`) by consumer (R11) into
  `Main_GpuInstanceCompute` (the GI rotation, stays on Graphics forever — `Main_ShadowCull` consumes
  it) and `Main_ObjectCompute` (ocean + particles, the Step 10 async pass). R15 makes this cheaper
  than it used to be: the builder already partitions the scene by `PrepareCompute()` returning true,
  so each new pass gets its own filtered `ObjectComputeList` and there is no Prepare/Record pair to
  keep in sync by hand. Add the enum entry and its `RenderPassToWString` case — note
  `kMainRenderGraphPassCount` is `RenderPass::Main_Count` (`SceneRenderer.h:37`), so graph capacity
  follows the enum automatically.
- **Add the GPU scope `Main_ShoreWetness` is missing** while you are in there (see Step 10's wetness
  note) — without it neither this step nor Step 10 can show what that pass costs or whether it
  overlapped.
- **Measure the split**: the trace must now show two scopes summing to roughly the old 135 us. That
  number is the real ceiling for the ocean/particle move, and it is not 135.

Do not "improve" anything else while in there.

**Acceptance:** all gates CLEAN, 0 MISSING; visual parity via `--shot` at a frozen wind clock; the
two new scopes appear in the trace with the expected split; the frame's CPU cost grows by at most
the dissolved group's worth of per-CL overhead (report the number, do not hide it); the async
submission from Step 8 is unaffected.

**DONE 2026-08-31, uncommitted.**

- **`ComputeFeedsShadowCull()`**, a new virtual on `RenderableObjectBase`, is the split predicate —
  deliberately NOT a reuse of `IsGpuInstancedCaster()`. That one means "casts shadows through GPU
  instancing", a statement about DRAWING which happens to be true of the same class today; tying the
  compute split to it would mis-split silently the moment the two stop coinciding.
- **One builder, two passes.** The lambda that walks the scene is shared and parameterised by the
  predicate, so there is still ONE walk and ONE overflow rule — only the filter differs.
- **The CL group became two groups plus a loose pass**: `{PrologueClear, GpuInstanceCompute}`,
  then `ObjectCompute` alone (a grouped pass cannot change queue), then `{SurfSim, ShoreWetness}`.
  SurfSim is its group's FIRST member, which is what lets it keep an outside prereq. That is **+1**
  command list, not the +3 a full dissolution would have cost.
- **`Main_ShoreWetness` got the GPU scope it never had.**

**The split, measured:** `Pass_GpuInstanceCompute` **11 us**, `Pass_ObjectCompute` (ocean +
particles) **145 us**, sum 155 against 147-164 before — no work lost, none duplicated. The GI half
being 11 us confirms what the plan assumed but had never measured: essentially all of the movable
value is in the ocean/particle half.

**Costs nothing.** CPU frame 3273 us vs Step 8's 3281; GPU.Frame 3268 vs 3281. Gates CLEAN, 0
MISSING. Visual parity: cross-step 1.52 % / 1.53 % against a same-config floor of 1.19 % today and
0.88-1.85 % measured at Step 8 — inside it.

**A bug this step made and then caught, worth keeping:** the first cut passed the new scope name to
the CPU scope only, while `Pass_ObjectCompute`'s body hard-coded the GPU scope. Result:
`Pass_GpuInstanceCompute` was **absent from the GPU track** and `Pass_ObjectCompute` appeared TWICE
per frame. Two passes sharing one body must be handed their identity, not assume it — the trace said
so immediately (n=488 where 244 was expected).

### Step 10 — the remaining movers, one at a time

Move the remaining eligible compute passes one at a time, each with **Step 8's acceptance** in full.
`Main_RTTrace` already moved there; the other five are below, and the ocean/particle half of
`Main_ObjectCompute` comes first because Step 9 has just prepared it.

**The criterion is INDEPENDENT CONCURRENT GRAPHICS WORK, not distance to the consumer.** An earlier
version of this step ranked candidates by how many passes separated them from their first consumer
and got the answer backwards in both directions: it declined `Pass_ShadowCull`, `Pass_Gtao` and
`Pass_Hzb` because each feeds the very next pass, and nominated the then-monolithic
`Main_RTReflections` because its consumer is far away. Both readings are wrong. What matters is
whether there is graphics work the candidate does not depend on, which can therefore run alongside
it — a pass whose consumer is next
in line still moves if the raster work beside it is independent, and a pass whose consumer is distant
does not move if it is itself transitively stuck behind that raster work.

The answer comes from the graph's own prereq lists, which is the authoritative statement of what
depends on what. Verified 2026-08-27 in `SceneRenderer_Graph.cpp`:

**Movable — six passes, ~477 us median / ~583 us mean, i.e. 14.8 % / 18 % of the frame
(2026-08-28 b, HEAD `9d35d8c`):**

| candidate | med / mean | hides behind | the proof |
|---|---|---|---|
| `Main_RTTrace` | 118 / 131 us | `Pass_VsmPageRender` | **MOVED IN STEP 8.** prereqs `{ pGbuf, pBuildAS }` and it declares **no `D.light`** — depth, gb1 and the two payload UAVs only (`SceneRenderer_Graph.cpp:556-560`). Consumer is `Main_RTResolve` at ~1564 |
| `Main_ObjectCompute` (ocean + particles) + `Main_ShoreWetness` | <=135 / 145 us | `Pass_VsmPageRender` | consumer is `Main_Transparent` at ~1723; the pass sits at ~111. Needs Step 9's split first; take the wetness pass with it (note below) |
| `Main_BuildAS` | 86 / **206** us | everything after it | first pass in the frame; consumer is `Main_RTTrace` at ~753. Bimodal (p95 625) — quote the mean. Declares nothing, see the note below |
| `Pass_Gtao` | 60 / 65 us | `Pass_VsmPageRender` | prereqs `{ pGbuf, pHzb }` while `Main_Lighting` is `{ pGbuf, pVsmPageRender, pGtao }` — the graph proves GTAO does not wait on shadows (`:440`, `:584`). Declares NON_PIXEL only, so it needs no D7 work |
| `Pass_ShadowCull` | 50 / 50 us | `ExecuteBundles` + the opaque `RenderObjectBatch` (137 us of raster at 342-458) | prereqs `{ pShoreDepth }` — it does **not** depend on the G-buffer (`:217`) |
| `Pass_Hzb` | 28 / 36 us | `Pass_VsmPageRender` | prereqs `{ pGbuf }` and nothing else (`:409`) |

**Ordering on the compute queue.** `Main_BuildAS -> Main_RTTrace` is a chain and serialises with
itself (204 us median, 337 mean) — so once BuildAS also moves, it lands in front of the pass Step 8
already put there; the other four are mutually independent. All of it must complete by
`Main_RTResolve` at ~1564, and the graphics work available in that window totals ~840 us against
~477 us of compute — it fits, with the BuildAS/RTTrace chain as the critical path to watch.

**Two of the six need D7 before they can move**: `Main_RTTrace` (depth, gb1) and `Pass_Hzb` (depth)
declare `kSrvAll`, which carries the compute-illegal `PIXEL_SHADER_RESOURCE` bit (see R10). Their
async form declares NON_PIXEL only and the graphics readers keep the PIXEL half. `Pass_Gtao`,
`Pass_ShadowCull` and the wetness pass are already clean.

**`Main_BuildAS` declares no resource states at all** — the AS build bypasses the barrier compile
entirely and the buffers stay in `RAYTRACING_ACCELERATION_STRUCTURE` (`SceneRenderer_Graph.cpp:58-77`).
That is R14's shape: no declaration, no derived edge. It is saved by having `pBuildAS` as an explicit
**mtDep** of `Main_RTTrace` (`:556`), which is a graph edge D2 can compile a fence from without any
resource declaration. Confirm that before moving it — if the ordering were only positional, the TLAS
would silently unorder against its only reader.

**Not movable, with the reason:**

| candidate | cost | why not |
|---|---|---|
| `Pass_BloomConv` | **595 us** (largest single item in the frame) | inside `Pass_Tonemap`; the tone curve consumes it immediately, and the whole tail chain (bloom -> curve -> DLSS -> metering) is serial by data |
| `Main_RTResolve` | 13 us | the wedged remnant of the RT split — declares `D.light` plus the trace payloads; see below. Too small to matter |
| `Pass_VsmPageRequest` | 85 us | reads the completed depth buffer and feeds `Pass_VsmPageRender` directly; serial by construction |
| `Pass_ExposureMetering` | 36 us | reads the finished scene colour and is already last |
| `Pass_Lighting` | 43 us | prereqs name `pVsmPageRender` explicitly |
| `VsmPageRender.Setup` | 14 us | collapsed from 107 us; below the level worth a step |
| `Main_SurfSim` / `Main_ShoreWetness` | **<=6 us combined, and UNMEASURED** | not a cost case — see "The wetness pass" below; they ride along with the ocean/particle move rather than getting a step |

**The RT reflection was WEDGED, and `9d35d8c` unwedged it. This is the single biggest change to
this plan's conclusions.** The previous version of this section argued that RT could not move: the
monolithic `Main_RTReflections` declared `D.light` (the lit HDR target) as its on-screen-hit fast
path, so everything before it wrote what it read and everything after read what it wrote — zero
adjacent independent graphics work. The stated remedy was a gather-then-shade split, filed as
RT-plan work and an explicit non-goal here.

**That split has landed.** `Main_RTReflections` is gone; `Main_RTTrace` + `Main_RTResolve` replace
it, and the commit says so in the enum comment: "RTTrace needs only TLAS/depth/gb1 and is the pass
that later moves to the compute queue; RTResolve is the only RT consumer of the lighting output."
What it bought, measured:

| | before (08-28 a) | after (08-28 b) |
|---|---|---|
| RT total | 87 us, all wedged | 131 us |
| movable half | 0 | **`Main_RTTrace` 118 us** |
| wedged remnant | 87 us | `Main_RTResolve` **13 us** |

So ~90 % of the RT cost became movable, and it is now the second-largest mover in the frame. The
remaining `Main_RTResolve` (13 us) stays on graphics for exactly the old reason — it declares
`D.light`, `rtPayload`, `rtPayloadUv` and prereqs `{ pSky, pWetness, pRtTrace }`
(`SceneRenderer_Graph.cpp:835-840`) — and at 13 us there is nothing to argue about.

**The general lesson, worth keeping:** the async budget is not fixed by the frame, it is fixed by how
the passes are CUT. A pass that reads one thing it does not really need for most of its work is a
pass that cannot move; splitting it along that seam is worth more than any scheduling cleverness.
`Main_ObjectCompute` (Step 9) is the same shape and has not been split yet.

**The wetness pass is the cleanest async candidate in the frame, and it should ride along with
Step 10's ocean/particle move for reasons that are not about its cost.**

- **Its states are already compute-queue legal.** `OceanWetness::BuildUpdatePass` registers only
  `NON_PIXEL_SHADER_RESOURCE` and `UNORDERED_ACCESS` (`OceanWetness.cpp:219-231`) — no
  `PIXEL_SHADER_RESOURCE` anywhere. R10 does not bite it at all, so unlike the ocean sim and the surf
  sim it needs no D7 hand-over rework. It is the one pass that is async-legal exactly as written.
- **Its slack matches ObjectCompute's.** Its first consumer is `Main_RTResolve`, which names
  `pWetness` in its prereqs (`SceneRenderer_Graph.cpp:836`) and sits at ~1564 in the traced timeline;
  then `Main_Compose` reads the current history as an SRV (`OceanWetness.cpp:56-58`) and
  `Main_Transparent` stamps into the current stamp as a UAV (`OceanRenderable.cpp:783-786`). The pass
  itself sits at ~173. That is a ~2.2 ms window.
- **Its cost is a BOUND, not a measurement, because it has no GPU scope.** `ProfilerScopes` has
  `Ocean.SurfSim` but nothing for the wetness update, so the pass is invisible in the trace. All that
  can be said is that both CL-group siblings execute between `Pass_ObjectCompute`'s scope closing at
  173 and `Pass_ShadowCull`'s opening at 179, i.e. **<=6 us combined** — and in that capture the surf
  sim did not run at all (its scope is absent), so most of those 6 us are wetness plus list-change
  overhead. **Absence from the trace means unmeasured, not zero** — this engine has already paid for
  that confusion once with an 82 us hole that turned out to be bundle work. Step 9 adds the
  missing GPU scope.
- **It is a sharper exercise of D3 than the displacement.** The ocean displacement is a plain
  producer -> consumer read. The wetness stamp is not: the async pass writes `stamp_[write]` as a
  UAV, and then the GRAPHICS queue writes the same resource as a UAV in `Main_Transparent`. A
  compute -> graphics handoff that is UAV on both sides is a sharper test of the release/acquire
  pair than a read-only consumer.
- **The cost it does add: `Main_RTResolve` gains a cross-queue wait**, because `pWetness` is one of
  its prereqs and the resolve stays on graphics. The edge is declared and cheap, but
  Step 10 must NAME it rather than discover it.

Two frames this bound does not cover, and neither may have appeared in the 123 captured: a
`relocate` frame (the wetness window shifts) and a `clearHistory` frame, both of which do more work
in the same dispatch.

**Procedure per candidate**, before any edit: read its builder's prereqs and declarations, list the
graphics passes that do NOT transitively depend on it, and confirm from the timeline that they
overlap its position. Then move it, and require the two-track trace to show it running under the
partner named in the table. If the trace does not show that overlap, revert the move — a relocated
pass that does not overlap is a fence for nothing.

**Expected outcome: five more passes move, for ~359 us median / ~452 mean on top of Step 8's 118 us.**
That is the analysis's prediction, not a promise — contention is measured, not assumed, and a
candidate that regresses is reverted and written up. Declining a candidate remains a legitimate,
recordable result; what is not legitimate is moving one because the list looks unfinished.

**One of them fixes a hole Step 8 leaves open:** with only `Main_RTTrace` async, a machine without RT
hardware never exercises the async queue at all. `Main_ObjectCompute` and `Pass_ShadowCull` exist in
every configuration, so moving either closes that gap — do one of them early in this step rather than
last.

---

**DONE 2026-08-31, uncommitted. `Main_BuildAS` moved, and it is the one that pays.**

**The headline: -3.0 % wall-clock**, against a 0.6 % run-to-run spread — five times the noise. A/B
inside one binary via `--no-async-compute`, interleaved.

**WHY, and why Step 8's conclusion was too pessimistic.** Step 8 measured RTTrace alone at +0.8 %
(a wash) and I generalised from it that contention would eat every mover. That was ONE data point,
and it happened to be the worst case. The discriminating experiment says the cost depends entirely
on the mover's WORKLOAD CHARACTER:

| mover | on graphics | on compute | slowdown |
|---|---|---|---|
| `Pass_BuildAS` (AS build, fixed-function-ish) | 204 us | 238 us | **+17 %** |
| `Pass_RTTrace` (BVH traversal, bandwidth/latency-bound) | 137 us | 257 us | **+88 %** |

An acceleration-structure build contends **five times less** than incoherent BVH traversal against
the same depth-only raster, so its 204 us hide almost for free. Net across the frame: `GPU.Frame`
3247 -> 3148 us. **Run the discriminating experiment before generalising from one candidate.**

**`Main_BuildAS` is also the cleanest possible mover, by construction:** it declares NOTHING (the AS
buffers bypass the barrier compile), so it has no D7 hand-over problem at all — the exact blocker
that stopped the ocean sim below. Its cross-queue edge comes from `Main_RTTrace`'s explicit **mtDep**
on it, not from any resource declaration, which is R14's shape with the dependency written down. It
moved on the first try with no invariant failures.

**`Main_ObjectCompute` (ocean + particles, 145 us): TRIED, BLOCKED, reverted to Graphics.** The
compile refused it by name — `takes res=Ocean.PrevDisplacement from the graphics queue, which left
it in 0xC0`. The blocker is structural, not a missing declaration: the ocean's displacement and foam
maps cross the queue boundary **in both directions every frame** — a PIXEL shader samples them in
`Main_Transparent` at the end of the frame, and this compute pass takes them at the start of the
next. Moving it needs D7's release half from the transparent pass, and that pass fans out over
several command lists, so the release cannot ride an existing barrier point the way `Main_Hzb`'s did
for RTTrace. Lowering the sim's own declarations to NON_PIXEL was necessary but not sufficient, and
was reverted with them. **Reopen only with a plan for the hand-back**, not by weakening the guard.

**Declined without moving, with the reason:** `Pass_Gtao` (60 us), `Pass_ShadowCull` (50 us) and
`Pass_Hzb` (28 us) are all depth-sampling or cull compute — RTTrace's character, not BuildAS's. The
measured model puts their expected value near zero against a real risk of another D7 rework.
Recorded as declined rather than attempted, which is what this step allows; revisit if the model is
ever contradicted.

**Results.** All three gates `verdict: CLEAN`, 0 MISSING, no invariant failures, barrier count 7912
unchanged from step 9. Visual parity: cross-config 1.961 % / 1.569 % against a same-config floor of
1.966 % measured in the same session — at or below the floor.

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
produces a byte-identical graphics submission to the pre-Step-8 build.

**DONE 2026-08-31, uncommitted.**

**Churn with both queues live:** `--scene-stress-gbv=30` CLEAN in **VSM** and in **Legacy** shadow
mode, plus 30 iterations under `--no-async-compute`. 0 MISSING, no invariant failures. That harness
is nothing but level reload, level switch, window resize, DLSS-mode and render-scale changes — i.e.
exactly the paths that idle the GPU and rebuild resources, now doing it with two queues.

**R12 (out-of-band submissions) is CLOSED, and by construction rather than by hope.** Every
`UploadBatch` call site in the engine — level load, scene mega-buffer rebuild, and eleven editor
commands — uses `SubmitAndWait`, and the editor's thumbnail cache pairs its direct-queue signal with
`WaitForPreviousFrame`. Both funnel into `FrameScheduler::WaitForGpuIdle`, which **Step 2 made idle
BOTH queues**. So an upload can never be in flight against async work: the wait between them is a
full two-queue idle. Verified by grep over every call site, not by sampling one.

**The async pass VANISHING mid-session is covered, which also closes Step 8's non-RT-hardware hole.**
`--scene-stress-gbv=20 --rt-force-as-fail` forces every acceleration-structure allocation to fail,
which stickily disables RT — and with it BOTH async passes (`Main_BuildAS` and `Main_RTTrace`)
disappear from the graph entirely, mid-run, while the two-queue machinery stays live (both fences
still signalled per frame, slots still released on both). **CLEAN after 20 iterations.** That is the
same shape as running on hardware with no RT at all, which is the configuration Step 8 flagged as
never exercising the async queue.

**NOT tested, and not claimed: a forced device removal.** The engine has no hook for it — DRED is
configured for diagnosis, and `--rt-force-as-fail` is an allocation-failure hook, not a removal one.
Writing one was out of scope for this step. The gap is narrow (`WaitForGpuIdle` now signals and waits
on both queues, and a removed device fails both identically), but it is a gap, and it is recorded as
one rather than glossed.

**`--no-async-compute` restores the single-queue frame** — 30 CLEAN iterations, and the submission is
26 lists with no compute segment. It is NOT byte-identical to the pre-Step-8 dump, and Step 8 records
why: `Main_RTTrace` gained a real prereq on `Main_Hzb`, which moves it two places in the topological
order. That is a correctness fix, not the flag failing.

---

## Risk register

- **Queue-illegal states (R10) are the blocker the first draft missed.** `PIXEL_SHADER_RESOURCE`,
  `RENDER_TARGET`, `DEPTH_*` cannot appear in a barrier on a compute list, the engine's combined
  read states contain them everywhere (ocean sim AND surf sim), and the enhanced translation table
  is queue-blind. Steps 5 and 7 exist for this; do not defer it into Step 8 and discover it as a
  debug-layer error.
- **The barrier compile's single-linear-order assumption is the structural work (Step 7),** and its
  **cross-frame cache (R9) is the part that fails silently.** A cache hit serving barriers whose
  before-state the other queue already moved past is GPU corruption with no message. Every claim of
  "unchanged barriers" runs under `--barrier-cache-verify`.
- **Undeclared reads (R14).** Graph position, not declaration, is what orders them today. Moving a
  pass to another queue keeps its position and destroys that ordering, and because the read is
  undeclared no fence edge is generated to replace it. This one produces no error at all — just
  wrong pixels, intermittently.
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
  overlapped with an occupancy-bound raster pass can slow both. Hence Step 8's "no regression" bar
  and the permanent `--no-async-compute`.
- **The COMPUTE command-list lane has never executed (R2).** Cheap to prove in Step 1; expensive to
  debug if it first runs concurrently with everything else in Step 8.
- **This document goes stale fast.** Its first version was invalidated inside an hour by a refactor
  landing underneath it — every `SceneRenderer.cpp:NNN` citation, the authoring API, and every number
  in the reference table. Re-verify citations at the start of each step (see the Executor Guide).
- ~~The bind cache is `thread_local` and reset per command-list acquire.~~ **Closed** — R13:
  `BeginThreadCommandList` already resets it for every list type (`Renderer.cpp:751`).

## Follow-on: the runtime toggle (developer window)

Step 8 shipped `--no-async-compute` as a boot flag. It is now **also a checkbox** on the developer
window's Frame tab (the old Render tab was split into Frame / AA / Scale / Visibility / Reflections /
Fog on 2026-09-06), next to the trace controls — because that is where its effect is read.

Flipping it at runtime is sound for three reasons, and would be a landmine without any one of them:

- the graph is **rebuilt from scratch every frame** (`SceneRenderer::Render` -> `RenderGraph::Reset`),
  so `g_noAsyncCompute` is read per frame, on the main thread, before `ExecuteParallel`;
- the barrier compile cache carries the pass **queue in its key** (`CompileInputsUnchanged` compares
  `c.queue`), so a flip MISSES the slot instead of serving barriers compiled for the other queue;
- `SubmitTimeline::BeginBatch` sets the batch queue explicitly, so a pooled slot cannot keep the
  queue it had last frame.

The control does not lie in either direction. On a device where the second queue failed to create
(`GraphicsDevice::ComputeQueue() == nullptr`, non-fatal since Step 1) there is no checkbox at all,
only the state line — an inert control is worse than none. And the checkbox is followed by what the
switch actually PRODUCED on the GPU last frame: `render::g_asyncComputeLists` /
`render::g_crossQueueWaits`, counted in the submit loop over the segments that were really executed,
not over what the graph intended.

**Measured, not assumed** (this is the whole point of the counters):

- One process, one trace, the flag flipped halfway through a 120-frame capture:
  `Pass_BuildAS` and `Pass_RTTrace` both run **tid1 x62 -> tid0 x60** — one clean transition, no
  interleaving. Control: `Pass_VsmPageRender` is **tid0 x123**, it never moves.
- The readout itself: **lists=2 waits=3** with async on, **lists=0 waits=0** under
  `--no-async-compute` (the single-segment collapse, no synchronisation at all).
- Gate after the change: `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN,
  `barriers: emit enhanced=7912` — the same count as Step 10.

### OPEN BUG: device removed by RAPID toggling of the async checkbox

Reported live and REPRODUCIBLE FOR THE USER, in `Release_Editor`: boot in the current configuration,
click the developer-window async checkbox repeatedly. **Clicking about once a second is fine; speed
the clicking up and it fires.** The first report added CSM and some RT-control toggling to the
sequence, but those turned out to be incidental — the rate is the variable. No
`logs/invariant_failure.log`, so none of the registration or compile guards saw it: the GPU faulted.

**NOT reproduced headlessly, in fifteen runs.** Flipping the flag in the phase a click actually
lands (after `BeginFrame`, before the graph build), 600 times at frame rate — far faster than any
clicking — stayed clean in Release_Editor with the drain both ENABLED and BYPASSED (an env switch so
both arms ran on one binary). The earlier scripted sequence (async off -> SSR -> rtDebugView ->
RT -> Legacy -> async on) was likewise clean in Debug+GBV, Release and Release_Editor, three runs
each, every one verified to have crossed the feature (`lists 0 -> 2, waits 3`).

So a headless flip is NOT the same event as a click, and the missing ingredient is still unknown.
The most obvious untested difference: in a headless run the developer window is CLOSED, so the frame
that a real click lands in also carries the whole ImGui/editor panel workload.

Killed by reading, not by running:

- **command-list type reuse.** `FrameResource` pools allocators and lists in TYPE-INDEXED lanes
  (`QueueIndex_`), so a flip cannot hand a DIRECT list to the compute queue.
- **a split graph.** `DeveloperWindow::Draw` runs on the main thread before `scene.Render`, so a
  click cannot land between two `AddPass2` calls and put half a frame on each queue.
- **an unsynchronised AS build.** `Main_RTDebug` carries its own mtDep on `gb.pBuildAS`, so the
  `rtDebugView && !rtReflect` shape (BuildAS async, RTTrace absent) still has a fence edge.
- **the frame fences.** `SignalFrame` signals BOTH with one value every frame, submitted or not;
  `SignalCrossQueue` is monotonic and its wait is GPU-side. Neither starves when a queue goes idle.
- **`ProbeComputeLaneOnce`.** Gated on `--compute-lane-probe`, and it never submits.

Shipped, and neither is a diagnosis:

- **`Renderer::SyncAsyncQueueMode()`** drains both queues when the switch changes, called from
  `SceneRenderer::Render` immediately before the graph is built. It started in `BeginFrame` and was
  wrong there: the developer window is drawn BETWEEN `BeginFrame` and the graph build, so a click
  landed after the check and the first frame under the new topology went out undrained. Correct
  hygiene either way — every compile guard validates ONE frame's graph and none can see two
  in-flight frames disagreeing about queue ownership — but **unproven as a fix**.
- **`Renderer::ReportDeviceRemovalOnce()`**, called every `BeginFrame` and from `Present`'s catch.
  `Present` was a bare `ThrowIfFailed`, so the real occurrence left NOTHING on disk. The reason
  class is most of the answer: `DEVICE_HUNG` = a wait whose signal never came, `DEVICE_RESET` = a
  fault in this app's work, `DRIVER_INTERNAL_ERROR` = a malformed command.
  The per-frame poll is **DEFAULT OFF** and turned on with **`--dr-check`**
  (`render::g_deviceRemovalCheck`). Off by choice, not by cost: `GetDeviceRemovedReason` was measured
  at **0.207 us** — 20000 calls over three Release runs gave 0.2060 / 0.2069 / 0.2109 us — so once per
  frame against a ~3270 us CPU frame is **0.006 %**, some fifty times under the 0.6 % run-to-run
  spread, i.e. unmeasurable. It is off to keep the frame loop free of a diagnostic nobody is
  currently using; turn it on when hunting a removal that does not surface at Present.
  `Present`'s catch is NOT gated and always reports: that path costs nothing until something has
  already gone wrong, so there is no reason to be able to lose it.

**USER REPORT after the drain moved to the graph-build point: rapid clicking no longer fires it.**
One session, so it is evidence and not proof — but it is the first thing that has changed the
behaviour, and it points at in-flight topology overlap being the mechanism after all.

**Next step is the user's, not a script's:** reproduce on a binary built after this, then read
`logs/device_removed.log`.

Gate after both: `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN, `enhanced=7912`.

## Follow-on: Main_ObjectCompute moved to the compute queue

Step 10 left the ocean sim + particles on the graphics queue and recorded the blocker. Both halves of
that blocker are now gone, and the pass runs async for **-2.6 % wall-clock**.

**The state cycle.** The sim's maps crossed the queue boundary twice a frame and the sim itself set
the state its far-away consumer wanted: `NON_PIXEL|PIXEL`. That PIXEL bit is direct-queue-exclusive,
so it made every transition in the pass illegal the moment it changed queue. The fix is to let each
side own its half:

- the sim leaves the maps **NON_PIXEL** (`OceanSimulation::kSimMapReadState`), legal on both queues;
- **`OceanRenderable::PrepareRender`** acquires the PIXEL bit on the graphics queue, where adding it
  is legal — including the FOAM map, which was being read undeclared and only worked because the sim
  pre-set its state;
- **`Main_PrologueClear`** strips it again at the top of the next frame. That is D7's release half,
  and the reason it had no home before was that the search was aimed at the transparent pass. The
  prologue is on the graphics queue, is the frame's first pass, and is already a direct prereq —
  exactly the shape `Main_Hzb` has for `Main_RTTrace`.

**The slack.** Legality alone bought NOTHING: measured, `Pass_ObjectCompute` overlapped **0 %** of
any graphics work, because `Main_SurfSim` and `Main_TerrainDepth` had prereqs on it and the whole
frame chains behind them. Neither reads anything the sim writes — the surf sim owns its own
wave/foam/spawner textures, the terrain-depth pass renders the shore map and builds its SDF — so
those arcs were ordering inherited from when this was all one graphics chain. Repointed to
`Main_GpuInstanceCompute`, and the real consumer named instead: `Main_Transparent`, where the ocean
surface samples the maps and the particle emitters (transparent objects) are drawn.

**That prereq on `Main_Transparent` is load-bearing.** The graph derives a cross-queue fence from
prereqs and mtDeps and NOT from resource declarations, so without it the transparent pass would read
maps the compute queue is still writing.

| | control (HEAD) | async + slack |
|---|---|---|
| GPU.Frame mean | 3108, 3151 us | 3052, 3048, 3048 us |
| | mean 3130 | mean **3049 (-2.6 %)** |
| `Pass_ObjectCompute` | 113 us | 234 us (**+107 %**) |
| overlap | n/a | **95 %** covered (ExecuteBundles, VsmPageRequest, VsmPageRender) |

The ranges do not touch (worst control 3108 > best after 3052) and the control spread is 1.4 %. The
pass itself more than doubles under contention — a worse ratio than RTTrace's +88 %, as expected for
a bandwidth-bound FFT — and still wins, because 95 % of it is hidden.

**A real barrier defect fell out of this.** `barriers::EmitOne`'s UAV-barrier path hard-coded
`SyncBefore = COMPUTE_SHADING | PIXEL_SHADING` and never saw step 8's compute-queue narrowing, which
lived inside `EmitEnhanced` only. The ocean sim UAV-barriers its textures between dispatches, so the
first pass to do that on the compute queue failed `Close()` with E_INVALIDARG. Both sites now share
one `kComputeQueueSync` table and one `NarrowSyncForQueue`. A queue-legality rule that lives at one
of two emit sites is not a rule.

**Diagnostics that made this cheap** (both kept):

- `Renderer::EndThreadCommandList` names the LIST and the HRESULT on a Close failure instead of
  "Close() failed" — that alone pointed at `ObjectCompute (compute queue) hr=0x80070057`;
- `Renderer::DumpDebugLayerMessages` drains the D3D12 debug layer into
  `logs/invariant_failure.log`, because with `--gbv` the layer had already named the exact problem
  and was saying it to `OutputDebugString`, i.e. nowhere on a headless run.

Gates: `--scene-stress-gbv=20 --barrier-cmp --canonical-check` CLEAN; the same in
`--shadow-mode=legacy` CLEAN; the same under `--no-async-compute` CLEAN. Barriers
`enhanced=8050 legacy=0` (7912 before — the prologue release and the foam acquire), and legacy=0
confirms the new narrowing forces no fallbacks.

## FIXED: async BuildAS hung the device (in-place wind BLAS refit)

Open since step 10 and only caught once the diagnostics below existed. `Main_BuildAS` on the compute
queue intermittently hung the GPU: **DXGI_ERROR_DEVICE_HUNG, no page fault**.

**Cause.** `AccelerationStructureManager::BuildOrRefitWindSlot` updated the wind BLAS **in place**
(`Source == Dest == s.blas`, one buffer and one scratch per slot, no per-frame copies), while
`Main_BuildAS` is registered with **no prereqs at all** — its compute segment is submitted first,
with no cross-queue wait. With kFrameCount frames in flight, frame N+1's refit therefore overwrote a
BLAS that frame N's graphics RT passes (RTResolve / GlassReflections / RTDebug) were still tracing.
A traversal over a half-rewritten BVH does not fault, it spins. On the graphics queue the same refit
was strictly ordered after those readers, which is why it never bit before step 10.

**Evidence, in the order it arrived:**

| arm (~3500-frame sessions) | hangs |
|---|---|
| async on | **4 / 6** |
| `--no-async-compute` | 0 / 6 |
| `--set=rt.windBlas:0` (async on) | 0 / 4 |
| HEAD without the ObjectCompute move | 1 / 4 — so not that change |
| **after the fix** | **0 / 8** |

DRED, once it printed the op HISTORY rather than just a count, put the stall inside the build itself:
`op[15] BUILD_AS, op[16] BUILD_AS, op[17] <-- STALLED HERE BUILD_AS`, with every later compute list
parked at BEGIN_COMMAND_LIST and all graphics done.

**Fix.** One BLAS + scratch **per frame in flight** (`WindBlasSlot::frames`). The deformed VB does
not need it: only the compute queue touches it, and that queue is serial. The rebuild cadence moved
from a frame-number test to a per-copy refit count (`kWindRefitsBeforeRebuild`), because a copy is
now only touched every kFrameCount frames and the old rule would have tripled each chain's length.

**Cost:** GPU.Frame 3049 -> 3080 us, i.e. +1.0 % against a 1.4 % control spread — no measurable
regression. `Pass_BuildAS` mean actually fell (178 -> 146 us) while its max rose (877 -> 1130 us):
fewer, larger rebuilds. Total AS memory **25.24 MB** including the tripling, so the extra is bounded
by ~17 MB and is not a real budget item. Gate `--scene-stress-gbv=20 --barrier-cmp
--canonical-check` CLEAN, `enhanced=8050 legacy=0`.

**Why nothing caught this earlier, worth keeping:**

- **GBV cannot.** No API rule is broken — the commands are legal, `Close()` succeeds, the barriers
  are all present. And GBV serialises the queues, which removes the very concurrency that causes it.
  The debug layer catches CONTRACT violations (it named the PIXEL_SHADING sync instantly); this is a
  data race between queues, which is a different category.
- **The report did not survive.** A device removal usually surfaces as a `ThrowIfFailed`, and on a
  worker thread that is an uncaught throw: the process dies through `std::terminate` (0xC0000409)
  before the next BeginFrame. One hang in three left nothing on disk until a `std::set_terminate`
  handler was installed.
- **A breadcrumb count names nothing.** "op 17 of 29" became useful only as `op[17] BUILD_AS`.

## Non-goals

- **A copy queue** for uploads. The same architecture makes it straightforward afterwards, but it
  has its own lifetime rules — separate piece of work. (Step 11 still has to *decide* what the
  existing direct-queue uploads mean for the compute queue; that is not the same as building one.)
- **Splitting rasterisation** across queues. Two raster workloads contend for the same units.
- **Reworking what the passes themselves do.** This plan changes where work runs, not what it is.
  The one exception is Step 9, which splits a pass along a line that already exists inside its
  builder.

## After this plan — where the second queue is actually worth more

Recorded here so it is not re-derived later, and explicitly out of scope for the eleven steps above.
The ~14.8 % budget of Step 10 covers *overlapping work that must finish this frame*, and it is bounded
by how much independent graphics work the frame contains. The larger prize is work that does **not**
have to finish this frame at all, which is bounded by nothing:

- **Wind-deformed BLAS refit** — **this LANDED in `fe23d99`**: `Main_BuildAS` now runs a wind
  deformation material (`rtAs_.Build(..., GetRtWindDeformMaterial())`), and it is no longer cheap:
  86 us median but **206 us mean, p95 625 us**. It has moved out of this section and into Step 10's
  movable list. What remains here is the harder version: refitting at a lower rate than once per
  frame, which its bimodal cost makes attractive — that needs the AS lifetime decoupled from the
  frame, not just a second queue.
- **Editor thumbnail rendering** (`AssetThumbnailCache`), which today contends with the frame.
- **Ocean mip chain / foam**, if the split in Step 8 shows them separable from the FFT.

Those are "second queue as a background lane", not "second queue as an overlap", and they are worth
a separate plan once the machinery here exists.
