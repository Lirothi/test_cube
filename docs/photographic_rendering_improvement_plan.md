# Photographic Lighting and Image Pipeline — Implementation Plan (for AI executors)

This document turns the current image-quality direction into a sequence of independently landable tasks. An AI executor should be able to select one named step, implement only that step against the contracts below, verify it, and stop without leaving the renderer in a half-migrated state.

The visual reference is **`docs/ref/ref_wind_test.png`** — a bright tropical aerial scene framed almost exactly like the `overview` canonical view, which makes the two directly comparable. It is AI-generated, so the target is not pixel matching. The target is the same perceptual hierarchy: retained sky and sand highlights, readable shaded foliage, neutral daylight, believable sky fill, atmospheric depth, grounded contact, and stable water response.

Measured against the P0 baseline with the same tooling (`docs/photographic_baseline_measurements.md`, section 7), the gap is **not** simply "the reference is brighter":

| | ours (P0) | reference | |
|---|---:|---:|---|
| median luma | 0.113 | 0.196 | reference is 0.80 stops brighter |
| p95 / p99 | 0.446 / 0.559 | 0.693 / 0.814 | reference actually uses the top of the range |
| p02 | 0.0211 | 0.0152 | **our blacks are lifted, not the reference's** |
| clipped % | 0.000 | 0.125 | a real camera clips a little, on purpose |
| below 2% luma | 1.85% | 3.42% | the reference has *more* deep shadow |

So the defect is **low contrast in both directions at once**: our highlights are compressed into the mid range while our blacks are milky. Brightening the image globally would make it worse. The fix is the exposure and tone-curve work in M1 (P2, P3), and the numbers above are the directional target for it — directional, because the reference is 2.22:1 with proportionally less sky than our 16:9 frame, and because non-goal 1 still stands.

Last renderer audit: **2026-08-15**.

---

## 1. Goal

Move the renderer from independently tuned light/sky multipliers and a fixed filmic curve to a coherent photographic pipeline:

- camera exposure adapts to scene luminance without pumping on the ocean;
- sun, sky, camera exposure, and tone mapping have separate responsibilities;
- diffuse and specular environment lighting agree;
- indirect lighting is spatially grounded by AO and, later, diffuse GI;
- distance is communicated by aerial perspective rather than by flat color grading;
- highlights bloom subtly without hiding material detail;
- the result remains stable both with DLSS and at native resolution.

The first useful milestone is not “add every effect.” It is **M1: stable photographic exposure and tone response**. Each later milestone must improve the image without compensating for a broken earlier stage.

---

## 2. Non-goals

- Do not reproduce an AI reference image pixel-for-pixel.
- Do not introduce a path tracer or require hardware ray tracing for the shipping raster path.
- Do not build volumetric clouds in this plan.
- Do not replace the existing ocean, foliage, shadow, or reflection systems wholesale.
- Do not retune every level while the lighting pipeline is still changing.
- Do not hide exposure problems with a level-specific LUT.
- Do not feed the new camera exposure into DLSS until a dedicated A/B test proves that it does not reintroduce water smearing.
- Do not combine unrelated material-content work with renderer infrastructure commits.
- **Do not calibrate or gate this plan on night or overcast conditions.** The project has exactly one skybox and no time-of-day system, so neither condition exists as authored content: the dark test levels (`d_emissive_test`, `e1_particles_test`) still render the daylight sky and are box-on-a-plane scenes, not tropical ones. The whole plan is tuned and judged on the **sunset** condition in `wind_test.json`. Multi-condition exposure behaviour stays a documented design constraint (section 4, decision 5a) rather than a milestone, and becomes real work only once a night sky asset exists.

---

## 3. Current-state diagnosis

The renderer already has useful foundations: HDR lighting, deferred G-buffer passes, motion vectors, DLSS, skybox reflections, material AO storage, two-sided foliage shading, multiple shadow paths, and a compute tonemap. The missing pieces are mostly coherence between those systems.

### 3.1 Exposure is currently baked into lighting

- `shaders/lighting_cs.hlsl` multiplies the final directional-light result by the directional light `exposure` value.
- Local lights (spot/point) are accumulated **without** that multiplier (documented in `shaders/rt_reflections_cs.hlsl`), so the current `exposure` is not a consistent camera analogue even across light types.
- The skybox has its own independent exposure, and the editor material preview (`shaders/editor_preview.hlsl`) a third one.
- `shaders/tonemap_cs.hlsl` applies a fixed Narkowicz-style ACES fit and a `pow(..., 1 / 2.2)` output transform.
- There is no scene-luminance metering, persistent camera exposure, or separate exposure compensation.

This makes the light control behave partly like sun intensity and partly like camera exposure. It also makes bright exterior scenes difficult to balance: lowering it dims illumination instead of closing the virtual camera.

### 3.2 Ambient light is flat and sun-tinted

The directional-light path currently forms ambient diffuse from albedo and a scalar ambient intensity, then shares the directional light color/multiplier. It is not a directional sky irradiance term and cannot naturally provide blue sky fill, warm ground bounce, or orientation-dependent foliage fill.

### 3.3 Environment specular is an approximation

`shaders/compose_cs.hlsl` samples a skybox mip from roughness and applies a Fresnel approximation. It does not yet use a prefiltered GGX environment cubemap plus a BRDF integration LUT. Diffuse irradiance is also missing from the same environment representation.

The implementation source of truth for this work already exists in:

- `docs/two_sided_foliage_and_ibl_plan.md`, steps **F7-F9**.

This plan depends on those steps and must not create a competing IBL implementation.

### 3.4 Material AO is stored but not consistently consumed

`GBAux.r` carries material AO, but the main directional ambient path does not yet use it as a complete indirect-lighting contract. There is no screen-space GTAO pass for contact grounding and large-scale creases.

### 3.5 Atmosphere is local rather than global

The ocean contains horizon/fog logic, but opaque world geometry has no unified aerial-perspective pass driven by camera distance, height, sun direction, and atmosphere parameters. At long distance the scene therefore stays graphically crisp instead of gaining believable depth.

### 3.6 DLSS exposure is a known constraint

`sources/rendering/core/DlssHandler.cpp` documents an empirical result: the current preset-F water fix depends on NGX auto-exposure, while a manually tagged exposure of `1` brings motion smearing back. Therefore the new engine camera exposure is initially a **post-DLSS display transform**, not a value supplied to NGX.

### 3.7 Missing finishing stages

There is no exposure-aware bloom/glare stage, no physically meaningful display transform, and no global diffuse GI. These are later stages, not substitutes for a correct exposure/IBL foundation.

---

## 4. Locked architectural decisions

These decisions remain in force unless a step explicitly records new measured evidence and updates this document.

1. **HDR scene-referred lighting stays unexposed until the display pipeline.** Camera exposure is not baked into individual light passes.
2. **Engine camera exposure is initially applied after DLSS and before bloom/tonemap as defined by the render-graph contract below.** NGX keeps its current internal auto-exposure behavior.
3. **Sun intensity, sky intensity, and camera exposure are separate settings.** Changing one must not silently rewrite the others.
4. **Automatic exposure uses a log-luminance histogram, clipped percentiles, and temporal adaptation.** A single average luminance is too sensitive to the sun glint and sky. The histogram build and the exposure solve run **entirely on the GPU** into a small persistent resource consumed the same frame or with at most one frame of latency; no CPU readback or synchronization stall may sit on the frame path. Any CPU-visible copy is debug-only and asynchronous.
5. **Exposure adaptation state is persistent and resettable.** Resize, camera cut, level load, and large teleports must not spend seconds adapting from stale history.
5a. **Percentile metering alone cannot preserve the relative brightness of different lighting conditions.** Percentiles are scale-invariant by construction, so a correctly metered night and a correctly metered noon land on the same target and the camera flattens both into the same mid-grey. The mechanism that fixes it is an **exposure compensation curve** -- compensation as a function of the metered scene EV rather than a single scalar; clamps are a safety net, not a policy. This is recorded as a design constraint so P2 does not bake the flattening behaviour in as if it were correct. It is **not** an acceptance criterion in this plan: see the scope note in section 2.
6. **IBL implementation comes from F7-F9 of `two_sided_foliage_and_ibl_plan.md`.** This plan only defines integration and acceptance criteria.
7. **AO modulates indirect illumination only.** It must not blacken direct sun or emissive surfaces.
8. **The first atmosphere implementation is analytic and inexpensive.** Volumetric froxels are a separate future project.
9. **Bloom is subtle, HDR, and exposure-aware.** It cannot be used to mask clipping or a poor tone curve.
10. **Every visual step has an off switch or legacy fallback until final sign-off.**
11. **Every step is independently buildable and revertible.** Dormant plumbing must be screenshot-equivalent by default.

---

## 5. Executor conventions

### 5.1 Scope and repository safety

- Read the complete selected step and all of its dependencies before editing.
- Do not start P0 while large unrelated renderer work sits uncommitted in the worktree (at the time of writing: the ocean wetness/surf tuning set). Ask the user to commit or shelve it first; landing this plan on top of an uncommitted ocean state destroys regression attribution (see the feature pile-up risk in section 11).
- Preserve unrelated user changes in the dirty worktree.
- One logical step per change set. Do not opportunistically implement the next step.
- Do not commit unless the user explicitly requests a commit.
- C++, HLSL, and Visual Studio project files use CRLF in this repository. This Markdown document uses LF; preserve the existing style of any other touched text file.
- Verify every touched text file for mixed line endings before handoff.

### 5.2 Deterministic visual testing

- Reproduce reported views from the HUD `Cam:` position and `rot:` quaternion. Do not guess camera angles.
- Use `--cam-pos=x,y,z --cam-rot=x,y,z,w` after the chosen level.
- Use `--wind-freeze=<seconds>` for image comparisons involving water or foliage.
- Capture the same scene in at least:
  - native resolution / DLSS off;
  - the project’s normal DLSS quality mode;
  - one bright sky-dominant view;
  - one shaded foliage/shore view.
- Warm temporal systems for a fixed number of frames before the capture.
- For adaptation tests, record a short sequence rather than comparing unrelated still frames.

### 5.3 Build and shader verification

