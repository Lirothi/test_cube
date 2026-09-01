# VSM SMRT sampling plan (shadow-map ray marching)

**Status: PLAN COMPLETE, Steps 1-4 + the two follow-ups (uncommitted). SMRT marches, uses many rays, produces a penumbra
that widens with distance from contact, and the clipmap depth range now ships at UE's 1000.
Default is still `vsm.smrtRayCount 0` -- the single-tap path is untouched.**
Written 2026-09-01 after the user identified the cause of a measured gap.

## Why

Our VSM clipmap is sampled with a single `SampleCmp` per level: offset the receiver along its normal,
compare one depth, done (`VsmClipmapShadow` / `VsmSampleNDC` in `shaders/vsm_sample.hlsli`). Self
shadowing is suppressed by a CONSTANT NDC depth bias (`g_clipmapDepthBias`, shaped per level by
`g_clipmapDepthBiasDecay` and a texel floor).

UE have no such constant. Their VSM sampling is **SMRT** — a ray marched through the shadow map from
the receiver toward the light (`Shaders/Private/VirtualShadowMaps/VirtualShadowMapSMRTTemplate.ush`,
`VirtualShadowMapProjectionDirectional.ush`). The only "bias" is derived per pixel from the march
itself:

```hlsl
// Add a small relative error to the comparison to avoid missing surfaces due to numeric precision
const float EpsScale = 1.05f;
const float CompareTolerance = abs(DeltaReferenceDepth) * EpsScale;
const bool bBehind = (SampleDepth - ReferenceDepth) > CompareTolerance;
...
float HalfCompareTolerance = 0.5 * CompareTolerance;
bool bHit = abs(DepthDiff + HalfCompareTolerance) < HalfCompareTolerance;
```

`DeltaReferenceDepth` is this pixel's own depth step along the ray, so the tolerance is a property of
the geometry and the march, not a tuned constant. A ray that starts at the receiver and walks toward
the light cannot mistake the receiver for its own occluder, which is what a constant bias exists to
prevent. UE also derive the depth slope from the sample history along the ray rather than from a
single receiver-plane estimate.

**This was measured, not assumed.** Removing our constant to "do it like Epic" without the march makes
things worse, because the constant is the only thing suppressing self-shadowing in a single-tap
sampler: on wind_test, `--set=vsm.clipmapDepthBias:0` moved acne from **0.0066 %** to **0.0517 %**
against a **0.0096 %** noise floor, and darkened contact shadows (shadow lift -0.653 vs -0.002).

## What it buys beyond parity

1. **The clipmap depth RANGE stops mattering.** Verified: with `clipmapDepthBias:0` the result is
   identical at `ZRangeScale` 50 and 1000 (acne 0.0517 % vs 0.0516 %). Today the range is bounded by
   the bias it rescales -- which is exactly why `g_clipmapZRangeScale` ships at 50 instead of UE's
   1000 (see `VirtualShadowMap.h`). With SMRT that ceiling is gone, and with it the class of bug
   where a tall caster's top is clipped by a level's near plane.
2. **Contact hardening and real penumbrae.** The march yields an occluder DISTANCE, which is what
   makes a shadow sharp at the contact point and soft far from it. A single comparison cannot.
3. **Slope handling without a receiver-plane estimate.** `DepthSlope` comes from the sample history,
   so surfaces nearly parallel to the light stop needing `VSM_MAX_DEPTH_SLOPE_UV` clamps.

## UE's shape, with their defaults

| cvar | default | meaning |
|---|---|---|
| `r.Shadow.Virtual.SMRT.RayCountDirectional` | 7 | rays per pixel; 0 disables SMRT |
| `r.Shadow.Virtual.SMRT.SamplesPerRayDirectional` | 8 | steps along each ray |
| `r.Shadow.Virtual.SMRT.ExtrapolateMaxSlopeDirectional` | 5.0 | max slope when extrapolating behind an occluder |
| `r.Shadow.Virtual.SMRT.TexelDitherScaleDirectional` | 2.0 | dither on the ray casts, hides resolution aliasing |
| `r.Shadow.Virtual.SMRT.AdaptiveRayCount` | on | early-out once the result is unambiguous |
| `r.Shadow.Virtual.SMRT.MaxRayAngleFromLight` | — | caps the cone rays are drawn from |

