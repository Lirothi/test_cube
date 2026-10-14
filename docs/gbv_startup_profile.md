# Why `--scene-stress-gbv` takes minutes (2026-09-01)

## The question

"Debug GBV takes forever to start up, it used to be faster." Iterations cost 2.5 minutes each, so
the instrumentation had to be broad enough to answer in one run.

## The answer, in one line

**It is not startup.** Boot is 5.3 s. The run is slow because GBV rewrites every shader the first
time its pipeline is bound, and `lighting_cs.hlsl` alone costs **76 s** to rewrite.

## Measured (Debug, `--scene-stress-gbv=2`, same binary for every row)

| arm | wall |
|---|---|
| `--scene-stress=2` (no GBV) | **9.1 s** |
| `--scene-stress-gbv=2` | **151–166 s** |

Where the GBV time goes (from the `[profiling]` boot-profile records in the session log (formerly `logs/boot_profile.log`)):

| phase | time |
|---|---|
| boot: window + device + level load + first upload | **5.3 s** |
| 9 rendered frames | **119–141 s** |
| teardown | ~23 s |

Of those 9 frames, **two** account for ~116 s: the first warmup frame (73 s) and the first frame
after a level reload (43 s). Every other frame is under 1 s.

Per pass, CPU-side, inside the pass body:

| pass | total CPU |
|---|---|
| `Lighting` | **76.1 s** |
| `SpotLights` | 15.0 s |
| `PointLights` | 14.3 s |
| `GlassReflections` | 6.7 s |
| `RTTrace` | 6.6 s |
| `ExposureMetering` | 5.0 s |

## What is NOT the cause (all measured, all ruled out)

| suspect | measured | verdict |
|---|---|---|
| PSO creation | 406 ms GBV vs 411 ms non-GBV | identical — not it |
| `ExecuteCommandLists` | 1.24 s over 90 submits | not it |
| `Present` | 7.3 ms over 9 frames | not it |
| waiting on the GPU (`BeginFrame`) | 1.9 ms over 9 frames | **the GPU is not slow; this is all CPU** |
| level load / object spawn | 105 ms and 72 ms per reload | not it |
| texture + mesh loading | 403 ms + 54 ms, once, at boot | not it |
| InfoQueue drain | 0.0 ms | not it |

The reason it hides: the rewrite happens at **first bind in a command list**, not at PSO creation —
so it lands on the CPU, in one frame, inside a pass body. Every place you would naturally look
(PSO cache, shader compile, device init) is innocent.

## Why it got slower

`lighting_cs.hlsl` grew. The rewrite cost scales with the bytecode:

| date | size |
|---|---|
| 2026-06-19 | 6.7 KB |
| 2026-08-17 | 16.2 KB |
| 2026-09-01 | 26.3 KB |

Plus its includes: `vsm_sample.hlsli` 20 KB, `csm_sample.hlsli` 17 KB. So the shader that costs 76 s
to instrument is ~4x the size it was in June — which is the whole story of "it used to be faster".

## The knob: `--gbv-mode=`

GBV's shader patch mode, `D3D12_GPU_BASED_VALIDATION_SHADER_PATCH_MODE`, set through
`ID3D12DebugDevice1::SetDebugParameter` after device creation (`GraphicsDevice::InitDevice`).

| `--gbv-mode=` | wall (2 iterations) | what it does |
|---|---|---|
| `guarded` (default, unchanged) | **163–166 s** | D3D12's own default: bounds-check control flow around every access |
| `unguarded` | 151–235 s | same reports, no guard branches — **within noise of `guarded`** |
| `state` | **14.1 s** | resource-state tracking only, no shader rewriting |
| `none` | — | GBV on, shader patching off |

Run-to-run spread is large (guarded measured at 159, 163, 163, 166 and 215 s), so **guarded and
unguarded are indistinguishable** -- the earlier "unguarded is 5 % faster" was noise. `state` at
14.1 / 14.1 / 14.2 / 14.2 s is the only real difference.

## VERDICT: do NOT gate on `state`

Verified by injecting the exact violation from the ghost-drag hang (`--gbv-selftest=1`: root
signature re-set, so every root argument is invalidated, descriptor tables then not bound, draw
issued anyway):

