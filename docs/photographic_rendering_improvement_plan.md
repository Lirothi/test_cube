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
             +--> P3 Tone map, color transform, and output encoding
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

### P3 — Replace the display transform with a controlled color pipeline

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
