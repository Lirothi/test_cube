# Async compute — architecture plan

**Status: NOT STARTED.**

**Goal: the renderer gains a second execution queue as a first-class architectural capability.**
The render graph learns to schedule a pass onto a `D3D12_COMMAND_LIST_TYPE_COMPUTE` queue,
cross-queue dependencies become fence edges the graph owns, and the barrier compile learns
per-queue resource ownership *and per-queue state legality*.

This is a capability, not an optimisation. Perf is a consequence and a regression check — **it is
not the acceptance criterion for any step.** The engine should be able to express "this work runs
on the async queue" because that is the shape a modern renderer needs; what it is worth on today's
scene is a separate question, answered later and per-pass. The honest numbers on today's scene are
computed in "Reference points" below — **6.6 % for the first mover alone, ~11 % theoretical across
every pass that can move, ~5-7 % realistically after contention** (measured 2026-08-28). Write those
down now so nobody is surprised by them at the end, and note two things: the budget only converts to
frame time because the frame is measurably GPU-bound, and **these percentages have already moved
three times in two days** because the frame moves around them. The structural claims come from the
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

**Citations were verified against the tree on 2026-08-28 at HEAD `fe23d99` ("rt wind reaction"),
working tree clean; the numbers come from a capture taken at that HEAD.** Those dates matter. This
file was first written against a tree that changed underneath it within the hour —
`SceneRenderer.cpp` was split into `SceneRenderer_{Graph,Geometry,Lighting,Post,Reflections,Shadows}.cpp`,
the render graph moved to `AddPass2`, and four passes were added to `RenderPass`. A day later five
more commits landed (`RT upgrades`, `rt wind reaction`, denoiser/SSR tuning), `Main_RTDenoise` was
deleted and folded into `Main_ReflectionTemporal`, and the measured frame changed by 38 %.
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
your eyes** — group by name across ALL frames, take medians. A single frame proves nothing;
`--wind-freeze` makes the scene reproducible. The GPU track is `tid == 0` ("GPU Queue" in the
`thread_name` metadata); `GPU.Frame` events carry a per-frame suffix, so strip it before grouping.

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

So per-queue state is not only about *which* barrier is emitted (Step 7) but about *whether the
requested state can be expressed on that queue at all*. That gets its own checked rule (Step 5).

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

**D4. Every step keeps the default behaviour until Step 9, and there is a permanent off switch.**
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

**Re-measured 2026-08-28** from `traces/trace_20260828_163221_release_000.json`, 123 frames,
medians of the GPU track (`tid 0`), at HEAD `fe23d99`.

**READ THIS BEFORE USING ANY NUMBER BELOW.** This table is not a property of the engine. It is a
property of the camera, the level's settings and the code at the moment of capture, and it moves
FAST. Between 2026-08-27 and 2026-08-28 — one day, five commits — it moved like this:

| scope | 08-27 | 08-28 | |
|---|---|---|---|
| GPU.Frame | 3476 us | **2157 us** | -38 % |
| Pass_VsmPageRender | 1237 us | **312 us** | -75 % |
| Ocean.Surface | 60 us | **292 us** | x4.9 |
| RenderObjectBatch | 63 us | **304 us** | x4.8 |
| Pass_BloomConv | 303 us | **absent** | |
| Pass_Gtao | 73 us | **absent** | |
| Pass_RTReflections | 176 us | **87 us** | -51 % |

Two of those passes did not merely get cheaper, they **stopped appearing** — GTAO and the bloom
convolution are off in this configuration. So: **the structural claims in this plan come from the
render graph's prereq lists, which are stable; the percentages come from this table, which has a
shelf life of about a day.** Re-capture before Step 9 and again before Step 10, and treat any
inherited number as wrong until re-measured. The first version of this plan was wrecked exactly this
way.