| mode | wall | catches it? |
|---|---|---|
| `guarded` | 215 s | **YES** -- `id=935 GPU-BASED VALIDATION: Draw, Uninitialized root argument accessed ... Pipeline State: 'shaders/skybox.hlsl'` |
| `unguarded` | 235 s | **YES** -- same message |
| `state` | 14.2 s | **NO** -- `gbv.log` empty, harness reports `verdict: CLEAN` |

So `state` returns a **false CLEAN** on the precise bug class that produced a silent queue stall
this session. It is a resource-state smoke pass, not a gate. Use `guarded` (the unchanged default)
for anything whose verdict you intend to trust.

`state` is still useful for what it is: an 11x-faster pass that exercises level churn, resize, DLSS
mode switches and teardown with resource-state validation on -- fine while iterating, never as the
final word.

### `--gbv-selftest=N[@Pass]`

Issues N draws that commit the unbound-root-argument violation on purpose (`Material::Bind`, Debug
only, never armed by default). It exists because a validation mode's NAME does not tell you what it
reports, and "it probably still catches it" is not something to hand someone deciding what to trust.
Re-setting the root signature is what makes it deterministic -- that invalidates every root
argument, so the skipped tables are genuinely unbound at draw time rather than left over from an
earlier draw on the same command list.

`@Pass` (2026-10-14) aims the draws at the lists of one pass (`1@Skybox`); without it they go to the
first N graphics binds, which is whichever pass the scheduler records first -- passes record in
parallel. Every injected draw logs where it landed:
`gbv self-test: injected an unbound-root-argument draw into command list 'Skybox' (shader
shaders/skybox.hlsl)`. **No such line = nothing was injected, and the silence that follows proves
nothing.** Accepted on an ordinary `--gbv` run as well as under `--scene-stress-gbv`.

### The control that made this trustworthy

The first A/B ran guarded → unguarded → state sequentially and produced 159 / 151 / 14 s — a
monotone decrease, which is equally consistent with the runtime's patched-shader cache warming up
across runs. Running **guarded twice in a row** gave 166.3 s and 163.2 s: repeatable, no warming.
Only then was the 14 s meaningful.

The knob was also verified to actually reach the driver rather than being inferred from wall time —
the `[profiling]` boot-profile records in the session log (formerly `logs/boot_profile.log`) counters carry `QueryInterface` and `SetDebugParameter` HRESULTs, and
`logs/gbv.log` opens with the mode the run actually used.

## `--gbv-passes=` -- patch only the passes under work (2026-10-14)

By October the full wind_test frame waited ~2.5 minutes for its first frame under `--gbv`
(Lighting 144-152 s of rewrite, VolumetricFog 61-68 s, Tonemap 22 s, SdsmAnalyze 19 s, ...), and
`state` is not an answer (above: it is falsely clean). Most GBV runs check ONE pass that just
changed, so the rewrite of every other shader is pure waiting.

`--gbv-passes=CloudTrace,Compose` (`rendering/core/GbvPassFilter.*`):

- the device's `DefaultShaderPatchMode` becomes the **rest mode**, `--gbv-rest=state|none`:
  `STATE_TRACKING_ONLY` by default (A/B below), `NONE` on request;
- every command list recorded for a listed pass gets the requested `--gbv-mode` (guarded unless
  asked) through `ID3D12DebugCommandList1::SetDebugParameter(...GPU_BASED_VALIDATION_SETTINGS)`.
  The hook is `SetCommandListName` (plus `BeginCL`): the one call every list a pass records into
  goes through -- `BeginCL`'s, a body's own `BeginThreadCommandList`, each fan-out worker's -- and
  the unnamed bundles of `RenderObjectBatch` take their pass's verdict where they are begun (they
  accept the call -- no failure with `GBuffer.OpaqueSimple` listed; whether GBV then checks INSIDE a
  bundle was not tested). Both
  queues alike: `--gbv-passes=RTTrace,CloudShadow` (both async compute) moved the rewrite into
  exactly those two (4.6 s and 2.9 s of CPU record; next pass 0.08 s). The list pool resets every
  list to the rest mode when it hands it out, so a pooled list never carries a listed pass's mode
  into whatever records into it next;