- Build `Debug|x64` after every source change.
- Build `Release_Editor|x64` at milestone boundaries.
- HLSL is compiled at runtime; a successful C++ build is not sufficient.
- Run the application long enough to exercise every new permutation and check the log for shader errors.
- For dormant resource/plumbing steps, compare a frozen before/after screenshot and require no intentional image delta.

### 5.4 Performance reporting

Record GPU pass timings before and after each active step on the same camera, resolution, DLSS mode, and frozen simulation time. The budgets below are provisional guardrails on the current development GPU, not cross-hardware guarantees:

| Feature | Provisional incremental budget |
|---|---:|
| Histogram + exposure solve | 0.15 ms |
| Tone/color pipeline delta | 0.10 ms |
| Split-sum IBL integration | 0.20 ms |
| Half-resolution GTAO + denoise | 0.70 ms |
| Analytic aerial perspective | 0.30 ms |
| Bloom downsample/upsample | 0.45 ms |

If a step exceeds its budget, report the measured cost and stop before silently reducing quality or merging unrelated optimizations.

The budgets above sum to roughly **+1.9 ms** of new GPU work. On current light frames (about 2 ms GPU on wind_test-class scenes after the VSM optimizations) enabling the full stack would nearly double the GPU frame. The cumulative cost is therefore an explicit user decision, not an implicit consequence: report the running total at every milestone boundary and treat the sum — not only the per-feature numbers — as a gate for what ships enabled by default.

---

## 6. Shared interface contracts

### 6.1 Photographic settings

Create one serializable settings block owned by the renderer/level environment rather than by an individual light:

```cpp
struct CameraExposureSettings
{
    bool enabled = false;
    bool autoExposure = true;
    float compensationEv = 0.0f;
    float minEv100 = -6.0f;
    float maxEv100 = 16.0f;
    float lowPercentile = 0.02f;
    float highPercentile = 0.95f;
    float speedUp = 3.0f;
    float speedDown = 1.0f;
    float manualEv100 = 10.0f;
};
```

Exact storage location may follow existing render/environment settings conventions, but the JSON and editor names must describe camera exposure, not directional-light exposure.

Later steps may add:

```cpp
struct ColorPipelineSettings;
struct AtmosphereSettings;
struct AmbientOcclusionSettings;
struct BloomSettings;
```

Each block must have conservative defaults and must serialize independently.

### 6.2 Exposure value convention

- Persistent value: `EV100` or log2 exposure, with its meaning documented once.
- Shader value: linear pre-exposure multiplier derived from the persistent value.
- Debug UI shows both the adapted EV and the linear multiplier.
- Clamp only at the metering/settings boundary; do not scatter hidden clamps through shaders.
- Delta time used for adaptation must be capped so a debugger pause does not cause an instant jump.

### 6.3 Render-order contract

Initial ordering:

```text
Opaque/lighting/transparency in scene-referred HDR
    -> DLSS or native resolve
    -> engine camera exposure
    -> HDR bloom extraction and reconstruction
    -> tone map + gamut handling + sRGB output
    -> UI
```

Histogram metering reads a stable pre-tonemap HDR source. It may read the internal-resolution scene before DLSS for cost, but its metering result must not change materially when toggling DLSS. If it does, meter from a fixed-resolution downsample instead.

Do not tag the engine exposure texture as `sl::kBufferTypeExposure` in the initial implementation.

### 6.4 Reset contract

Exposure and all later temporal histories reset on:

- level load/unload;
- camera cut or explicit camera-history reset;
- large camera teleport;
- display/internal-resolution reallocation where history coordinates change;
- feature toggle from disabled to enabled;
- invalid/NaN luminance input.

The first frame after reset uses the current metered target, not an arbitrary black or white default.

### 6.5 Debug contract

The developer UI must eventually expose:

- current and target EV;
- linear exposure multiplier;
- metered low/high percentile luminance;
- exposure histogram visualization or bins summary;
- pre/post exposure and pre/post tone-map texture views;
- toggles for legacy tone map, AO, atmosphere, bloom, and IBL fallback;
- per-pass GPU timings.

Debug views must not mutate serialized level settings.

---

## 7. Dependency graph and milestones

```text
P0 Baseline and diagnostics
 |
 +--> P1 Exposure resources and settings (dormant)
       |
       +--> P2 Histogram metering and eye adaptation
             |
             +--> P2B Metering refinements (weight mask, hybrid adaptation) [UE ref]
             |
             +--> P3 Tone map, color transform, and output encoding
                   |
                   +--> P3B Local exposure (the "HDR photo" look) [UE ref]
                   |
                   +--> P4 Separate camera, sun, and sky controls
                         |
                         +--> P5 Environment lighting integration [F7 + F8]
                         |     |
                         |     +--> P6A Material AO contract [F9]
                         |             |
                         |             +--> P6B Dynamic GTAO
                         |
                         +--> P7 Global aerial perspective
                         |
                         +--> P8 Exposure-aware bloom
                         |
                         +--> P9 Diffuse GI / ground-bounce progression
                         |
                         +--> P10 Contact and cloud-shadow polish
                               |
                               +--> P11 Scene calibration and final sign-off
```

Milestones:

- **M0 — Measurable baseline:** P0.
- **M1 — Photographic camera:** P1-P3.
- **M2 — Coherent daylight:** P4-P5.
- **M3 — Spatial grounding and depth:** P6A-P7.
- **M4 — Highlight and indirect-light polish:** P8-P10.
- **M5 — Authored reference quality:** P11.

P6B, P7, and P8 may be developed in parallel after M2, but each must be evaluated against the same M2 baseline.

---

## 8. Step template

Every executor handoff for a completed step must contain:

```text
Step:
Files touched:
Behavioral change:
Default/fallback behavior:
Build result:
Runtime shader result:
Visual captures:
GPU timing before/after:
Known limitations:
Line-ending check:
```

---

## 9. Implementation steps

### P0 — Establish a deterministic image-quality baseline

**STATUS: DONE 2026-08-15** (uncommitted). Manifest `docs/photographic_baseline_manifest.json`,
driver `tools/photographic_baseline.py`, results `docs/photographic_baseline_measurements.md`,
captures in `logs/baseline/` (gitignored). Three canonical views, all in `wind_test.json`
(`atoll.json` excluded — unauthored environment), each captured native + DLSS Balanced. Added
`--dlss=<mode>` and `--no-hud`, without which the plan's own native/DLSS matrix and every
downstream screenshot-equivalence check were not capturable headlessly.

Two findings that change how later steps must be read:
- **The current image does not clip, it crushes.** Two of three views clip exactly 0.000% of
  pixels; `shore_grove` puts 22% of the frame below 2% luma with p95 at 0.226. M1 will show up in
  the dark%/p95 columns, not in clipped%.
- **`Pass_Tonemap` is mostly DLSS.** 0.049 ms native vs 0.30-0.36 ms with DLSS on, because
  `slEvaluateFeature` is recorded inside that scope. P3 replaces the curve and will barely move
  this row; do not read a regression into it.

Measured noise floor for "screenshot-equivalent" gates: two runs of the same frozen frame differ in
0.099% of pixels, all below the horizon (ocean temporal state), with every metric stable to 5-6
decimals. **Dormant-plumbing steps must therefore gate on metrics with a tolerance, not on bytes:**
zero differing pixels above the horizon, and every section-2 metric within 1e-4.

**Depends on:** nothing.

**Goal:** make every later visual claim reproducible and measurable.

**Touch:** diagnostics/capture code only if existing command-line and profiler support is insufficient; otherwise add documentation and checked-in capture metadata only.

**Implement:**

1. Select two canonical levels/views:
   - tropical exterior overview with sky, ocean, sand, and foliage;
   - low/medium camera view with shaded foliage, beach contact, and bright reflection.
2. Store the level, exact camera position, exact quaternion, output resolution, DLSS mode, and `--wind-freeze` value in a small manifest under `docs/` or the existing test-data convention.
3. Capture native and DLSS variants from identical poses.
4. Record existing GPU timings for lighting, compose/reflections, DLSS, and tonemap.
5. Record reference measurements:
   - median and high-percentile HDR luminance if a readback path already exists;
   - clipped-pixel ratio after current tonemap;
   - screenshot histogram as a fallback.
6. Document the current level settings for directional light, skybox, and post-processing.

**Interface contract:** later steps reuse the same manifest. Updating a canonical camera requires an explicit reason in the handoff.

**Done when:** an executor can reproduce all baseline frames without manually steering the camera.

**Verify:** run each canonical capture twice with the same frozen time; residual differences must be limited to known temporal jitter.

---

### P1 — Add dormant exposure resources and serialized settings

**STATUS: DONE 2026-08-15** (uncommitted), except for one gate blocked by a pre-existing bug
(see the end of this section).

Done — implement items 1, 4, 5, 6:
- `render::CameraExposureSettings` + EV100 conventions in `sources/rendering/core/PhotographicSettings.h`,
  JSON round-trip in `PhotographicSettingsJson.h` (both in the vcxproj and .filters).
- `Scene` owns one instance, mirroring `dirLight_`.
- Level section `"cameraExposure"`: parsed in `JsonLevel.cpp`, a document singleton in
  `EditorSceneDocument`, written by `LevelDocumentSerializer`, applied live by `EnvironmentRuntime`
  (and reset to defaults on remove), an inspector drawer in `InspectorPanel`, and a
  Create > Camera Exposure menu entry so a level can gain the section at all.
- Read-only readout in the dev window's Render tab (both EV and the linear multiplier, per section
  6.2); it never writes back, per the section 6.5 rule that debug views must not mutate level data.

**Gated:** Debug + Release build clean. Screenshot equivalence against the P0 `overview/native`
baseline, using the P0-defined tolerance, in **both** directions -- a level without the section and
a level carrying one with deliberately malformed values (inverted EV range, inverted percentiles,
negative speed, to exercise the boundary clamps): worst metric delta 5.4e-05 and 2.7e-05 against a
1e-4 gate, and **zero** channel difference above the horizon in both. Residual is the P0 ocean
jitter only.

Done — implement items 2 and 3: `ExposureMetering` (`sources/rendering/core/ExposureMetering.{h,cpp}`,
both in the vcxproj and .filters), owned by `Renderer`, holding a 256-bin log-luminance histogram
and a 16-byte adapted-exposure record. Both are **raw** (byte-address) UAV buffers, not structured:
`InterlockedAdd` needs raw or structured, and `ClearUnorderedAccessViewUint` — which is how P2 will
clear the bins every frame — rejects structured buffers outright. Both are declared through
`GpuResource::Attach` at `UNORDERED_ACCESS`, which is also their canonical resting state, so P2 adds
no transition at either end. Nothing is dispatched, bound or transitioned yet.