| scope | median | share |
|---|---|---|
| **GPU.Frame** | **2157 us** | 100 % |
| Pass_VsmPageRender | 312 us | 14.5 % |
| RenderObjectBatch (transparent/ocean draws) | 304 us | 14.1 % |
| &nbsp;&nbsp;Ocean.Surface | 292 us | 13.5 % |
| Pass_DLSS | 265 us | 12.3 % |
| **Pass_ObjectCompute** | **142 us** | **6.6 %** |
| ExecuteBundles | 108 us | 5.0 % |
| Pass_VsmPageRequest | 87 us | 4.0 % |
| Pass_RTReflections | 87 us | 4.0 % |
| &nbsp;&nbsp;VsmPageRender.Scatter | 83 us | 3.8 % |
| Transparent.Driver | 73 us | 3.4 % |
| Pass_Tonemap | 67 us | 3.1 % |
| Pass_ShadowCull | 49 us | 2.3 % |
| Pass_OceanReflection | 48 us | 2.2 % |
| Pass_Lighting | 43 us | 2.0 % |
| Pass_SpotLights | 38 us | 1.8 % |
| Pass_ExposureMetering | 33 us | 1.5 % |
| Pass_Hzb | 27 us | 1.3 % |
| Pass_PointLights | 27 us | 1.3 % |
| Pass_BuildAS | 20 us | 0.9 % |
| VsmPageRender.Setup | 15 us | 0.7 % |

**The scopes NEST — do not add the shares up.** `Tonemap.Curve` and `Tonemap.Resolve` sit inside
`Pass_Tonemap`; `VsmPageRender.Scatter` and `.Setup` sit inside `Pass_VsmPageRender`;
`Ocean.Surface` sits inside the transparent `RenderObjectBatch`.

**The frame is still GPU-BOUND, but by a narrower margin than a day ago.** From the same capture:

| | 08-27 | 08-28 |
|---|---|---|
| CPU frame | 3468 us | 2140 us |
| GPU frame | 3476 us | 2157 us |
| `Renderer::WaitForFrame` | 2814 us | **1226 us** |
| actual CPU work per frame | ~654 us | **~915 us** |

The CPU still idles 57 % of the frame on the fence, so GPU savings still convert ~1:1 — but the
headroom is now 2157 us of GPU against 915 us of CPU work rather than 3476 against 654. **Re-check
this ratio before Step 9**; if the scene keeps getting cheaper on the GPU while the CPU side does
not, the whole async budget stops converting to frame time and the plan's value becomes purely
architectural.

**The frame is still almost perfectly SERIAL.** One median-duration frame, 32 scopes, **46 us of
total gaps** out of 2157 — no accidental parallelism to lose, plenty of room to create some:

```
Pass_BuildAS            0 ..   19     Pass_Lighting         1039 .. 1081
Pass_PrologueClear     23 ..   41     Pass_SpotLights       1085 .. 1123
Pass_ObjectCompute     44 ..  184     Pass_PointLights      1126 .. 1153
Pass_ShadowCull       189 ..  240     Pass_Skybox           1157 .. 1164
GBuffer.Driver        243 ..  250     Pass_RTReflections    1167 .. 1256
ExecuteBundles        253 ..  360     Pass_Reflection.Temporal 1259 .. 1275
RenderObjectBatch     363 ..  385     Pass_Reflection.Blur  1279 .. 1299
Pass_VsmPageRequest   387 ..  474     Pass_Compose          1302 .. 1324
Pass_Hzb              477 ..  505     Glass (Gbuf + Refl)   1328 .. 1360
Pass_VsmPageRender    509 .. 1036     Transparent.Driver    1363 .. 1436
                                      RenderObjectBatch     1440 .. 1771
                                      Pass_ExposureMetering 1776 .. 1809
                                      Pass_DLSS             1811 .. 2076
                                      Pass_Tonemap          2080 .. 2146
```