- names are `RenderPassToWString`'s, case-insensitive; a name covers its dotted sub-passes
  (`GBuffer` = `GBuffer.Driver`, `.OpaqueSimple`, ...). Unknown names are warned about and the valid
  ones are listed; if NO name matches, the device patches everything as without the flag;
- a **CL group** records into one list, which can carry one mode: one listed member lists the whole
  group, logged once -- `CL group [RTResolve, Reflection.Temporal, Reflection.Blur, Compose] records
  into ONE command list and has a listed member, so the whole list is patched -- added: RTResolve,
  Reflection.Temporal, Reflection.Blur`;
- logged once per process at device creation (`render.rhi`): which passes are patched, and the
  `[gbv] requested patch mode = ... on --gbv-passes lists only, device default = STATE_TRACKING_ONLY`
  line. The
  boot profile counts `gbv: device default patch mode` and `gbv-passes: patched-mode
  SetDebugParameter calls` -- the evidence that both halves reached the runtime;
- implies `--gbv` (the substring), Debug only, and **ignored by `--scene-stress-gbv`** (warned):
  the gate's verdict covers the whole frame.

### Measured (Debug, wind_test, `--gbv --no-streamline --shot-delay=3`, same binary)

Alternated, because consecutive GBV runs warm a patched-shader cache. This table predates the rest
mode and ran with the rest at NONE:

| run | first frame presented | process wall | CPU record, top pass |
|---|---|---|---|
| full GBV #1 | 167.3 s | 171.9 s | Lighting 152.1 s (another engine process was running at start) |
| `--gbv-passes=CloudTrace` #1 | 16.1 s | 21.3 s | CloudTrace 9.4 s, next Tonemap 0.09 s |
| full GBV #2 | **154.8 s** | 159.8 s | Lighting 144.4 s, VolumetricFog 60.6 s, CloudTrace 14.7 s |
| `--gbv-passes=CloudTrace` #2 | **15.6 s** | 21.3 s | CloudTrace 9.0 s |

**9.9x on the second runs.** What is left is the listed pass's own rewrite (CloudTrace ~9 s) plus
the Debug boot (~6.5 s). `--gbv-passes=Compose,GBuffer.OpaqueSimple` (with its group): 14.4 s.

### The rest of the frame: NONE or STATE_TRACKING_ONLY (A/B in one binary)

`--gbv-passes=CloudTrace`, alternated, the same command line otherwise:

| `--gbv-rest=` | first frame | id=1044 raised on |
|---|---|---|
| none #1 | 16.1 s | `'SkyAtmosphereLuts'` -- the first unpatched list |
| state #1 | 17.0 s | `'GBuffer.Indirect'` -- its ExecuteIndirect, exactly as in a full run |
| none #2 | 16.2 s | `'SkyAtmosphereLuts'` |
| state #2 | 19.6 s | `'GBuffer.Indirect'` |
| state #3 (the self-test run below, default) | 16.2 s | `'GBuffer.Indirect'` |

STATE costs **0 to 3.5 s** (16.2 -> 16.2-19.6 s; its spread is wider -- the light rewrite of every
shader runs on the recording threads in parallel: Lighting 7.9-10.9 s of CPU record, VolumetricFog
4.3-5.9 s, all overlapped). In exchange the narrowed run's state tracking is as exact as a full
run's: with NONE the runtime declares it "possibly imprecise" from the FIRST unpatched list on; with
STATE only the ExecuteIndirect that also trips it in a full run does. That is worth two seconds,
so **STATE is the default**; `--gbv-rest=none` is the fastest smoke.

**Caveat on every number above, and the re-measure.** Those runs were made from a git worktree that
lacked `import_staging/sandy_gravel_1k/textures/*.dds` (gitignored; the island's material points at
them), so the island's material had no textures and `GBufferBindingGuard` SKIPPED its draw -- palms
standing in water, `UNBOUND sampler table ... gbuffer.hlsl` in the log. Its shaders were therefore
never bound or patched. Re-measured with the island drawn (no load failure, no UNBOUND line):

| run | first frame |
|---|---|
| `--gbv-passes=CloudTrace` (rest STATE) | 16.3 s |
| `--gbv-passes=CloudTrace --gbv-rest=none` | 15.7 s |
| full GBV | 161.3 s |