Ray setup (`VirtualShadowMapProjectionDirectional.ush`):
```hlsl
float RayLength = VirtualShadowMap.SMRTRayLengthScale * DistanceFromViewOrigin;
for (i = 0; i < MaxRayCount; i++) {
    float4 RandSample = VirtualShadowMapGetRandomSample(PixelPos, View.StateFrameIndex, i, MaxRayCount);
    float3 RayDir = GetRandomDirectionalLightRayDir(Light, RandSample.xy);
    float2 TexelOffset = (RandSample.zw - 0.5f) * DitherScale;
    ...
}
```
Note their own warning on `RayLength`: *"Too high values will cause shadows to detach from their
contact points (unless more samples are used). Too low values will greatly restrict how large
penumbras can be."* That is the knob that replaces our depth bias as the thing to tune.

## Steps (one per commit; Debug+Release 0/0; VSM `--scene-stress-gbv`; visual A/B)

### Step 1 — single ray, no dither, behind a flag (dormant by default) -- DONE

Files: `shaders/vsm_smrt.hlsli` (new), `VsmClipmapShadow` in `shaders/vsm_sample.hlsli`, the CB
mirrors in `lighting_cs.hlsl` / `glass.hlsl` / `SceneResourceBootstrapper.h` /
`SceneRenderInternal.h`, the four `vsm::g_smrt*` knobs and their `--set=` handlers.

**The one thing that did not transcribe: UE run REVERSE-Z, we run DIRECT Z.** Three depth
comparisons in the march flip (marked `FLIP` in the file). Copied verbatim they do not look
"slightly off" -- they invert the shadow.

Depth is read with `Pool.Load`, not `SampleCmp`: the march wants a depth at one texel, and Load adds
no sampler and no descriptor, so the root signature and the (positional) descriptor tables are
untouched.

