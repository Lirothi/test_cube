# Photographic Rendering — P0 Baseline Measurements

Reference measurements for `docs/photographic_rendering_improvement_plan.md`, step **P0**.
Captured **2026-08-15** at git `b3870af` ("wetness sim"), clean worktree.

Reproduce everything with one command from the project root:

```bash
python tools/photographic_baseline.py
```

The image being aimed at is `docs/ref/ref_wind_test.png`; it is measured against this baseline in
section 6. Cameras, modes and the capture recipe live in `docs/photographic_baseline_manifest.json`.
Captures and profiler dumps land in `logs/baseline/` (gitignored — only this report and the
manifest are checked in). `--analyze-only` re-measures existing captures without re-running the
engine.

---

## 1. Canonical views

All views are in `data/levels/wind_test.json`. `atoll.json` is deliberately excluded: its
environment is unauthored, so grading against it would calibrate the renderer to a scene nobody
intends to ship.

| Id | Framing | Why it is canonical |
|---|---|---|
| `overview` | Aerial, front-lit atoll, camera `0, 60, -200` | Plan P0 item 1 — sky, ocean, sand and foliage in one frame; the closest framing to the AI reference image. |
| `shore_grove` | Ground level in the palm grove, camera `-30.74, 4.75, 70.7` | Plan P0 item 2 — shaded foliage, beach contact, bright water. This is the level's own `freeCameraStart`. |
| `sun_glint` | Sun-facing, back-lit island over a glint field, camera `-130, 55, -110` | Third view, added with reason. P2 is judged on rejecting ocean glints through percentile clipping and P8 on bloom not turning glints into a white fog bank; neither of the other two views contains a specular glint field. |

Each view is captured in two modes: `native` (`--dlss=off`, render scale 1.0) and `dlss_balanced`
(`--dlss=balanced`, render scale ~0.58 — the compiled default).

**Capture recipe:** `--wind-freeze=10`, `--shot-delay=12`, `--no-hud`, `--shadow-mode=vsm`,
2560x1440 output. Warmup landed every capture between frame 3833 and 5654, well past the point
where the profiler's moving averages have settled.

---

## 2. Image measurements

Taken from the final SDR PNG. The renderer has no HDR readback path, and the plan names the
screenshot histogram as the accepted P0 fallback. Luma is Rec. 709 over sRGB-linearised channels;
"clipped" is any channel at 8-bit level >= 254; "dark" is luma < 0.02.

| View / mode | mean | median | p02 | p95 | p99 | clipped % | dark % |
|---|---:|---:|---:|---:|---:|---:|---:|
| overview / native | 0.1707 | 0.1131 | 0.0211 | 0.4464 | 0.5589 | 0.000 | 1.85 |
| overview / dlss_balanced | 0.1691 | 0.1132 | 0.0246 | 0.4447 | 0.5557 | 0.000 | 1.05 |
| shore_grove / native | 0.0791 | 0.0626 | 0.0095 | 0.2259 | 0.2786 | 0.000 | 22.19 |
| shore_grove / dlss_balanced | 0.0762 | 0.0620 | 0.0104 | 0.2121 | 0.2564 | 0.000 | 18.69 |
| sun_glint / native | 0.2379 | 0.1573 | 0.0157 | 0.7094 | 0.8477 | 0.044 | 2.93 |
| sun_glint / dlss_balanced | 0.2365 | 0.1602 | 0.0215 | 0.6853 | 0.8454 | 0.037 | 1.77 |

### What these numbers already say

The headline is that **the current image does not clip — it crushes.** Two of the three views clip
exactly zero pixels, and even the sun-facing glint field clips 0.04%. Meanwhile `shore_grove` puts
**22% of the frame below 2% luma** and its 95th percentile sits at 0.226, i.e. the entire shaded
grove lives in the bottom quarter of the display range.

That is the fixed Narkowicz curve plus a baked light `exposure` of 2.0 behaving as a single
hard-coded camera: there is headroom left unused at the top while the shadows collapse. It also
means the plan's P3 acceptance criterion ("shaded foliage does not collapse to black") has a
concrete target to beat — the `dark %` column — and that highlight-recovery work (P3 gamut
compression, P8 bloom) has almost nothing to recover in the current image, because nothing is
clipping yet. Expect the dark% and p95 columns, not the clipped% column, to be where M1 shows up.