**The first-mover ceiling: 6.6 %.** `Pass_ObjectCompute` is 142 us of a 2157 us GPU frame. The async
part is smaller still, because the GI rotation stays on graphics (R11), so measure the split's share
in Step 8 before believing any Step 9 number. Note the direction of travel: this ceiling was 7.8 %,
then 3.9 %, now 6.6 % — the pass barely moved, the frame moved around it.

**The full analytic budget across every pass that can move: ~240 us, ~11 %.** Derived in Step 10
from the graph's own prereq lists. Realistically **5-7 %** after contention, since both queues share
shader cores.

**The overlap partner is still `Pass_VsmPageRender`, but it is no longer huge.** At 527 us in the
frame above (312 us median) it remains the single largest raster block and the only one long enough
to hide the movers behind — and depth-only rasterisation of many instances is geometry- and
fixed-function-bound with low ALU occupancy, which is the best case for hiding compute underneath.
But the "a third of the frame" framing from the previous capture is gone: it is now 14.5 %.
**Step 9's overlap partner is named: `Pass_VsmPageRender`.** If the two-track trace does not show
the async pass running under it, the move did not work.

**Context, so the budget is not oversold.** The two biggest items in the frame are now the
transparent/ocean draw (`RenderObjectBatch` 304 us, of which `Ocean.Surface` is 292) and `Pass_DLSS`
(265 us). Async compute touches neither. This plan is worth doing for the capability; it is not the
frame's biggest number and must not be sold as one.

**First mover: the ocean + particle half of `Main_ObjectCompute`** (Step 9). It is pure compute, its
states are compute-legal or made so by D7, and its consumer is `Main_Transparent` at the very end of
the frame — so the overlap window spans shadow cull, CSM, the G-buffer, the VSM page request and
page render, lighting, compose and RT. The GI rotation does NOT move: `Main_ShadowCull` consumes its
output two passes later (R11), which is a fence, not an overlap.

**GPU.Frame stops meaning "all GPU work" at Step 9.** It is bracketed by two timestamps on the
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
succeeds and is logged; `--trace` GPU.Frame median within run-to-run noise of the pre-step number
(record both).

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

### Step 4 — the graph learns the word "queue" (inert)

`RenderQueue { Graphics, AsyncCompute }` on pass registration, defaulting to `Graphics`. **The API
is `AddPass2` (R15)** — one builder per pass, `SetPassPrepare` is private and must stay that way.
The pass context exposes the queue so `BeginCL` (`RenderGraph.h:131`) acquires the right list type.
**Every existing pass stays on Graphics.**

**Acceptance:** all gates CLEAN; GPU.Frame unchanged within noise; the enum is threaded through but
provably unused — grep shows no `AsyncCompute` at any call site.

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

### Step 6 — per-queue submission and fence edges (inert)

`SubmitTimeline` groups batches per queue and returns one list array per queue; `Renderer` submits
each to its own queue. Graph dependencies that cross queues compile into signal/wait pairs (D2).

**Acceptance:** the graphics queue's submitted array is **byte-identical** to today — dump
`fixedSubmitScratch_` (the WRAPPED array: profiler-begin list, work lists, epilogue — R3) in order
under a temporary flag, before and after, and diff them. The compute array is empty and no fence
edge exists yet because no pass is async. All gates CLEAN. If the array differs, the step is not
done, however plausible the difference looks.

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

### Step 8 — split `Main_ObjectCompute` by consumer, still all on Graphics (inert)

Graph surgery only, no queue change — so it is separately bisectable from the async flip.

- Dissolve the `BeginCLGroup` around `Main_PrologueClear` + `Main_ObjectCompute` + `Main_SurfSim` +
  `Main_ShoreWetness` (`SceneRenderer_Graph.cpp:81-132`), because a grouped pass cannot change queue
  (R8). Four members means **three** extra command lists per frame, not one — expected, report it,
  do not chase it. If the CPU cost is unacceptable, the alternative is to keep a group *per queue*
  rather than dissolving; decide with the measurement, not in advance.