**Measured** on wind_test at the user's shadow camera (`--cam-pos=15.07,5.13,69.20
--cam-rot=-0.0495,0.9505,-0.2171,-0.2167`), Release, `--dlss=off --wind-freeze`, one binary:

| arm | mean abs diff | pixels > 2/255 |
|---|---|---|
| noise floor (rayCount 0 twice) | 0.0022 | **0.008 %** |
| rayCount 0 -> 1 | 1.8913 | **9.548 %** |

| | shadow coverage |
|---|---|
| single-tap | 54.958 % |
| SMRT 1 ray | 54.403 % (**-0.56 pp**) |

newly shadowed 0.044 %, newly lit 0.599 %.

What those numbers say: the shadow lands in the SAME PLACE (coverage moves half a percentage
point), which is the structural check on the Z flips -- an inverted comparison would have moved
coverage tens of points. The small net loss is the 3x3 PCF box no longer dilating the shadow. And
the acne proxy is 0.044 % **with the constant depth bias not applied at all on the SMRT path** --
the march returns before `levelDepthBias` is computed. That is the plan's central claim, measured.

Still open by design at this step: edges are hard and aliased (one ray, no dither, no filter --
Steps 3), and the cross-level blend is skipped on the SMRT path (Step 2 replaces it with a ray that
walks into the coarser level).

**GOTCHA PAID FOR HERE.** The first cut wrote the ray loop as `for (r = 0; r < smrt.rayCount; ++r)`
with the bound coming straight from a constant buffer. A GPU loop bound taken from unvalidated CB
data is a device-hang generator: if the field ever fails to resolve, `UpdateCBField` writes nothing,
the shader reads whatever was in that memory, and an enormous uint is billions of iterations per
pixel -- with no bad pointer and nothing for the debug layer to report. "Off by default" does not
protect against it, because the garbage IS the default when the write never lands. Both loops are
now bounded by LITERALS (`VSM_SMRT_MAX_RAYS`, `VSM_SMRT_MAX_SAMPLES_PER_RAY`) and the CB value can
only cut them short; `numSteps` is also clamped from below (0 gave 1/0 -> NaN sample times). An
unresolved CB field now reports to `logs/cb_field_missing.log` instead of being silent.

### Step 1 (original text) — single ray, no dither, behind a flag (dormant by default)
Port `SMRTFindSample` + the march loop for the CLIPMAP case only, driven by a new
`vsm::g_smrtRayCount` (0 = today's `SampleCmp` path, unchanged). One ray, `SamplesPerRay` steps,
`CompareTolerance` exactly as above. Keep the existing normal bias (it is already UE's formula, see
`vsm_sample.hlsli`). **Verify:** with the flag at 0 the image is bit-identical; at 1 it renders at all.

### WHAT THE "BLOCKER" ACTUALLY WAS (resolved 2026-09-01)

**The march was never dead. My diagnosis was wrong, and only an instrument found it.**

Made the shader return `validSamples / stepsEvaluated` for one run: **99.9 % of ground pixels came
back 1.0** -- every step of every march found a page. The whole story below about samples falling
outside the level was false, and two changes were made on the strength of it before anyone checked.

The real reason `samplesPerRay` did not matter is much duller: **with ONE ray, the answer is decided
at the receiver.** A single ray through this geometry gets the same verdict at 1 step or 32, because
the occluders are large compared with the sample spacing. Sample count only starts to matter once
there are MANY rays spread over the light's disc -- which is Step 3, and which is where a penumbra
comes from in the first place. A single ray is a hard-shadow test dressed up as a march.

The lesson, paid for over three wrong hypotheses: **when a knob does nothing, instrument the shader
before theorising about why.** Reading more UE source only produced better-sounding guesses.

### STEP 3 -- MANY RAYS (done, measured)

Ported: `GetRandomDirectionalLightRayDir` (concentric disc sample over the light's angular radius),
the R2 quasirandom per-ray offsets from `VirtualShadowMapSMRTCommon.ush`, and the texel dither with
the receiver-plane bias each offset earns (`SMRTClipmapRayInitialize`). Per-pixel decorrelation is a
world-space hash rather than UE's blue noise -- worse spectrum, but stable in world space so the
dither does not crawl as the camera moves.

New knobs: `vsm.smrtSourceAngleDeg` (UE's 0.5357, the real sun's disc) and `vsm.smrtTexelDitherScale`
(UE's 2.0), both in the dev window beside the ray count.

**Measured, wind frozen, one binary, noise floor 0.008 %:**

| | shadow coverage | mid-tone (penumbra) pixels |
|---|---|---|
| single-tap PCF (reference) | 54.958 % | 63.127 % |
| SMRT 1 ray | 52.609 % | 72.753 % |
| SMRT 7 rays, sun 0.5357 deg | 52.936 % | 72.071 % |
| SMRT 7 rays, sun 3.0 deg | 53.748 % | 70.947 % |

rays 1 -> 7 moves **5.117 %** of pixels; sun angle 0.5357 -> 3.0 deg moves **10.072 %**. Coverage
stays in a tight band around the reference, which is the check that the extra softness is not just
more shadow.

**THE FOURTH DIRECT-Z FLIP, and it cost a measurement.** UE write `RayStartUVZ.z += OptimalBias` for
the dither offset because larger depth is NEARER the light for them. Transcribed literally it pushes
the ray's start AWAY from the light -- below the surface it stands on -- so every ray instantly finds
that surface as its own occluder. Measured with the sign wrong: coverage **54.96 % -> 73.70 %**, a
scene drowning in self-shadow that at a glance reads as "softer shadows". Isolated by turning the
dither off (52.89 %), which is the only reason it was caught rather than shipped.

### THE MIS-DIAGNOSIS (kept for the record)

**The march never marches.** At shipping settings `samplesPerRay` 1 and 32 produce IDENTICAL
images, and `rayLengthScale` 1.5 / 4 / 8 likewise. Every knob that should dominate does nothing,
because every sample except the one at the receiver is discarded.

Why: the ray's step in shadow UV is `rayLength / levelExtent`. With the default 12 m base extent and
a ray of 1.5x distance-to-camera, that step is ~1 UV per unit ray parameter -- so every sample above
the receiver lands OUTSIDE the level the receiver picked. The march then needs to continue in a
coarser level, and there is nothing there to continue into: `vsm_page_request_cs.hlsl` ends its
clipmap block with

```hlsl
break; // finest containing clipmap level only
```

It marks the finest containing level, plus the next coarser one inside the blend band. Nothing else.
So the SMRT level-crossing added in Step 2 searches coarser levels, finds no resident page at any of
them, and gives up -- leaving one valid sample per ray, at the receiver.

**Measured three ways, same binary, wind frozen, noise floor 0.008 % of pixels:**

| condition | samples 1 vs 8 |
|---|---|
| default (extent 12, default blend) | 0.003 % -- at the floor, march dead |
| extent 12, `clipmapBlendWidth` 0.5 (one extra coarse level, wide band) | 0.006 % -- still the floor |
| **extent 200 (the whole ray fits inside level 0)** | **0.043 % -- 5x the floor, march alive** |

The third row is the control that makes it a diagnosis rather than a guess: give the ray a level big
enough to stay inside, and samples-per-ray starts mattering immediately.

**This also reframes Step 1.** Its measured 9.5 % pixel change against the single-tap path was real,
but it was NOT the march: it was one point comparison replacing a 3x3 PCF plus a constant bias. The
visual verdict on Step 1 was given on that.

### The chain is IMPLEMENTED (2026-09-01) -- and it was not sufficient

`shaders/vsm_page_propagate_cs.hlsl`, a transcription of UE's `PropagateMappedMips` directional
branch, plus the two-bit page-table format it needs:

* bit 31 MAPPED HERE (unchanged meaning -- every existing pass still reads only this, so nothing
  that renders changed behaviour), bit 30 RESOLVABLE, bits 16..19 LOD OFFSET. UE's
  `bThisLODValidForRendering` / `bAnyLODValid` split, mapped onto the free bits we already had.
* The pass runs after allocation, rewrites every non-mapped clipmap page unconditionally (the table
  persists across frames, so a pointer from last frame may name a page that now belongs to someone
  else), and is gated on SMRT being on so it costs nothing while the feature is off.
* Level-to-level page mapping is 2D arithmetic in the FIXED light frame `Scene::UpdateClipmap`
  snaps every level in, published as `vsm::ClipmapSquares`. UE use integer corner offsets for the
  same job; ours works from the page CENTRE, which carries a half-page margin.
* The depth read still requires MAPPED HERE, deliberately: a fallback entry's physical page belongs
  to a coarser level covering 2^k the area, so the sub-page position computed from this level's
  grid would be wrong for it. The offset is used as a HINT that tells the crossing search which
  level to jump to; correctness comes from re-projecting into that level.

**Measured: it moves the result but does not fix the symptom.** SMRT shadow coverage went 54.403 %
-> 54.944 % (the single-tap reference is 54.958 %), so the chain is demonstrably doing work. But
`samplesPerRay` 1 vs 8 is still **0.008 %** -- exactly the noise floor. The march still does not
depend on how many samples it takes.

Verified not-a-regression: with SMRT off the image is coverage 54.958 %, identical to the reference,
and the pass does not dispatch at all.

**The open question.** UE's `GetMappedClipmapId` picks the ray's level with
`CalcAbsoluteClipmapLevel = log2(distance to clipmap origin)` -- read, and it is the same TIGHT
relationship ours uses, so "UE start the ray in a roomier level" is NOT the explanation. Something
else keeps their ray inside its level's UV range where ours leaves it on the first step; their
`SampleVirtualShadowMapClipmap` indexes `uint2(ShadowMapUV * VSM_LEVEL0_DIM_PAGES_XY)` with no
range check at all, which means a UE ray simply never goes out of range, and the reason it does not
is what remains to be found. The next move is an instrument, not another transcription: a debug view
that shows, per pixel, how many samples of the march were VALID. Everything else is guessing, and
two guesses have already been spent here.

### What was missing, and what UE do instead

UE never search levels in the SMRT loop. Their page table entry carries `bAnyLODValid` and
`LODOffset`, so a lookup at an unmapped fine page resolves to the nearest MAPPED coarser page at
lookup time (`SampleVirtualShadowMapClipmap`), and the sampled depth is converted back into the
starting level's range with a scale/bias pair. No extra pages are made resident; the fallback is a
property of the page TABLE.

So the dependency is a page-table change, not shader work:

* **(A) UE's LODOffset chain (recommended).** After allocation, fill each unmapped clipmap page with
  the offset to the nearest mapped coarser level. The shader's crossing then costs a table read
  instead of a search, and residency is unchanged -- no pool pressure. This is what UE ship.
* **(B) Request the whole coarser chain per pixel.** Three lines in the request shader (the local
  mip chain already does exactly this loop), but every clipmap pixel then pins up to 10 levels and
  the pool is 1024 pages total. Cheap to try, likely to over-subscribe.

Until one of them exists, Steps 3 and 4 cannot be judged: multi-ray penumbra and the ZRangeScale
ceiling both depend on a march that actually samples along its ray.

### Step 2 — correctness pass
Ray length from `DistanceFromViewOrigin`, the depth-slope-from-history extrapolation, and the
level-crossing case (a ray that walks out of the level it started in must continue in the coarser
one -- `FSMRTClipmapRayState` exists in UE precisely for this and is the part with no analogue here).
**Verify:** acne AND contact, against today's single-tap path, on wind_test with the tall caster; the
constant depth bias set to 0 for the SMRT arm. Target: acne at or below the single-tap path's
0.0066 % without the constant.

### Step 3 — multiple rays + dither + adaptive count
`RayCount` 7, texel dither 2.0, adaptive early-out. **Verify:** penumbra quality vs the single-tap
path; `Pass_Lighting` cost (this is per-pixel work in the lighting pass, so the budget question is
"how much of the frame", not "how much of VSM").

### FOLLOW-UPS (done, measured)

**Adaptive ray count** -- UE's `SMRTAdaptiveRayCount`, transcribed including the part that is easy
to miss: they divide by the rays ACTUALLY SHOT (`RayCount = min(i+1, MaxRayCount)`), not by the
maximum. Dividing by the maximum after an early break would report a fully occluded pixel as
partially lit, i.e. the optimisation would silently lighten every umbra. Guarded by
`VSM_SMRT_COMPUTE`, mirroring UE's `#if COMPUTESHADER`: `WaveActiveAllTrue` over a pixel shader's
helper lanes does not mean what the heuristic assumes, so glass always shoots the full count.