Lifecycle: created in `InitD3D12` unconditionally (~1 KB total — gating them on `enabled` would
mean the dormant default exercises no lifecycle, which is the one thing this step exists to prove);
released in `Shutdown` before `canonicalStates_.Clear()` so they undeclare themselves rather than
being swept; the member is declared after `canonicalStates_` so reverse-order destruction still
undeclares against a live registry if `Shutdown` never ran. The resources are resolution-independent,
so `OnResize` deliberately does **not** recreate them and only calls `RequestReset()`; `JsonLevel::Load`
does the same per section 6.4, so a level switch cannot spend seconds adapting from the previous
level's brightness.

**Gated:** Debug + Release build clean. Screenshot equivalence against the P0 baseline holds after
the resource work (worst metric delta 2.7e-05 vs the 1e-4 gate, zero channel difference above the
horizon). `--scene-stress=30` **CLEAN** in Release, exercising exactly the new lifecycle paths —
ResizeWindow, ReloadLevel, SwitchLevel, DlssMode and shutdown — with `emit enhanced=8439 legacy=0`,
i.e. the new declarations did not push the barrier compile onto the legacy path.

**GBV gate: CLEAN** (`--scene-stress=8 --scene-stress-gbv`, Debug, `emit enhanced=3051 legacy=0`).
This required first fixing a pre-existing bug that made the Debug build unable to render a single
frame — see below; that fix is a separate logical change and is not part of this plan.

#### Pre-existing Debug bug found and fixed while gating P1

At `b3870af` the Debug build asserted on the first frame of every level:
`RenderGraph::EndCLGroup` — "grouped pass (non-first) has a prereq from outside the group"
(`RenderGraph.h:472`, from `SceneRenderer.cpp`). Confirmed pre-existing by building `b3870af` in a
clean detached worktree with no local changes and reproducing it identically, so it is neither this
plan's doing nor the parallel ocean work's.

Cause: `Main_Compose` listed `pWetness` among its prereqs, but Compose is the third member of the
reflection CL group while `pWetness` is the tail of the earlier compute group. `BeginCLGroup`'s
contract allows an outside prereq only on a group's **first** member, because the group records as
one command list and nothing can wait in its middle.

Fix: the dependency moved to the group's first member (`Main_RTReflections` / `Main_ReflectionSource`,
all three reflection-source variants), which orders the whole list after the wetness update and
preserves exactly the guarantee Compose needed. Reflection now waits for wetness too, at no real
cost — wetness is the tail of the early compute group and has long since finished. Release image is
unchanged (same 2.7e-05 / zero-above-horizon gate), Release `--scene-stress=30` still CLEAN.

**Depends on:** P0.

**Goal:** add the resource/settings plumbing required by eye adaptation without changing a pixel.

**Touch candidates:**

- `sources/rendering/core/Renderer.h/.cpp`;
- render-graph resource/pass declarations;
- environment/settings JSON serialization;
- editor environment inspector;
- a new exposure helper class only if it materially reduces `Renderer` ownership complexity.

**Implement:**

1. Add `CameraExposureSettings` with `enabled = false` by default.
2. Add persistent GPU resources for histogram bins and one exposure value.
3. Add resource lifecycle handling for startup, resize, device loss, level transition, and shutdown.
4. Add a developer debug readout for the dormant value.
5. Do not dispatch metering or multiply scene color yet.
6. Ensure old levels without the settings block load identically.

**Interface contract:** when disabled, the linear exposure multiplier is exactly `1.0` and no extra active GPU pass is scheduled.

**Done when:** settings round-trip through level JSON/editor undo-redo and the frozen P0 image is unchanged.

**Verify:** Debug build, editor save/reload, undo/redo, device/resize smoke test, native/DLSS screenshot diff.

---

### P2 — Implement histogram metering and temporal eye adaptation

**STATUS: DONE 2026-08-15** (uncommitted). All ten implement items.

Shaders: `shaders/exposure_histogram_cs.hlsl` (`CSClear` + `CSBuild`) and
`shaders/exposure_solve_cs.hlsl`. One new render-graph pass `Main_ExposureMetering`, scheduled
between the scene and the tonemap and deliberately OUTSIDE the tonemap CL group — it is that
group's external prereq, which the group contract only permits on a first member.

**The metering samples a FIXED 256x144 normalised grid, not one thread per pixel.** That is what
makes the pass cost the same at any resolution (item 1) and, more importantly, what makes native
and DLSS meter *the same normalised positions* — so the section 6.3 parity contract holds by
construction rather than by luck. Measured: native and DLSS settle on a median within **0.0000
stops** of each other.

Exposure is applied in `tonemap_cs.hlsl`, after the DLSS resolve that happens earlier in the same
pass and immediately before the tone curve. The exposure record is bound as a **UAV, read-only**,
purely so it never leaves its canonical `UNORDERED_ACCESS` state; an SRV binding would cost a
transition down and back every frame for 16 bytes this pass does not write. NGX auto-exposure is
untouched and nothing is tagged `kBufferTypeExposure` (item 10).

**Gated.** Dormant path is screenshot-equivalent to the P0 baseline (worst metric 2.7e-05 against
the 1e-4 gate, zero channel difference above the horizon). Manual EV sweep is monotonic and
correctly signed — median 0.436 / 0.218 / 0.088 / 0.034 / 0.014 at EV -2/-1/0/+1/+2, i.e. higher EV
is a darker image. Adaptation settles: 8 s and 20 s warmups on the same frozen frame land on an
identical median, 0.00000 stops apart. On `sun_glint` the camera closes 0.46 stops and cuts clipped
pixels from 0.044% to 0.007% without over-reacting, which is the percentile clipping doing its job
(item 4) with no water or sky detection anywhere. Release `--scene-stress=30` CLEAN, Debug
`--scene-stress=8 --scene-stress-gbv` CLEAN, Debug run of the enabled path exits 0 under the
D3D12 debug layer.

**Cost: 0.028 ms** for the whole metering pass against a 0.15 ms budget — 5x under. GPU.Frame
1.600 -> 1.616. Dormant costs 0.001 ms (one empty command list).

**Live tuning surface (dev window "Exposure" tab).** Every setting is a slider there, plus a live
readback of what the solve actually produced: adapted EV100 and its linear multiplier, the target
EV100, the metered low/high percentile luminance, a settled/adapting indicator and a "Reset
adaptation" button. The readback is a 4-slot ring copied 16 bytes per frame (+0.002 ms) and read
from the oldest slot, so no fence is needed. A "Copy JSON to clipboard" button emits the
`cameraExposure` block ready to paste into a level — the tab edits **runtime** state only and
deliberately does not write the level, which keeps section 6.5's "debug views must not mutate
serialised settings" true while still being tunable. The tab is available in Release: this is the
tuning surface, not an editor feature.

#### Finding that changes P4: this renderer's HDR is NOT photometric

Section 6.1's `manualEv100 = 10.0` default renders a **black screen**. EV100 10 means roughly
10,000 cd/m^2, but scene-referred linear values here sit around 0.1-3 for a lit daylight surface,
so the multiplier `1/(1.2*2^10)` = 0.0008 annihilates the image. EV100 in this engine is therefore
relative to an arbitrary linear scale, not to real luminance. The default is now **0.0**, which
measures just under the authored look; auto-exposure settles near -0.3.

