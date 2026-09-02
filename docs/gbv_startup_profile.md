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

### `--gbv-selftest=N`

Issues N draws that commit the unbound-root-argument violation on purpose (`Material::Bind`, Debug
only, never armed by default). It exists because a validation mode's NAME does not tell you what it
reports, and "it probably still catches it" is not something to hand someone deciding what to trust.
Re-setting the root signature is what makes it deterministic -- that invalidates every root
argument, so the skipped tables are genuinely unbound at draw time rather than left over from an
earlier draw on the same command list.

### The control that made this trustworthy

The first A/B ran guarded → unguarded → state sequentially and produced 159 / 151 / 14 s — a
monotone decrease, which is equally consistent with the runtime's patched-shader cache warming up
across runs. Running **guarded twice in a row** gave 166.3 s and 163.2 s: repeatable, no warming.
Only then was the 14 s meaningful.

The knob was also verified to actually reach the driver rather than being inferred from wall time —
the `[profiling]` boot-profile records in the session log (formerly `logs/boot_profile.log`) counters carry `QueryInterface` and `SetDebugParameter` HRESULTs, and
`logs/gbv.log` opens with the mode the run actually used.

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