| | Pass_Lighting (GPU) |
|---|---|
| single-tap | 0.147 ms |
| SMRT 7 rays, adaptive OFF | 0.863 / 0.859 ms |
| SMRT 7 rays, adaptive ON | 0.425 / 0.425 ms |

**2.0x**, repeated twice with under 0.5 % spread, for 0.232 % of pixels changed (floor 0.007 %) and
shadow coverage 54.959 % -> 54.948 %. SMRT with it on costs +0.28 ms over the single-tap path.

**Temporal dither** -- rotates the per-pixel sample set once per frame (UE's `View.StateFrameIndex`
into their blue-noise lookup). Noise inside the shadow, DLSS ON:

| | temporal OFF | temporal ON | |
|---|---|---|---|
| 1 ray | 2.9462 | 2.5417 | **-13.7 %** |
| 7 rays | 2.4586 | 2.3947 | -2.6 % |

It is a temporal sample-count multiplier, so it helps most exactly where the spatial count is
lowest. Default ON; turn it off to judge a still frame or when running `--dlss=off`.

**What the TEXEL dither actually costs and buys** (asked because it is invisible at 7 rays -- and
it is, on a camera at the finest clipmap level):

| | |
|---|---|
| cost | Pass_Lighting 0.418 -> 0.430 ms, **+0.011 ms** (2.6 %) |
| effect, 7 rays, FINE texels | **none measurable** -- 4.965 % of pixels vs a 3.876 % run-to-run floor under DLSS; in-shadow noise 2.4589 vs 2.4617 |
| effect, COARSE texels (`clipmapBaseExtent` 8 -> 96) | **29.853 %** of pixels, floor 0.116 % |