- Split the object-compute builder (`SceneRenderer_Graph.cpp:95-117`) by consumer (R11) into
  `Main_GpuInstanceCompute` (the GI rotation, stays on Graphics forever — `Main_ShadowCull` consumes
  it) and `Main_ObjectCompute` (ocean + particles, the future async pass). R15 makes this cheaper
  than it used to be: the builder already partitions the scene by `PrepareCompute()` returning true,
  so each new pass gets its own filtered `ObjectComputeList` and there is no Prepare/Record pair to
  keep in sync by hand. Add the enum entry and its `RenderPassToWString` case — note
  `kMainRenderGraphPassCount` is `RenderPass::Main_Count` (`SceneRenderer.h:37`), so graph capacity
  follows the enum automatically.
- **Measure the split**: the trace must now show two scopes summing to roughly the old 134 us. That
  number is the real ceiling for Step 9, and it is not 134.

Do not "improve" anything else while in there.

**Acceptance:** all gates CLEAN, 0 MISSING; visual parity via `--shot` at a frozen wind clock; the
two new scopes appear in the trace with the expected split; the frame's CPU cost grows by at most
the dissolved group's worth of per-CL overhead (report the number, do not hide it).

### Step 9 — the first real user: the ocean/particle compute on the async queue

Move `Main_ObjectCompute` (post-split: ocean + particles) to `AsyncCompute`, and add
`--no-async-compute` (D4).

Before moving it, verify independence from the passes it will overlap: list every resource it writes
(**its builder's declarations are the authoritative list — read them, do not guess; this plan names
`Main_Transparent` as the consumer from `SceneRenderer_Graph.cpp:1124-1128`, and that claim is a
starting point, not a substitute for the check**) and every resource the concurrent graphics passes
read, and intersect. A non-empty intersection is not a blocker — it is a required fence edge (D2) —
but it must be **declared**, not discovered by the debug layer. Check R14 explicitly here: the surf
sim's undeclared shore-map reads are the known instance, and the ocean sim is the same subsystem.

Expect D7 to bite: the ocean's own registrations use `NON_PIXEL | PIXEL_SHADER_RESOURCE`
(`OceanSimulation.cpp:1102-1133`). Those registrations move to the *consumer* side; the async pass
hands the maps over in a compute-legal state. Step 5's rule will tell you exactly which lines to
change, by name.

**Take `Main_ShoreWetness` along in this step** (rationale in Step 10). It costs almost nothing, its
states are already compute-legal so it needs no D7 work, and its stamp gives this step a
compute -> graphics UAV handoff to exercise rather than a plain producer/consumer read. It brings
one obligation with it: `pWetness` is a prereq of `Main_RTReflections`
(`SceneRenderer_Graph.cpp:797`), which cannot move itself, so RT reflections gain a cross-queue wait
— **name that edge here**, do not let the debug layer find it. And add the GPU scope the wetness pass
is missing, or Step 9's own trace cannot show whether it overlapped.

**Acceptance:**
- All three gates CLEAN, zero debug-layer messages, 0 MISSING.
- **The two-track trace shows the pass genuinely overlapping `Pass_VsmPageRender`**, not merely
  relocated. That partner is named on the strength of the measurement above — it is a third of the
  frame and it is raster. This is the step's real acceptance criterion: the architecture works.
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

**The criterion is INDEPENDENT CONCURRENT GRAPHICS WORK, not distance to the consumer.** An earlier
version of this step ranked candidates by how many passes separated them from their first consumer
and got the answer backwards in both directions: it declined `Pass_ShadowCull`, `Pass_Gtao` and
`Pass_Hzb` because each feeds the very next pass, and nominated `Pass_RTReflections` because its
consumer is far away. Both readings are wrong. What matters is whether there is graphics work the
candidate does not depend on, which can therefore run alongside it — a pass whose consumer is next
in line still moves if the raster work beside it is independent, and a pass whose consumer is distant
does not move if it is itself transitively stuck behind that raster work.

