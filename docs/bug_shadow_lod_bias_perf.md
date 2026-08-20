# RESOLVED — the shadow LOD bias slider does not give its performance back

**Status: root cause found and fixed (2026-08-20).** The slider was innocent — ANY mid-session
`Scene::RebuildShadowCasters` (bias change, editor spawn/delete) snapped the caster bounds from an
under-padded load-time state to their wind-padded size, permanently. The "leak" was the load-time
build never applying the W5 sway pad that every mid-session rebuild applied.

**Resolution (user decision, same day): the W5 sway pad is REMOVED, not made consistent.** The
user had lived on the unpadded state for the entire W5+ era (the bug meant every fresh boot ran
unpadded) and never observed the page-edge popping the pad guards against — while the pad's
measured price was a permanent 2.5x on `Pass_VsmPageRender` (+1.4 ms GPU on wind_test). Cull
bounds now use the static world AABB verbatim, everywhere, so both builds are identical-and-fast
and the ambient-global dependency that caused the hysteresis no longer exists. Post-removal:
fresh boot ≈ mid-session rebuild ≈ **0.92 ms** (A/B ×2 interleaved: 0.922 / 0.922 / 0.928 /
0.933; `Pass_Compose` control flat), HUD round trip 2.79 → 2.78 ms, screenshots identical.

## Root cause

`ShadowGpuData::FillBounds` pads a swaying caster's AABB by
`windStrength * vfx::g_maxSwayExtentMeters` (W5: so the per-page / Rung-0 cull never clips a
leaning frond → shadow popping at page edges). That global is published by **`Scene::Tick`** each
frame — and by nothing else.

* **Level-load build** (`FinalizeLevelLoad` → `Rebuild`) ran **before the first Tick**:
  `g_maxSwayExtentMeters == 0` → every palm baked a TIGHT, unpadded AABB. (Worse, the then-current
  `WindState::MaxSwayExtentMeters()` read the Tick-**derived** `swayAmplitude` field, so even
  calling it at load returned 0 despite the authored params being loaded.)
* **Any mid-session rebuild** ran after thousands of Ticks: full extent (wind_test:
  `1.1m sway × 0.61 strength × 1.2 gust ceil × 1.25 grove ceil × 3.12 profile ≈ 3.14 m`) → every
  swaying caster's half-extents padded by ~3.1 m **per axis**.

Fatter bounds ⇒ each caster scatters into more clipmap pages (finest level page ≈ 0.75 m!) ⇒ more
(page, caster) pairs ⇒ more instances rasterized per page. That is the whole regression:
`VsmPageRender.Scatter` +26% (more appends), the draw +~1.4 ms (more pairs × triangles),
`VsmPageRender.Setup` unmoved (resident pages unchanged — see below).

## Why every earlier probe (correctly) found nothing

* Resident page set: **flat across the round trip** — measured via the new `logs/vsm_pages.log`
  (see below): resident 362.4 → 362.3 → 362.5 avg, request flat, per-clipmap-level distribution
  identical. §4's "pool settles into a larger residency" suspicion is disproved; the request/alloc
  path never sees bounds.
* Mega layout byte-identical, `MegaReady()==1`, LOD tables pure functions of the bias — all true.
  The ONE per-rebuild input nobody compared was the **bounds buffer contents**.
* The image stayed correct because the pad is cull conservatism: extra pairs rasterize into (or
  clip out of) the same pages the tight build already drew.

## The fix, in two stages

**Stage 1 (consistency — superseded the same day):** publish the sway extent before the load-time
build, so boot bakes the same padded bounds a rebuild does. Verified: A/B converged at ~2.34 ms —
the hysteresis was gone, but boot now paid the pad's full +1.4 ms.

