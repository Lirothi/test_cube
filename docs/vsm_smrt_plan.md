# VSM SMRT sampling plan (shadow-map ray marching)

**Status: NOT STARTED.** Written 2026-09-01 after the user identified the cause of a measured gap.

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

### Step 1 — single ray, no dither, behind a flag (dormant by default)
Port `SMRTFindSample` + the march loop for the CLIPMAP case only, driven by a new
`vsm::g_smrtRayCount` (0 = today's `SampleCmp` path, unchanged). One ray, `SamplesPerRay` steps,
`CompareTolerance` exactly as above. Keep the existing normal bias (it is already UE's formula, see
`vsm_sample.hlsli`). **Verify:** with the flag at 0 the image is bit-identical; at 1 it renders at all.

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

### Step 4 — take the ceiling off
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