The answer comes from the graph's own prereq lists, which is the authoritative statement of what
depends on what. Verified 2026-08-27 in `SceneRenderer_Graph.cpp`:

**Movable — ~240 us total, ~11 % of the frame (2026-08-28 numbers):**

| candidate | cost | hides behind | the proof |
|---|---|---|---|
| `Main_ObjectCompute` (ocean + particles) | <=142 us | `Pass_VsmPageRender` (527 in-frame) | consumer is `Main_Transparent` at ~1363, the pass sits at ~44 (Step 9) |
| `Pass_ShadowCull` | 49 us | `ExecuteBundles` + the opaque `RenderObjectBatch` (129 us of raster at 253-385) | prereqs are `{ pShoreDepth }` — it does **not** depend on the G-buffer, so the G-buffer raster can run beside it (`SceneRenderer_Graph.cpp:217`) |
| `Pass_Hzb` | 27 us | `Pass_VsmPageRender` | prereqs are `{ pGbuf }` and nothing else (`:409`) |
| `Main_BuildAS` | 20 us | anything after it | it is the FIRST pass in the frame and its consumer is `Main_RTReflections` at ~1167 — ~1.1 ms of slack. See the note below: it declares nothing, so its edge comes from its mtDep, not from resource declarations |
| `Pass_Gtao` | **0 here, 73 us when enabled** | `Pass_VsmPageRender` | prereqs are `{ pGbuf, pHzb }`, while `Main_Lighting` is `{ pGbuf, pVsmPageRender, pGtao }` — the graph proves GTAO does not wait on shadows (`:440`, `:584`). **Absent from the 08-28 capture** (GTAO off in that configuration), so it is structurally movable but currently free. Do not drop it from the list on one capture's evidence |

Together they put ~240 us on the compute queue spread across a ~1.3 ms graphics window, so they do
not contend with each other and can move in any order.

**`Main_BuildAS` is a special case worth its own line.** It declares no resource states at all — the
AS build bypasses the barrier compile entirely and the buffers stay in
`RAYTRACING_ACCELERATION_STRUCTURE` (`SceneRenderer_Graph.cpp:58-77`). That is R14's shape: no
declaration, no derived edge. It is saved by having `pBuildAS` as an explicit **mtDep** of
`Main_RTReflections` (`:797`), which is a graph edge D2 can compile a fence from without any
resource declaration. Confirm that before moving it, because if the edge were only positional the
move would silently unorder the TLAS against its only reader. Since `fe23d99` this pass also runs a
wind-deformation refit (`rtAs_.Build(..., GetRtWindDeformMaterial())`), so it is real GPU work now
and worth re-measuring rather than assuming it stays at 20 us.

**Not movable, with the reason:**

| candidate | cost | why not |
|---|---|---|
| `Pass_BloomConv` | absent 08-28 (was 303 us) | inside `Pass_Tonemap`; the tone curve consumes it immediately, and the whole tail chain (bloom -> curve -> DLSS -> metering) is serial by data |
| `Pass_RTReflections` | 87 us | **wedged, see below** — it reads the lit scene, and everything after it reads what it writes |
| `Pass_VsmPageRequest` | 87 us | reads the completed depth buffer and feeds `Pass_VsmPageRender` directly; serial by construction |
| `Pass_ExposureMetering` | 33 us | reads the finished scene colour and is already last |
| `Pass_Lighting` | 43 us | prereqs name `pVsmPageRender` explicitly |
| `VsmPageRender.Setup` | 15 us | collapsed from 107 us; below the level worth a step |
| `Main_SurfSim` / `Main_ShoreWetness` | **<=6 us combined, and UNMEASURED** | not a cost case — see "The wetness pass" below; they ride along with Step 9 rather than getting a step |