Same picture: ~10x, STATE within a second or two of NONE. A run whose frame is missing a mesh is
measuring a different frame -- look at the `--shot` before trusting a time.

### What the narrowing gives up -- measured, not assumed

`--gbv-selftest=1@Skybox` injects the unbound-root-argument draw into the Skybox list, with the rest
at NONE and again at STATE (same result both times):

| run | injected into | GBV |
|---|---|---|
| `--gbv-passes=Skybox` | `'Skybox'` (shaders/skybox.hlsl) | **id=935** `Draw, Uninitialized root argument accessed ... Command List: 'Skybox'` |
| `--gbv-passes=CloudTrace` | `'Skybox'` (shaders/skybox.hlsl) | **silent** -- no id=935; render.validation holds only the id=1044 note below and the exit-time live-object report every run has |

So:

1. **A shader in an UNLISTED pass is not validated at all**: uninitialized root arguments,
   out-of-bounds descriptors, the state of what it reads -- everything GBV checks from inside the
   rewritten shader. STATE only TRACKS (that is the mode shown falsely clean above). A narrowed run
   that comes back clean says the listed passes are clean, and nothing about the others. That is
   the whole trade; it is why this is never a gate.
2. **With `--gbv-rest=none`, resource-state tracking gets imprecise -- the runtime says so itself**,
   once per run: `id=1044 ... Execution of Command List 'SkyAtmosphereLuts', contains a shader op
   ... recorded while using Shader Patch Mode NONE, or contains an ExecuteIndirect that changes
   VB/IB/Root bindings. Hence, all further GPU-based validation may undervalidate or produce true
   GBV errors with imprecise tracked state (labelled with 'Possibly imprecise') for resources in the
   COMMON state or Promoted-from-COMMON state`. The second clause is why the FULL run and the STATE
   default raise it too, on `GBuffer.Indirect` (its ExecuteIndirect): every frame of ours is
   "possibly imprecise" for COMMON resources from that pass on. NONE only moves the point to the
   first unpatched list. Read a listed pass's errors on COMMON / promoted resources with that label
   in mind.
3. A CL group is patched or not as one list (above), so listing one member validates -- and pays
   for -- its neighbours.

Root-signature / API-level validation by the debug layer itself is not shader patching and is
unchanged.

## The instrumentation (kept)

`sources/core/diagnostics/BootProfile.h` — writes the `[profiling]` boot-profile records in the session log (formerly `logs/boot_profile.log`).

- **Scopes** (`BOOT_SCOPE`): a nested timeline with wall and *self* time, so unattributed work is
  visible as a gap rather than invisible. Dumped at "InitScene complete", at the first frame, and
  at shutdown — a run that never finishes still leaves its boot timing.
- **Buckets** (`boot::AddBucket`): repeated work aggregated to count + total + **the slowest few by
  name**. Naming the worst item is the point; the aggregate alone would have said "shaders are slow".
- **Counters** (`boot::AddCount`).

Instrumented: device/swapchain/target creation, material presets, text + debug-draw init, level
load, per-object-type spawn, mesh and texture loads, shader cache hits and dxc compiles (with DXIL
size), PSO cache hits/rejects/creates (by shader name), per render pass CPU record, submits,
present, per frame, per stress op, InfoQueue drain, teardown.

**Per-frame instrumentation is behind `boot::g_frameProfiling`** — a mutex and a string per pass per
frame is not something to ship on. On for `--scene-stress*`, `--gbv`, and `--boot-profile`; the boot
timeline itself is always written.

## Gotchas paid for here

- **`logs/gbv.log` already existed.** The info-queue setup does `std::remove` on it "fresh per run",
  *after* device creation — so a line written at device creation was silently deleted, and a knob
  that provably worked looked like dead code. Check whether a log name is already taken.
- **A diagnostic that only writes to one file is not evidence.** The same verdict goes to the boot
  profile's counters, which is what actually proved the mode reached the device.
- **Scopes must be placed by call site, not by text match.** The first attempt matched the swapchain
  creation calls in the *resize* path instead of `InitD3D12`, and produced a timeline with a
  1151 ms hole in it.