The coarse case is the whole point and it is exactly UE's stated purpose ("hide aliasing due to
insufficient shadow resolution"): without the dither the shadow-map texel grid shows through as a
regular checkerboard staircase; with it the same shadow is smooth. Where resolution is sufficient
there is nothing to hide, which is why it is invisible on the shadow camera. Kept at UE's 2.0 --
it is insurance for distance and coarse levels at 2.6 % of one pass.

**Two transcription corrections found by re-reading the original rather than trusting the port:**

* `UniformSampleDiskConcentric` -- my first version carried a SIGNED radius, which puts every
  sample from a negative quadrant on its ANTIPODE. Still a uniform disc, but no longer the mapping
  the quasirandom sequence was stratified for. Now transcribed from `ConcentricDiskSamplingHelper`
  including the 0.99999994 rescale, the 2^-64 epsilon and the sign-bit copy.
* The dither scale was missing UE's factor of **0.5** (`0.5f / CalcLevelDimsTexels(0)`), so it ran
  at twice their amplitude -- on a one-ray march, twice the noise for nothing.

### WHAT THE USER'S SCREENSHOTS CAUGHT THAT THE METRICS DID NOT

Two artifacts, and shadow COVERAGE was blind to both -- it measures how much is shadowed, never
the SILHOUETTE. 52.9 % vs 54.9 % looked healthy while palm fronds had turned into slabs.