**Stage 2 (the shipped resolution):** with the tradeoff finally visible, the user chose the other
side — the padded configuration had effectively never been the one in use, and its artifact had
never been observed. The pad mechanism was deleted outright (per the "delete, don't disable, a
control the engine is not heading toward" rule):

* `ShadowGpuData::FillBounds` — pad block + `windStrength` parameter removed; bounds are the
  static world AABB verbatim. The rationale comment lives HERE — it is where a future reader
  would reintroduce the pad.
* `shaders/shadow_gi_scatter_cs.hlsl` + its `ScatterCB` CPU mirror — `gSwayPad` field and the
  extent grow removed (CB repacked identically on both sides).
* `vfx::g_maxSwayExtentMeters` + `WindState::MaxSwayExtentMeters()` + the publishes in
  `Scene::Tick` / `EnvironmentRuntime` — deleted; `wind.hlsli`'s `kWindBendPeak` mirror constant
  (only ever a documentation mirror for that function) deleted with it.

The wind sway itself (gbuffer + shadow VS displacement) is untouched — only the CULL bounds no
longer reserve room for it.

**Residual risk, accepted deliberately:** a frond displaced beyond its static AABB can in theory
be culled from a page/view whose area it leans into (shadow truncation at a page edge, worst near
the camera where finest-level pages are ~0.75 m). Months of unpadded use never surfaced it. If it
is ever seen: reintroduce the pad DIRECTIONALLY (downwind XZ + small Y — not the old ~4.7× all-axis
worst case), bake it identically at load and rebuild, and re-measure; the old formula is in git at
this commit's parent.

## Post-removal measurements (interleaved A/B ×2, medians, first quarter dropped)

A = fresh boot at bias 1; B = boot at 0 switched to 1 by `--set` (one mid-session rebuild):

| GPU scope | A1 | B1 | A2 | B2 |
|---|---|---|---|---|
| `Pass_VsmPageRender` | 0.922 | 0.922 | 0.928 | 0.933 |
| `VsmPageRender.Scatter` | 0.096 | 0.098 | 0.097 | 0.097 |
| `VsmPageRender.Setup` | 0.010 | 0.010 | 0.009 | 0.010 |
| `Pass_Compose` (control) | 0.030 | 0.030 | 0.031 | 0.030 |

Pre-fix the same A/B read 0.916 vs 2.34 (the bug); stage 1 read 2.34 vs 2.34. HUD round trip
(`--sweep=vsm.shadowLodBias:1,0,1,1`, one process): 2.79 → 2.78 ms. Screenshots identical to the
original fresh-boot state, GI shadows intact (the edited scatter shader's PSO builds and runs).

## Tooling added by this investigation (kept)

* `--sweep=vsm.shadowLodBias:<v0>,<v1>,...` / `--set=vsm.shadowLodBias:<v>` — the slider,
  headless (`ApplySweepValue`, `sources/app/App.cpp`).
* `--set=vsm.logPageStats:1` — mirrors `vsm::g_logPageStats`; now ALSO writes
  `logs/vsm_pages.log`, one line per readback sample: frame, bias, requested (+ per mip level),
  resident/new/fail counters, and the resident breakdown per view class from the physOwner
  snapshot (`local=` + `clip=[L0..L7]`). `VirtualShadowMap::PollPageRequestDebug`.

### Repro recipes (both still work)

One-process HUD round trip:

```bash
x64/Release/test_cube.exe --level=data/levels/wind_test.json --wind-freeze --cam-pos=-59.50,4.25,52.73 --cam-rot=-0.0392,0.9604,-0.1793,-0.2098 --sweep=vsm.shadowLodBias:1,0,1,1 --set=vsm.logPageStats:1 --shot=<dir>/rt.png --shot-delay=12 --shot-interval=6
```

Per-pass A/B (no `--shot` — it exits before the trace lands; interleave runs, 15 s cooldowns,
report `Pass_Compose` alongside):

```bash
x64/Release/test_cube.exe --level=data/levels/wind_test.json --wind-freeze --cam-pos=-59.50,4.25,52.73 --cam-rot=-0.0392,0.9604,-0.1793,-0.2098 --vsm-lodbias=1 --trace=300 --shot-delay=12
x64/Release/test_cube.exe --level=data/levels/wind_test.json --wind-freeze --cam-pos=-59.50,4.25,52.73 --cam-rot=-0.0392,0.9604,-0.1793,-0.2098 --vsm-lodbias=0 --set=vsm.shadowLodBias:1 --trace=300 --shot-delay=12
```

## Lessons

* **A hysteresis A/B has no privileged baseline.** Fresh-boot-cheap vs after-rebuild-expensive
  does not say which side is broken; here the cheap side violated the design — and once the
  design's measured price (+1.4 ms) was on the table next to its never-observed benefit, the
  DESIGN was what got fixed. Surface the tradeoff to the owner; don't silently pick a side.
* When every CPU-side input is proven byte-identical, enumerate what the rebuild snapshots from
  **ambient global state** — the bounds pad came in through a global a different system publishes
  on a different schedule.
* The scatter sub-scope moving +26% while setup stayed flat was the discriminating datum: pairs
  grew while resident pages did not → the only remaining input was bounds.