Native and DLSS agree closely on every metric, so the mode is not a confound: medians match to
three decimals and p95/p99 to within 3%. The one consistent difference is at the bottom of the
range — DLSS reports 1.0–3.5 percentage points fewer dark pixels and a p02 raised by 0.001–0.006 in
every view. That is upscaler filtering lifting the darkest foliage samples, not an exposure
difference, and it is why P2's native/DLSS parity check must compare *settled exposure*, not the
shadow statistics.

---

## 3. GPU timings

`--profdump` averages, milliseconds, same captures. The rows below are the **GPU** section; the
dump also has a CPU section with identically named rows that measure recording cost instead.

| Row | overview native | overview dlss | shore native | shore dlss | glint native | glint dlss |
|---|---:|---:|---:|---:|---:|---:|
| **GPU.Frame** | 1.609 | 1.408 | 3.002 | 2.682 | 1.649 | 1.478 |
| CPU.Frame | 2.298 | 2.103 | 3.099 | 2.831 | 2.115 | 2.104 |
| Pass_Lighting | 0.058 | 0.026 | 0.086 | 0.030 | 0.067 | 0.027 |
| Pass_Compose | 0.040 | 0.016 | 0.100 | 0.034 | 0.042 | 0.016 |
| Pass_Tonemap | 0.049 | 0.296 | 0.055 | 0.355 | 0.049 | 0.299 |
| Pass_Skybox | 0.020 | 0.010 | 0.013 | 0.008 | 0.021 | 0.010 |
| Ocean.Surface | 0.306 | 0.142 | 0.096 | 0.066 | 0.270 | 0.123 |
| Pass_VsmPageRender | 0.267 | 0.263 | 1.261 | 1.214 | 0.314 | 0.311 |
| RenderObjectBatch | 0.313 | 0.145 | 0.103 | 0.072 | 0.273 | 0.126 |
| ExecuteBundles | 0.289 | 0.175 | 0.579 | 0.282 | 0.309 | 0.194 |

**Read `Pass_Tonemap` carefully.** It costs 0.049 ms native but 0.30–0.36 ms with DLSS on. That
delta is not the tone curve — the DLSS `slEvaluateFeature` call is recorded inside that scope. P3
replaces the curve and will barely move this row; do not read a tonemap regression into it without
separating the two.

The stages this plan actually adds to are cheap today: lighting, compose and the tone map together
are **0.15 ms native / 0.34 ms with DLSS** (the latter almost entirely NGX). Against a GPU frame of
1.4–3.0 ms, the plan's provisional budgets summing to ~1.9 ms would roughly double the frame if
every feature shipped enabled — which is the reason the plan now requires reporting the running
total at each milestone boundary rather than only per-feature costs.

`shore_grove` is the expensive view (GPU 3.0 ms) and it is shadow-bound: `Pass_VsmPageRender` alone
is 1.26 ms of it, from 610 palms casting into the clipmap. That is pre-existing and unrelated to
this plan, but it makes `shore_grove` the view where a new half-resolution pass (P6B GTAO) will hurt
least in relative terms and where a full-resolution one will hurt most in absolute terms.

---

## 4. Determinism and the noise floor

P0 requires that two runs of the same frozen capture differ only by known temporal jitter. Two
back-to-back `overview / native` captures at `--wind-freeze=10`:

- **0.0994%** of pixels differ (3666 of 3,686,400), max single-channel delta 56, mean absolute
  channel delta 0.0014 of 255;
- every differing pixel is below the horizon line (bounding box `y 441..1439`) — the water;
- all measured metrics are stable to 5–6 decimal places: median luma moves by 3e-6, p95/p99/p02 and
  clipped% by 0.0, dark% by 3e-5 percentage points; the largest histogram bin moves by 20 pixels
  out of 3.7 million (0.0005%).

The residual is reflection/ocean temporal state, not the lighting: `--shot` fires on a wall-clock
delay, so two runs grab different frame indices and anything driven by frame index lands
differently on the water.

**Consequence for later steps:** a "screenshot-equivalent" check in P1, P5, P6A or P7 must compare
*metrics with a tolerance*, not bytes. A defensible dormant-plumbing gate is: zero pixels differ
above the horizon, and every metric in section 2 within 1e-4. Anything larger than that is a real
image change, not jitter.

---

## 5. Environment settings at baseline

From `data/levels/wind_test.json`. These are the values P4's migration has to keep recognisable.

**Directional light** — this is the block P4 splits apart:

| Field | Value |
|---|---|
| `direction` | `-1.5, -0.7, -0.5` (sun sits toward +X/+Z, ~24 deg above the horizon) |
| `color` | `1.0, 0.752, 0.628` (warm) |
| `exposure` | **2.0** — the value that is currently acting as a camera |
| `ambient` | **0.11** — flat scalar, tinted by the sun colour (plan section 3.2) |

**Skybox:** `textures/skybox.dds`, with its own independent exposure in `skybox.hlsl`.

**Camera:** `hfovDeg 90`, `zNear 0.01`, `zFar 10000`.

**Post-processing:** the level carries **no** post-process block at all — SSR technique 1, FXAA off
and DLSS Balanced are runtime `SceneRenderSettings` defaults, not authored per level. There is
nothing to migrate here, but it also means P3's new colour-pipeline settings will be the first
post-processing state the level format has to serialise.

**Ocean atmosphere (P7 collision risk).** The ocean already owns a local horizon/fog model, which
the plan's P7 risk section calls out as the thing a global atmosphere would double up on:

| Field | Value |
|---|---|
| `fogDensity` | 0.10 |
| `horizonFogStrength` | 0.50 |
| `horizonFogDistanceScale` | 2.50 |
| `skyScatterStrength` | 0.35 |
| `sunScatterStrength` | 0.35 |
| `scatterSpread` | 0.20 |

Opaque geometry has no equivalent. In the `overview` capture this is directly visible: the water
softens toward the horizon while the island and its palms stay fully saturated and crisp at the
same distance. That mismatch is P7's actual target.

**Wind** (frozen for all captures): `strength 0.61`, `directionDeg 90`, `swayFrequency 1.2`,
`foliageSwayMeters 1.1`, gust amplitude 0.2 @ 0.1 Hz, seed 3. Ocean `windForce 0.60`.

---

## 6. The reference target

`docs/ref/ref_wind_test.png` is the image this plan is aiming at. It is framed almost exactly like
the `overview` canonical view — aerial, atoll with a lagoon, ocean to the horizon — so the two
compare directly. Measured with the same tooling:

| Metric | ours (`overview/native`) | reference | Reading |
|---|---:|---:|---|
| lumaMean | 0.1707 | 0.2679 | |
| lumaMedian | 0.1131 | 0.1964 | reference 1.74x = **0.80 stops** brighter |
| lumaP02 | 0.0211 | 0.0152 | **ours is higher — our blacks are lifted** |
| lumaP95 | 0.4464 | 0.6930 | |
| lumaP99 | 0.5589 | 0.8137 | reference uses the top of the range; we do not |
| clipped % | 0.000 | 0.125 | a real camera clips a little, deliberately |
| below 2% luma | 1.85 | 3.42 | reference has **more** deep shadow, not less |

Histogram, percentage of pixels per 16 bins, dark to bright:

```text
ours   0.0  1.8  4.2 15.3 15.6 14.3  9.4  8.1  8.3  7.1  9.4  4.7  1.3  0.2  0.1  0.0
ref    0.2  2.0  6.0  7.4  7.3  9.2 13.6  9.2  8.0  7.2  7.0  7.6  6.7  5.9  2.3  0.3
```

We pile 45% of the frame into bins 3-5 and fall off a cliff above bin 11 — the top third of the
display range is essentially empty. The reference spreads across bins 6-14 and still carries content
at bin 14.

**The conclusion this forces:** the defect is low contrast in *both* directions simultaneously —
compressed highlights *and* milky blacks. A global brightness lift would make it worse, because our
p02 is already above the reference's. That is exactly the shape of a fixed tone curve doing the job
of a camera, and it is what M1 (P2 exposure, P3 tone curve) has to fix. Treat the table as a
directional target, not a goal to hit: the reference is 2.22:1 with proportionally less sky than our
16:9 frame, and non-goal 1 rules out pixel matching.

---

## 7. Tooling added for this step

P0 permits touching capture/CLI code where existing support is insufficient. Two flags were
missing and both are required by the plan's own verification matrix:

- **`--dlss=<off|perf|balanced|quality|ultraperf|ultraquality|dlaa>`** — the mode was only
  reachable through F-keys or the dev window, so the native/DLSS pair that every step is judged on
  could not be captured headlessly at all.
- **`--no-hud`** — the FPS/MS readout is composited into the backbuffer that `--shot` reads back,
  so it changes between two runs of the same frozen frame. Without this, every downstream
  "no intentional image delta" check would be diffing the frame counter.

Neither changes rendering when unused: `--dlss` leaves the compiled default alone unless passed,
and the HUD is only suppressed on request.