**1. Page-shaped rectangles.** The level acceptance margin (`acceptEdge` 0.5) pushes a receiver one
clipmap level COARSER, so the coarse level's pages show through as rectangular blocks and frond
leaflets merge into bars. It was added on the "the ray runs out of level" theory that instrumenting
the march later disproved, so it had no justification left. Now `vsm.smrtLevelMargin`, **default
1.0** (no margin) -- side-by-side, 1.0 restores the leaflets and matches the single-tap silhouette.

**2. Salt-and-pepper grain, and it is NOT the dither.** Measured local deviation from a 3x3 mean
inside the shadow:

| | noise |
|---|---|
| single-tap reference | 5.37 |
| SMRT 1 ray, sun 0.536 deg | **10.09** |
| SMRT 1 ray, sun angle 0 (no direction jitter) | 8.36 |
| SMRT 7 rays | 6.06 |
| SMRT 16 rays | **5.29** |

Two sources, both inherent to one ray: the per-pixel direction jitter is not averaged (10.09 ->
8.36 when it is removed), and the march POINT-samples one texel per step while the single-tap path
averages a 3x3 PCF. Ray count is the fix -- at 16 rays it is quieter than the reference. The dither
is a red herring here but it is NOT inert: turning it off moves 1.768 % of pixels (floor 0.008 %).
An earlier claim that dither 0 and 2.0 were "identical" came from eyeballing a crop and was wrong.

**Consequence for the UI:** the dev-window checkbox used to enable SMRT at ONE ray -- the noisiest
configuration the feature has. It now enables at 7, UE's default.

### STEP 4 -- CEILING OFF (done, measured)

`g_clipmapZRangeScale` 50 -> **1000**, which is UE's own default. It sat at 50 because the constant
NDC depth bias is a fraction of that range, so stretching the range stretched the bias and detached
shadows; SMRT derives its tolerance from its own depth step, and the single-tap arm rescales its
constant by `kClipmapRangeMultipleRef / ClipmapRangeMultiple()`, so the coupling is gone either way.

| arm | ZRange 50 -> 1000 | coverage |
|---|---|---|
| SMRT 7 rays | 0.022 % of pixels | 54.995 % both |
| shipping single-tap | 0.030 % | 54.958 % both |

Noise floor is 0.008 %, so both are within a whisker of nothing.

**The control that makes those numbers mean anything**: at `ZRangeScale 3` the range DOES bind --
coverage falls to 53.632 % and 3.197 % of pixels change, i.e. the tall caster's shadow is visibly
cut off. So the sweep crosses the feature, and 50/200/1000 are all comfortably past where it stops
mattering. The ceiling is off for free.

**The constant depth bias was NOT zeroed, and that is a correction to this plan.** With SMRT on it
is already dead code (the march returns before it is computed), but the single-tap arm still needs
it: zeroing it there moves coverage 54.958 % -> 56.922 %, and that two-point gain is ACNE. The plan
assumed SMRT would be the only path; while both ship, the constant belongs to the arm that has no
march to derive one from.

### Step 4 (original text) — take the ceiling off
With SMRT carrying the bias, set `g_clipmapDepthBias = 0` and raise `g_clipmapZRangeScale` toward
UE's 1000. **Verify:** the tall-caster truncation cannot come back at any camera; shadow lift stays
at the noise floor.

## Risks / notes
- **Cost lands in `Pass_Lighting`, not in the shadow passes.** 7 rays x 8 samples is 56 taps where we
  do 1. UE afford it because SMRT replaces both the filter and the bias; measure before assuming.
- **The single-tap path must stay** as the fallback and the A/B arm, exactly as `csm.filterMode:0`
  did for the CSM tent kernels.
- **Do not delete `g_clipmapDepthBias` until Step 4 measures**; it is load-bearing today and the
  measurement above says so.
- **Glass samples the clipmap too** (`glass.hlsl` shares `vsm_sample.hlsli`); whatever SMRT costs, it
  costs there as well.