Consequences to carry into P4 and P11: the optional "lux-backed UI" in P4 item 1 is **not** free —
it first requires establishing a scene-to-luminance scale, and until that exists, sun intensity in
lux would be a label rather than a unit. The `minEv100 = -6 / maxEv100 = 16` defaults are likewise
photometric assumptions; they remain deliberately wide (i.e. "off", per P1's note) and should be
narrowed against measured values, not against textbook ones.

One interaction worth stating plainly: with the camera enabled, the directional light's legacy
`exposure = 2.0` is now being compensated for by auto-exposure rather than removed. That is exactly
the double duty P4 exists to separate, and it is why enabling the camera on `wind_test` moves the
image so little (+0.08 stops on `overview`) — the two are cancelling.

**Depends on:** P1.

**Goal:** generate a stable camera exposure from HDR scene luminance.

**Touch candidates:** new histogram/adaptation compute shaders; renderer pass scheduling; exposure settings UI; reset/camera-history hooks.

**Implement:**

1. Downsample the selected HDR metering source into a fixed-cost luminance input.
2. Build a log2-luminance histogram with fixed min/max log luminance and clear it every frame.
3. Solve target luminance from configurable low/high percentiles.
4. Reject rare ocean glints and the sun through percentile clipping, not hard-coded water detection.
5. Convert target luminance to target EV plus `compensationEv`.
6. Adapt temporally with independent `speedUp` and `speedDown` controls.
7. Cap adaptation delta time and implement every reset listed in section 6.4.
8. Add manual-EV mode using the same downstream exposure representation.
9. Apply the engine exposure after DLSS/native resolve according to section 6.3.
10. Keep DLSS’s current NGX auto-exposure configuration unchanged.

**Interface contract:** toggling DLSS must not cause a visible exposure jump after both paths have settled on the same frame.

**Done when:** moving from shaded foliage to bright sky adapts smoothly, while panning across a sun glint does not visibly pulse the whole frame.

**Verify:**

- record dark-to-bright and bright-to-dark sequences;
- test frozen ocean with stationary camera for exposure drift;
- toggle native/DLSS after reset and compare settled exposure;
- teleport camera and confirm immediate valid initialization;
- inject/encounter black and invalid samples without NaNs.

---

### P2B — Metering refinements from the UE reference

**STATUS: CLOSED 2026-08-15.** Items 1, 3, 5 and 6 implemented; item 2 deferred by section 2;
item 4 **struck as based on a misreading** (see below). Plus the histogram visualisation, the
`--sweep` harness and an offline shader compile check.

- **Item 3, hybrid adaptation, DONE.** Beyond `adaptationStartDistance` stops from the target the
  camera moves linearly (constant stops/second, so a large transition is time-bounded); inside it,
  exponentially, so the last stretch eases in instead of arriving at full rate and stopping dead.
  The slope-match factors are derived on the CPU exactly as UE derives them, which is what makes
  the two halves join without a visible change of rate. Default distance 1.5 stops, matching
  `r.EyeAdaptation.ExponentialTransitionDistance`. Verified converging: 8 s and 25 s warmups on the
  same frozen frame land on an identical median.
- **Item 5, black bucket influence, DONE.** Scales the darkest bucket's weight; 1.0 = unchanged.
- **Item 4 was WRONG and is struck.** It claimed UE clamps the target but not the adapted value.
  Re-reading the source: UE clamps **both** — `TargetAverageLuminance` at
  `PostProcessEyeAdaptation.usf:172` and `SmoothedExposure` at `:184`. The "no clamping here"
  comment applies *inside* `ComputeEyeAdaptation`, not to the caller, and the line above the second
  clamp is a TODO wish rather than a decision. Our behaviour already matches UE; there was nothing
  to change. Recorded rather than quietly deleted, because the misreading is instructive.

- **Metering weight mask DONE.** The histogram now accumulates *weights* rather than counts, in
  fixed point (256 units per 1.0 of weight; worst case 9.4M in one bin, three orders below uint32
  overflow). The weight is procedural rather than a texture — a mask would have to be authored and
  bound before it could be tried at all, and the radial shape is what one would contain anyway.
  Controls: strength, inner/outer radius (fractions of the half-diagonal), and a **sky bias** that
  additionally de-weights the top of the frame, because a purely radial mask still lets the sky
  dominate the moment the camera tilts up. Floored at 0.05 as UE does. `strength = 0` returns
  exactly 1.0 per sample, so off is bit-identical to the unweighted histogram.

  Measured, strength sweep at sky bias 0.6 (median / sub-2% shadows):

  | strength | grove | sun-facing glint |
  |---|---|---|
  | 0.00 | 0.1804 / 0.29 | 0.1093 / 4.03 |
  | 0.35 | 0.1796 / 0.30 | 0.1180 / 3.75 |
  | **0.70** | **0.1783 / 0.33** | **0.1340 / 3.36** |
  | 1.00 | 0.1770 / 0.37 | 0.1571 / 2.93 |

  **The mask barely touches the grove and moves the glint view by +0.29 stops at 0.7 — and that is
  the correct result, not a weak one.** Shade fills the whole grove frame, so there is no bright
  region to de-weight; that case belongs to the low percentile. The two knobs address two different
  failures and the measurement separates them cleanly. Defaults are the measured configuration
  (strength 0.7, sky bias 0.6), not rounded guesses.

- **Middle grey fixed.** `kMiddleGrey = 0.18f` and `kEv100LuminanceScale = 8.0f` are now explicit
  constants in `PhotographicSettings.h`, and `ExposureMultiplierFromEv100` is
  `kMiddleGrey * (S/K) / 2^EV100` instead of the saturation-based `1/(1.2 * 2^EV100)` that silently
  targeted 0.104. Mirrored in `tonemap_cs.hlsl`. Measured on the shore view: mean 0.1616 -> 0.2941,
  median 0.1531 -> 0.2929 (about +0.94 stops once the tone curve's non-linearity is included).
  The dormant path is unaffected and still bit-identical to the P0 baseline.
- **Histogram plot in the dev window** (plan section 6.5). The 256 bins round-trip through the same
  readback ring as the exposure record, peak-normalised so a big flat sky does not squash the shape.
  The plot shades the window the percentiles actually keep — found by walking the **cumulative**
  distribution, because placing the markers at `lowPercentile * binCount` would misreport them —
  and marks where the adapted exposure sits on the same log-luminance axis.

- **Low percentile default 0.02 -> 0.65, and a degenerate-window crash-to-white fixed.** The 0.02
  default let essentially all shade into the meter, so a shaded frame dragged the camera wide open:
  flying through the palm grove the median went 0.063 (camera off) -> **0.343** and the sub-2%
  shadow population collapsed 22% -> 0.01%. That was the reported over-exposure. Narkowicz
  recommends discarding 50-80% of the darkest samples; 0.65 is mid-band. Measured (high percentile
  fixed at 0.80), median / sub-2% shadows, overview | grove:

  | lowPercentile | overview | grove |
  |---|---|---|
  | 0.02 | 0.291 / 0.00 | 0.343 / 0.01 |
  | 0.35 | 0.197 / 0.11 | 0.229 / 0.05 |
  | 0.50 | 0.159 / 0.93 | 0.206 / 0.06 |
  | **0.65** | 0.123 / 1.65 | 0.180 / 0.29 |
  | 0.80 | 0.090 / 2.73 | 0.135 / 4.29 |
  | *reference* | *0.196 / 3.42* | |

  Note the tension this exposes: the **median** wants ~0.35-0.50 while the **shadow population**
  wants 0.80. No single global value satisfies both, which is the concrete argument for items 1
  (weight mask) and for P3B.
- Separately, `exposure_solve_cs.hlsl` used to white out the frame when the two percentiles met
  (empty window -> average falls back to minLogLum -> camera opens to the clamp). Trivially
  reachable by dragging both sliders together. A degenerate window is now widened, and an empty
  selection holds the previous exposure instead of exposing for black.

**Note for the remaining items:** the baseline captures now predate the middle-grey fix, so anything
comparing against them has to account for a ~0.9 stop offset, or recapture.

#### Tooling: offline shader compile check (`tools/check_shaders.py`)

**MSBuild does not compile HLSL in this project** — shaders compile at runtime. A syntax error
therefore surfaces as a *silently missing feature* in Release (the material is null, its pass
early-outs, and the frame merely looks wrong) and as an assert on a **constant-buffer field name**
in Debug. Neither points at the actual error.

That is not hypothetical: closing this step, `exposure_solve_cs.hlsl` failed to compile because a
local was named `linear`, which is an HLSL interpolation modifier. Release ran happily with the
metering solve simply not executing — the grove median silently read 0.104 instead of 0.178 — and
Debug asserted inside `Material::ComputeCBFieldHandle` on the *first* field name, which looks like
a constant-buffer problem and is not. `python tools/check_shaders.py` compiles every compute entry
point with the SDK's dxc and reports the real error. **Run it after any shader edit**; it takes
about a second. The same trap waits for `sample`, `centroid` and `precise`.

#### Tooling: single-process settings sweeps (`--sweep`)

Sweeping a setting used to mean one process launch per value — about 14 s each, plus boot variance
between them. `--sweep=<setting>:<v0>,<v1>,...` reuses the `--shot-count` series machinery for
settings instead of time: it sets the shot count from the value list, applies value[i] before shot i,
resets the exposure adaptation so each shot settles on its own value, and waits `--shot-interval`.

```
--shot=out.png --sweep=exposure.lowPercentile:0.02,0.35,0.5,0.65,0.8 --shot-interval=2.5
```

Five values in **29 s from one boot** instead of ~70 s from five, and validated against the
per-launch numbers: the first four medians reproduced to four decimals. Recognised settings are
listed in `App.h`; an unknown name logs once rather than failing silently.



**Depends on:** P2. **Not started.** Added 2026-08-15 after reading the UE5 auto-exposure sources the
user supplied (see section 13 for where they live and a file map). Each item below is a
concrete gap between our P2 and what UE ships, ordered by what it buys us.

1. **Metering weight mask** — `AdaptationWeightTexture(ScreenUV)` in `PostProcessHistogram.usf`,
   floored at 0.05 in `PostProcessEyeAdaptation.usf`. UE weights every histogram sample by a screen
   mask, which is classic centre-weighted metering. **This is the principled fix for the glint
   complaint**: instead of discarding bright samples globally (our high-percentile hack), you
   de-weight the part of the frame the player is not looking at. Requires the histogram to
   accumulate *weights* rather than counts — UE accumulates float weights in groupshared and
   `InterlockedAdd`s fixed-point, which our raw-buffer histogram can do the same way. Note a
   procedural centre-weight needs **no texture at all**, so the first version of this is a few
   lines in the existing shader; a mask texture is only needed if the weighting has to be authored.
2. **Exposure compensation curve** — `EyeAdaptation_ExposureCompensationCurve` multiplies the scalar
   `ExposureCompensationSettings` (`PostProcessEyeAdaptation.usf:177`). This is exactly the
   mechanism section 4 decision 5a says is required to keep lighting conditions apart, now
   confirmed as the shipping approach rather than a guess. Still out of scope per section 2 until a
   night sky exists, but the shape of the fix is no longer speculative.
3. **Hybrid exponential/linear adaptation** — `ComputeEyeAdaptation` picks
   `LinearAdaption` when `|log difference| > StartDistance` and `ExponentialAdaption`
   (`1 - exp2(-dt * speed)`) otherwise. Ours is purely linear, so small corrections crawl in at
   constant speed and then stop dead; the hybrid eases out naturally while still bounding the time
   of large transitions. Cheap and a clear quality win.
4. **Clamp the target, not the adapted value.** UE clamps `AverageSceneLuminance` into
   min/max *before* deriving the target and deliberately does **not** clamp the smoothed result —
   the comment in `PostProcessHistogramCommon.ush` says clamping it produces a harsh transition
   when moving between volumes with different ranges. **We currently clamp the adapted value**, so
   we have the behaviour they explicitly removed. Worth matching.
5. **Black bucket influence** — `EyeAdaptation_BlackHistogramBucketInfluence` scales the weight of
   the darkest bucket so a scene with large pure-black regions does not drag the meter. A one-line
   knob once the histogram is weighted.
6. **Middle grey — NOT equivalent, and this is a real bug.** UE computes
   `TargetExposure = TargetAverageLuminance / 0.18`. Ours composes `Ev100FromLuminance = log2(L*8)`
   with `ExposureMultiplierFromEv100 = 1/(1.2 * 2^EV)`, which maps the metered luminance to
   `L / (9.6L) = 0.104`, not 0.18. **We are systematically 0.79 stops under-exposed** against the
   convention every reference implementation and every artist assumes. Measured during P3: a
   `compensationEv` of +0.79 puts the frame's mean and median essentially on the reference image's.
   Fix by making 0.18 an explicit constant in the solve rather than a consequence of two composed
   formulas — and note that changing it moves every level that has already been tuned around the
   old value, so it needs a recapture of the P0-comparison set in the same change.

**Done when:** looking into the sun over water no longer requires the high percentile to be tuned
down to 0.80 to stay readable, because the mask is doing that work instead.

---

### P3 — Replace the display transform with a controlled color pipeline

**STATUS: DONE 2026-08-15** (uncommitted), with one finding that lands on P2B, not here.

`shaders/agx.hlsli` + a branch in `tonemap_cs.hlsl`; `render::ColorPipelineSettings` (tone curve +
AgX slope/power/saturation) serialised as a `colorPipeline` level section with the curve stored as
a **name** (`"agx"` / `"legacy"`) so a level keeps meaning what it says; live controls in the dev
window's Exposure tab, next to the exposure knobs on purpose — judging a curve without seeing the
exposure is how a curve problem gets "fixed" with exposure.

- **Legacy mode is bit-identical to the pre-P3 image** (verified: max channel delta 0 above the
  horizon against the P0 baseline). That is the whole point of keeping it — a suspected regression
  can be A/B'd against the curve instead of argued about.
- The real **sRGB transfer function** replaces `pow(1/2.2)` on the AgX path. The two diverge most
  in the deep shadows, where sRGB's linear toe stops near-black being lifted.
- Highlight desaturation / gamut compression (P3 item 5) is **inherent to AgX's inset matrix**, not
  bolted on afterwards.

**Two bugs found and fixed during implementation, both of which tinted the frame pink:**

1. **Two AgX variants, not interchangeable.** The three.js one converts to linear Rec.2020 first and
   uses inset/outset matrices fitted for that space; the "minimal AgX" one uses matrices acting
   directly on linear sRGB. The first implementation did both, transforming the primaries twice.
2. **The outset matrix was transposed.** HLSL's `float3x3(...)` fills ROWS and `mul(M, v)` dots each
   row with `v`; the GLSL references use `mat3(...)`, which fills COLUMNS. Transcribing their
   literals in order silently transposes. Inset and outset are not symmetric, so a transposed
   outset stops being the inset's inverse and leaves a residual colour transform: measured, neutral
   white came out as **(1.091, 0.956, 0.953)** — red up 9%, green and blue down ~4.5%, i.e. a pink
   cast on every neutral surface in the frame.

**The lesson, and it is the reason bug 2 survived a first "fix":** a colour matrix must be checked
numerically, not by eyeballing one frame. `inset * outset` must be the identity and white must stay
white — two lines of arithmetic that would have caught this immediately, where a visual check after
fixing bug 1 only showed "still pink" without saying why. `agx.hlsli` documents both traps and
states the invariant at the matrices.

**Measured, and it is not the flattering result.** `overview`, auto-exposure on, high percentile
0.80:

| | mean | median | p02 | p99 | dark% | p99/p02 |
|---|---:|---:|---:|---:|---:|---:|
| legacy | 0.2144 | 0.1523 | 0.0281 | 0.6327 | 1.11 | 22.5 |
| AgX neutral | 0.1726 | 0.1422 | 0.0371 | 0.4309 | 0.14 | 11.6 |
| AgX "punchy" (1.0/1.35/1.4) | 0.0809 | 0.0525 | 0.0077 | 0.2875 | 14.28 | 37.2 |
| reference | 0.2679 | 0.1964 | 0.0152 | 0.8137 | 3.42 | 53.6 |

On these **luminance** metrics AgX-neutral is further from the reference than the legacy fit: it is
flatter (spread 11.6 vs 22.5) and its highlights land lower. That is AgX behaving as designed — it
is a neutral base to grade on, not a look.

#### DEFAULT REVERSED to the legacy ACES fit — decision recorded here per section 4

The plan's P3 originally defaulted to AgX. That is overturned by measurement plus one fact about the
target, and section 4 permits exactly this ("unless a step explicitly records new measured evidence
and updates this document"). AgX stays implemented, correct, and one radio button away.

1. **AgX protects against a problem we do not have.** Its value is graceful behaviour at clipping
   and out-of-gamut. The P0 measurements show we clip **0.000%** of pixels on two of three canonical
   views and 0.044% on the third. There is nothing for it to protect, and it charges contrast:
   spread 10.9 vs the legacy fit's 18.1 on the same frame.
2. **Correcting the exposure under-shoot makes it flatter, not better.** Measured, not assumed: AgX
   at +0.79 EV gives median 0.233 but spread **7.7** — brighter and *more* washed, because the
   content moves into the gentlest part of a 16.5-stop sigmoid while the blacks lift. A grade
   (slope 1.15 / power 1.2 / sat 1.3) recovers to 12.3, still under legacy's 18.1. An earlier
   prediction that the exposure fix would rescue AgX was wrong.
3. **Unreal — the look being targeted — does not use AgX at all.** Zero occurrences in the engine
   source. It ships an ACES-derived filmic curve with artist controls (Slope, Toe, Shoulder, Black
   clip, White clip) plus colour grading, baked into a 3D LUT. Our legacy Narkowicz fit is an
   approximation of that same ACES curve, which is why it reads closer to the target.

**Revisit AgX at P11**, once P2B and P3B have pushed the image to actually use the top of the range
and clipping starts to occur (the reference photograph clips 0.125%). At that point its highlight
behaviour stops being theoretical.

**Carry forward regardless of the curve:** the real sRGB transfer function is objectively more
correct than `pow(1/2.2)` and is independent of which curve runs. It is currently tied to the AgX
branch; it should be lifted out so both paths use it — with the caveat that doing so breaks the
legacy path's bit-identity with the P0 baseline, so it needs its own recapture.

#### Finding for P2B: our exposure under-shoots the 18% convention by ~0.8 stops

`ExposureMultiplierFromEv100` is `1/(1.2 * 2^EV)`, the saturation-based formulation. Composed with
`Ev100FromLuminance = log2(L*8)` it maps the metered luminance to `L / (9.6L) = 0.104` — but the
photographic convention, and what UE uses (`TargetExposure = TargetAverageLuminance / 0.18`), is
**0.18**. We are therefore systematically **0.79 stops under-exposed** relative to every reference
implementation and every artist's expectation.

Confirmed by sweep: AgX with `compensationEv = +0.79` lands mean 0.2572 / median 0.2252 against the
reference's 0.2679 / 0.1964 — essentially on target, from a one-line constant. It also lifts p02 to
0.0710 (reference 0.0152) and empties the deep shadows, which is the same lesson as before: a global
multiplier slides the histogram, it cannot stretch it. **Fix the constant in P2B** (it is an
exposure convention question, not a curve question), and expect the remaining gap — short highlights
and milky blacks — to need P3B.

---

### P3 — original specification

**Depends on:** P2.

**Goal:** preserve bright tropical highlight detail and color while producing a correct display-referred output.

**Touch candidates:** `shaders/tonemap_cs.hlsl`, tonemap constants, post-processing settings/editor UI.

**Implement:**

1. Preserve the current Narkowicz curve as a legacy/debug mode.
2. Add one modern, documented default transform. **Default choice: AgX.** For this content target (bright tropics, saturated cyan water) the Narkowicz/ACES fits are known to skew hue toward orange and clip saturated blues and cyans into flat color; AgX's highlight rolloff is built for exactly this failure mode. A complete ACES-style fitted pipeline is an acceptable alternative only with explicit input/output assumptions documented and a saturated-water A/B against AgX.
3. Apply exposure exactly once before the tone curve.
4. Replace raw gamma `2.2` with the correct sRGB transfer function for SDR output.
5. Add controlled highlight desaturation/gamut compression so bright cyan water and warm sand do not clip into synthetic flat colors.
6. Add optional white-balance controls only if they operate in a documented color space; default to neutral.
7. Keep UI composition outside the scene tone curve.
8. Add pre/post tone-map debug views.

**Interface contract:** no light shader owns camera exposure or display encoding.

**Done when:** cloud and sand highlights retain structure, shaded foliage does not collapse to black, and neutral gray remains neutral at default white balance.

**Verify:** grayscale ramp, saturated color chart/test material, P0 views, clipped-pixel ratio, native/DLSS parity, legacy-mode image comparison.

---

### P3C — The Unreal film pipeline (curve controls + colour grading)

**Depends on:** P3. **Not started.** Added 2026-08-15 after the user stated the target plainly —
"I want it like Unreal" — and after checking the UE sources: **AgX appears nowhere in that engine**.

What UE actually ships, and therefore what "like Unreal" means:

1. **An ACES-derived filmic curve with five artist controls** — `FilmSlope`, `FilmToe`,
   `FilmShoulder`, `FilmBlackClip`, `FilmWhiteClip` (`FPostProcessSettings`, category "Film").
   Our fixed Narkowicz fit approximates the same curve but exposes **no** controls, so there is no
   way to ask for more contrast at the curve level — which is exactly what the image needs.
2. **Colour grading: saturation, contrast, gamma, gain, offset** — and each of those exists four
   times over: a global set plus separate **Shadows / Midtones / Highlights** ranges. This is where
   UE's "juicy" actually comes from; the curve alone does not produce it.
3. **Both baked into a 3D LUT** (`CombineLUTs` -> `ColorGradingLUT`, a `Texture3D`), so the tonemap
   shader is one LUT sample per pixel rather than a curve plus a grade evaluated per pixel.

**STATUS: stages 1 (film curve) and 2 (colour grading) DONE 2026-08-15. Stages 3-4 not started.**

**Stage 1, the film curve.** `shaders/film_curve.hlsli`, transcribed from `FilmToneMap`, selected
by `ToneCurve::Filmic` alongside Legacy and AgX. Unreal's five controls — slope, toe, shoulder,
black clip, white clip — with their defaults. Three segments solved in log10, with the match points
solved so **0.18 in gives 0.18 out however the knobs are set**, which is what stops it fighting the
exposure solve (that targets the same 0.18). Pre/post desaturation 0.96/0.93 included.

Measured on `overview` against the reference photograph:

| | median | p02 | p99 | chroma |
|---|---:|---:|---:|---:|
| legacy fit | 0.2711 | 0.0543 | 0.767 | 0.273 |
| **Filmic (UE)** | **0.1889** | **0.0291** | 0.688 | 0.284 |
| legacy + Vivid grade | 0.1842 | 0.0250 | 0.776 | 0.434 |
| reference | 0.1964 | 0.0152 | 0.814 | 0.445 |

The curve alone lands the median essentially on the reference (0.189 vs 0.196) and roughly halves
the black-lift, with **no grading at all** — the tonal half of the gap closes with the curve, and
the chroma half with the grade. They are complementary, not alternatives.

**Not included, and it is a colour difference rather than a tonal one:** Unreal runs this in AP1
working space with the ACES glow module and red modifier applied in AP0 first. `ACESCommon.ush` has
since arrived, so this is now a contained follow-up rather than a blocker — see section 13.0.
**Verify every matrix numerically when adding it** (round-trip to identity, white stays white); the
AgX outset was silently transposed during P3 and tinted the whole frame pink.

`shaders/color_grade.hlsli`: saturation, contrast, gamma, gain, offset, applied in scene-referred
**linear before the tone curve** — the same place UE bakes it into its LUT. Grading after the curve
would push display code values whose range is already compressed. Contrast pivots on middle grey
(0.18, the same constant the exposure solve targets) so raising it stretches around the midtones
instead of darkening everything — which is exactly the trap AgX's `power` look falls into. Works on
**both** curves. Every value neutral by default, and the shader skips the whole block when they are,
so a level that grades nothing is bit-identical.

**Measured, and it does the thing global exposure could not.** Saturation moves chroma 0.273 ->
0.398 while leaving the median at 0.271 -> 0.270, i.e. it does not touch brightness. Contrast at
1.35 drops p02 0.0543 -> 0.0322 *and* lifts p99 0.767 -> 0.874 with the median almost still
(0.2711 -> 0.2754): **it stretches the histogram rather than sliding it**, which is precisely what
was missing when the only tool was a global multiplier.

A combined grade (saturation 1.4, contrast 1.25, exposure compensation -0.4) lands at
median 0.184 / p02 0.0250 / p99 0.776 / chroma 0.434 against the reference photograph's
0.196 / 0.0152 / 0.814 / 0.445 — close on every axis, from controls alone, with no local exposure
involved. That materially weakens the case for P3B; re-evaluate it only after the film curve lands.

**The UE film curve source is now available** at `D:\Programming\ue_autoexposure\` (in the folder
root, not under the engine-relative paths): `TonemapCommon.ush` — `FilmToneMap` plus every helper
it needs (`rgb_2_saturation`, `rgb_2_yc`, `sigmoid_shaper`, `glow_fwd`, `rgb_2_hue`, `center_hue`,
`AP1_RGB2Y`, the AP0/AP1 matrices) — with `PostProcessCombineLUTs.usf/.cpp` for how it is driven.
The curve is: convert to AP1, ACES glow module and red modifier in AP0, pre-desaturate to 0.96,
a three-segment toe/straight/shoulder curve in **log10** parameterised by
`FilmSlope/FilmToe/FilmShoulder/FilmBlackClip/FilmWhiteClip` and matched so 0.18 in gives 0.18 out,
post-desaturate to 0.93, returning AP1. **Transcribe the matrices numerically-verified** — an
inset/outset pair was silently transposed during P3 and tinted the whole frame pink.

**Implement, in this order — each stage is independently useful:**

1. Replace the fixed Narkowicz fit with the parameterised filmic curve, defaulting to values that
   reproduce today's image so the change is a no-op until a control is moved.
2. Global grading: saturation / contrast / gamma / gain / offset, applied in a documented space.
3. Per-range (shadows/midtones/highlights) grading, if the global set proves insufficient.
4. The 3D LUT bake, as a **performance** step only — not for correctness. Worth noting the engine
   now has approval for 3D textures (see P3B), so the same capability serves both.

**Done when:** the canonical views can be pushed to the reference's contrast and saturation with
curve and grade controls alone, without touching exposure to fake it.

**Interface contract:** defaults reproduce the pre-P3C image. Grading is a *level* setting, not a
renderer default — section 9's P11 rule that a level grade must not become a hidden global default
still stands.

---

### P3B — Local exposure (the "HDR photo" look)

**Depends on:** P3. **Not started.** Added 2026-08-15 after P2 measurement showed global exposure
cannot reach the reference image, and after the user reported the exact symptom this exists to fix:
looking into the sun over water crushes the whole frame, "and the eye is HDR".

**The measurement that forces this step.** Dynamic spread, p99/p02, on the canonical views:

| | p02 | median | p99 | spread |
|---|---:|---:|---:|---:|
| ours (`overview`, P0) | 0.0211 | 0.1131 | 0.5589 | **26.5x** |
| `docs/ref/ref_wind_test.png` | 0.0152 | 0.1964 | 0.8137 | **53.6x** |

The reference has a *higher* median and *deeper* blacks at the same time, over twice our dynamic
spread. **A global exposure multiplier cannot produce that**: it slides the whole histogram, it
cannot stretch it. Confirmed empirically — widening the metering window on `overview` raised the
median from 0.113 to 0.152 but *reduced* the sub-2% shadow population from 1.72% to 1.11%, moving
away from the reference's 3.42%. Brightening and deepening at once is not something one number can
do. That is the whole argument for this step, and it is why P2's percentile work, while it fixed
the crush, could never fix the look.

**Goal:** preserve highlight and shadow detail simultaneously by varying exposure spatially, the way
consumer "HDR photo" processing and the human retina both do (local, not global, adaptation).

**Approach.** Follow the UE implementation (section 13 has the location) rather than inventing one;
the maths below is read off `PostProcessHistogramCommon.ush` and `PostProcessLocalExposure.usf`.

The decomposition is a **bilateral grid**, not a 2D bilateral filter — that detail matters, because
the grid is nearly free for us:

- The grid is a `Texture3D<float2>` of `(tileX, tileY, luminanceBin)` holding
  `(sum of log-luminance, sample count)`, depth 32.
- **UE builds it inside the histogram pass** (`PostProcessHistogram.usf:224` writes
  `BilateralGridRWTexture[uint3(GroupId.xy, GroupIndex)] = float2(SumLuminance, Sum)`). Our P2
  histogram pass already walks the scene and already bins by log-luminance, so the grid is close to
  a by-product of work we do anyway.
- Base log-luminance = `grid.x / grid.y` sampled trilinearly, with the z coordinate being the
  histogram position of the pixel's own log-luminance. Where the cell is empty (`grid.y < 0.001`,
  which happens because the grid is populated at half resolution) it falls back to a separately
  blurred log-luminance, blended by `BlurredLuminanceBlend`.

Then, per pixel (`CalculateLogLocalExposure`):

```text
Detail       = LogLum - BaseLogLum
BaseCentered = BaseLogLum - LogMiddleGrey
ContrastScale= BaseCentered > 0 ? HighlightContrastScale : ShadowContrastScale
                                                    (+ a smoothstep threshold region)
LogLocalLum  = LogMiddleGrey + ThresholdOffset
             + BaseCentered * ContrastScale
             + Detail * DetailStrength
return exp2(LogLocalLum - LogLum)          // a per-pixel multiplier on scene colour
```

Compressing only `BaseCentered` while passing `Detail` through is the entire trick: it reduces the
scene's dynamic range without touching micro-contrast, which is why the result reads as vivid
rather than as the flat, haloed "HDR look". The thresholds exist so the effect can be held off
until a pixel is far enough from middle grey, which is what stops mid-tones from being churned.

**Engine readiness (audited 2026-08-15 — nothing is missing, one decision to make):**

- **3D textures would be a first for this engine** — nothing creates or samples one today. Nothing
  blocks it either: `render::CreateCommittedTexture` forwards a raw `D3D12_RESOURCE_DESC` (and
  copies it field-by-field into `D3D12_RESOURCE_DESC1` on the enhanced path), so `TEXTURE3D` plus
  `DepthOrArraySize` needs no wrapper change; descriptor staging and `GpuResource::Attach`/`Declare`
  are dimension-agnostic. **The user has explicitly approved adding 3D texture support if this step
  needs it (2026-08-15)**, so treat it as a normal piece of work: create the resource, add the SRV
  and UAV view helpers next to the existing 2D ones, and note in the handoff that it is the
  engine's first 3D resource so the next reader is not surprised.
- **The sampler is already correct.** `SamplerManager::LinearClamp` is
  `MIN_MAG_MIP_LINEAR` with `AddressU = AddressV = AddressW = CLAMP`, i.e. proper trilinear
  filtering of the grid with a clamped W. No new sampler.
- **The one real constraint: groupshared budget, and it forces a bin-count decision.** UE's grid
  build keeps a **per-thread** histogram in groupshared —
  `SharedHistogram[HISTOGRAM_SIZE][THREADGROUP_SIZEX][THREADGROUP_SIZEY]`. At their 64 bins with an
  8x8 group that is 64*8*8 = 4096 uints = **16 KB**, inside the 32 KB limit. **Our histogram is
  256 bins**, where the same structure would be 256*8*8 = 16384 uints = **64 KB, over the limit**.
  UE additionally requires `THREADGROUP_SIZEX * THREADGROUP_SIZEY >= HISTOGRAM_SIZE` for the grid
  write (one thread per bin emits one grid cell), and our dispatch helper mandates
  `numthreads(8,8,1)` = 64 threads — which caps the grid path at 64 bins from the other direction
  too. **So the grid path uses 64 bins.** That is almost certainly why UE picked 64 in the first
  place. The global exposure histogram can stay at 256 if we want the extra percentile precision;
  they are separate consumers of the same pass.
- **Grid XY resolution falls out of the existing dispatch.** `BilateralGridRWTexture[uint3(GroupId.xy, GroupIndex)]`
  makes the grid's XY the threadgroup count. Our fixed 256x144 sample grid at 8x8 groups gives
  32x18 groups, so the grid is 32x18x64. Sensible without any new tuning.

**Implement:**

1. Extend the P2 histogram pass to also populate a `Texture3D<float2>` bilateral grid, plus a
   blurred log-luminance fallback texture. Restructuring the histogram into per-group groupshared
   accumulation (instead of our current direct global atomics) is a prerequisite for the grid, and
   is independently faster — far fewer global atomics.
2. Apply pass: compute the base, evaluate `CalculateLogLocalExposure`, multiply scene colour.
   Placed after the global exposure and before the tone curve.
3. Controls, all defaulting to a **no-op** (contrast scales 1.0, detail strength 1.0, thresholds
   such that nothing is affected): highlight/shadow contrast scale, detail strength, blurred-blend,
   highlight/shadow threshold and threshold strength, middle-grey compensation.
4. Debug views: base layer, detail layer, final per-pixel multiplier
   (UE has `PostProcessVisualizeLocalExposure.usf` for exactly this).

**Alternative worth knowing about:** UE also ships an **exposure-fusion** path
(`FusionSetupCS` / `FusionBlendCS` in `PostProcessLocalExposure.usf`) — Mertens-style, blending
three tone-mapped exposures with Gaussian weights around a target luminance through a Laplacian
pyramid. It is the more expensive branch and is not the default. Do not build both.

**Interface contract:** at default settings the output is screenshot-equivalent to P3. The pass must
be resolution-independent in cost like P2's metering.

**Done when:** looking into the sun over water keeps the shaded side of the island readable while
the glint stays bright, without the global exposure having to close down; and the canonical views'
p99/p02 spread moves materially toward the reference's 53.6x.

**Verify:** the `sun_glint` view specifically, plus halo inspection at high-contrast edges (the
classic failure), camera-motion stability, native/DLSS parity, GPU timing. Provisional budget:
0.40 ms.

**Risk:** halos and the flat "tone-mapped HDR" cliche. Mitigation: compress the base layer only,
keep defaults conservative, and judge on the reference rather than on the metrics alone.

---

### P4 — Separate camera exposure from sun and sky intensity

**Depends on:** P3.

**Goal:** make lighting controls physically understandable and stop using directional-light exposure as a camera substitute.

**Touch candidates:**

- `sources/rendering/lighting/DirectionalLight.h/.cpp`;
- `sources/rendering/lighting/Skybox.h/.cpp`;
- environment runtime/editor/JSON migration;
- `shaders/lighting_cs.hlsl`;
- every other `exposure` consumer — census at the time of writing: `shaders/skybox.hlsl`, `shaders/glass.hlsl`, `shaders/rt_reflections_cs.hlsl`, `shaders/editor_preview.hlsl`, and all three ocean surface variants (`shaders/ocean_surface.hlsl`, `shaders/ocean_surface_legacy.hlsli`, `shaders/ocean_surface_pre_foam_rewrite.hlsl`). The migration must cover all of them or explicitly gate the stragglers; re-run the census (search shaders for `exposure`) before starting.

**Implement:**

1. Introduce an explicit directional-light intensity control. It may use calibrated relative units first; a lux-backed UI is optional only if all conversions are documented.
2. Keep sky radiance intensity independent from the sun.
3. Remove camera-like exposure multiplication from individual direct-light outputs.
4. Stop tinting all ambient fill by the sun color.
5. Provide a compatibility migration for existing `exposure` fields, and be honest about its limits: because the current semantics are inconsistent (directional light multiplied by `exposure`, spot/point lights not, skybox and editor preview on separate values), no remap can reproduce old frames exactly — moving to a full-frame camera exposure necessarily shifts the balance between sun, local lights, emissive, and sky. The migration target is "close enough to retune deliberately", not equivalence: capture per-level before/after comparisons and budget an explicit retune pass as part of this step's acceptance.
6. Update opaque, foliage, ocean, glass, reflection, and editor-preview consumers consistently.
7. Expose camera exposure, sun intensity, and sky intensity in separate inspector sections with helpful tooltips.

**Interface contract:** doubling sun intensity changes direct solar illumination, not sky/background brightness or camera response.

**Done when:** the canonical level can be balanced with neutral daylight and readable shadows without using a warm global ambient multiplier.

**Verify:** old-level migration comparison; toggle each control independently; inspect opaque/ocean/glass parity; editor save/reload and undo/redo.

---

### P5 — Integrate physically coherent diffuse and specular IBL

**Depends on:** P4 and `two_sided_foliage_and_ibl_plan.md` F7-F8.

**Goal:** replace flat ambient and raw roughness-mip reflection fallback with a shared environment-lighting model.

**Source of truth:** implement **F7** and **F8** from `docs/two_sided_foliage_and_ibl_plan.md`. Do not duplicate their asset/runtime contracts here.

**Integration requirements:**

1. Diffuse environment irradiance becomes the default ambient source for opaque and foliage materials.
2. Specular environment lighting uses the prefiltered GGX cubemap and BRDF LUT.
3. Both lobes use the same sky orientation and intensity conventions established in P4.
4. The legacy sky-mip fallback remains available until sign-off.
5. Ocean/glass parity may remain specialized, but their sky intensity and roughness semantics must match the common environment.
6. Evaluate energy balance under the new camera exposure instead of retuning exposure to compensate.

**Done when:** sky-facing surfaces receive believable cool fill, downward-facing surfaces do not look equally bright, rough materials broaden reflections correctly, and foliage remains readable in shade.

**Verify:** canonical views, roughness sweep, metal/dielectric sphere grid, sky rotation, missing-environment fallback, GPU timing.

---

### P6A — Consume material AO through one indirect-light contract

**Depends on:** P5 and `two_sided_foliage_and_ibl_plan.md` F9.

**Goal:** make authored AO meaningful without corrupting direct lighting.

**Source of truth:** implement **F9** from `docs/two_sided_foliage_and_ibl_plan.md`.

**Integration requirements:**

1. `GBAux.r` modulates diffuse IBL.
2. Specular occlusion uses a view/roughness-aware approximation rather than multiplying all specular by raw AO.
3. Direct sun, local direct lights, emissive, and sky background are unaffected.
4. Default AO of `1` is screenshot-equivalent to P5.

**Done when:** creases and authored contact regions gain depth without dirtying sunlit sand or killing grazing reflections.

**Verify:** AO sweep on dielectric/metal materials; direct-only and indirect-only debug views; P0 captures.

---

### P6B — Add dynamic GTAO with edge-aware temporal filtering

**Depends on:** P6A.

**Goal:** ground vegetation, rocks, and terrain contacts that material AO cannot represent.

**Touch candidates:** new GTAO compute passes/shaders, depth/normal/motion inputs, render-graph resources, AO settings/debug UI, lighting/compose consumption.

**Implement:**

1. Compute horizon-based/GTAO at half resolution from linear depth and geometric normal.
2. Scale radius and thickness in view/world units; avoid a purely pixel-sized radius.
3. Bilaterally denoise using depth and normal discontinuities.
4. Add temporal accumulation with motion vectors and disocclusion rejection.
5. Upsample edge-aware to the lighting resolution.
6. Combine dynamic AO with material AO through a documented, bounded rule.
7. Apply the result only to indirect diffuse and specular-occlusion inputs.
8. Add raw, denoised, history-weight, and final-combined debug views.

**Interface contract:** invalid/background depth returns AO `1`; thin foliage must not create full-screen halos.

**Done when:** palm/terrain and rock/sand contacts read clearly, with no dark outlines against sky and no camera-motion crawling.

**Verify:** still and moving camera, native/DLSS, thin geometry, horizon, depth discontinuities, feature-off equivalence, GPU timing.

---

### P7 — Add global analytic aerial perspective

**Depends on:** P4; evaluate after P5 for final balance.

**Goal:** give distant islands, foliage, ocean, and terrain coherent atmospheric depth.

**Touch candidates:** atmosphere settings/serialization/editor, depth-aware compose or post-lighting shader, sky/ocean integration.

**Implement:**

1. Add an analytic exponential-height atmosphere/fog model with extinction and sun-colored in-scattering.
2. Reconstruct world/view distance from depth for opaque pixels.
3. Use camera height and world height so elevated cameras do not receive a uniform screen-space wash.
4. Match the sky/horizon color continuously; do not create a seam between geometry fog and the skybox.
5. Share parameters with the ocean’s horizon treatment or retire duplicated ocean-local terms in a later, explicit substep.
6. Handle background/invalid depth explicitly.
7. Expose density, height falloff, start distance, max opacity, and sun-scatter strength with conservative defaults.
8. Add transmittance and in-scattering debug views.

**Interface contract:** disabling atmosphere is screenshot-equivalent to M2. The first implementation does not allocate a 3D froxel volume.

**Done when:** distant geometry approaches the horizon color smoothly while near beach contrast remains intact.

**Verify:** low/high camera, look toward/away from sun, sky seam, ocean horizon parity, native/DLSS, GPU timing.

---

### P8 — Add exposure-aware HDR bloom and glare

**Depends on:** P3.

**Goal:** reproduce subtle optical highlight spread without flattening the scene.

**Touch candidates:** post-process render-graph resources, bloom downsample/upsample compute shaders, settings/editor/debug UI.

**Implement:**

1. Extract bloom from the exposed HDR image using an EV/luminance threshold and soft knee.
2. Build a compact downsample pyramid with stable filtering.
3. Reconstruct with a tent/energy-controlled upsample chain.
4. Composite bloom in HDR before the tone curve.
5. Keep intensity low by default; bright ocean glints may sparkle but must not become a white fog bank.
6. Optionally add a very small anamorphic/glare term only as a separate toggle after ordinary bloom is signed off.
7. Add threshold, soft-knee, intensity, and radius controls plus per-mip debug views.

**Interface contract:** `intensity = 0` schedules no unnecessary active work or produces exact no-op output.

**Done when:** sunlit water and cloud edges gain a restrained optical response while sand/foliage texture remains visible.

**Verify:** exposure sweep, isolated HDR emissive test, ocean glint motion, native/DLSS stability, GPU timing.

---

### P9 — Add diffuse indirect-light progression

**Depends on:** P5 and P6A.

**Goal:** add missing ground/foliage color bounce after environment lighting is correct.

This is intentionally split into independently landable substeps. Stop after the cheapest stage that meets the art target.

#### P9A — Hemispherical ground-bounce approximation

1. Add a bounded ground-hemisphere irradiance term driven by environment/level parameters.
2. Weight it by surface orientation and indirect visibility.
3. Do not derive it from the sun color alone.
4. Keep it optional and low frequency.

**Done when:** upward/downward orientation reads more naturally and shaded palm trunks receive restrained warm ground fill without visible screen-space artifacts.

#### P9B — Probe-volume infrastructure (dormant)

1. Define probe volume placement, storage, update budget, scrolling/reset rules, and debug visualization.
2. Allocate resources and serialize settings with the feature disabled.
3. Leave the image unchanged.

**Done when:** probes can be visualized and lifecycle-tested without contributing lighting.

#### P9C — Dynamic diffuse probe lighting

1. Update a bounded number of probes per frame using the available ray-query/RT path, with a non-RT fallback policy documented.
2. Store low-frequency irradiance and visibility.
3. Sample probes with spatial interpolation, visibility weighting, hysteresis, and relocation/classification as needed.
4. Apply only to indirect diffuse; keep IBL as the far-field/environment term.

**Done when:** large-scale bounce improves shaded island interiors without light leaks, flicker, or a hard probe-volume boundary.

**Verify for every substep:** feature-off equivalence, canonical captures, moving camera, day-light parameter changes, GPU timing, non-RT behavior.

---

### P10 — Add contact and large-scale shadow polish

**Depends on:** P6B and P7.

**Goal:** improve the final shadow cues not covered by the existing shadow architecture.

**Related source of truth:** use `docs/rt_shadows_integration_plan.md` for ray-traced shadow work. This step must not fork that architecture.

**Implement as separate toggles/substeps:**

1. **P10A — Contact shadows:** short, depth-aware screen-space or ray-query contact shadows for palm/terrain contacts; reject off-screen uncertainty instead of stretching artifacts.
2. **P10B — Cloud-shadow modulation:** low-frequency, world-anchored moving shadow field affecting direct sun only, with temporal stability and distance-aware filtering.
3. **P10C — Shadow color balance:** ensure shadowed regions are illuminated by IBL/GI rather than by an arbitrary minimum-light clamp.

**Done when:** fine contacts are readable, broad sun variation adds scale, and no substep darkens ambient/sky lighting incorrectly.

**Verify:** moving camera/sun, thin foliage, coastline, native/DLSS, shadow feature combinations, GPU timing.

---

### P11 — Calibrate the tropical scene and perform final sign-off

**Depends on:** the selected M4 feature set.

**Goal:** tune content after the renderer’s responsibilities are stable.

**Touch:** canonical tropical level(s), environment settings, material instances/assets only with explicit user approval for content changes.

**Implement:**

1. Start from neutral camera white balance and exposure compensation.
2. Calibrate sun and sky intensities independently.
3. Tune atmosphere from near-shore clarity to distant-horizon depth.
4. Audit physically implausible albedo/roughness values for sand, foliage, trunks, and water-adjacent materials.
5. Tune bloom only after highlights are correct without it.
6. Compare native and DLSS outputs at the same display resolution.
7. Produce final before/after captures and a small table of settings.
8. Obtain human visual approval; automated metrics cannot decide the final grade.

**Interface contract:** renderer defaults remain neutral. A level grade cannot become a hidden global default unless separately approved.

**Done when:** the image has retained highlights, readable shade, believable tropical color separation, grounded contacts, atmospheric scale, stable water, and no feature is compensating for a known bug in another stage.

**Verify:** all canonical views, morning/noon stress variants if available, native/DLSS, stationary/moving camera, performance capture, save/reload.

---

## 10. Global acceptance checklist

### Exposure and color

- [ ] Camera exposure is separate from sun and sky intensity.
- [ ] Ocean glints do not cause visible whole-frame pumping.
- [ ] Dark-to-bright and bright-to-dark adaptation speeds are independently controllable.
- [ ] Exposure initializes correctly after cuts, teleports, resize, and level load.
- [ ] Cloud and sand highlight structure survives the tone curve.
- [ ] Neutral values remain neutral; saturated water does not clip into flat cyan.
- [ ] SDR output uses a correct sRGB transfer function.

### Lighting

- [ ] Diffuse sky fill is directional, not a flat sun-tinted ambient term.
- [ ] Rough specular IBL uses prefiltered GGX data and a BRDF LUT.
- [ ] Material AO and GTAO affect indirect lighting only.
- [ ] Foliage, opaque materials, ocean, glass, and reflections share coherent intensity semantics.
- [ ] Shaded foliage remains readable without an arbitrary ambient floor.

### Depth and polish

- [ ] Distant geometry blends into the atmosphere without a sky seam.
- [ ] Near-shore contrast and water clarity remain intact.
- [ ] Bloom is stable, subtle, and cannot hide clipping.
- [ ] Contact shadows do not halo thin geometry.
- [ ] Any GI volume has no visible boundary or light leaks in canonical views.

### Temporal and performance

- [ ] DLSS keeps the existing water-smear fix.
- [ ] Native and DLSS modes settle on materially the same exposure.
- [ ] No new camera-motion flicker, history trail, or adaptation oscillation is visible.
- [ ] Each active pass has a recorded GPU cost and respects or explicitly reports against its provisional budget.
- [ ] Feature-off paths reproduce the preceding milestone.

### Repository hygiene

- [ ] Debug and milestone Release_Editor builds succeed.
- [ ] Runtime shader compilation succeeds for every exercised permutation.
- [ ] Level JSON/editor undo-redo round-trips new settings.
- [ ] Touched text files contain no mixed line endings.
- [ ] Unrelated dirty-worktree changes remain untouched.

---

## 11. Risks and rollback strategy

### Exposure destabilizes DLSS water

**Risk:** passing manual exposure to NGX or moving exposure ahead of DLSS can restore the known smear.

**Mitigation:** keep NGX auto-exposure untouched and apply engine exposure after DLSS in P2. Any later experiment must be a separate toggle with native/DLSS motion captures.

**Rollback:** disable engine exposure contribution; the persistent exposure resources may remain dormant.

### Existing levels change too much

**Risk:** separating light intensity from camera exposure changes every authored balance.

**Mitigation:** P4 includes explicit legacy-field migration and comparison captures before content retuning.

**Rollback:** retain legacy interpretation behind a versioned compatibility path until levels are migrated.

### AO creates a “game filter” look

**Risk:** excessive radius/intensity produces dark outlines and dirty sand.

**Mitigation:** indirect-only application, half-resolution edge-aware filtering, debug views, conservative defaults.

**Rollback:** disable GTAO while retaining material AO.

### Atmosphere duplicates ocean fog

**Risk:** overlapping models produce a horizon band or double extinction.

**Mitigation:** first match parameters and debug transmittance; retire ocean-local duplication only in an explicit follow-up.

**Rollback:** disable global atmosphere for ocean pixels or restore the previous ocean-local path.

### Feature pile-up hides the real improvement

**Risk:** simultaneous exposure, IBL, AO, bloom, and grading changes make regressions impossible to attribute.

**Mitigation:** milestone captures and one-step change sets are mandatory.

**Rollback:** return to the last accepted milestone, not to arbitrary per-level compensation.

---

## 12. Recommended execution order

For the largest visual gain per unit of risk:

1. P0 — deterministic baseline.
2. P1-P2 — eye adaptation with the DLSS-safe placement.
3. P3 — tone/color/output correctness.
4. P4 — disentangle camera, sun, and sky controls.
5. P5 — complete F7-F8 IBL integration.
6. P6A — consume material AO.
7. P7 — analytic aerial perspective.
8. P6B — dynamic GTAO.
9. P8 — restrained bloom.
10. P9A — evaluate cheap ground bounce.
11. P10 — contact/cloud-shadow polish as needed.
12. P9B-P9C — probe GI only if the accepted image still lacks indirect depth and the performance budget allows it.
13. P11 — final scene calibration and human sign-off.

Do not start final level grading before M2. The fastest route to the target image is to make exposure and environment lighting coherent first, then judge which finishing features are still visibly necessary.

---

## 13. Reference implementation map

### 13.0 Reference files this plan still needs

Keep this list current. When a step is blocked on engine source that is not in the drop, name the
**exact file** here rather than working around it or transcribing from memory — the drop lives on
the user's machine and copying one more file is cheap, whereas a matrix reconstructed from memory
already cost this project a frame-wide pink tint once.

| File | Needed for | Status |
|---|---|---|
| `TonemapCommon.ush` | `FilmToneMap`, the parameterised film curve | **have** |
| `PostProcessCombineLUTs.usf` / `.cpp` | how curve + grade are driven and baked into the LUT | **have** |
| `ACESCommon.ush` | AP0/AP1 matrices, `AP1_RGB2Y`, and the helpers `rgb_2_saturation`, `rgb_2_yc`, `sigmoid_shaper`, `glow_fwd`, `rgb_2_hue`, `center_hue` | **have** |
| `ACES/ACES_v1.3.ush` | referenced by TonemapCommon for the v1.3 output transforms; only needed if the ACES ODT path is ever wanted | not needed yet |
| `PostProcessLocalExposure.cpp` | bilateral grid construction parameters for P3B, if P3B is revived | not needed yet |

Everything is dropped in the **root** of `D:\Programming\ue_autoexposure\`, not under the
engine-relative subpaths — look there first.



The UE5 auto-exposure sources live **outside this repository**, at `D:\Programming\ue_autoexposure\`,
deliberately so they are never committed here — they are third-party engine code kept for reading
only. Engine-relative paths are intact under that root and its own `README.md` is the full index;
the paths in the table below are relative to it. Reference material, not something to copy verbatim
— UE's engine-side conventions do not map onto ours. The files that actually answer questions:

| Question | File |
|---|---|
| Percentile trim + adaptation maths, local-exposure formula, all shared constants | `Engine/Shaders/Private/PostProcessHistogramCommon.ush` |
| Histogram build, per-sample weighting, **and the bilateral-grid write** | `Engine/Shaders/Private/PostProcessHistogram.usf` |
| Target luminance, middle-grey remap, compensation curve, output packing | `Engine/Shaders/Private/PostProcessEyeAdaptation.usf` |
| Local exposure apply + the exposure-fusion alternative | `Engine/Shaders/Private/PostProcessLocalExposure.usf` |
| Settings → shader parameters, all the `r.EyeAdaptation.*` knobs | `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessEyeAdaptation.cpp` |
| Where the passes sit in the frame graph | `Engine/Source/Runtime/Renderer/Private/PostProcess/PostProcessing.cpp` |
| Artist-facing property set and defaults | `Engine/Source/Runtime/Engine/Classes/Engine/Scene.h` (`FPostProcessSettings`) |

Two conventions of theirs are worth keeping in mind when reading: `View.OneOverPreExposure` appears
everywhere because UE pre-exposes scene colour at write time (we do not — decision 1 keeps our HDR
unexposed until the display transform), and their histogram is 64 buckets against our 256, so bucket
counts are not comparable between the two.