**`Pass_RTReflections` is WEDGED, and this is the question worth answering carefully, because as a
technique RT is an excellent async candidate.** Inline RayQuery is latency- and divergence-bound
rather than ALU-saturating, which is exactly the workload you want overlapping rasterisation. The
blocker here is not the queue, it is the data:

- The pass **declares `D.light` as a read** — the lit HDR target — alongside depth, gb1 and the
  reflection UAV (`SceneRenderer_Graph.cpp:798-801`), and uses it as the **on-screen hit fast path**
  (`SceneRenderer_Reflections.cpp:229`, "lit HDR (fast path)"). It also consumes the spot/point light
  buffers filled earlier in the frame (`:242`).
- So everything immediately BEFORE it writes what it reads: `Main_Lighting` (which itself waits on
  `pVsmPageRender`), then `Main_SpotLights`, `Main_PointLights`, `Main_Skybox`.
- And everything immediately AFTER it reads what it writes: `Main_ReflectionTemporal` ->
  `Main_ReflectionBlur` -> `Main_Compose` -> glass -> transparent.
- In the 08-28 timeline that is the stretch 1039..1363: lighting, three small light passes, RT, then
  the reflection resolve chain. **There is no independent graphics work adjacent to it in either
  direction.** Moving RT to the compute queue would leave the graphics queue idle for its 87 us and
  add a fence — a guaranteed small loss.

**The only way RT becomes async-useful is to split the TRACE from the SHADE.** Ray traversal needs
the TLAS, depth and gb1 — all ready when the G-buffer finishes at ~385 — while only the on-screen
fast path needs `D.light`. A trace pass writing hit data could then run on the compute queue
underneath `Pass_VsmPageRender` (509..1036), leaving a cheap resolve to wait for lighting. That is
the standard gather-then-shade split, and it is worth writing down — but it **reworks what the pass
does**, which this plan lists as a non-goal. It belongs in the RT plan, not here. Do not attempt it
as part of Step 10.

**The wetness pass is the cleanest async candidate in the frame, and it should ride along with
Step 9 for reasons that are not about its cost.**

- **Its states are already compute-queue legal.** `OceanWetness::BuildUpdatePass` registers only
  `NON_PIXEL_SHADER_RESOURCE` and `UNORDERED_ACCESS` (`OceanWetness.cpp:219-231`) — no
  `PIXEL_SHADER_RESOURCE` anywhere. R10 does not bite it at all, so unlike the ocean sim and the surf
  sim it needs no D7 hand-over rework. It is the one pass that is async-legal exactly as written.
- **Its slack matches ObjectCompute's.** Its first consumer is `Main_RTReflections`, which names
  `pWetness` in its prereqs (`SceneRenderer_Graph.cpp:797`) and sits at ~2413 in the traced timeline;
  then `Main_Compose` reads the current history as an SRV (`OceanWetness.cpp:56-58`) and
  `Main_Transparent` stamps into the current stamp as a UAV (`OceanRenderable.cpp:783-786`). The pass
  itself sits at ~173. That is a ~2.2 ms window.
- **Its cost is a BOUND, not a measurement, because it has no GPU scope.** `ProfilerScopes` has
  `Ocean.SurfSim` but nothing for the wetness update, so the pass is invisible in the trace. All that
  can be said is that both CL-group siblings execute between `Pass_ObjectCompute`'s scope closing at
  173 and `Pass_ShadowCull`'s opening at 179, i.e. **<=6 us combined** — and in that capture the surf
  sim did not run at all (its scope is absent), so most of those 6 us are wetness plus list-change
  overhead. **Absence from the trace means unmeasured, not zero** — this engine has already paid for
  that confusion once with an 82 us hole that turned out to be bundle work. Step 8 should add the
  missing GPU scope while it is in there.
- **It is the better first exercise of D3.** The ocean displacement is a plain producer -> consumer
  read. The wetness stamp is not: the async pass writes `stamp_[write]` as a UAV, and then the
  GRAPHICS queue writes the same resource as a UAV in `Main_Transparent`. A compute -> graphics
  handoff that is UAV on both sides is a sharper test of the release/acquire pair than a read-only
  consumer.
- **The cost it does add: `Main_RTReflections` gains a cross-queue wait**, because `pWetness` is one
  of its prereqs and RT reflections cannot move themselves. The edge is declared and cheap, but
  Step 9 must NAME it rather than discover it.

Two frames this bound does not cover, and neither may have appeared in the 123 captured: a
`relocate` frame (the wetness window shifts) and a `clearHistory` frame, both of which do more work
in the same dispatch.

**Procedure per candidate**, before any edit: read its builder's prereqs and declarations, list the
graphics passes that do NOT transitively depend on it, and confirm from the timeline that they
overlap its position. Then move it, and require the two-track trace to show it running under the
partner named in the table. If the trace does not show that overlap, revert the move — a relocated
pass that does not overlap is a fence for nothing.

**Expected outcome: three more passes move, for ~174 us on top of Step 9.** That is the analysis's
prediction, not a promise — contention is measured, not assumed, and a candidate that regresses is
reverted and written up. Declining a candidate remains a legitimate, recordable result; what is not
legitimate is moving one because the list looks unfinished.

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
  read states contain them everywhere (ocean sim AND surf sim), and the enhanced translation table
  is queue-blind. Steps 5 and 7 exist for this; do not defer it into Step 9 and discover it as a
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
  overlapped with an occupancy-bound raster pass can slow both. Hence Step 9's "no regression" bar
  and the permanent `--no-async-compute`.
- **The COMPUTE command-list lane has never executed (R2).** Cheap to prove in Step 1; expensive to
  debug if it first runs concurrently with everything else in Step 9.
- **This document goes stale fast.** Its first version was invalidated inside an hour by a refactor
  landing underneath it — every `SceneRenderer.cpp:NNN` citation, the authoring API, and every number
  in the reference table. Re-verify citations at the start of each step (see the Executor Guide).
- ~~The bind cache is `thread_local` and reset per command-list acquire.~~ **Closed** — R13:
  `BeginThreadCommandList` already resets it for every list type (`Renderer.cpp:751`).

## Non-goals

- **A copy queue** for uploads. The same architecture makes it straightforward afterwards, but it
  has its own lifetime rules — separate piece of work. (Step 11 still has to *decide* what the
  existing direct-queue uploads mean for the compute queue; that is not the same as building one.)
- **Splitting rasterisation** across queues. Two raster workloads contend for the same units.
- **Reworking what the passes themselves do.** This plan changes where work runs, not what it is.
  The one exception is Step 8, which splits a pass along a line that already exists inside its
  builder.

## After this plan — where the second queue is actually worth more

Recorded here so it is not re-derived later, and explicitly out of scope for the eleven steps above.
The ~11 % budget of Step 10 covers *overlapping work that must finish this frame*, and it is bounded
by how much independent graphics work the frame contains. The larger prize is work that does **not**
have to finish this frame at all, which is bounded by nothing:

- **Wind-deformed BLAS refit** — **this LANDED in `fe23d99`**: `Main_BuildAS` now runs a wind
  deformation material (`rtAs_.Build(..., GetRtWindDeformMaterial())`). It measured 20 us on
  2026-08-28, so it has moved out of this section and into Step 10's movable list. What remains here
  is the harder version: refitting at a lower rate than once per frame, which needs the AS lifetime
  decoupled from the frame rather than just a second queue.
- **Editor thumbnail rendering** (`AssetThumbnailCache`), which today contends with the frame.
- **Ocean mip chain / foam**, if the split in Step 8 shows them separable from the FFT.

Those are "second queue as a background lane", not "second queue as an overlap", and they are worth
a separate plan once the machinery here exists.
