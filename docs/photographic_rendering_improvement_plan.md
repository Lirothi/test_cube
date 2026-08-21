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
8. **The first atmosphere implementation is analytic and inexpensive.** Volumetric froxels are a separate step, **P15**, queued 2026-08-19 after P7 shipped and the user asked for UE's volumetric fog. It does not replace the analytic model: the volume is the near slab and the analytic fog stays as the far field, which is how UE compose the two.
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
                         |     |
                         |     +--> P15 Volumetric fog, froxel volume [UE ref] (near slab; P7 stays as the far field)
                         |
                         +--> P8 Exposure-aware bloom
                         |     |
                         |     +--> P8B Post-process settings container
                         |           |
                         |           +--> P8C Convolution bloom, FFT + kernel [UE ref] (flares come from the kernel)
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

> **Re-evaluated 2026-08-16, after the curve landed: P3B was still worth building, but for a
> narrower reason than originally argued.** The grade genuinely does stretch the histogram, so the
> "only local exposure can do this" framing in P3B was too strong — global contrast around a pivot
> stretches too. What survives is that the grade applies the *same* stretch to every pixel, so
> pushing it far enough to reach the reference spread also pushes the brightest regions toward the
> top of the range, whereas P3B's stretch is per-neighbourhood: it reached 52.9x spread on
> `overview` with **zero clipped pixels**, and on `sun_glint` it cut clipping 65x while moving the
> median 2%. Use the grade for the look and P3B for the range; they are not substitutes.

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

**Approach.** Follow the UE implementation (section 13 has the location) rather than inventing one.

> **Provenance, stated honestly:** unlike P3C's film curve, the maths below was **not** read off the
> UE source — `PostProcessHistogramCommon.ush` and `PostProcessLocalExposure.usf` are *not* in the
> drop (see 13.0). It follows the published algorithm, and the shipped behaviour was verified by
> measurement instead (see the status block). That is weaker evidence than a transcription: if the
> two files ever land, diff `CalculateLogLocalExposure` against `shaders/local_exposure.hlsli`
> before extending this step.

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

**STATUS: DONE 2026-08-16, uncommitted. Gated and measured.** Defaults are a true no-op, so the
step ships the capability and leaves the choice of values to level tuning.

- `shaders/local_exposure.hlsli` — `LocalExposureMultiplier`, transcribed from UE's
  `CalculateLogLocalExposure`: split log-luminance into base + detail, compress **only** the base by
  a highlight/shadow contrast scale, pass the detail through, with a threshold region so mid-tones
  are left alone. Neutral parameters return exactly 1.0.
- `shaders/exposure_baselum_cs.hlsl` — the base layer, a 256x144 R16_FLOAT blurred **log**-luminance
  image, box-filtered in log space (averaging linearly would let one bright sample dominate a
  neighbourhood, which is the halo this exists to avoid). Designed to run **inside the existing
  metering pass**, so it costs no extra pass, barrier or command list.

**Deliberate scope decision: the base layer is a blur, not the bilateral grid.** UE blends between
the two and falls back to the blur wherever the grid has no data, so blur-only is a configuration of
the same algorithm rather than a different one. What it gives up is halo resistance at high-contrast
edges. The grid remains the upgrade and stays cheap for us because the histogram pass already bins
by log-luminance — but it needs the engine's first 3D texture, and doing that badly is expensive, so
it is not the thing to start with.

**Engine source this step touched** (per the user's standing rule that every step records its files):

| File | What changed |
|---|---|
| `shaders/local_exposure.hlsli` | new — `LocalExposureMultiplier` + `LocalExposureIsNeutral` |
| `shaders/exposure_baselum_cs.hlsl` | new — the 256x144 R16_FLOAT blurred log-luminance base |
| `shaders/tonemap_cs.hlsl` | `t1` base-lum binding, 5 CB fields, apply block after the grade and before the curve |
| `sources/rendering/core/ExposureMetering.h/.cpp` | `baseLum_` texture + SRV/UAV, descriptor heap 4 -> 6 |
| `sources/rendering/core/PhotographicSettings.h` | 5 `local*` fields on `ColorPipelineSettings`, all neutral |
| `sources/rendering/core/PhotographicSettingsJson.h` | parse / clamp / serialise |
| `sources/app/scene/SceneResourceBootstrapper.h/.cpp` | base-lum material + CB, tonemap CB writes |
| `sources/app/scene/SceneRenderer.cpp` | step "2b" dispatch inside `Pass_ExposureMetering`, declarations, tonemap SRV |
| `sources/app/ui/DeveloperWindow.cpp` | sliders, tooltips, Off/Gentle/Strong presets |
| `sources/editor/ui/InspectorPanel.cpp` | the same five controls on the Color Pipeline object |
| `sources/app/App.cpp` | `--sweep` keys, incl. the composite `color.localContrast` |

**The base-lum dispatch lives inside `Pass_ExposureMetering`, not in a pass of its own** — it reads
exactly the source the histogram just read, so it costs no extra command list, barrier or graph
node. It runs **unconditionally**, not gated on the settings being non-neutral: the settings are
live-editable and a gated texture would be one frame stale the moment a slider moved. Measured
cost of the whole metering pass (histogram + base + solve + readback copies) is **0.031 ms GPU** of
a 1.686 ms frame, so the gate would be saving nothing worth a pop.

**Measured: the two knobs are orthogonal, which is the whole claim.** `sun_glint`, native, one knob
at a time:

| | median | p05 | p95 | p99 | clip% | dark% |
|---|---:|---:|---:|---:|---:|---:|
| neutral | 0.1924 | 0.0339 | 0.7657 | 0.8781 | 0.194 | 3.245 |
| highlight 0.55 | 0.1884 | 0.0336 | **0.5965** | **0.7835** | **0.003** | 3.252 |
| shadow 0.55 | 0.2151 | **0.0556** | 0.7671 | 0.8790 | 0.194 | **0.929** |

Highlight compression moves p95/p99/clip and leaves p05 and dark% untouched; shadow compression does
the exact reverse. 65x less clipping on the glint field for a 2% median move.

**The finding that matters, and it inverts the step's own assumption.** The plan above assumed the
useful direction is *compression* (scales below 1). On the reference-framing `overview` view the
useful direction is **expansion — scales above 1**, because our problem there was never too much
range, it was too little. Sweeping both scales together (`--sweep=color.localContrast:...`):

| both scales | p02 | median | p99 | spread | clip% |
|---|---:|---:|---:|---:|---:|
| 1.00 (neutral) | 0.0304 | 0.2016 | 0.7174 | 23.6x | 0.000 |
| 1.20 | 0.0242 | 0.1941 | 0.7804 | 32.3x | 0.000 |
| 1.35 | 0.0198 | 0.1886 | 0.8201 | 41.4x | 0.000 |
| 1.50 | 0.0161 | 0.1831 | 0.8539 | **52.9x** | 0.000 |
| **`ref_wind_test.png`** | **0.0152** | **0.1964** | **0.8137** | **53.6x** | — |

At 1.5 the spread lands on the reference's 53.6x essentially exactly, from 23.6x, **while still
clipping zero pixels** — the blacks deepen and the highlights rise in the same frame. That is the
thing section P3B was written to make possible and that no global multiplier can do; the median sits
0.013 low, which is about +0.1 EV of compensation away. 1.35 is the more conservative landing spot.

> **Numbers above were taken with both thresholds at 0.** The level has since been authored with
> the "Gentle" preset, whose thresholds are 0.5, and a threshold delays the effect: re-measured on
> the committed level the same sweep gives 1.0 -> 23.6x, 1.2 -> 28.4x, 1.35 -> 33.1x, 1.5 -> 38.7x,
> still at 0.000% clipping. Both sets are correct measurements of different configurations — when
> quoting a spread, quote the thresholds with it.

**Halo check passed.** The `hl 1.0 -> 0.55` difference field on `sun_glint` is smooth, is exactly
zero over the unaffected ocean, and shows **no positive rim** adjacent to the strongly-modified sky
(max positive excursion 5/255 against a 55/255 negative swing). A blur-based base layer's classic
failure did not appear at these strengths; judge again if anyone pushes past 1.5.

**Still open (deliberately, not blocked):** the bilateral grid (the halo-resistant base layer, needs
the engine's first 3D texture) and the three debug views. Neither is needed for the step to be
useful, and the grid's value only shows up at strengths past what looks good here.

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

**Verified 2026-08-16.** `tools/check_shaders.py` 9/9; Release + Debug both build; Release
`--scene-stress=30` **CLEAN** (`enhanced=8439 legacy=0`); a plain Debug run exits 0 with asserts
live. Dormant-equivalence holds: with the shipped neutral defaults the `overview/native` median is
**0.2016**, identical to the pre-P3B capture, so the new CB fields did not disturb the grade or film
parameters packed beside them. Cost **0.031 ms** for the whole metering pass against a 0.40 ms
budget for this step alone. Halo and orthogonality results are in the status block above.
`--sweep=color.localContrast:...` was added for this and is the fastest way to re-measure.

**Risk:** halos and the flat "tone-mapped HDR" cliche. Mitigation: compress the base layer only,
keep defaults conservative, and judge on the reference rather than on the metrics alone.

---

### P4 — Separate camera exposure from sun and sky intensity

**Depends on:** P3. **STATUS: DONE 2026-08-16, uncommitted.** The architecture landed with a
PROVEN-lossless migration; the two genuinely-visible corrections ship behind switches that are OFF,
so the retune this step's acceptance calls for is a deliberate act rather than a surprise.

**Goal:** make lighting controls physically understandable and stop using directional-light exposure as a camera substitute.

**Touch candidates:**

- `sources/rendering/lighting/DirectionalLight.h/.cpp`;
- `sources/rendering/lighting/Skybox.h/.cpp`;
- environment runtime/editor/JSON migration;
- `shaders/lighting_cs.hlsl`;
- every other `exposure` consumer — census at the time of writing: `shaders/skybox.hlsl`, `shaders/glass.hlsl`, `shaders/rt_reflections_cs.hlsl`, `shaders/editor_preview.hlsl`, and all three ocean surface variants (`shaders/ocean_surface.hlsl`, `shaders/ocean_surface_legacy.hlsli`). Census RE-RUN 2026-08-16:
`ocean_surface_pre_foam_rewrite.hlsl` was a dead pre-rewrite backup referenced by nothing and has
been deleted (recoverable from commit `6322c18`), so the list above is now the whole set. Two more
C++ owners the original list missed: `OceanRenderable::GetSunDirAmbient/GetSunColorExposure` build
the ocean's constants from the light directly, and missing them is what made the first migration
attempt non-lossless. The migration must cover all of them or explicitly gate the stragglers; re-run the census (search shaders for `exposure`) before starting.

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

---

#### What shipped, and the measurement that reframed the step

**The step's own premise, measured.** Before writing anything, `--sweep=light.exposure` on
`wind_test` (auto-exposure on, `overview`):

| `directionalLight.exposure` | median | p02 | p99 |
|---|---:|---:|---:|
| 1.0 | 0.2175 | 0.0194 | 0.7232 |
| 2.0 (the level's own) | 0.2023 | 0.0322 | 0.6777 |
| 4.0 | 0.2020 | 0.0423 | 0.6140 |

Doubling the sun moves the median by **0.0003**. The metering cancels it completely. What it does
still move is p02 and p99 — because it scales the sun and the ambient but NOT the sky background,
the spot/point lights or emissive. So post-P1-P3 that field is not a brightness control at all any
more: it is a **scene-versus-sky ratio** control wearing a camera's name. That is a sharper argument
for this step than the one written above, and it is the one to quote.

**The migration is lossless, and the plan's claim that it could not be is wrong.** Every consumer
has the same algebraic shape — lighting_cs computes `(ambient*lightRgb + SUM brdf*lightRgb*shadow) *
exposure`, and the ocean, glass and RT paths all light with `sunColor * exposure` — so the multiplier
moves INTO the colour and out of the trailing multiply for an identical product. `sunIntensity =
exposure`, colour handed out as `GetEffectiveColor()`, `exposure` retired to 1.0. **`ambient` must
be left alone**: the fill term is `ambient * lightRgb`, so it picks the factor up through the colour
already, and scaling it too would square it on every shaded surface.

Proven rather than argued: `--sweep=light.legacySplit:2.0` puts the running scene back into the
legacy configuration, and against the migrated default it measures **mean |delta| = 0.0016/255,
0.11% of pixels touched at all** — i.e. the particle jitter floor, which an unrelated two-run
determinism check independently put at the same level.

> **That probe was REMOVED at P16.2, and the result above is a snapshot of 2026-08-16, not a
> standing invariant.** Re-run on wind_test it now measures **27.6 meanabs against a 0.81 noise
> floor** — the frame 27/255 brighter. Nothing about P4 broke: F8 and F9 landed afterwards and moved
> the ground under it. `lighting_cs` still ends in `color * exposure + skySpecular`, so the legacy
> multiplier now scales the **sky irradiance fill**, which the sun's own intensity does not touch,
> and skips the **sky specular**, which is added after it. Neither term existed when the equivalence
> was measured. A probe that asserts an invariant which no longer holds is worse than no probe.
> P16.2's own equivalence check needs no code: run the level as authored, then with
> `--set=light.sunIlluminanceLux:<the level's own value>`, and the two must be identical.

**What is NOT lossless, and is therefore off by default:**

- `ambientTintedBySun` (default **true** = legacy). Lighting tinted the whole fill by the sun colour,
  so at sunset the shaded side of everything went orange, when in reality a shadowed surface is lit
  by the blue sky it can see and not by the sun it cannot. Unticking it uses `ambientColor`, which
  defaults to `DirectionalLight::DefaultSkyFillColor` — a daylight sky hue rescaled to the SUN's
  luminance, so the switch is a pure hue change that cannot be misread as a brightness bug.
  **Two bugs were shipped here and fixed the same day, both found by the user flipping the boxes:**
  (1) the three fill fields were parsed only inside the `if (contains("sunIntensity"))` branch, so
  on every existing level — all of which still carry the legacy field — the checkboxes were never
  read at all. Only the INTENSITY may branch; the fill fields are orthogonal and are now read
  unconditionally in both load paths. (2) the colour was originally seeded to the effective sun
  colour, making the switch a deliberate no-op; that was over-cautious, and a control that does
  nothing is worse than one that does too much.
  Measured on `shore_grove`, driven through the real level JSON rather than the setters: the darkest
  15% of pixels move **B/R 0.089 -> 0.449** while the frame median holds (0.2077 -> 0.2073), mean
  |delta| 19.1/255. A pure hue change in the shadows, which is exactly the intent. At full blue it
  is too much for this level — around a third to half of the way reads right; that is a taste call,
  hence the colour picker. The picker is shown ALWAYS and merely disabled while the tint drives the
  fill: hiding it until the box was unticked was what made the switch feel dead.
- `unifiedSkyFill` — **built, then REMOVED the same day, because its premise was wrong.** The claim
  was that the ocean's sky fill disagreed with the opaque one. It has no sky fill: in both ocean
  variants the `ambient` value reaches exactly one function, `LitFoamColor`, where it multiplies a
  hardcoded `kSkyColor`. It lights FOAM and nothing else, and the water surface's "ambient" was a
  constant baked into the shader. There was nothing for a boolean to reconcile, and it measured at
  mean |delta| 0.004 for exactly that reason. Deleted rather than kept as a control that lies.

**The hardcoded sky is gone (user request, same session).** `SkyFillRadiance(normal)` samples the
real cubemap at its blurriest mip as a crude irradiance lookup — the same texture the roughness-1
reflection already uses, so it costs one sample and no new binding — falling back to the old
constant only for a black or absent skybox. Applied to **both** ocean variants. Measured on the foam
pixels themselves, with the shoreline band enabled: B/R 0.905 -> 0.886, luma 189.8 -> 187.6. Small,
and honestly so: the foam's sky term weighs only `ambient (0.1) + 0.3 * (1 - n.y)` against a
dominant sun term, and this level's skybox happens to sit close to the constant in both hue and
magnitude. What changes is that the foam now *tracks* the sky at all — under a night or overcast
cubemap it used to stay lit by a daylight blue. Giving the water SURFACE real sky lighting is P5.

Three things this cost, worth remembering:
- **The compiled default is `g_shoreRunup = false`, i.e. the LEGACY ocean** — `ocean_surface_legacy.hlsli`
  is what ships, despite being documented as a byte-faithful baseline of commit `3e54d5d`. Editing
  only the modern variant changes nothing on screen. Both are now edited; that file's "verbatim"
  contract is deliberately broken and the header says so.
- **`wind_test` ships `shoreLegacyContactFoamStrength = 0.0`** — there is no contact foam at all
  until it is raised (~0.1). The canonical `overview` and `shore_grove` views contain almost no
  foam either, so foam measurements taken there measure noise. Use cam `0,9,-128` rot
  `0.1045,0,0,0.9945`: a shallow beach with the band across the whole frame.
- I first normalised the sky sample to the old constant's luminance to protect existing tuning, and
  added a `--ocean-foam-sky-raw` flag to A/B it. Measured difference on foam: ~4/255. Both the
  normalisation and the flag were deleted. Measure before shipping a knob.

**Files this step needed** (per the user's standing rule):
`sources/rendering/lighting/DirectionalLight.h/.cpp` (the model + the migration),
`sources/app/levels/JsonLevel.cpp` and `sources/editor/scene/EnvironmentRuntime.cpp` (both load
paths, branching on `sunIntensity` presence), `sources/app/scene/SceneRenderer.cpp` (4 constant
sites: ocean/glass view constants, lighting_cs, and BOTH RT reflection blocks),
`sources/ocean/OceanRenderable.cpp` (the ocean's own constants — the site that broke the first
attempt), `sources/app/scene/SceneResourceBootstrapper.h/.cpp` (`ambientRgb` field + handle + write),
`shaders/lighting_cs.hlsl` (`ambientRgb` in the CB, fill term uses it),
`sources/editor/ui/InspectorPanel.cpp` and `sources/editor/EditorController.cpp` (controls and
new-object defaults; the serializer needed nothing, it writes `env.properties` verbatim),
`sources/app/App.cpp` + `sources/app/scene/Scene.h` (sweep keys, incl. the `light.legacySplit` and
`light.ambientSkyBlue` measurement probes).

**Level compatibility:** a level carrying only `exposure` is migrated on load; a level carrying
`sunIntensity` is taken at face value and its `exposure` is ignored. The inspector seeds the Sun
Intensity row from the legacy value, so the first drag does not make the image jump. No level file
was rewritten by this step.

**Left for a follow-up:** items 3 and 4 of the list above are delivered as switches rather than as
the new unconditional behaviour, and the spot/point passes still bypass the sun intensity entirely
(they always did). Retiring `exposure` from the level JSON altogether is a mechanical follow-up once
the switches have been signed off.

**Verify:** old-level migration comparison; toggle each control independently; inspect opaque/ocean/glass parity; editor save/reload and undo/redo.

---

### P5 — Integrate physically coherent diffuse and specular IBL — DONE (2026-08-17, uncommitted)

Requirements 1-4 and 6 came with F7/F8. Requirement 5 -- ocean and glass sharing the common
environment's intensity and roughness semantics -- was closed separately, and it is where the last
guessed constants lived:

| consumer | before | after |
|---|---|---|
| compose (opaque) | `rough * kSkyRoughMaxMip` on the display cube | prefiltered cube, `IblMipFromRoughness` |
| ocean, modern variant | `rough * kSkyRoughMaxMip` | same shared mapping |
| ocean, LEGACY variant (the shipping one) | hardcoded **mip 3**, no relation to roughness at all | roughness from the Bruneton **slope variance**, then the shared mapping |
| ocean foam fill | blurriest mip of the DISPLAY cube | the real cosine-convolved **irradiance cube** |
| glass | `rough * 5.0` | same shared mapping |

**The legacy ocean had no per-pixel roughness** -- it is a Bruneton model, which is why a fixed mip
was there in the first place. Rather than invent one, it now uses the physical quantity roughness
stands for and which that model already computes: `sqrt(slopeVarianceSquared)` is an RMS microfacet
slope, clamped to 0.02..0.6 because open water is a near-mirror and the variance spikes on crests.

Every consumer also honours the level's sky intensity now; the ocean sampled the raw cube before,
so one control used to mean two different things depending on which surface you looked at.

**Measured** (identical sky, wind_test, adaptation off): mean |delta| 0.60/255, 10.2% of pixels,
sky background untouched (+0.00 -- it does not go through these paths), water +0.20, island +1.14.
Modest, and honestly so: this scene's water is near-mirror where the old fixed mip was already
close, and it has no glossy props. The change is a semantics fix, not a look upgrade -- it shows on
a roughness sweep, which is what the Verify list below asks for and what the scene lacks.

**Still open, deliberately:** `kSkyRoughMaxMip` survives in compose and the modern ocean as the
fallback for skies with no derivatives. It is no longer used by any sky that has them.

**The Verify list's roughness sweep now exists as a gate**, which closes the one thing this step
could not check on wind_test: `data/levels/roughness_sweep.json` (3 rows x 8 spheres, dielectric /
white metal / copper, against the sky and nothing else) plus `tools/roughness_sweep.py`, which
renders it and asserts that per-sphere high-frequency energy falls monotonically along each row.
Exit 0 = pass. Run it after any change to the bake, the roughness/mip mapping or the split-sum.

---

### P5 (original specification)

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

### P6A — Consume material AO through one indirect-light contract — DONE (2026-08-17, committed)

Its source of truth is F9 in `docs/two_sided_foliage_and_ibl_plan.md`, which is done; see there for
the numbers. Against this step's own four requirements:
1. `GBAux.r` modulates diffuse IBL -- done, in `lighting_cs`, both the irradiance and flat paths.
2. Specular occlusion is view/roughness-aware, not a raw AO multiply -- `IblSpecularOcclusion`.
3. Direct sun, local lights, emissive and the sky background are untouched -- by construction, the
   term is applied only to the indirect diffuse fill and the fallback sky reflection.
4. AO = 1 is screenshot-equivalent -- proven analytically over the whole domain, and no shipped
   material authors a non-unit AO.

---

### P6A (original specification)

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

### P6B — Add dynamic GTAO with edge-aware temporal filtering — COMPLETE (items 1-8, 2026-08-17)

**P6B IS COMPLETE (items 1-8).** What follows was written as the steps landed; the consumption
section at the end covers items 6-7.

**The producing side (items 1-5, and 8).** The chain
runs raw estimate -> bilateral denoise -> temporal accumulation -> edge-aware upsample and lands a
render-resolution AO target that **nobody samples**. That is the point: every step so far is
measurable and none of them can change a pixel, so items 6-7 are a pure consumption change against
a receiver F9 already built.

| item | state |
|---|---|
| 1. GTAO at half res from linear depth + geometric normal | **done** |
| 2. radius in world/view units, not pixels | **done** |
| 3. bilateral denoise on depth/normal discontinuities | **done** |
| 4. temporal accumulation + disocclusion rejection | **done** |
| 5. edge-aware upsample to lighting resolution | **done** |
| 6. combine with material AO by a documented, bounded rule | **done** |
| 7. apply only to indirect diffuse + specular occlusion | **done** |
| 8. raw / denoised / history-weight / combined debug views | **done for all four stages** |

**Implementation.** Four compute kernels, one render-graph pass.

| stage | shader | source |
|---|---|---|
| raw | `shaders/gtao_cs.hlsl` | UE `GTAOCombinedPSandCS` + `SearchForLargestAngleDual` + `ComputeInnerIntegral` |
| denoise | `shaders/gtao_filter_cs.hlsl` | UE `GTAOSpatialFilterCS`, minus the LDS staging, plus a normal term |
| temporal | `shaders/gtao_temporal_cs.hlsl` | UE `GTAOTemporalFilterPSAndCS` + `ReadHistoryClamp` |
| upsample | `shaders/gtao_upsample_cs.hlsl` | **not** UE — joint bilateral (see below) |

*Raw*: for each of N screen directions, walk outward in BOTH directions for the largest elevation
angle any sample subtends, then project the normal into that walk's plane and evaluate the visible
arc in closed form. That closed form is what makes it "ground truth" rather than the
hemisphere-sampling guess SSAO does.

*Denoise*: **the bilateral weight is against a fitted plane, not against the centre depth**, and
**the plane is fitted in DEVICE z**. Both are UE's and both matter: a floor at a grazing angle has a
huge depth range inside a 5x5 window, so a plain `|z - zCentre|` test refuses to blur it exactly
where the noise is worst; and device z is an affine function of `1/viewZ`, which interpolates
linearly in screen space, so a plane really is a plane in device z and the two-tap extrapolation is
exact for one. The tolerance is authored in WORLD metres and converted per pixel
(`metres * depthB / linearZ^2`), because a fixed device-z tolerance means a different physical
distance at every depth. Added beyond UE: a **normal** term, since the plan asks for normal
discontinuities and depth alone cannot separate the two faces of a convex edge.

*Temporal*: the raw pass already rotates its direction set by `frameIndex`, so consecutive frames
estimate the same occlusion from different directions — averaging them is what buys a 2x6-tap pass
the quality of a much wider one. Reprojection is by `gbVelocity` alone (every G-buffer variant here
writes velocity for every pixel, sky included, so UE's `ClipToPrevClip` fallback would only be a
second disagreeing source of truth). Disocclusion is UE's test — sample the velocity at the SOURCE
location and ask whether it moves like this pixel — **with one correction**: their `CompareVeloc`
sums the components (`abs(V12.x + V12.y)`), which cancels for a difference of `(+a, -a)`, i.e.
reports perfect agreement for two velocities 90 degrees apart. Uses `length()`.

*Upsample*: deliberately NOT UE's. Their shipping `GTAOUpsamplePSAndCS` is a five-tap box average
with no depth term, and the `SmartUpsample` that would have been the reference is inside `#if 0`.
This is the standard joint-bilateral upsample with depth as the guide, plus a nearest-depth fallback
for a destination pixel no half-res texel sampled.

**Structure.** All four dispatches record into ONE command list under one `AddPass2` pass: the
builder decides the chain once, declares each stage's UAV->SRV flip at its own barrier point, and
the record body emits exactly those points with `EmitPoint`. Four dispatches of ~0.03 ms do not each
deserve a command list; this is the same shape the reflection blur uses to ping-pong its two
dispatches inside one pass. **A body that early-returned half way would leave declared points
unemitted**, so every gate — materials, CB sizes, descriptor handles — is evaluated in the builder.

**AN ENGINE INVARIANT THIS STEP DISCOVERED THE HARD WAY (user hit it as a debug-layer break in the
texture inspector).** `Renderer::RenderImGui` displays a preview with
`TransitionExplicit(cl, res, GetCanonicalState(res), PIXEL_SHADER_RESOURCE)` — it uses the resource's
CANONICAL state as the before-state and never transitions back. That is only sound because
`NON_PIXEL_SHADER_RESOURCE` and `PIXEL_SHADER_RESOURCE` share one enhanced-barrier layout
(`D3D12_BARRIER_LAYOUT_SHADER_RESOURCE`). So: **a deferred target that can be shown in the inspector
must rest in a state whose layout is SHADER_RESOURCE.** `gtaoUpsampled` had been made to rest as a
UAV — saving one barrier, and honestly reflecting where the frame left it — which put the inspector's
transition into a different layout and left the next frame's compiled barrier claiming
`UNORDERED_ACCESS` against a resource already in `SHADER_RESOURCE`: `INCOMPATIBLE_BARRIER_LAYOUT`.
It now rests NPS and `Pass_Gtao` transitions it back at the end of its chain. Note the fullscreen
debug blit does NOT go through that path, which is why every headless run was clean while the
inspector broke.

**And then the general fix, which is where it belonged**: `RenderImGui` now transitions every preview
resource BACK to its canonical after `imguiLayer_.Render`. That removes the constraint entirely
rather than restricting what a target may rest in — `tonemap` and `fxaa` also rest as UAVs and are
also in the inspector's list, so the same break was reachable through them and is now closed too.
The graph decides resting states; an out-of-graph peek borrows and returns.

**Targets.** `gtao` (raw), `gtaoFiltered`, `gtaoHistory`, all half render res, plus `gtaoUpsampled`
at render res. `gtaoHistory` is the one deferred target read ACROSS frames:
`Renderer::GetDeferredForPrevFrame()` returns frame N-1's set (the frame index cycles in order, so
it is genuinely the previous frame). Nothing about that needs special handling — the compile sees
UAV -> SRV inside the pass and SRV -> UAV two frames later, from ordinary declarations, and the
fixed-point property the barrier cache needs holds because the resource begins and ends each frame
shader-readable. `historyValid` is 0 on the first frame after a resize, a level switch or the stage
being switched on, and the kernel then seeds from this frame instead of reading a texture that was
never written.

**Settings live on `SceneRenderSettings::gtao`, and the app layer owns them.** `AppController`
re-pushes the whole struct into the Scene every Tick, so a live change (the `--sweep` harness, the
developer window) must write `AppController::SettingsRef()`. Writing `Scene`'s copy survives
exactly one frame -- that is why there is no mutable accessor there.

**A NORMALISATION BUG THE FIRST MEASUREMENTS HID, found because the user asked what the diagonal
banding in the raw debug view was.** UE scale the arc integral by `2/PI` at the end of the raw pass
(`PostProcessAmbientOcclusion.usf:908`) and then multiply by `PI/2` again at the top of their
SPATIAL FILTER (`SumAO *= (PI/2.0)`). The pair cancels. Transcribing only the first half left the
whole chain darkened by a constant **1/1.571**: an open beach read AO **0.637**, i.e. exactly `2/PI`,
where an unoccluded surface must read 1.0. Nothing downstream would have flagged it — the AO "worked",
it was just uniformly too dark, and consuming it at item 7 would have tinted the entire frame.

Fixed by dropping the `2/PI` in the raw kernel rather than reintroducing `PI/2` in the filter,
because this engine's denoise stage is OPTIONAL (`gtao.denoise`) and `intensity` is applied in the
raw kernel: a normalisation that only holds when a later optional pass runs is one waiting to be
switched off. **Every stage now carries AO in [0,1] with "unoccluded" meaning exactly 1.**

Verified two ways. Simulating the kernel against an analytic unoccluded plane returns **0.9999** for
the bare integral at every tilt (0, 30, 60 degrees) and depth (10 m, 60 m). On the engine, flat sand
went from linear AO 0.6412 to **0.9619** — the shortfall from 1.0 being genuine occlusion from the
surrounding palms. The give-away in the old numbers, in hindsight, was `occluded %` sitting at
**95%** on a frame that is mostly open beach; it now reads **58%**.

*(Note for reading any of these captures: the debug blit writes to an sRGB backbuffer, so an 8-bit
value of 208 is linear 0.816, not 0.816 of the AO range. The measurements below are in the capture's
8-bit space, which is fine for relative comparisons and wrong for absolute AO.)*

**Measured** (wind_test, 2560x1440, defaults 2 angles x 6 steps, 5x5 filter, blend 0.1). Each stage
captured through the debug blit and measured as *hf* = mean `|x - boxblur(x)|`, the noise the
filters exist to remove:

| stage | mean | hf (noise) | p01 | occluded % | grad p99.9 |
|---|---|---|---|---|---|
| raw | 0.9764 | 0.00699 | 0.788 | 28.54 | 0.139 |
| + denoise | 0.9768 | **0.00411** (-41%) | 0.824 | 26.54 | 0.114 |
| + temporal | 0.9735 | **0.00232** (-67% cumulative) | 0.867 | 35.32 | 0.055 |
| upsampled (render res) | 0.9733 | 0.00323 | 0.859 | 35.09 | **0.080** |

The mean moves by 0.003 (0.8/255) across the whole chain: these are denoisers, not a gain change.
The p01 tail RISES while `occluded %` does not fall — the noise floor's dark excursions are being
averaged away, not the contacts. **The upsample's rise in `hf` and peak gradient is the point, and
the A/B proves it is edge-awareness rather than just more pixels**: same source data, only
`gtao.upsampleTolerance` changed, 0.02 (edge-aware) vs 1000 (degenerates to plain bilinear) gives
grad p99.9 **0.080 vs 0.049** — 64% more edge preserved.

**THE NORMAL SOURCE WAS WRONG, and it cost more than everything else on this page combined.** The
user asked whether a dark wedge across an open dune was expected. It was not: on that view the AO
read a median of **0.352** and a 90th percentile of **0.638** on a surface with nothing above it.

`r.GTAO.UseNormals` defaults to **0** in UE — they derive the normal from the DEPTH buffer, and only
optionally read the G-buffer. This implementation read the G-buffer from the start, which means it
fed the arc integral a NORMAL-MAPPED normal while the horizon search walks bare depth. The integral
then computes the visible arc of a surface the search never looked at, so wherever the shading
normal tilts away from the geometric one, part of the hemisphere falls "below the surface" and reads
as occlusion. On a detail-mapped surface like sand that is not a subtle bias — it is most of the
signal. Switched to the depth-derived normal (UE's `TakeSmallerAbsDelta` cross-product
reconstruction), kept behind `gtao.useGBufferNormal` purely so the two can be compared:

| view | metric | G-buffer normal | depth-derived |
|---|---|---|---|
| dune (open slope) | median AO | 0.352 | **1.000** |
| dune (open slope) | p10 AO | 0.040 | **0.956** |
| palms on sand | pixels below 0.9 | 48.0% | **15.9%** |
| palms on sand | pixels below 0.6 | 13.1% | **0.19%** |
| palms on sand | p01 (deepest contacts) | 0.227 | 0.701 |

The last row is the one that says this is a fix and not just a brightening: the deepest contacts are
still occluded, they just no longer sit inside a scene-wide false darkening. Raw noise also fell by
more than half (hf 0.01537 -> 0.00699) — the G-buffer normal was injecting the normal map's own
high-frequency detail into an occlusion estimate.

**The regular grid visible in the RAW debug view is the sampling pattern, and it is supposed to be
there.** Characterised because it looked alarming: period 8.56 display px, stripes at 16.5 degrees,
a smooth wave of 12.4 levels peak-to-peak (so a real signal, not 8-bit quantisation — the patch
occupies 40 distinct levels). It moves every frame from two deliberate sources: `frameIndex` rotates
the direction set (which is precisely what gives the temporal stage something to average — a
stationary pattern would average to itself), and DLSS's sub-pixel jitter moves the depth buffer the
search reads. Neither is a defect and neither is a transcription error: `r.GTAO.NumAngles` is 2 in
UE too, the pass order matches (search -> spatial -> temporal -> upsample), and the radius clamp is
theirs verbatim. **The chain removes it**: peak-to-peak 12.4 -> 1.47 levels, and frame-to-frame
motion on flat sand goes from **2.921/255 in raw to 0.000/255 at the end of the chain** over 6 frames
with a static camera and frozen wind.

**Cost** (GPU, same run each time, `--profdump`):

| config | `Pass_Gtao` | GPU.Frame |
|---|---|---|
| off | — | 2.699 ms |
| raw only | 0.087 ms | 2.856 ms |
| + denoise | 0.104 ms | 2.858 ms |
| full chain | **0.116 ms** | 2.872 ms |

**Feature-off equivalence.** Measured on `roughness_sweep` (a static level — wind_test's ocean and
wind sims are wall-clock driven, so two runs of it differ by more than this change does, and
comparing across them measures the harness): off vs on **0.0334/255**, against an off-vs-off control
of **0.0656/255**. The change is half the harness's own run-to-run floor.

**DISTANT FLICKER, reported by the user from the `GTAO combined` view, and what it turned out to be.**
Reproducing his exact viewpoint (`--cam-pos` / `--cam-rot` from the HUD) and measuring frame-to-frame
standard deviation by distance band, with a static camera and frozen wind so any motion is the
estimator's own:

| stage | far (y 25-50%) | mid | near (y 75-100%) |
|---|---|---|---|
| raw | 9.763 | 5.432 | 4.376 |
| denoised | 7.436 | 0.980 | 0.699 |
| temporal | 4.729 | 0.181 | **0.063** |

Near the camera the chain kills flicker by a factor of 70; at distance only by two. Three candidate
causes were tested and two were wrong:

* **Not the estimator's parameters.** `numSteps` 12, `numAngles` 4, `worldRadius` 2.0 and
  `temporalBlendWeight` 0.03 all moved far-band flicker by under 6%; two of them made it worse.
* **Not the direction-rotation cycle**, though that WAS a real transcription error and is now fixed.
  UE take the per-frame rotation from a six-entry table (`Rots[Frame % 6]`, i.e. 30/150/90/120/60/0
  degrees visited out of order) and the offset from a four-entry one; I had written
  `(frameIndex & 63) * PI/64`, a 64-frame ramp walked in order, so a ~10-frame history saw a narrow
  sliding subset of directions instead of a complete spread. Correcting it to their tables changed
  far flicker by 3.5% (5.227 -> 5.044) — worth having for fidelity, but it is not the cause, and it
  is recorded here as such rather than presented as the fix.
* **It is DLSS's jitter, and the temporal clamp was too tight to absorb it.** With DLSS off, far-band
  flicker on the combined target drops from **4.555 to 1.294**. The depth buffer moves by a sub-pixel
  every frame (correctly — the AO reconstructs from the same jittered projection it was rendered
  with), so at distance, where the depth gradient is steep and the geometry is thin, each frame is a
  materially different estimate. UE's `RangeVal` of 0.1 is a window our per-frame spread simply
  exceeds, so the history was being clamped back onto each noisy frame instead of accumulating.

`temporalClampRange` is now a setting (`gtao.temporalClampRange`, sweep key and slider), **default
0.35, a measured deviation from UE's 0.1**: far flicker 0.1 -> 4.600, 0.35 -> 2.935, 1.0 -> 2.501.
1.0 is barely better than 0.35 and removes the clamp entirely, so 0.35 is where the curve bends.
Checked for the obvious cost — ghosting — two ways: on wind-moved foliage, per-frame detail falls
3.84 -> 2.76 -> 2.58 across the three settings, but with frozen wind (so the time-average IS the
true structure) the structure's own detail reads 2.96 / 2.50 / 2.66, i.e. non-monotonic and flat.
What the wider window removes is noise, not structure. Shipped result by band:

| | y 0-25% | y 25-50% | y 50-75% | y 75-100% |
|---|---|---|---|---|
| clamp 0.1 | 4.617 | 5.227 | 0.623 | 0.067 |
| clamp 0.35 | **2.767** | **3.081** | **0.390** | **0.043** |

**Still open:** this is measured with a STATIC camera. Under camera motion `range` closes to zero by
design (`lerp(range, 0, velocityMag)`), so the temporal stage disables itself exactly when
reprojection is least trustworthy — the flicker will return in motion, and whether that matters is a
judgement to make after item 7, when the AO is modulating indirect light and passing through DLSS's
own temporal filter rather than being viewed raw.

**ITEMS 6-7: THE COMBINE RULE AND THE CONSUMERS.**

The rule is UE's, from `DiffuseIndirectComposite.usf:371`
(`lerp(1, MaterialAO * DynamicAO, AOMask * AmbientOcclusionStaticFraction)`): **a product**. That is
the right shape because the two terms describe INDEPENDENT occluders — the material's own cavities,
which no depth buffer can see, and the geometry around the pixel, which no baked map can see. It is
bounded in [0,1] by construction, monotonic in both inputs, and an exact identity when either input
is 1. (`min` was the alternative: it never double-counts, but it also refuses to let a cavity deepen
a real contact, which is the case this pass exists to render.)

**One deliberate deviation.** UE's `AmbientOcclusionStaticFraction` damps the WHOLE product. Here
`gtao.strength` damps only the DYNAMIC term, because the material term already shipped in F9 and is
not this step's to switch off. That distinction is not cosmetic — it is what makes `strength = 0` an
exact no-op against the pre-P6B build, and the sweep level's AO row is what caught the first version
getting it wrong (damping the product moved that row by **177/255**).

Applied in two places, both on INDIRECT light only:
* `lighting_cs` multiplies the indirect diffuse fill (irradiance cube and flat fallback alike).
* `compose_cs` feeds `IblSpecularOcclusion(NoV, combined, roughness)` on the fallback sky. RT/SSR
  hits are still left alone — they traced the geometry the AO stands in for.

Direct sun, spot, point and emissive are untouched, per the plan.

**Ordering.** `Main_Lighting` now takes `Main_Gtao` as a prerequisite. Until this step the AO pass
was a leaf nobody depended on and the two were free to run concurrently — correct only while nothing
read the result. Compose inherits the ordering transitively.

**Measured.** Feature-off equivalence on the static sweep level: `strength = 0` vs `gtao.enabled = 0`
is **0.0613/255** against an off-vs-off control of **0.0589/255** — an exact no-op within the
harness floor. Applied on wind_test (same binary, frozen wind, `strength` 0 vs 1): mean |delta|
**1.735/255** over 93.6% of pixels, strongest in the canopy (2.077) and weakest on open sand
(1.516), with overall luminance moving **-0.03%**. That last number is the point: this darkens
contacts, it does not dim the scene.

**A BUG THIS STEP EXPOSED IN THE RENDER GRAPH ITSELF.** `ResourceStateDeclList` was
`inl_vector<ResourceStateDecl, 10>`, and the VSM-mode lighting pass declared exactly 10 states.
Adding the AO target made it 11. `inl_vector` only ASSERTS on overflow — so Debug aborted (exit 3,
no message, since RendererInvariantFailure only writes to OutputDebugString) while **Release
silently wrote past the inline storage and looked fine**. Capacity is now 16, sized with headroom
rather than to the current maximum. Worth remembering: a Release build that passes every gate proves
nothing about a container whose bounds check compiles out.

**KNOWN LIMITATION, item 2 is weaker than it reads.** `worldRadius` has little authority at its
default: 0.2 and 0.75 are indistinguishable on sand (8-bit 208.39 vs 208.35) and only 4.0 moves it
(203.16). The cause is the radius clamp,
`pixelRadius = max(min(worldRadiusAdj / linearZ, 256), numSteps)` — the lower bound in `numSteps`
means that beyond a modest distance the radius is fixed in PIXELS, not world units, which is the
"scene breathing" item 2 exists to prevent. The clamp is UE's verbatim
(`PostProcessAmbientOcclusion.usf:875`), but this pass runs at half resolution, so in full-frame
terms the floor bites twice as hard as it does for them. Not addressed here; it wants its own
measurement against a moving camera, since the symptom is temporal.

**Inspect it**: texture debug viewer -> Lighting -> "GTAO" / "GTAO denoised" / "GTAO temporal" /
"GTAO combined". Headless, the fullscreen debug blit now takes a target:
`--set=gtao.enabled:1;debug.texMode:1;debug.tex:N` with N = 1..4 (0 = the cascade shadow atlas it
was hardwired to). Sweep keys: `gtao.enabled/worldRadius/thickness/intensity/numAngles/numSteps`,
`gtao.denoise/temporal/filterRadius/filterPlaneTolerance/temporalBlendWeight/upsampleTolerance`.

**Two pieces of harness this step added, because without them items 3-5 could not be measured:**
* **`--set=<name>:<value>[;...]`** pins settings for a whole run using the same name table `--sweep`
  uses. `--sweep` varies exactly one setting, so an A/B needing two switches at once (a feature on
  AND the debug view that shows it) previously required editing defaults and rebuilding.
* **`SceneRenderSettings::debugTexTarget`** — the fullscreen debug blit used to be hardwired to the
  cascade shadow atlas, so a half-res intermediate could only ever be judged by eye in the GUI. It
  now also DECLARES the texture it samples; it never did, and got away with it because the shadow
  atlas happened to be readable.

**What is NOT verified here, and needs the user's eye:** camera-motion behaviour. A headless shot
has a static camera, so the disocclusion path is exercised only by wind-moved foliage. The plan's
"no camera-motion crawling" criterion is judged by moving the camera in the editor with the debug
view on "GTAO temporal".

---

### P6B (original specification)

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

### P6C — Build an HZB depth pyramid — COMPLETE (steps 1-6, 2026-08-18)

**Depends on:** nothing. **Consumers:** P6B's horizon search, the existing SSR march, and P9's
screen-space GI — which is the reason it is worth building rather than a micro-optimisation.

**Why.** The engine has no depth pyramid at all today (checked: no HZB, no depth mips, nothing for
occlusion culling either). Three places want one:

* **P6B GTAO.** UE's shipping horizon search is `SearchForLargestAngleDual_HZB` — at large step
  radii it reads a coarser mip instead of mip 0. Two effects: cache locality (consecutive taps at a
  large radius land far apart and miss), and aggregation (a coarse mip averages, so the estimate
  stops aliasing). Ours reads flat depth, which is recorded as a known divergence in P6B.
* **SSR.** `ssr_cs` marches a fixed coarse loop then refines. A pyramid turns that into a proper
  hierarchical march: fewer steps AND fewer missed intersections, which is a quality win, not only
  a speed one. Needs the CLOSEST pyramid, not the one GTAO wants — see (3).
* **P9 SSGI.** The whole `SSRT*` set in the reference drop is built on HZB — there it is part of the
  tracing algorithm, not an optimisation.

**Honest scoping note.** On P6B alone this would buy close to nothing: the pass costs 0.116 ms at
2 angles x 6 steps, and the remaining flicker comes from reprojection under camera motion, not from
depth sampling. It is queued as INFRASTRUCTURE for the three consumers together, and P6B should be
retrofitted onto it rather than the pyramid being justified by P6B.

**Implement:**

1. A `hzb` target: R32_FLOAT (or R16), half render resolution at mip 0, full mip chain down to 1x1.
2. One compute pass after the G-buffer that reduces the depth buffer into mip 0, then successive
   mips. Prefer a single-dispatch multi-mip reduction over one dispatch per mip.
3. Decide and DOCUMENT the reduction operator. **This was stated wrong when the step was queued and
   is corrected here.** With reversed-Z, `min(deviceZ)` is the FURTHEST surface, not the closest, and
   UE build TWO pyramids for that reason:
   * **Furthest** (`min` deviceZ) — what `GetHZBParametersForAO` binds, i.e. what a horizon search
     wants. Taking the furthest depth in a tile UNDER-estimates occlusion, so a coarse mip cannot
     invent contact shadows from geometry too small to matter at that scale.
   * **Closest** (`max` deviceZ) — what `HZBTracing.ush` binds, i.e. what a ray march wants, so a
     step cannot tunnel through a surface the tile does contain.
   Our consumers therefore do NOT share one pyramid: GTAO needs Furthest, SSR and P9 need Closest.
   Build **Furthest first** (GTAO is the only consumer that exists today) and add Closest with the
   SSR retrofit — the kernel writes one more UAV, so the second pyramid is an addition, not a
   rewrite. Do not build a pyramid nobody reads yet.
4. Handle non-power-of-two sizes explicitly — the classic source of a half-texel drift that only
   shows at the frame edge.
5. Retrofit the GTAO horizon search onto it (`SearchForLargestAngleDual_HZB`), selecting the mip
   from the step radius the way the reference does.
6. Retrofit the SSR march — which first needs the Closest pyramid from (3).

**Interface contract:** disabling HZB consumption is screenshot-equivalent to the current build.
The pyramid is built whether or not anyone consumes it, so the build cost is measured separately.

**BUILT AND VERIFIED (steps 1-4).** `shaders/hzb_build_cs.hlsl`, one dispatch per level inside one
render-graph pass (`Main_Hzb`, ordered after the G-buffer). Mip 0 is half the render resolution --
deliberately the same grid the GTAO chain already runs on, so a horizon search can move between "the
depth I sampled" and "the tile that contains it" without a second mapping. R32_FLOAT rather than UE's
fp16: the pyramid stores DEVICE Z, whose useful precision under reversed-Z sits near 0, which is
exactly where 16-bit floats are coarsest.

**The whole chain lives in UNORDERED_ACCESS for the duration of the build.** This engine's barrier
layer transitions whole resources (ALL_SUBRESOURCES), so it cannot hold mip N-1 in SHADER_RESOURCE
while mip N is a UAV. Reading the source mip through its own UAV sidesteps that completely and costs
nothing -- a RWTexture2D is readable -- and the levels are separated by UAV barriers rather than
transitions. One dispatch per mip instead of UE's four-mips-per-dispatch groupshared reduction: the
levels are tiny, the barriers are cheap, and this version can be checked line for line against a CPU
reduction. The batched form is a drop-in replacement for that loop if the build ever measures as
significant.

**Odd extents are handled explicitly** (plan item 4): halving an odd dimension drops a row or column,
whose texels would then never reach any mip -- a silent hole at the frame edge that only appears at
particular resolutions. The last texel along an odd axis folds the leftover neighbours in.

**THE INVARIANT, and it passes exactly.** Mip N+1 must be the 2x2 min-reduction of mip N:

| check | texels | exact |
|---|---|---|
| mip 0 -> 1 | 230400 | **100.00%** |
| mip 1 -> 2 | 57600 | **100.00%** |
| mip 2 -> 3 | 14400 | **100.00%** |
| mip 3 -> 4 | 3600 | **100.00%** |
| mip 4 -> 5 | 880 | **100.00%** |

max error 0 throughout. Mip 0 against the depth buffer itself is **100.00%, max error 0** on a level
with no transparent geometry.

**Two measurement traps this hit first, both worth remembering:**
* The first run scored 75-88%, and the fault was the MEASUREMENT. The debug blit stretched each mip
  with a LINEAR sampler, so no sample ever landed on a texel centre. The blit now uses POINT for the
  depth-like targets, and the same comparison became exact. (The pow() stretch it applies is
  deliberately MONOTONE, which is what makes checking a min-reduction on the displayed values
  equivalent to checking it on the raw ones.)
* Mip 0 vs depth reads 93% on wind_test, and that is CORRECT, not a bug: the pyramid is built right
  after the G-buffer, while the blit shows the FINAL depth, into which the transparent pass has since
  written the ocean. The mismatches are strictly one-sided (the pyramid is always the further of the
  two) and vanish on a level without transparency. Also: the mips must be captured in ONE process
  with `--dlss=off`, or DLSS's per-frame jitter makes two captures two different depth buffers.

**Cost:** `Pass_Hzb` **0.030 ms** GPU / 0.064 ms CPU at 2560x1440 with DLSS off (11 levels from
1280x720). GTAO for scale, same frame: 0.111 ms.

**Nothing consumes it yet** -- the only reader is the debug blit -- so the interface contract holds
trivially and steps 5-6 (the GTAO and SSR retrofits) are separately measurable.

**Inspect it:** developer window -> Debug -> Fullscreen debug texture -> "HZB (depth pyramid)", with
the "HZB mip" slider; "Scene depth" is next to it for comparison. Headless:
`--set=debug.texMode:1;debug.tex:5;debug.texMip:N` (and `debug.tex:6` for depth).

**STEP 5 DONE: THE GTAO RETROFIT, AND IT PAYS FOR THE PYRAMID.** The horizon search now reads the
pyramid, taking a coarser mip the further a step reaches -- UE's schedule exactly
(`SearchForLargestAngleDual_HZB`): the bias, +1 at the third tap, +2 from the fifth on. Two effects,
and the second is the one that matters more than it sounds: far steps stop scattering across memory,
and a coarse level AGGREGATES, so the estimate stops aliasing off whichever single texel a long step
happened to land on.

Measured on wind_test at 2560x1440 with DLSS off, same binary, `gtao.useHzb` 0 vs 1:

| | Pass_Gtao | + Pass_Hzb | hf (noise) | pixels below 0.9 | p01 |
|---|---|---|---|---|---|
| flat depth | 0.337 ms | 0.373 ms | 0.01166 | 18.41% | 0.509 |
| **HZB, bias 0** | **0.263 ms** | **0.298 ms** | **0.01039** | 15.75% | 0.552 |
| HZB, bias 1 | — | — | 0.00870 | 12.27% | 0.610 |

**-22% on the AO pass, and -20% even counting the pyramid's own build** -- so the pyramid pays for
itself with one consumer, before SSR or P9 touch it. Noise falls 11% as well.

The occlusion does thin slightly (18.4% -> 15.8% of pixels below 0.9), and that is the pyramid
working as designed rather than a regression: it stores the FURTHEST depth per tile, so a coarse
level under-estimates rather than inventing contacts. `hzbMipBias` is the dial on that trade --
bias 1 is cheaper and smoother still, but thins occlusion to 12.3%, which is why the default is 0.

**Also changed here:** the pyramid now RESTS shader-readable (`Pass_Hzb` transitions back at the end
of its build) instead of resting as a UAV, because it has a real SRV consumer now -- and because a
target the texture inspector can show must rest in a state whose barrier layout is SHADER_RESOURCE.

**A measurement trap worth recording:** the first A/B was shot from the dune viewpoint and showed
0.01/255 difference. That view has almost no occlusion to begin with (1.7% of pixels below 0.9), so
there was nothing for either method to disagree about. Re-measured on the default view, where the
palms actually occlude, and the difference is real.

**Knobs:** `gtao.useHzb` (default ON) and `gtao.hzbMipBias` (default 0), in the developer window, the
editor inspector, the level JSON and the `--set`/`--sweep` harness.

**STEP 6 DONE: THE SSR RETROFIT. 3.1x CHEAPER *AND* IT FINDS MORE.**

The second pyramid exists now -- same dispatch, one more `max`, its own target. The CLOSEST
reduction (max device Z = the NEAREST surface in a tile) is what a ray march needs and the exact
opposite of what the horizon search needs: if a march read the furthest depth, a tile holding a near
railing and a far wall would report the wall and the ray would tunnel through the railing. The
closest chain is built ONLY on frames a screen-space march is the active reflection source
(`writeClosest`), from ONE flag (`SceneRenderer::ssrHizActive_`) shared by the build and both SSR
dispatches -- two independent evaluations of "is HiZ on" is how a pass ends up tracing a chain
nobody filled in.

`shaders/ssr_trace_hiz.hlsli` is UE's `TraceHZB` (HZBTracing.ush) transcribed, not approximated:
stackless traversal walking tile boundaries, ascending a mip on every skipped tile and descending on
every candidate, ending when it drops below mip 0. UE's shipping constants for reflections
(`MaxIterations` 50, `RelativeDepthThickness` 0.005). It reuses `BuildSsrHit` from the log march, so
a technique A/B compares the SEARCH and nothing else. **The Lettier tracer was deleted** in the same
change: LogMarch strictly dominated it and a third path made every SSR comparison a three-way.

Measured on `data/levels/ssr_bronze_palms.json`, 2560x1440, DLSS off, GTAO off, exposure locked,
same binary:

| | Pass_ReflectionSource | Pass_Hzb | both | GPU.Frame | SSR pixels with a hit | mean ray visibility |
|---|---|---|---|---|---|---|
| Log March | 0.108 ms | 0.035 ms | 0.143 ms | 0.770 ms | 22.28% | 0.0614 |
| **HiZ** | **0.035 ms** | 0.043 ms | **0.078 ms** | **0.702 ms** | **23.85%** | **0.0730** |

**3.1x off the reflection pass, -45% counting the second pyramid it needs, -8.8% of the whole GPU
frame** -- and it is not paying for that with misses: **7.0% more pixels find a hit**, mean ray
visibility is up 19%, and fully-visible pixels go from 0.22% to 0.52%. This is the
quality-*and*-speed case the step was queued on, and the first time the pyramid pays for itself
twice. 17.48% of pixels change against a noise floor of 0.0043%.

**THE MEASUREMENT NEEDED THREE NEW THINGS, and none was optional:**
* **`--set=render.reflectionSource:N`** (0 None, 1 SkyOnly, 2 SSR, 3 RT). The default is RT and this
  machine has RT hardware, so the screen-space path NEVER RUNS by default -- there was no headless
  way to exercise, measure or gate SSR at all. The first "HiZ" capture of this step had quietly been
  tracing the TLAS. Paired with `--set=ssr.technique:N` for the A/B inside one binary.
* **Debug target 8 = the reflection buffer's ALPHA**, i.e. the ray's own visibility. Inferring
  coverage from the composited frame does not work: an attempt to mask "reflective pixels" by
  differencing against SkyOnly marked 94% of the frame, because SkyOnly also changes the
  sky-specular fallback on every surface. The hit mask answers the question before shading, the
  glossy blur and compose can launder it -- and it is bit-identical across processes, which is what
  makes the coverage numbers above exact rather than indicative.
* **`data/levels/ssr_bronze_palms.json`.** A screen-space ray only shows a difference where there is
  a wide glossy near-horizontal reflector AND thin geometry standing on it; no existing level had
  both (wind_test's sand is not a mirror, roughness_sweep's spheres have nothing to reflect).
  Polished bronze floor, ~60 palms in three rings, wind strength 0, no ocean, exposure locked.

**PETER PANNING, and it was a transcription bug of exactly the kind [[transcription-half-a-pair]]
warns about.** The user spotted it by eye before any metric did -- reflections detached from the base
of every trunk, floating. Two halves of UE's self-intersection slack do not survive the move:
1. UE fade the slack over the first 10% of the RAY (`saturate(t*10)`). Fine for a short probe ray;
   here a grazing reflection across a mirror floor is clipped to the whole screen, so 10% of it is
   metres of world and every genuine contact inside that stretch is suppressed. Fixed by fading over
   TEXELS TRAVELLED from the origin (3 texels), which is what the artefact is actually about -- it
   is local to the reflector's own neighbourhood.
2. UE scale device Z (`tileZ *= 0.99`). Device Z is not linear in distance, so a fixed percentage is
   a hair near the camera and a chasm at range. Converted to a view-space push of the same relative
   size: `deviceZ' = depthA + (deviceZ - depthA) / (1 + slack)`.

**THE PYRAMID INVARIANT, and it passes exactly.** The closest pyramid must be >= the furthest one
everywhere (max >= min), and both must bracket the depth buffer:

| | closest >= furthest | strictly greater | worst violation |
|---|---|---|---|
| mip 0 | **100.000%** | 8.22% | 0 |
| mip 1 | **100.000%** | 17.10% | 0 |
| mip 2 | **100.000%** | 25.73% | 0 |

"Strictly greater" rising with the mip is the pyramid working: a coarser tile spans more depth. The
means bracket correctly too -- furthest 67.47 < depth 70.32 < closest 72.73, same monotone stretch.

**THREE MEASUREMENT TRAPS IN ONE STEP. Suspect the measurement first.**
* The invariant above first read 24% and looked like a broken reduction. It was the debug blit: its
  depth-stretch list was hardcoded `target == 5 || target == 6`, the new target 7 was not in it, so
  the closest pyramid blitted raw and came out with a range of 0..2/255. One predicate now covers
  all three depth-like targets.
* **A DEBUG RUN THAT NEVER RAN.** The first Debug gate on this level asserted at startup and wrote no
  screenshot, and the `barrier_diag.log` read afterwards had been written by the OTHER run in the
  same command -- so a "clean barrier gate" was reported for a path that had not executed. The
  assert was the level's own doing (`"ocean": {"enabled": false}` -- the loader takes a BOOLEAN to
  switch the ocean off, and an ocean OBJECT without a preset path trips an assert; Release swallows
  it and renders correctly, which is how a level looks fine and is still malformed). **Check the
  artefact exists, not just that the log looks clean.**
* **The shaded frame is not deterministic, and the first pixel statistics were near the noise
  floor.** Two IDENTICAL runs differed on 8.8% of pixels. `--shot-delay` is wall-clock seconds, so
  each run captures a different frame index, and two things are frame-indexed: auto-exposure (a
  temporal feedback loop) and GTAO's rotating noise + temporal history. With exposure locked and
  GTAO off the floor drops to 0.0043% / max 5. **The deterministic recipe is
  `--dlss=off --set=gtao.enabled:0` on a level with auto-exposure off** -- without it, any shaded
  A/B smaller than ~9% of pixels is measuring the harness.

**STEP 6 FOLLOW-UP: BANDING UNDER DLSS, AND THE TRAVERSAL NOW DESCENDS TO MIP -1.**

The user found horizontal bands across the far reflections on a GRAZING view with DLSS on. The
bisection that isolates the cause: **LogMarch + DLSS is clean, HiZ + DLSS is banded, and BOTH are
clean with DLSS off.** So it was not DLSS misbehaving and not a temporal problem in general -- it
was this tracer's own quantisation being exposed by the jitter. The traversal only ever answered to
the granularity of a mip-0 tile, and mip 0 is HALF the depth resolution: sub-pixel jitter moves the
ray start, a moved start falls into the neighbouring tile, and the hit jumps a WHOLE TILE. The
accumulator averages those discrete answers and paints bands. The log march has no such steps --
its bisection is continuous in the start position, which is exactly why it was clean.

Fixed the way UE fix it: `HZB_TRACE_INCLUDE_FULL_RES_DEPTH`, i.e. let the traversal descend to a
virtual **mip -1** whose "tile" is one depth-buffer texel. Cost `Pass_ReflectionSource` 0.035 ->
0.038 ms; bands gone at both views.

**TWO WRONG TURNS ON THE WAY, both worth keeping:**
* **A bisection bolted on after the traversal instead of a level inside it.** It refined between
  `lastAboveSurfaceT` and `t` -- and `t` is NOT a point behind the surface, because the loop leaves
  `t` alone whenever the ray is already below it. With a broken bracket the search converges
  wherever it likes and the thickness test accepts it: hits went 23.85% -> 36.62% and mean
  visibility 0.073 -> 0.209, i.e. mostly invented, and the banding got worse rather than better.
* **Skipping the self-intersection slack on mip -1.** It sounded principled -- a single depth texel
  summarises nothing, so there is no tile to be wrong about -- and it put BLACK BANDS across the
  whole floor on the main view while looking perfect on the grazing one. A grazing ray runs within
  a texel of the surface it left for a long way and re-intersects it at any resolution. The slack
  is about the ray hugging its own reflector, not about tile summarisation, which is why it fades
  over DISTANCE TRAVELLED and applies at every level. Hits with the slack back: 23.15% -- so the
  40.83% measured without it was self-intersection counting as reflection.

**And a lesson about the view, not the code:** the no-slack build looked *better* than correct on
the grazing view and was catastrophic on the level's own camera. One viewpoint is not a test.

**FOLLOW-UP: THE SKY'S INDIRECT SPECULAR MOVED FROM COMPOSE INTO LIGHTING.**

The user's third observation: the ocean's planar reflection shows other objects WITH their own
reflections, while in our screen-space reflection the bronze spheres are black discs with a
highlight. Cause is pass order, not the tracer. `Main_Lighting` -> `Main_Skybox` ->
`Main_ReflectionSource` -> `Main_Compose`: SSR samples the LIGHT target, and the sky's indirect
specular was added only in compose. A metal has no diffuse, so until compose ran it had nothing in
the light buffer except its direct highlight -- hence black. The ocean looked right because it
samples `sceneOpaque`, a copy of the composed frame.

The sky term does NOT depend on the reflection, so there is no cycle: it just sat on the wrong side
of the pass that needs it. It is computed in `lighting_cs` now, and compose adds only the
DIFFERENCE a reflection makes:

    lighting : skyCol            * weight
    compose  : (hit - skyCol*a)  * weight
    total    : (hit + skyCol*(1-a)) * weight     <- exactly what compose alone used to produce

Both sides call `IblSkyRadiance` / `IblSpecularWeight` in `ibl_common.hlsli` -- the identity only
holds while they agree, so neither open-codes it. `kSkyRoughMaxMip` and `FresnelSchlick` moved there
with them. Added to lighting: t12 prefiltered sky, t13 BRDF LUT, t14 raw sky cube, an s3 sampler,
and three constants mirroring compose's.

**Verified by A/B inside ONE BINARY** (shaders compile at load, so the old split was restored by
editing HLSL only, captured, and restored again), on ssr_bronze_palms, DLSS off, GTAO off, exposure
locked:

| mode | differing pixels | mean \|d\| | max |
|---|---|---|---|
| **SkyOnly** (no tracing at all -- must be neutral) | 1.28% | 0.004 | **2/255** |
| RT | 19.8% | 0.548 | 68 |
| HiZ SSR | 8.1% | 0.143 | 119 |

SkyOnly at max 2/255 is the neutrality proof -- floating-point ordering through an RGBA16F target,
nothing more. **RT changing is CORRECT and was not anticipated:** `rt_reflections_cs` samples the
light target too, so hardware-traced reflections got the same fix for free.

**A TRAP WORTH KEEPING: the two passes had different SAMPLERS.** The first attempt read the BRDF LUT
through lighting's only linear sampler, which is LinearWRAP (it exists for the caustics flipbook).
The LUT is a 2D table indexed by (NdotV, roughness); wrapping it returns the wrong Fresnel at both
edges, and SkyOnly -- which must be bit-neutral -- came out 3.4% different with max 70/255. Added a
LinearCLAMP at s3. **Two passes that must agree numerically have to agree about their samplers too,
and that is invisible in the formula.**

**A measurement note:** the first "neutrality" check compared against captures taken BEFORE the test
level locked its exposure, and read 99% different -- nothing to do with the change. The A/B has to
hold everything else fixed, including the level file.

**SSR TEMPORAL RESOLVE (`shaders/ssr_temporal_cs.hlsl`, pass `Main_ReflectionTemporal`).**

The instability the user reported first -- reflections boiling while the camera turns -- is not a
tracer bug and no tracer fixes it. A screen-space ray is violently sensitive to its own start: at a
grazing angle DLSS's sub-pixel jitter moves the reflected hit by tens of pixels every frame, and
DLSS cannot resolve that downstream either, because the motion vectors it gets describe the
REFLECTOR while the reflected image moves to a completely different law.

**CHECKED AGAINST UE FIRST, AND THEIR SSR FILTER IS NOT THEIR GTAO FILTER.** SSR goes through their
TAA as `ETAAPassConfig::ScreenSpaceReflections` = TemporalAA.usf `TAA_PASS_CONFIG == 3`:

    AA_HISTORY_PAYLOAD (HISTORY_PAYLOAD_RGB_OPACITY)   colour AND opacity filtered together
    AA_DYNAMIC 1                                       reproject by velocity
    AA_FILTERED 1
    AA_LERP 8                                          this frame is worth 1/8
    AA_YCOCG 1                                         CLAMP IN YCoCg, NOT IN RGB

Two of those a GTAO-shaped filter would have got wrong, and both are implemented as written: the
neighbourhood clamp happens **in YCoCg** (clamping RGB per channel pulls one channel back and not
the others, which shifts hue -- coloured fringing a luminance/chroma split does not produce; their
exact non-normalised RGBToYCoCg/YCoCgToRGB pair is reproduced), and the blend is **1/8** rather than
a number chosen by taste. Reprojection, the disocclusion test and the per-tap clamped history read
keep the shape of `gtao_temporal_cs.hlsl`.

**MEASURED -- this is the whole point of the pass.** Two runs of the same still camera with DLSS on,
shot 1 of the user's viewpoints, difference between the two frames (lower = less boiling):

| | whole frame | reflection band | worst pixel in band |
|---|---|---|---|
| temporal OFF | 0.756 | 1.119 | 49/255 |
| **temporal ON** | **0.207** | **0.142** | **12/255** |

**7.9x less frame-to-frame movement in the reflections**, and the reflections themselves read
denser -- the torn look of the raw buffer is what the resolve exists to close. Cost
`Pass_Reflection.Temporal` **0.014 ms**.

Wiring: the resolve writes `reflectionHistory`, which is both this frame's result and next frame's
history (per-frame set, exactly like `gtaoHistory`), and the blur's first tap reads it. The blur's
second tap still lands in `reflection`, so compose is untouched. History validity is tracked against
the reflection SIZE, so a resize or a level switch seeds from this frame instead of reading a
stale-sized texture. Knobs: `ssr.temporal`, `ssr.temporalBlend`, `ssr.temporalClampExpand`.

**UE HIT REPROJECTION WAS A SEPARATE MISSING HALF, NOT THE FILTER ABOVE.** The live UE path in
`SSRTReflections.usf` does not shade a hit from current SceneColor. It calls
`SSRTRayCast.ush::ReprojectHit`, first projecting the complete current `HitUVz` through
`View.ClipToPrevClip`, then overriding that camera result with the velocity sampled AT THE HIT, and
finally samples the previous temporal SceneColor. It also takes the minimum of the current- and
previous-screen vignettes. `ScreenSpaceRayTracing.cpp` binds current SceneColor plus dummy velocity
only when no previous temporal history exists.

That path is now present in `ssr_cs.hlsl` for the UE technique only:

* t5 is the previous frame set's full-HDR `Deferred.scene`; t6 is the current RG16F G-buffer
  velocity (`currUv - prevUv`). Opaque and glass SSR declare and bind both resources.
* `clipToPrevClip = invProj * invView * prevView * prevProj` is the camera fallback. Our velocity is
  not UE-encoded and has no validity bit, so non-zero velocity subtracts directly from hit UV and
  cleared zero uses the matrix result.
* The ray result now retains device Z, so reprojection consumes the same full `HitUVz` UE use.
  Previous SceneColor is bilinear-sampled, negative/NaN HDR is forced to black in the same shape as
  UE's `SampleScreenColor`, and the two-frame vignette multiplies opacity.
* SceneColor history is independent of `ssr.temporal`: `Deferred.scene` exists every frame. First
  frame, resize, level reset, and an explicit `Camera::ResetHistory()` cut seed from current
  `LightTarget`; a camera history revision prevents a stale previous image after teleports.

The new high oblique report also exposed two older source deviations: the port had HZB start mip 0
instead of UE's 1 and a generic 100-unit ray length instead of UE's `WorldTMax = SceneDepth`. Both
are restored. Mirror-smooth surfaces use UE quality 4's collapsed 24-step budget rather than
quality 2's 16. Because our HZB mip 0 already reduces a 2x2 block and there is no later off-screen
fallback, the accepted coarse candidate gets one full-resolution depth confirmation; this rejects
the most obvious foreign-depth curtains for one extra tap, without changing the 24 coarse steps.

RT at the exact reported camera (`0.58,12.85,18.30`, quaternion
`0.0142,0.9737,-0.2182,0.0634`) proves that the long reflected projection on this view is real --
the RT image extends just as far -- while the remaining softness/torn detail is the fixed-step
screen-space technique failing to resolve thin alpha-tested foliage, not reprojection inventing the
length. Final measured GPU cost at DLSS Quality render scale 0.58: `Pass_ReflectionSource` 0.024 ms,
`Pass_Reflection.Temporal` 0.017 ms. The exact camera gate also found and fixed a harness bug:
`Release_Editor` restored its
saved editor camera after the CLI override, so `--cam-pos/--cam-rot` now explicitly win and are not
autosaved back to editor state.

**Verify:** mip-chain correctness against a CPU reduction of the same depth buffer (an invariant with
a known answer, not a look test); GTAO before/after on the roughness sweep and wind_test; SSR
before/after; GPU cost of the build pass on its own.

---

### P7 — Add global analytic aerial perspective — DONE (2026-08-19)

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

**Interface contract:** disabling atmosphere is screenshot-equivalent to M2. The first implementation does not allocate a 3D froxel volume — that is **P15**, which keeps this model as its far field rather than replacing it.

**Done when:** distant geometry approaches the horizon color smoothly while near beach contrast remains intact.

**Verify:** low/high camera, look toward/away from sun, sky seam, ocean horizon parity, native/DLSS, GPU timing.

**OUTCOME. Transcribed from UE, not derived.** The first version here was written from first
principles and differed from `HeightFogCommon.ush` in four ways that all mattered, every one of them
now corrected in `shaders/atmosphere.hlsli`:

1. **Base 2, not e.** UE integrate and transmit in `exp2`. That rescales what `density` MEANS by
   ln2, so a model written in `exp` cannot be compared against their numbers at all.
2. **The removable singularity takes a Taylor expansion, not a constant.** At Falloff -> 0 the
   integral tends to ln2, and UE carry the first-order term so the branch is continuous in the
   derivative. A hard 1.0 there is both the wrong limit and a visible crease.
3. **The Falloff clamp is load-bearing** (`max(-127, ...)`): without it `exp2` of a large negative
   number gives a horizon line of NaNs.
4. **`FogMaxOpacity` is a FLOOR ON TRANSMITTANCE, not a scale on coverage.** Scaling `(1 - t)`
   bends the whole curve where UE clip its far end -- a different image everywhere, not just at
   distance.

**Defaults: only the dimensionless one transfers.** `DirectionalInscatteringExponent = 4` is used as
theirs. `FogDensity = 0.02` / `FogHeightFalloff = 0.2` do NOT: UE author against a CENTIMETRE world
and this engine is metres. No conversion factor was invented -- the shape of the model is UE's, the
magnitudes are this project's to tune.

**One deliberate departure.** The in-scattering colour is the sky sampled DOWN THE VIEW RAY, where
UE use an authored `FogInscatteringColor`. That is what satisfies item 4 by construction: at grazing
angles the fog colour and the pixel behind it converge on the same sample, so there is no seam to
tune away.

**The ocean shares the medium, and exactly one horizon term runs.** The water's own `HorizonBlend`
and the global fog are the same effect authored twice, so the fog branch replaces it rather than
stacking on it. BOTH surfaces carry it -- `ocean_surface.hlsl` and `ocean_surface_legacy.hlsli` --
which matters because `ocean::g_shoreRunup` defaults FALSE and the legacy file is the one that
actually runs; fogging only the modern one would have shipped a feature that never executes.
`PackAtmosphere` produces the numbers once for both compose and the water, so they cannot disagree.

**Level-scoped, like GTAO.** `Scene::AtmosphereRef` is the source of truth, `AtmosphereSettingsJson`
is the single mapping all three consumers go through, the environment object is "Aerial Perspective"
(always listed, with both the apply and the RESET-on-delete branches), and the developer window and
inspector edit the scene copy rather than the per-frame transport.

**Debug views (item 8)** are a `fogDebugView` mode on compose and on both ocean surfaces:
transmittance and coverage-weighted in-scattering. Two limitations are stated in the tooltip rather
than left to be discovered: un-measured pixels are painted BLACK (so "no fog" and "not part of this
view" cannot be confused), and the view is written into scene colour, so exposure and the tone curve
still run on it -- read it as relative, not as a number.

**Measured** on `wind_test` / `overview`, wind frozen, exposure fixed:
* fog OFF versus the pre-P7 build: **0.03% of pixels**, lower water 0.00% -- the interface
  contract's "screenshot-equivalent" holds, and the ocean's tuned look is untouched.
* fog ON: 71% of pixels move, sky 0.10-0.18% (i.e. the noise floor -- the sky is correctly excluded).
* GPU: `Pass_Compose` **0.0410 -> 0.0440 ms (+0.003)**; the whole GPU frame is 1.806 -> 1.800 ms,
  i.e. inside run-to-run noise. Captured at native and at DLSS Quality.

**THE TRAP THIS FEATURE SETS, and it cost an hour before it was spotted:** AUTO-EXPOSURE REACTS TO
FOG. The first A/B reported 99.9% of pixels changed INCLUDING THE SKY, which reads exactly like a
broken background test -- the sky was being fogged. It was not: fog moves the frame's average
luminance, metering follows, and every pixel shifts. With `exposure.autoExposure:0` the sky sits at
0.139% (the floor) and the gating was correct all along. Any fog comparison MUST fix exposure first.

**Left for later, deliberately:** density defaults are a starting point and have never been tuned on
the canonical views; that is the user's pass, and the knobs exist for it.

---

### P8 — Add exposure-aware HDR bloom and glare — DONE (2026-08-19)

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

**OUTCOME. The threshold is transcribed from UE's `BloomSetupCommon` (PostProcessBloom.usf) and the
composite from their tonemapper; the pyramid is the Call of Duty one the spec asks for.**

* **Threshold against EXPOSED luminance**, which is what makes the step "exposure-aware" rather than
  "a blur": `saturate((Luminance(c) * ExposureScale - threshold) * knee) * c`. Authored in the units
  the VIEWER sees, so a scene that gets darker keeps its bloom instead of quietly losing it, while
  the OUTPUT stays in scene units so one global exposure still covers scene and bloom alike. `knee`
  is exposed where UE hardwire 0.5 -- it is the difference between a hard cut and a shoulder, and
  hardwiring it would be a control that lies about being tunable.
* **The composite is UE's, including what it deliberately omits** (`PostProcessTonemap.usf:515-518`):
  bloom receives the GLOBAL exposure but NOT the local exposure and NOT the colour grade. A halo that
  spread from elsewhere is not part of this pixel's neighbourhood, which is what local exposure is a
  function of. Added before the curve, because bloom is scene-referred light.
* **Pyramid**: 13-tap downsample (Jimenez, SIGGRAPH 2014) with a Karis average on the FIRST level
  only, then a 3x3 tent upsample whose weights sum to exactly 1, so walking back up neither gains nor
  loses energy. `radius` spaces the taps in DESTINATION texels, which is what makes it mean the same
  thing at every level.
* **Two chains, not one ping-pong**: the upsample of level N reads BOTH up[N+1] and down[N]. Built
  entirely in UNORDERED_ACCESS with UAV barriers between levels, reading each source through its own
  UAV -- the HZB pyramid's construction, for the identical reason (this barrier layer transitions
  whole resources).
* **Where it runs**: inside `Pass_Tonemap`, between the DLSS evaluate and the tone curve, because
  both of those live in that pass. Sized off the DISPLAY resolution, so the pyramid does not change
  shape with the DLSS quality mode while the image it describes does not.

**Measured** (`wind_test`, wind frozen, exposure pinned, DLSS off), bloom off vs on at intensity 0.6
and threshold 1.0:

| view | changed | mean | max | clipped |
|---|---:|---:|---:|---:|
| `sun_glint` | 67.7% | 5.29 | 111 | 0.92% |
| `overview` | 14.9% | 0.15 | 53 | 0.000% |

That contrast IS the acceptance criterion: the glint field responds, the view without a glint field
barely moves, i.e. the effect is not a screen-wide wash. **GPU: `Pass_Bloom` 0.067 ms**, identical on
both views (it is resolution-bound, not content-bound); the frame goes 1.78 -> 1.83 ms. With bloom
off, `Pass_Bloom` does not appear in the profiler dump at all -- the interface contract's "schedules
no unnecessary active work" is observable, not merely asserted.

**THRESHOLD < 0 DISABLES THE THRESHOLD, AND THAT IS NOW THE DEFAULT, BECAUSE IT IS UE'S.**
`FPostProcessSettings::BloomThreshold` ships at **-1** (`Scene.cpp:423`), and their own field
documentation reads: *"-1: all pixels affect bloom equally (physically correct, faster as a threshold
pass is omitted)"*. Their renderer skips the whole threshold permutation at that value
(`bThresholdEnabled = Threshold > -1.0f`). A lens scatters light from everything in front of it; a
threshold is an artistic control that buys punch by discarding physics.

**It also decides how hard bloom answers to EXPOSURE**, which is what prompted this. With a
threshold, raising exposure does two things at once -- it scales the pixels already over the line AND
pushes new ones over it -- so bloom grows faster than the image. Measured (`wind_test`/`sun_glint`,
pyramid, intensity 0.6, mean 8-bit addition over bloom-off; note EV100 here is inverted, larger =
darker):

| manual EV100 | threshold 1.0 | threshold -1 |
|---|---:|---:|
| -1 (brightest) | +13.77 | +38.80 |
| 0 | +5.35 | +35.10 |
| +1 (darkest) | +1.67 | +24.00 |

**Two stops of darkening cut the thresholded bloom by 8x and the unthresholded one by 1.6x.** So yes,
bloom reacting to exposure is correct and deliberate -- but the SIZE of that reaction is the
threshold's doing, and the physically-correct setting is the one where it barely reacts at all.

**The firefly clamp cannot be judged from a still.** On a frozen frame it moves the mean by 0.2/255;
what it is for is the TEMPORAL case -- a sun glint crossing a texel boundary pumping the whole
pyramid -- which a single capture cannot show. It defaults ON for that reason, and the honest state
of the evidence is that the still says almost nothing about it.

**THE TRAP THIS STEP SET, and it is a gate defect as much as a code one: A COMPUTE ENTRY WITH NO
`[RootSignature]` ATTRIBUTE COMPILES.** `check_shaders.py` reported a clean 22/22 while the engine
could not build the PSO at all -- `Material::CreateCompute` fails, KEEPS the Material object, and
leaves its pipeline state null, so a `!= nullptr` gate passed and the pass dispatched with no
pipeline. Symptom was exit 122 and no screenshot, with nothing in any log. Two fixes, both kept:
the gate now checks `GetPipelineState() != nullptr`, and **`check_shaders.py` now fails an entry
that compiles without the attribute** (verified by stripping it and watching the tool fail).

**Left for the user:** intensity/threshold defaults are a starting point, not a tuned result. Ships
DISABLED, like P7.

---

### P8B — Fold the post-process sections into one object — DONE (2026-08-19)

**Depends on:** P7 and P8, deliberately. The set of settings is still growing; building the
container before it stops growing means building it twice, and dragging every already-authored level
through two migrations instead of one.

**The problem.** Level-wide look settings arrive as one top-level JSON section and one editor
environment singleton EACH: `cameraExposure` (P1), `colorPipeline` (P3), `gtao` (P6B), with P7's
atmosphere, P8's bloom and P9's GI still to come. Six objects in the outliner, none of which is
placed anywhere, all of which are always in effect.

**What this is NOT.** Not a copy of Unreal's PostProcessVolume. `FPostProcessSettings` carries **479**
`bOverride_` flags, and that is not bureaucracy — it is the price of SPATIAL blending: once more than
one volume can apply, every property must record whether it was set or inherited, or "exposure is 0
here on purpose" cannot be told from "nobody touched exposure". Add `Priority`, `BlendRadius`,
`BlendWeight`, `bUnbound` and a per-frame interpolation of the whole set. We have no overlapping
volumes and no place that needs different settings, so all of that would be paid for nothing.

Worth stating plainly: **the engine already has an unbound volume** — one always-in-effect set per
level. It is simply spread across six names instead of one.

**Implement:**

1. One `postProcess` section and one environment object holding the existing groups as sub-objects,
   so the inspector can present them as collapsing headers rather than six outliner entries.
2. Keep READING the old top-level sections, mapping them onto the new structure. Levels already
   authored must load unchanged and only pick up the new shape when re-saved.
3. Round-trip test: load an old level, save it, load the result — the settings must compare equal.
   That is the gate, not a look test.
4. Retire the per-setting environment singletons from the outliner once (2) is proven.

**Explicitly out of scope, and the trigger that would change that:** spatial volumes with priority
and blend radius. The one plausible case in this project is the camera going UNDERWATER, where the
grade, exposure and AO all want different values. That is a single boundary and is far better served
by a threshold on camera height against the water plane than by a general volume system. Revisit only
if a second such case appears.

**Interim, done 2026-08-18:** the outliner groups these under a "Post Process" node with a tooltip
explaining what the group is, which removes the visual clutter without touching the level format or
requiring any migration.

**OUTCOME (2026-08-19).** One `postProcess` JSON section and one `postProcess` environment object
holding `cameraExposure`, `colorPipeline`, `gtao`, `atmosphere` and `bloom` as sub-objects; the
inspector draws them as five collapsing headers.

**The inspector refactor is the part worth describing, because it was done so that a mistake could
not be silent.** Every widget used to write to `p`, a reference to the object's whole property set.
It now writes to `tgt()`, which returns either the properties themselves (every other environment
type) or `properties[groupKey]` (a section of the folded object). `p` was DELETED as an identifier,
which turned all 106 use sites into compile errors until each had been classified as one of:

* a field read or write -> the GROUP,
* an undo snapshot or a command payload -> the WHOLE property set, always. `EditEnvironmentCommand`
  replaces an object's entire properties, so handing it a group would delete that group's siblings.

That distinction is invisible to a `json[...]` call — it would just insert a key and carry on — which
is exactly why the identifier had to go rather than be pattern-matched.

**Reading takes BOTH shapes, forever; only the writer changed.** `PostProcessSection(level, group)`
prefers `postProcess.<group>` and falls back to the legacy top-level `<group>`, per group, so a level
authored before this step loads unchanged and a half-migrated file is well defined. The editor
document migrates on LOAD; the file only takes the new shape when it is next saved, at which point
the serializer writes the folded section and ERASES the five legacy keys — two copies of one setting
is a "which one wins" question nobody should have to answer.

**Gate (`scratchpad/p8b_roundtrip.py`), against `overview`, exposure pinned, DLSS off:**

| level shape | vs the legacy file |
|---|---:|
| folded (all five moved under `postProcess`) | **0.076%** of pixels, mean 0.001 |
| mixed (folded, but `gtao` left only at top level) | **0.007%**, mean 0.000 |
| empty (both shapes stripped -> struct defaults) | 100%, mean 23.3 |

The first two are the noise floor. The third is the control: without it, "identical" would also be
the answer if the reader had stopped reading settings altogether.

**One-way migration, and it is worth stating plainly:** a binary from before this step reads a saved
folded level as all-defaults, because it does not know the section. Levels are not re-saved
automatically, so this only matters after an explicit save in the editor.

**Not done here, deliberately:** the individual `cameraExposure` / `gtao` / … environment types are
still understood by the inspector, the serializer and `EnvironmentRuntime`. The document no longer
CREATES them, so they are unreachable in normal use, but the code stays as the fallback for a stale
`editor_state.json` and costs nothing.

---

### P8C — Convolution bloom (FFT), which is also where the flares come from — DONE (2026-08-19)

**Depends on:** P8 (it replaces the pyramid as the bloom SOURCE, and falls back to it), and scheduled
AFTER P8B so the settings land in the container rather than becoming a seventh singleton.

**Goal:** one physically meaningful input -- an image of how a real lens smears a single point of
light -- producing the halo, the starburst from the aperture blades, the anamorphic streak, the rings
and the ghosts TOGETHER and consistently. This is the whole reason it is here instead of a separate
lens-flare feature: flares stop being a second effect that has to be tuned into agreement with bloom,
and become a property of the kernel image.

**What "convolution" means here.** The pyramid in P8 approximates the lens response with a stack of
Gaussians: symmetric, and structurally incapable of a streak. Convolution does the real operation --
every bright pixel is replaced by a copy of the kernel scaled by its brightness, and all the copies
are summed. Done directly that is hopeless: a 2560x1440 frame against a kernel covering half the
screen is ~3.4e12 multiply-adds per channel. The convolution theorem turns it into

    convolution(frame, kernel) = IFFT( FFT(frame) * FFT(kernel) )

i.e. O(N log N) instead of O(N*M), plus one elementwise complex multiply. `FFT(kernel)` is computed
ONCE and cached until the kernel or the resolution changes, so a frame costs two transforms and a
multiply, not three.

**THE ENGINE HAS AN FFT AND SO DOES THE DROP -- both were checked, 2026-08-19.**

* Ours: `shaders/ocean_fft.hlsl` (`OceanSimulation::DispatchFFT`) is a working 2D radix-2 Stockham
  transform. It is NOT reusable as written: it holds a whole row in `groupshared` with one thread per
  element (`numthreads(FFT_SIZE,1,1)`, 256 by define) and it deliberately wants the CIRCULAR
  convolution an FFT naturally gives, because a wave field must tile. Bloom needs a frame-sized
  transform and must NOT wrap.
* UE's, and this is the one to transcribe: **`GPUFastFourierTransform.usf` +
  `GPUFastFourierTransformCore.ush` + `GPUFastFourierTransform2DCore.ush` are all in the drop**, and
  they already solve both of those problems:
  - `GroupSharedComplexFFTCS` / `GSConvolutionWithTextureCS` -- when a scan line fits in shared
    memory, the whole convolution (forward, multiply by the filter, inverse) is ONE dispatch.
  - `ReorderFFTPassCS` / `ComplexFFTPassCS` / `GroupSharedSubComplexFFTCS` /
    `PackTwoForOneFFTPassCS` / `ComplexMultiplyImagesCS` -- the multi-pass decomposition for
    transforms too large for one group. `RADIX` and `SCAN_LINE_LENGTH` come from the CPU, radix
    2/3/4/8 are implemented, and the shared buffers are two `float` arrays of `SCAN_LINE_LENGTH`
    (32 KB at 4096), with an explicit branch for lengths above 4096.
  - `GroupSharedTwoForOneFFTCS` -- the "two for one" trick: two REAL channels per complex transform,
    which is how RGB costs two transforms rather than three.
  - `CopyWindowCS` -- the windowing that gives zero padding, i.e. LINEAR convolution instead of
    circular. This is the thing that stops a streak leaving the right edge from reappearing on the
    left.

**And the whole kernel pipeline is in the drop too** (`Shaders/Private/Bloom/*`), which is the half
that is easy to underestimate -- turning a photograph into a usable filter is most of the work:
`BloomFindKernelCenter` + `BloomSurveyKernelCenterEnergy` (locate the centre and how much energy sits
in it), `BloomSumScatterDispersionEnergy` / `BloomSurveyMaxScatterDispersion` / `BloomReduceKernelSurvey`
(the energy survey the normalisation is built on), `BloomResizeKernel` + `BloomDownsampleKernel`
(rescale the kernel to the current resolution -- the kernel is a property of the LENS and its angular
size, so it cannot be a fixed pixel count), `BloomClampKernel`, and
`BloomPackKernelConstants` / `BloomFinalizeApplyConstants`.

**THE SIZE POLICY, READ OUT OF `GPUFastFourierTransform.cpp` AND `PostProcessFFTBloom.cpp` so it does
not have to be re-derived:**

* **`GPUFFT::MaxScanLineLength() = 4096`.** That single number is the whole group-shared/multi-pass
  decision: `FitsInGroupSharedMemory(length)` is literally `length <= 4096`.
* Shader permutations exist for `SCAN_LINE_LENGTH` 2 … 4096, the sub-FFT pass is `RADIX 2`, and
  `MIXED_RADIX` is defined for lengths above 8.
* **The convolution runs on a DOWNSCALED frame** (`DownscaleResolutionFraction`), not the full one.
* **Padding, and UE's own compromise inside it:** the pad is half the kernel's pixel width
  (`0.5 * KernelSupportScale * Width`), and the transform buffer is
  `RoundUpToPowerOfTwo(imageSize + pad)`. But **if that would exceed 4096 the pad is CLAMPED**, with
  a comment saying so in as many words: the bloom may then wrap from one side of the screen to the
  other, and they accept it because the kernel's tails are faint. So the wrap test this plan asks
  for is a question of degree, not a pass/fail -- know which side of that line a chosen size sits on.
* `KernelSupportScale` is clamped to `[0, 1]`, i.e. the kernel may span at most 100% of screen WIDTH.
* Horizontal-first vs vertical-first is chosen per frame by **whichever writes less to main memory**
  (`bDoHorizontalFirst`); the two orders are mathematically identical.
* There is a `PreFilter` (min/max/mult) that boosts bright pixels before the transform, and it is
  documented as operating in PRE-EXPOSURE space -- which is the same units question P8 already had
  to answer.
* UE can run the whole thing on **async compute** (`r.Bloom.AsyncCompute`), which this engine's
  async plan (`docs/async_compute_plan.md`) would make available later.

**Implement:**

1. Transcribe the FFT core (`GPUFastFourierTransformCore.ush` first -- radix butterflies, the shared
   memory layout, the normalisation convention), then the 2D layer.
2. The single-dispatch convolution path for the sizes that fit; the multi-pass path only once a
   measurement says the fitting one is not enough.
3. Kernel preparation: centre, energy survey, resize to resolution, clamp, cache. The transformed
   kernel is cached and invalidated on kernel change or resize.
4. Zero-padded window so the convolution is linear, not circular.
5. Run it on a DOWNSAMPLED frame, as UE do. Full-resolution convolution buys nothing visible for a
   low-frequency effect and costs multiples.
6. A `bloomMethod` control: Standard (P8's pyramid) or Convolution. Standard stays the default and
   the fallback -- this path is heavier, and a scene with no bright highlights cannot tell them
   apart.
7. Ship the kernel as a small generated image (blades, rotation, a little chromatic spread) so the
   feature has no content dependency, with a texture path for a photographed kernel later.

**Interface contract:** with Convolution off, the image is P8's exactly and no transform is
scheduled. With P8 itself off, this cannot run -- there is nothing for it to be a method OF.

**Done when:** `sun_glint` shows a starburst whose blade count follows the kernel, and the streak
holds still relative to the water as the camera turns, with `overview` (no glint field) still
essentially unchanged.

**Verify:** the P8 pairing (`sun_glint` vs `overview`) plus a rotation sequence, native and DLSS, GPU
timing of the transform passes separately from the kernel preparation, and a **wrap test**: a single
very bright point near one screen edge must not deposit anything on the opposite edge. That test is
the one that catches a missing pad, and it fails silently otherwise.

**Traps this project has already paid for once:**

* **Fix exposure before any A/B** (`--set=exposure.autoExposure:0 --dlss=off`).
* **The threshold, if one is kept, must be measured against EXPOSED luminance** -- same as P8's.
* **Energy normalisation is not optional here.** The kernel is resized per resolution, and without
  the energy survey the bloom's brightness changes when the window does. UE dedicate four shaders to
  this for a reason.
* **Measure before choosing the multi-pass path.** The single-dispatch convolution is dramatically
  simpler and covers a downsampled frame at the sizes this project runs.

**OUTCOME (2026-08-19).** Shipped as `bloom.method` = 1, with the pyramid still the default.
New shaders: `bloom_fft_cs.hlsl` (one axis of the transform plus the spectral multiply) and
`bloom_conv_cs.hlsl` (pack / kernel / resolve). Three grids per frame set (`bloomFftA`, `bloomFftB`,
`bloomFftKernel`) at 1024x512 R32G32B32A32, holding a 640x360 image inside a zero pad.

**The transform is a radix-2 Stockham, and it was verified against numpy BEFORE anything was built on
it** (`scratchpad/fft_verify.py`, a line-by-line port of the shader's indexing): forward agrees to
5e-16, the round trip to 5e-16, the convolution theorem to 4e-16, and so does the two-real-channels-
in-one-complex packing. That packing is the reason RGB costs one transform pair rather than three:
convolution is LINEAR and the kernel is REAL, so (R + iG) convolved with it separates back out with
no unpacking. It stops being true for a complex kernel.

**Normalisation is free and exact:** the kernel's DC term IS its sum, so the multiply divides by
`KernelSpectrum[0,0].x`. Radius, blades and falloff can therefore be changed without touching
brightness -- the failure this plan warned about, and what UE spend four shaders on.

**Measured** on `sun_glint`, exposure pinned, DLSS off, against bloom OFF:

| | changed | mean | GPU |
|---|---:|---:|---:|
| pyramid | 70.3% | 7.77 | `Pass_Bloom` 0.067 ms |
| convolution | 32.5% | 2.82 | `Pass_BloomConv` 0.122 ms |

**1.8x the pyramid's cost for two transforms, a multiply and a cached kernel** -- the frame goes
1.886 -> 1.912 ms. Gates: 24/24 shaders, both configs, `--canonical-check` adds 9 resources and ends
`0 of 233` off-canonical, `--scene-stress=20` CLEAN, and **`--gbv` clean of anything this step
introduced** (one pre-existing `Ocean.SurfSimWaveA` SRV-in-UAV-layout error remains, confirmed
present with bloom off).

**FOUR BUGS THIS STEP PRODUCED, all worth remembering because none of them announced itself:**

1. **`RecordComputeDispatch` always divides the extent by 8**, because it exists for the 8x8 shaders
   everything else uses. This transform is a 1D group of 1024 and needs ONE GROUP PER SCAN LINE;
   passing the real thread count asked for 128x too many groups and the first convolution rendered
   nothing at all. A caller wanting `N` groups from that helper passes `N * 8`.
2. **The kernel spectrum was cached against ONE key while living in a PER-FRAME resource.** Slot 0
   built it; slots 1 and 2 convolved against an empty texture, so two frames in three had no bloom.
   The cache key is per frame slot now. Any cache whose payload is in `DeferredTargets` has this
   shape.
3. **GBV caught an SRV binding pointed at a UAV-layout resource** (id=1358): the transform declared
   its inputs `Texture2D` while the chain deliberately stays in UNORDERED_ACCESS. It compiled, it
   ran, and it produced plausible output. Reads go through `RWTexture2D` now, as the HZB and bloom
   pyramids already do.
4. **Karis-averaging the SETUP stage deletes the highlight instead of clamping it.** Weighting by
   1/(1+luma) turns a 100x glint against three dark neighbours into ~1. Setup box-averages four taps
   (which is what fixes the reported flicker: a half-res grid point-sampling a full-res image drops
   a moving glint in and out); Karis stays on the first downsample, where the data has already been
   averaged.

**The kernel defaults are MEASURED, not guessed** (`scratchpad/kernel_probe.py`). At radius 0.12 the
normalised kernel peaks at 1.4e-05 with 4% of its energy inside 32 texels -- every highlight spread
into a flat wash, which is exactly what the first working render looked like. 0.004 / falloff 3.0
puts 14% inside two texels and 65% inside eight. The kernel is also CUT at four radii so its tail
cannot exceed the vertical pad, which is what makes the convolution linear rather than wrapping; the
inspector's cap is set to match.

**TWO PRE-EXISTING GBV ERRORS WERE FOUND AND FIXED WHILE GATING THIS** (both id=1358, an SRV bound
at a resource in UNORDERED_ACCESS layout, both in the ocean surface and both present with bloom
OFF):

1. **The surf-sim wave/foam fields on the FIRST frame.** They are created as compute outputs, the
   ocean vertex shader samples them every frame, and nothing had handed them over yet. They are
   bound only once the sim has actually written them (`OceanSurfSim::HasSimulated`). The sim also
   skips its whole pass on a frame with nothing to integrate -- which `--wind-freeze` makes the
   normal case, and is why every headless capture hit it -- so that path now declares the hand-over
   even though it dispatches nothing.
2. **The ocean fallback descriptor pointed at `scene`.** Every slot the draw does not fill was given
   `sceneSRV`, and compose writes that target as a UAV, so every fallback-filled slot was a read in
   the wrong layout. It points at `sceneOpaque` now -- the copy this shader already samples for
   refraction, therefore readable by construction.

**`--gbv` now reports ZERO errors**, which is cleaner than the state this step started from. The
ocean image is unchanged: 0.026% of pixels, mean 0.000.

**THE FIRST KERNEL LOOKED LIKE NOTHING, AND THE REASON IS WORTH KEEPING.** It was a radial power law
with an angular wobble -- and an angular modulation of a radial profile CANNOT produce a ray, because
the result is still radial. A ray is a slow falloff ALONG a direction and a fast one ACROSS it. The
kernel is now three terms, each of which a real lens has and none of which the others can imitate:

* **spokes** -- N lines through the centre giving 2N rays, Gaussian across and Lorentzian along.
  Measured on-spoke vs 45-degrees-off at 30 texels: **1.0x radial-only, ~200x with spokes**;
* **an anamorphic bar** -- one long thin horizontal streak, because a squeezed axis is not something
  a symmetric star can fake;
* **chroma** -- the blue lane gets a wider kernel, which is free: the packing already carries
  (R + iG) and B as two INDEPENDENT complex lanes. R and G cannot be split from each other.

**Ghosts are gathered in the resolve, NOT convolved, and that is structural:** a convolution is
shift-invariant, while a ghost moves the opposite way to its source because it is a reflection
through the frame centre. The first version sampled the convolved image directly and copied the
bright sky as a rectangular slab that read as a UI panel; the sample is now weighted towards point
sources, which suppresses broad fields quadratically and leaves the sun.

**SIX GHOST DEFECTS THE USER CAUGHT BY EYE, all now fixed and all worth the note:**

* **Visible texels.** The resolve POINT-sampled the convolution grid. The grid is a quarter of the
  display width, the bloom target a half, and the tonemap stretches that over the full frame -- one
  grid texel is at least 4x4 screen pixels. A halo hides that; a ghost is a mirrored copy with a
  scale factor, which magnifies the grid further and turns the steps into blocks. There is no
  hardware filtering available -- the chain is read through UAVs and a `RWTexture2D` load is always
  point -- so `SampleGridBilinear` is written out by hand.
* **A rectangular slab.** A ghost is a copy of the frame, so where the copy runs past the frame's
  edge it stopped DEAD, in a straight line. Two masks now: a `smoothstep` fade as the sample
  approaches the edge it was read from, and UE's own
  `DiscMask(ScreenPos) * DiscMask(ScreenPos * 0.8)` (`PostProcessLensFlares.usf`) -- a circular
  falloff applied twice at different radii, which is what makes a flare sit IN the frame rather than
  on it.
* **Every ghost carried its own starburst.** They sampled the CONVOLVED grid, which by definition
  already contains the kernel -- so each mirrored copy dragged a full set of spokes with it and the
  frame filled with stars. A ghost is a reflection off a lens element, and it happens BEFORE the
  point spread that makes the rays; it must therefore read the RAW frame. The resolve now samples
  `HDRColor`, and the star is the sun's alone.
* **The sample pattern showing through as the shape.** Reading the raw frame means reading something
  sharp, so it has to be gathered rather than tapped. Five axis-aligned taps drew a visible PLUS SIGN
  of blobs. A ghost is an out-of-focus image of the APERTURE, so its shape is the iris: the gather is
  now two rings plus the centre, with the ring count following `blades`, which makes a six-bladed
  iris throw hexagonal ghosts for free.
* **A patch of cloud pasted over the palms.** Switching the source to the raw frame silently dropped
  TWO things that had come free with the grid, and the second is the interesting one.
  * The **threshold** was the grid's, because the grid is the setup stage's output. Reading
    `HDRColor` walked straight past it, so an entire bright cloud deck became a legal ghost source.
    Worse, the "squared response" that was supposed to suppress broad fields was
    `saturate(exposedLuma)`, which is pinned at 1 for anything above middle grey -- that is, for the
    whole sky -- so it suppressed exactly nothing. `GhostHighlight()` now applies the threshold PER
    TAP (averaging first would let a small bright source be diluted under the floor by its dark
    neighbours) and squares `amount`, which unlike luma actually varies. UE do the same thing
    structurally: `PostProcessLensFlares` samples the thresholded bloom chain, never scene colour.
  * **Brightness alone is the wrong test.** Under an overcast sunset the whole glow around the sun
    sits far above any usable threshold, so it passed, and its mirrored copy landed on the scene
    still carrying the cloud's own texture -- a pasted image, not an optical artefact. The missing
    quantity is COMPACTNESS: a ghost is only visible when its source is small compared to the
    aperture defocus, which is why real ones come from suns and street lamps and never from the sky.
    It is measured from taps already taken -- centre against the outer ring, normalised by the
    centre -- and squared. Measured on `wind_test` at the user's own settings, in the region he
    circled: pixels differing by more than 20/255 from ghosts-off fell 10.66% -> 1.19%, mean
    +3.78 -> +0.50. Verified in both directions: on a sky whose sun IS compact the ghosts remain,
    and now read as clean polygons of the iris.

  **The consequence is worth stating: under a diffuse sky there are no ghosts, by design.** That is
  correct optics, not a bug, but it does mean the ghost controls do nothing in such a level. If
  ghosts are wanted there anyway it is an art decision and needs its own knob -- a compactness
  floor -- rather than a quiet weakening of the gate.
* **Sharp.** Everything above narrowed WHICH sources throw a ghost; none of it changed the fact that
  a copy of a sharp image is sharp. The remaining ghosts still carried the source's own texture and
  read as photographs pasted over the palms.

  A ghost is DEFOCUSED -- it is an out-of-focus image of the aperture -- and a defocus is a blur
  wide enough to destroy the structure it acts on. A cloud band is hundreds of pixels across, so a
  gather of seventeen taps over 2% of the screen cannot touch it, and widening the gather far enough
  is not affordable: at the resolve's resolution a disc big enough would be over a hundred million
  samples a frame.

  **A MIP CHAIN IS THAT DEFOCUS, PREFILTERED, AND ONE ALREADY EXISTED.** The thresholded DOWN chain
  is what the pyramid method builds in its first half -- and the convolution method, being an
  either/or alternative to it, simply never ran it. So `Bloom_Downsample` was split out of
  `Bloom_Build` and is now called by both, and the ghosts gather from a coarse level of it through
  the new `ghostMip`. **This is also what UE do**: `PostProcessLensFlares` samples the bloom chain's
  mips, never scene colour, and that single fact explains both this defect and the threshold one
  above.

  `ghostMip` is derived from `kGhostBlurFraction` (1/24 of the frame) rather than authored, so the
  defocus covers the same fraction of the image at any resolution.

  **Cost, measured A/B inside ONE binary** (`bloom.ghostIntensity:0` skips the chain), median of
  three on `wind_test`: `Pass_BloomConv` 0.247 ms with the source, 0.131 ms without -- the ghosts'
  source costs **0.116 ms**. The 0.131 matches the 0.122 recorded when P8C landed, which is the
  check that nothing ELSE regressed. The dispatches are therefore skipped when
  `convGhosts == 0 || convGhostIntensity == 0`; the two barriers are not, because bloomDown is bound
  as an SRV on every convolution dispatch whether or not it was written (a descriptor table is
  positional and cannot have a hole).

  **If that 0.116 ms ever needs to come down**, the lever is the seed: the chain starts at half the
  display width, and mip 0 alone is most of the cost, while the ghosts only ever read around mip 5.
  Seeding from the convolution's own thresholded grid (already a quarter width) instead of from the
  frame would cut it roughly fourfold. NOT done: undersampling the seed is exactly what made bloom
  flicker once already.

  GBV is clean on all three paths -- convolution with ghosts, convolution with the chain skipped,
  and the pyramid method through the extracted function.

**Still open:** the defaults produce visible rays, an anamorphic bar and soft ghosts, but the overall
haze is heavy (mean +15.9/255 against bloom off on sun_glint) and wants an art pass on the balance
between core, spokes and bar. Every one of those is now a control.

**Superseded note:** the blade rays are faint at the shipped defaults -- the mechanism is there and the
blade count does change the shape, but making a starburst read as optics rather than as a smudge is
a tuning pass on a scene with a small, very bright source, and this project's sun is neither.

**WHERE THIS DIVERGES FROM UE, STATED PLAINLY.** The architecture is theirs; most of the details
are not. This section exists because the rest of the write-up reads like a transcription and it is
only partly one.

**Taken from UE, and faithful:**
* the bloom THRESHOLD and its exposure handling (P8, from `BloomSetupCommon`);
* the tonemap COMPOSITE, including what it deliberately omits (P8, `PostProcessTonemap.usf:515`);
* the shape of the feature: convolve a downscaled frame against a cached kernel spectrum.

**Written here, NOT transcribed:**
1. **The transform itself.** `bloom_fft_cs.hlsl` is a radix-2 Stockham written against this engine's
   own `ocean_fft.hlsl` and verified against numpy. UE's `GPUFastFourierTransformCore.ush` is a
   different, more general implementation (mixed radix 2/3/4/8, a two-buffer/one-buffer shared
   memory split, bank-conflict skewing). Ours is simpler and covers only what a 2048-point
   group-shared transform needs.
2. **THE KERNEL IS GENERATED; UE'S IS A TEXTURE.** This is the largest divergence and the one that
   decides how it LOOKS. `View.FFTBloomKernelTexture` is an authored image of a real lens's point
   spread, with eleven shaders behind it (`BloomFindKernelCenter`, `BloomSurveyKernelCenterEnergy`,
   `BloomResizeKernel`, `BloomClampKernel`, the scatter-dispersion survey, ...) plus
   `BloomConvolutionCenterUV`, mip selection and a resolution warning. None of that is used here:
   the kernel is core + spokes + anamorphic bar, computed analytically. **The reason is a project
   rule, not a technical one** -- importing a kernel image is content, and content conversion is the
   user's call. A photographed kernel is the single biggest available improvement to how this looks.
3. **Channel packing.** (R + iG) in one complex lane and B in the other, which is legal only because
   the kernel is real. UE use a genuine two-for-one real-signal transform
   (`GroupSharedTwoForOneFFTCS`) and can therefore give every channel its own kernel; here R and G
   are forced to share one.
4. **Energy normalisation by the spectrum's DC term**, where UE run a four-shader energy survey. The
   DC identity is exact for what it does, but it does not reproduce their scatter-dispersion
   handling.
5. **`kConvolutionGain = 8`** in the resolve. Pure calibration so `intensity` means the same thing in
   both methods; there is no equivalent in UE.
6. **Sizing.** A fixed quarter-resolution image inside a next-power-of-two grid with 25% headroom.
   UE derive it from `KernelSupportScale` and `BloomConvolutionBufferScale`, clamp the pad when the
   transform would exceed 4096, and choose horizontal-first or vertical-first per frame by which
   writes less memory (`bDoHorizontalFirst`). Their numbers are recorded above but not implemented.
7. **Ghosts.** Gathered as mirrored samples in the resolve. UE's are a bokeh SCATTER of tiled quads
   with a per-ghost tint table (`LensFlareTints[8]`) in `PostProcessLensFlares`.
8. **The multi-pass decomposition** for transforms above the group-shared limit is not implemented at
   all; only the single-dispatch path exists.

**If "match UE" is the goal, the order that buys the most per unit of work is: (2) the kernel image
and its preparation, then (6) the sizing policy, then (7) their flare pass, then (3) and (8).**

**What was considered and dropped:** UE's other flare implementation, the bokeh scatter in
`PostProcessLensFlares.usf` -- tile the frame, turn each bright tile into an aperture-textured quad.
It is cheaper and reuses P8's pyramid directly, but it produces ghosts as decoration rather than as
optics, its threshold is a hard cut that would strobe on this ocean, and it cannot produce a streak
or a starburst at all. Convolution subsumes it. The file stays in the reference table in case that
trade ever wants revisiting.

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

### P12 — Two correctness defects the diagnostics report every frame — DONE (2026-08-18)

Neither is caused by this plan's work; both were INVISIBLE until the barrier/canonical logs stopped
repeating themselves (2.2 MB and 3779 lines per eight-second run collapsed to five distinct lines,
`Renderer::DiagLogOnce`). They are queued here rather than left in a log nobody reads, because a
diagnostic that always has entries in it is a diagnostic people learn to skip.

**P12.1 — `ShadowGpuData.IndirectArgs` rests in two different states.**

    [canonical] off-canonical res=ShadowGpuData.IndirectArgs canonical=0x40 actual=0x200

0x40 is NON_PIXEL_SHADER_RESOURCE (what it is declared as), 0x200 INDIRECT_ARGUMENT. Reproduces on
every level including untouched HEAD ones (d_emissive_test, new1), and the frame-end summary
alternates `0 of N` / `1 of N`, so it is off-canonical on SOME frames and not others.

**DO NOT "FIX" IT BY FLIPPING THE DECLARATION -- that was tried and it merely reverses the report**
(`canonical=0x200 actual=0x40`). The buffer genuinely rests in different states depending on whether
`RecordCull` ran: that body early-outs on `count_ == 0 || numMeshGroups_ == 0`, while its closing
transition to INDIRECT_ARGUMENT (ShadowGpuData.cpp, "leave the args in INDIRECT_ARGUMENT") only
happens when it does not. The fix belongs in the cull's flow -- give the resource ONE resting state
on every frame, whichever way the body goes -- not in the label. Compare `VSM.PageDrawArgs`, which
declares INDIRECT_ARGUMENT and is consistent about it.

**Verify:** `--canonical-check` reports `frame end: 0 of N off-canonical` on every frame of a run
that both exercises and skips the cull (a level switch does both). No new `MISSING` lines.

**P12.2 — `InitialState` passed for BUFFERS, which D3D12 ignores.**

    [gbv] WARNING id=1328: CreateCommittedResource: Ignoring InitialState <X>.
                           Buffers are effectively created in state COMMON.

Six distinct variants, from `--gbv` on any level. Not an error: D3D12 always creates buffers in
COMMON regardless of what is asked for, and state promotion makes the first use correct anyway,
which is why this has never broken anything. It is still worth closing, because the canonical
registry is told the buffer was created in state X while the driver created it in COMMON -- the
declaration and the reality disagree, and that is exactly the class of quiet mismatch the canonical
check exists to catch. It also keeps `gbv.log` non-empty, which costs the same attention P12.1 does.

The fix is mechanical but WIDE: pass `D3D12_RESOURCE_STATE_COMMON` at every buffer
`CreateCommittedResource` AND change the matching `DeclareCreated`/`Attach` call so the registry is
told COMMON too. Roughly 30 sites across VirtualShadowMap, ShadowGpuData, ExposureMetering,
LightManager, FrameResource, ParticleEmitterObject, TextManager, BindlessTable,
AccelerationStructure, DebugDraw, OceanSurfSim and Profiler. Textures are NOT affected -- their
InitialState is honoured, so this must not be applied blindly to every call site.

**Verify:** `logs/gbv.log` is empty on a `--gbv` run; `--scene-stress=30` CLEAN; `--canonical-check`
unchanged. Do it as ONE change with the gates in between, not folded into unrelated work: it touches
the state every one of those resources starts life in.

**OUTCOME — P12.1. The diagnosis above named only half the cause, and the missing half is why the
label looked like the bug.** The args buffer had TWO last-touchers, not one. `RecordCull` closes by
leaving it in INDIRECT_ARGUMENT, and `VirtualShadowMap::PrepareRenderPass` separately borrows it as
an SRV (`NON_PIXEL_SHADER_RESOURCE`) and never handed it back. Whichever ran last set the resting
state -- and whether the VSM page-render pass exists at all depends on
`VsmActive() && IsAllocated() && !vsmSkipUpdate_`. So `atoll` rested NON_PIXEL (matching the old
declaration, no report) while `d_emissive_test` rested INDIRECT_ARGUMENT (report), and flipping the
label just swapped which of the two complained. The fix is both halves at once: declare
INDIRECT_ARGUMENT -- the rule its siblings `VisibleList` and `IndirectCounts` already follow, which
is *declare the state the owner leaves it in* -- and add the hand-back at VSM's consume point, which
is also the state the spot/point shadow passes scheduled after it ExecuteIndirect from.
Measured: `d_emissive_test` went from alternating `0 / 1 of 137` to a flat **0 of 137**; `new1` from
5 to 4 of 153; `atoll` unchanged at 4 of 198; `ShadowGpuData.IndirectArgs` no longer appears on any
level, including across 30 `--scene-stress` iterations of level switch / reload / resize / DLSS-mode
churn -- the scenario that exercises AND skips the cull. **0 MISSING** throughout.

**OUTCOME — P12.2. The second half of the recipe above was wrong, and following it literally would
have done damage.** "Change the matching `DeclareCreated`/`Attach` call so the registry is told
COMMON too" cannot be done, because the registry is never told a creation state:
`ResourceDeclarations::Declare(res, creationState, canonicalState)` begins with `(void)creationState;`
and stores only the canonical one, seeding `predicted` from it. Passing COMMON there is a no-op --
and passing COMMON as the *canonical* argument, which is the natural misreading, would have declared
every one of these buffers to rest in COMMON and produced a flood of new off-canonical reports. The
real change is the `CreateCommittedResource` InitialState argument and nothing else.
It is also far narrower than "roughly 30 sites": the six warned variants come from exactly five
creation points -- `ExposureMetering::CreateRawUavBuffer`, `OceanSurfSim`'s spawner buffer,
`VSM.PageTable`, `VSM.PageRequest`, and `VirtualShadowMap::CreateUavUintBuffer`, whose thirteen
callers account for the other four variants. The helper's `initial` parameter was **removed** rather
than defaulted: it claimed the buffer was "created directly there", which is precisely what D3D12
does not do, and a parameter the driver discards misleads its next reader. Buffers that legitimately
keep a non-COMMON initial state and must NOT be touched: UPLOAD heaps (`GENERIC_READ` is required),
READBACK heaps (`COPY_DEST` is required), and raytracing acceleration-structure results
(`RAYTRACING_ACCELERATION_STRUCTURE` is required and cannot be transitioned into) -- AS *scratch* was
already COMMON for this same reason. Textures are unaffected, as noted above.
Result: `logs/gbv.log` is not created at all on a `--gbv` run (zero messages, previously six),
`--scene-stress=30` reports `verdict: CLEAN`, and `--canonical-check` is unchanged.

**TRAP FOR THE NEXT PERSON:** `--gbv` on a **Release** binary validates NOTHING. The debug layer and
`SetEnableGPUBasedValidation` live under `#ifdef _DEBUG` in `GraphicsDevice::InitDevice`, so a
Release run produces an empty `gbv.log` whether or not anything is wrong. Run the Debug binary.

**P12.3 — the ocean ping-pong textures (same defect class, fixed with P12).** `Ocean.WetnessA/B`,
`Ocean.WetnessStampA/B` and `Ocean.ShoreSdfJumpB` all declared UAV and rested elsewhere. The A/B
naming made them look like one problem; they were three, and only one was really about parity:

* **History (`WetnessA/B`) — never a parity problem, just mis-declared.** `BuildUpdatePass` leaves
  the read slot NON_PIXEL and ends by putting the write slot there too, and `Main_Compose` then
  reads the current one as an SRV. BOTH rest in NON_PIXEL every frame, whichever side of the swap
  they are on. UAV described the state they hold for one dispatch, not the one they sit in.
* **Stamp (`WetnessStampA/B`) — the genuine parity case.** The ocean's own draw stamps into the
  CURRENT slot as a UAV, so UAV is right for it, but the other slot was abandoned in NON_PIXEL --
  hence a report that alternated between StampA and StampB as `current_` flipped. `BuildUpdatePass`
  now hands the read slot back to UAV at its closing point, so both rest in UAV and one declaration
  is true for either. Same fix shape as P12.1: the borrower returns what it borrowed.
* **`ShoreSdfJumpB` — not a ping-pong slot at all.** Despite the A/B naming, the jump flood
  ping-pongs but the resolve ALWAYS writes slot 1, `shoreSdfSrv_` is created on slot 1, and
  `BuildShoreSdf` ends by putting slot 1 into NON_PIXEL|PIXEL, where it then sits for every frame
  until the shore area moves. Slot 0 genuinely rests in UAV. Declared per slot now.

These are TEXTURES, so unlike P12.2's buffers their creation state IS honoured and had to be moved
to match each new declaration -- otherwise the first compiled barrier transitions from a state
nothing is in. Result: `atoll` and `new1` both report `frame end: 0 of N off-canonical`,
`--scene-stress=30` is `CLEAN` with 0 MISSING, and Debug `--gbv` still produces no log at all.

**P12.4 — the forward targets under `--dlss=off`.** `Deferred[N].Depth` (`canonical=0x40 actual=0x10`,
DEPTH_WRITE) and `Deferred[N].GBVelocity` (`actual=0x4`, RENDER_TARGET), on all three frame sets.
Third instance of the same shape as P12.1 and P12.3: **the resting state depended on which optional
consumer happened to run.** The transparent pass ends with both bound as forward targets because
that is what it drew into, and `DlssHandler::EvaluateDLSS` returns them to a read state as a side
effect of consuming them -- so with DLSS on the invariant held by accident, and with DLSS off there
was no owner at all. Fixed by giving the `else` branch of the tonemap pass the same hand-back the
DLSS branch already performs, declared in `Prepare` and emitted in the body, both gated on the same
`IsDlssActive()` predicate so the two cannot disagree. Two barriers per frame on a non-shipping path,
which is the honest price of an invariant that must not depend on the upscaler.
Verified at the same camera with `--dlss=off` AND `--dlss=quality`: both `frame end: 0 of 198`.
`--scene-stress=30`, whose churn includes DLSS-mode switches, is `CLEAN` and reports `0 of N` on
every distinct line -- no `off-canonical` entry survives anywhere. Debug `--gbv` with `--dlss=off`
produces no log.

**WITH P12.1/P12.3/P12.4 THE `--canonical-check` REPORT IS EMPTY.** That is the point of the exercise:
the diagnostic now carries information again, so the next entry that appears is a real finding rather
than one more line people have learned to scroll past. All three defects were the same mistake in
different clothes -- a resource whose resting state was decided by whoever touched it last, when the
set of touchers is conditional. The rule that fixes all three: **declare the state the OWNER leaves
it in, and make every conditional borrower hand it back.**

**P12.5 — the "LEAK" counter: one real duplicate, and a diagnostic that was lying about the rest.**
The per-name counter reported five names. Only ONE was a defect, and none of the five was a leak.

* **REAL, fixed: `Tex2D:textures/ocean/wind_gusts.png` x2.** `OceanRenderable` loaded the same file,
  with the same usage, into two members — `distantRoughnessTexture_` and `foamDetailTexture_` — for
  ocean_surface.hlsl's `DistantRoughnessMap` (t5) and `FoamDetailMap` (t6). Two byte-identical GPU
  textures. Now one `gustNoiseTexture_` bound to both slots; the descriptor table shape is unchanged.
  Verified invisible: the pre/post difference (41.5% of pixels touched, mean 0.500) is BELOW the
  noise floor of the same settings (42.5%, mean 0.505 — DLSS and GTAO are frame-indexed, so at
  default settings that floor is enormous and any A/B taken there must quote it).
* **NOT leaks: `damaged_plaster_normal.dds` x4, `bronze_albedo/mr/normal.dds` x2, and
  `GpuInstanced.Instances` x2.** These come from the `demo` level ALONE — they were only ever seen
  in a stress log because that harness loads `demo`. The referrers explain the counts exactly: four
  distinct materials name the plaster normal (presets `damaged_plaster.json` +
  `damaged_plaster_rg.json`, plus three inline object materials in `demo.json`), two name each
  bronze map (`bronze.json` + `bronze_copy11.json`). `MaterialDataManager` caches by MATERIAL NAME
  and each `MaterialData` owns its own `Texture2D`, so one file shared by N materials is N GPU
  copies by construction. Across 30 level-switching `--scene-stress` iterations the counts stayed
  FLAT at those single-level values and never climbed — which is what proves declares and forgets
  are balanced. Nothing is leaking.

The counter's premise ("a debug name is unique, so two live entries mean one was never forgotten")
is false for assets, whose debug name is the asset PATH. It now reports `duplicate-name ... N live
entries (check the referrer count before reading this as a leak)` and the code says how to tell the
two apart: count the referrers in `data/` — a count that MATCHES them is duplication, a count that
climbs with no new referrer is the leak, and a level-switching stress run is the discriminator.

**P12.6 — the shared-texture cache (closes the duplication P12.5 measured).** `Texture2D` now shares
file-backed textures. Identity is `texdecode::Key` — the SAME struct the decode cache keys on
(resolved path, usage, normalIsRG, alphaCoverageCutoff), reused deliberately so the two caches cannot
drift into disagreeing about what "the same texture" is.

Three decisions worth keeping:

* **The cache lives INSIDE `Texture2D`, not above it.** A `TextureCache` returning
  `shared_ptr<Texture2D>` would have meant changing every owner (`MaterialData` holds its three
  textures BY VALUE) and every `md->albedo.GetSRVCPU()` call site across SceneRenderer,
  GBufferRenderable, EditorPreviewRenderer and more. Instead a `Texture2D` becomes a VIEW: it holds
  a `shared_ptr` to the owning instance, and every accessor reads through `Source_()`. Not one call
  site changed. `GetSRVForFrame` forwards too, so the per-frame descriptor copy happens once for all
  materials sharing the texture rather than once per material.
* **The map holds WEAK references.** The cache is an index, not an owner: a texture lives exactly as
  long as the last material viewing it, so a level switch frees VRAM on the schedule it always did.
  Proven, not assumed: after 30 level-switching `--scene-stress` iterations the live-entry count
  settles at 12 — it does not climb.
* **The DDS sibling is resolved BEFORE the key is built.** A preset naming `x.png` and one naming
  `x.dds` load the same bytes; keying on the requested path would have given them separate copies,
  which is the exact duplication this exists to remove. A failed load is never cached.

Measured. `demo`: **19 loads, 6 shared** — six 684 KB GPU copies not made, ~4 MB. Across the
30-iteration stress churn: **89 loads, 24 shared**. `atoll`: 55 loads, 0 shared, correctly — it never
had duplicate textures. Every `Tex2D:` duplicate-name line is gone from `--canonical-check`.

**Pixel-neutral, verified against the noise floor the right way.** The first attempt compared
captures taken at DEFAULT settings and produced 86% of pixels changed against a 71% floor — a
"signal" that was entirely the capture recipe: `demo` animates, and without `--wind-freeze --dlss=off
--set=gtao.enabled:0` the floor swamps everything. Redone deterministically, with the cache bypassed
in the SAME binary (a temporary early-out, removed again after the measurement — repeat it by
guarding the map lookup in `Texture2D::CreateFromFile`): cache ON vs OFF is mean **0.233** / 6.05% of
pixels against a floor of mean **0.225** / 5.92%, and a second pairing lands at 0.078 / 3.58%, i.e.
CLOSER than the floor pair to each other. The bypass run reports `25 loads, 0 shared` versus
`19 loads, 6 shared`, which is what proves the lever actually moved.

Gates: both configurations build, `--scene-stress=30` `CLEAN`, `frame end: 0 of N` on every line,
Debug `--gbv` produces no log.

**P12.7 — `GpuInstanced.Instances`. The fourth and last of the class, and the one where GUESSING THE
CAUSE WAS WRONG.** Debug on `demo` reported `canonical=0xC0 actual=0x40`, Release on `demo` and Debug
on `atoll` clean. The obvious reading — "the GI→VSM scatter declares NON_PIXEL, the draw declares
NON_PIXEL|PIXEL, whichever runs last wins" — was only half right, and the half that was wrong is the
half that mattered. `--barrier-flip-trace` settled it in one run: across a whole frame the buffer is
touched exactly TWICE.

    [flip] pass=2 ... asked GpuInstanced.Instances 0x8    <- Main_ObjectCompute, the rotation compute
    [flip] pass=6 ... asked GpuInstanced.Instances 0x40   <- Main_ShadowCull, the GI scatter

There is no 0xC0 transition in the frame AT ALL. `GpuInstancedModels::PrepareRender` is not merely
losing a race — it never runs, because the camera does not draw those objects. So the buffer is not
"left by the last of two writers", it is left by the only reader that is UNCONDITIONAL, while the
owner's restore is conditional on being drawn. Trace the resource before theorising about ordering;
the trace named the answer in one run, where reading the two declaration sites suggested the wrong
one.

Fixed the same way as P12.1 and P12.3: the borrower returns what it borrowed. `PrepareCullPass` gains
a closing point that hands each GI caster's instance buffer back, and `RecordCull` performs the
matching transitions in the same order with the same skips. The target is
`Renderer::GetCanonicalState(giBuf)`, not a literal — the buffer belongs to the object, so the cull
restores it to wherever its OWNER declared it rests, and that stays true if the owner ever moves.

Verified: Debug `demo` `frame end: 0 of 170` (was 2), Release `demo` and the 30-iteration stress both
`0 of N` with `CLEAN` and no `MISSING`, Debug `--gbv` produces no log. Pixel-neutral on the
deterministic recipe — mean **0.052** / 2.77% of pixels against a floor of **0.225** / 5.92%, with the
second pairing landing exactly on the floor. The surviving `duplicate-name GpuInstanced.Instances: 2`
line is correct and not a defect: `demo` really does have two GPU-instanced objects.

**NOT A DEFECT, do not "fix" it by muting:** the three surviving `INFO extra (registered, pass did
not perform)` lines — `Ocean.Wetness*` from `Main_Compose` (24), `Ocean.WetnessStamp*` from
`Main_Transparent` (30), `Exposure.Value` from `Main_Tonemap` (34). That direction fires when a pass
DECLARES a state but its body never calls a named `Renderer::Transition`; all three bodies simply
bind the resource, which is correct. `RenderGraph.h` mutes the direction only for bodies that drive
`EmitPoint` markers. They alternate A/B because the declaration names the CURRENT slot. The FATAL
direction (`MISSING`) is what these runs are gated on, and it is silent.

### P13 — Fix the UE SSR march: its reflections come out SLANTED — DONE (2026-08-18)

`shaders/ssr_trace_ue.hlsli` transcribes Unreal's own SSR ray cast (SSRT/SSRTRayCast.ush,
`InitScreenSpaceRayFromWorldSpace` + `CastScreenSpaceRay`). It ran much cheaper than the log march,
but its output was wrong in a way that was obvious by eye and invisible to the original metric.

**THE SYMPTOM LOOKED GEOMETRIC.** A vertical trunk reflected in a horizontal mirror must come back
vertical. Ours came back slanted and torn, with a regular diagonal comb growing along long grazing
reflections. The projected ray itself was ultimately correct; the coherent sampling field made its
misses line up in the ray direction and impersonate a slope error.

**HOW NOT TO CHASE IT.** Five hypotheses were tested and killed by measurement; do not re-run them:

| tried | result |
|---|---|
| self-intersection at the origin (offset the start along the normal) | agreement moved 0.2 points |
| steps too coarse (16 -> 32) | +3 points |
| the ray stretching past its geometric length (clamp the clip factor to 1) | **worse**, 68 -> 39 |
| ray length (`WorldTMax = SceneDepth` -> the shared budget) | +0.1 points |
| HZB mip 1 -> mip 0 | +2 points |

And the reduction is NOT the difference: their `FurthestHZBOutput_0 = MinDeviceZ` (HZB.usf) is
exactly ours.

**AND DO NOT SCORE IT THE WAY IT WAS SCORED.** Hit-mask IoU against another tracer says nothing
about whether a reflection is in the right PLACE: this march scored 79.1% against hardware RT versus
the log march's 70.7% purely because its mask is WIDER (36.98% vs 31.48% of frame), so it covers
more of RT's hits by area while the reflections inside it are slanted rubbish. A pixel-count metric
cannot see a wrong image. Judge it by looking, and measure with something that has a notion of
POSITION -- e.g. compare the traced hit UV against the analytically reflected position on a flat
floor, where the right answer is a closed form and the error has a direction.

**ROOT CAUSE AND FIX, PART 1 -- SPACE.** UE phase their fixed samples with noise evaluated at integer
pixel coordinates (`InterleavedGradientNoise(SvPosition.xy, ...)`). The port reused `Hash12`, but
called it as `Hash12(pixelCoord * invScreenSize)`. Adjacent pixels therefore differed by roughly one
texel divided by the render extent before hashing and received strongly correlated phases. On a long
ray, the same one of 16 sparse intervals won across a whole neighbourhood, producing the diagonal
comb. Pixel-scale decorrelation removes that coherent slope.

**ROOT CAUSE AND FIX, PART 2 -- TIME.** The integer-coordinate fix was necessary but incomplete: it
left the same fine-grained holes in all frames, so `ssr_temporal_cs.hlsl` had no complementary ray
samples to accumulate. UE do not use a static phase. `SSRTReflections.usf` passes
`View.StateFrameIndexMod8` to `InterleavedGradientNoise`; `SceneRendering.cpp` sets that field to
`FrameIndex % 8`, and the SSR TAA/denoiser consumes the eight interleaved results. The port now carries
the renderer's modulo-8 frame index in the SSR constant buffer and uses UE's exact
`RandomInterleavedGradientNoise.ush` formula. Opaque and glass SSR receive the same frame seed. The
eight-frame cycle moves the one-ray/16-step lattice through complementary positions without changing
the ray, tolerance, mip, or number of depth taps.

**VERIFIED ON THE REPORTED VIEW.** `data/levels/ssr_bronze_palms.json`, camera position
`5.89, 0.21, 8.07`, quaternion `0.0023, 0.9947, 0.0234, -0.0998`, wind frozen. With temporal resolve
disabled, the before hit mask has the reported parallel diagonal streaks; after the spatial change
they are gone and reflected trunks remain vertical. A second A/B kept temporal enabled and compared
the old static phase with the modulo-8 UE sequence. Mean total variation of the fullscreen hit-mask
capture fell from **0.015603 to 0.013864 (-11.1%)** and mean absolute Laplacian from **0.005677 to
0.004858 (-14.4%)**: fewer one-pixel discontinuities, not just a different random pattern. The
modulo-8 result also differs from the static result on 11.5% of pixels by more than 5%, confirming
that temporal accumulation receives genuinely complementary samples. Release_Editor compiled and
ran the shader successfully. The current Release GPU profile measures `Pass_ReflectionSource` at
**0.026 ms** and `Pass_Reflection.Temporal` at **0.013 ms**. The earlier same-view comparison measured
**0.035 ms** for UE march versus **0.185 ms** for log march (**5.3x cheaper**); the phase sequence
adds only the seven-ALU UE noise expression and no depth fetches.

**UE QUALITY MODES AND ROBUST HIT CONFIRMATION (2026-08-18).** The port now exposes the actual
`r.SSR.Quality` layouts from `SSRTReflections.usf`, rather than a single approximate quality:

| preset | steps per ray | rays per pixel | rough reflection sampling |
|---|---:|---:|---|
| Low | 8 | 1 | no |
| Medium | 16 | 1 | no |
| High | 8 | 4 | visible-normal GGX |
| Epic | 12 | 12 | visible-normal GGX |

High and Epic read material roughness from GBuffer0 and use UE's PCG/Hammersley visible-GGX
sampling. As in UE, surfaces below roughness 0.1 collapse the whole quality budget into one mirror
ray, capped at 24 steps. This avoids paying for identical rays on the polished ocean while retaining
the real multi-ray path for rough reflectors. Glass currently uses the explicit roughness override
because its reflection prepass does not carry GBuffer0 roughness.

The developer window exposes the preset plus Custom controls for steps, rays, GGX sampling, surface
roughness/override, start mip, depth tolerance, confirmation retries, and full-depth refinement. The
same values are scriptable as `ssr.ueQuality`, `ssr.ueSteps`, `ssr.ueRays`, `ssr.ueGlossy`,
`ssr.ueUseSurfaceRoughness`, `ssr.ueRoughnessOverride`, `ssr.ueStartMip`, `ssr.ueTolerance`,
`ssr.ueConfirmRetries`, and `ssr.ueRefineSteps`.

Stock UE accepts the first coarse HZB overlap, which is fast but can stretch thin foliage and spheres
along the ray. Setting confirmation retries to zero reproduces that rule. The default robust extension
(`confirmRetries=4`, `refineSteps=4`) checks a coarse candidate against full-resolution depth,
subdivides its local interval, and continues marching when the candidate is rejected instead of
turning an early false overlap into either an elongated hit or a miss. Start mip 1 and depth tolerance
scale 4 remain UE's defaults.

The second reported view was reproduced exactly at camera `-11.32, 0.82, -35.25`, quaternion
`-0.0183, -0.1453, -0.0027, 0.9892`, with frozen wind. The robust path tightens the long sphere and
foliage streaks while retaining more distant hits than the log march; it cannot recover geometry that
never exists in screen space. At DLSS render scale 0.58, Epic on the polished ocean measured
`Pass_ReflectionSource` at **0.185 ms** and temporal resolve at **0.015 ms**. Forcing roughness 0.35
to exercise the full 12-ray Epic path measured **0.385 ms**; that is a deliberately expensive quality
mode, not the normal smooth-ocean cost.

**OPTIONAL LOGMARCH FAST PATH.** The main SSR material now compiles
`SSR_LOGMARCH_OPTIMIZED=1`; `ssr_trace_logmarch.hlsli` defaults it to 0, so omitting the material
define restores the legacy 16-step algorithm. The optimized permutation keeps all 128 coarse samples,
the stride/thickness growth, hit test, and visibility calculation. It makes two targeted changes:

1. Because this renderer uses reversed Z, `rayViewZ > sceneViewZ` is exactly equivalent to
   `rayDeviceZ < sceneDeviceZ`. Coarse misses and bisection decisions are therefore made directly in
   device Z, avoiding a reciprocal per sampled depth; the accepted hit is still converted to view Z
   for the authored world-space thickness.
2. Bisection uses 12 iterations instead of 16. At the source depth resolution the last four steps
   are far below a pixel and changed no visible structure in the reported scene. Both the main SSR
   and ocean reflection materials now opt in; the reported strict A/B below covers the main output.

Strict A/B used the reported camera and frozen wind with DLSS and temporal disabled, so projection
jitter and history could not hide a difference. Against the original raw hit-mask capture, mean
absolute 8-bit error is **0.0231 / 255**; only **0.719%** of pixels change at all and **0.105%** differ
by more than 5/255. The two masks are visually indistinguishable. Repeated Release GPU profiles put
the legacy path at **0.210-0.243 ms** and the conservative 12-step permutation at **0.158-0.162 ms**
(roughly **23-35% faster** in this view). A more elaborate homogeneous-ray/same-texel solve was
tested and removed: its extra live state/register pressure erased the saved work.

**OPTIONAL LOGMARCH BATCH4 SCHEDULE.** The coarse loop also has an independent
`SSR_LOGMARCH_BATCH4` permutation. The include defaults it to 0 and rejects enabling it without
`SSR_LOGMARCH_OPTIMIZED`; both the main SSR and ocean reflection materials opt in with
`SSR_LOGMARCH_BATCH4=1`. The permutation advances the exact scalar stride/thickness recurrence
four times into compact `float4` state, projects the four candidates, then issues all four depth
reads before consuming the first result. Only the request schedule changes: the earliest valid
crossing still enters the same 12-step refinement and hit builder. The terminal batch may therefore
over-fetch at most three depth taps, but it cannot select a later crossing.

The deterministic raw-mask A/B used the reported camera, frozen wind, native resolution, temporal
off, and DLSS off. Against the sequential optimized permutation, mean absolute 8-bit error is
**0.000793 / 255** over RGB; **0.0603%** of pixels change at all, **0.00502%** differ by more than
1/255, and **0.00174%** differ by more than 5/255 (maximum channel delta 12). Two repeated native
Release GPU profiles measured `Pass_ReflectionSource` at **0.146-0.147 ms** for Batch4 versus
**0.152-0.154 ms** sequential, a small but repeatable **4-5%** gain in this view. DLSS-quality runs
were close to the profiler noise floor, so no larger speedup is claimed.

**THE OCEAN PLANE NOW FOLLOWS THE SAME TECHNIQUE SWITCH, AND LOGMARCH REMAINS THE DEFAULT.**
`ocean_reflection_cs.hlsl` traced the log march unconditionally, so `ssr.technique` moved every
surface in the frame EXCEPT the largest reflective one -- the control did not tell the truth. It now
takes the same branch as `ssr_cs.hlsl`: t2 binds the furthest pyramid, the constant block carries
the identical technique/pyramid/`ueX` values, and `useHzb == 0` still falls back to the log march.
Two details are specific to this pass. The reflector is the water PLANE, not the displaced surface
(the wave normal is applied later, when the ocean shader samples this buffer), so it is always UE's
Roughness < 0.1 case: the ray count collapses on the CPU via `UeSsrMirrorRaySteps` and never reaches
the shader. And the noise is seeded with the dispatch's own integer coordinates rather than
`uv * screenSize`, because this target can be scaled below render resolution and a stride above one
texel re-correlates the interleaved-gradient phase -- the exact defect P13 fixed. The clamps that
both passes share moved into `ResolveUeSsrSettings` so there is one copy of them.

**The default stays `SsrTechnique::LogMarch`, decided by the image, not by the cost.** The UE march
is correct after P13 and it is the cheaper search, but on water the log march's dense mask is
markedly the better picture, and water is the largest reflective surface in these scenes. Measured
on `atoll` with frozen wind, DLSS off and GTAO off: the run-to-run noise floor is **0.0397%** of
pixels changed, the default frame versus an explicit `ssr.technique:0` differs on **0.0203%** (below
the floor, i.e. the ocean's default path is unchanged by this work), and `ssr.technique:1` moves
**0.53%** of pixels -- the switch is live and now includes the water. Gates: both configurations
build, 17/17 shaders compile (`ocean_reflection_cs.hlsl` was missing from `tools/check_shaders.py`
and is now listed), the barrier comparator reports **0 MISSING** with the off-canonical set
unchanged from baseline, and Debug GBV reports only the pre-existing id=1328 warnings tracked as
P12.2. Note for whoever runs that gate next: `--gbv` on a Release binary validates NOTHING, because
the debug layer and `SetEnableGPUBasedValidation` sit under `#ifdef _DEBUG`.

### P14 — Where the log march's time actually goes, and what that says about HZB skipping — MEASURED (2026-08-18)

A proposal came in to add conservative empty-space skipping over the CLOSEST pyramid, with every
candidate confirmed against full-resolution depth. The chain choice is right (closest = `max` device
Z under reversed-Z = the NEAREST surface, which is what "is the ray in front of everything in this
tile" needs). Before implementing it, the log march was priced by ablation, because a skip only ever
buys back time spent marching through empty space.

Two compile-time budgets were added to `ssr_trace_logmarch.hlsli` — `SSR_LOGMARCH_COARSE_STEPS` and
`SSR_LOGMARCH_REFINE_STEPS`, both stated at their defaults in `SceneResourceBootstrapper`. Refine 0
takes the hit at the coarse crossing (`uvHigh`/`depthHigh` are already seeded from it); coarse 0
leaves nothing but the pass's own setup. `ssr_bronze_palms`, level default camera, wind frozen,
native resolution, SSR forced, GTAO off, `--trace=120`, median of 122 GPU samples per config:

| coarse × refine | `Pass_ReflectionSource` |
|---|---:|
| 0 × – (setup only) | 0.0100 ms |
| 64 × 0 | 0.0420 ms |
| 128 × 0 | 0.0530 ms |
| 64 × 12 | 0.0590 ms |
| **128 × 12 (shipping)** | **0.0740 ms** |

Which decomposes exactly (the parts sum to the whole, which is the check that the ablation is sound):

* setup / G-buffer read / write — **0.0100 ms, 13.5%**
* coarse steps 1–64 — **0.0320 ms, 43%**
* coarse steps 65–128 — **0.0110 ms, 15%**
* 12 bisection steps — **0.0210 ms, 28%**

**The coarse loop is NOT exhaustion-dominated.** Doubling the budget from 64 to 128 costs only 34% of
what the first 64 cost, so most rays leave it early — on a hit or by walking off screen — and only a
minority run deep. That deep tail, 15% of the pass, is the ENTIRE budget an empty-space skip can
target. Bisection, the second-largest block at 28%, is untouched by any skip: it is sub-pixel
refinement AFTER a crossing is known.

**And the prerequisite is not free.** The closest chain is not built at all today
(`ssrHizActive_ = false`, SceneRenderer.cpp — nothing reads it since the HiZ tracer left the SSR
path). Enabling `writeClosest` moved `Pass_Hzb` from **0.0350 to 0.0430 ms**, so the second pyramid
costs **+0.0080 ms** every frame.

**Verdict: a PERFECT skip nets 0.0110 − 0.0080 = +0.003 ms, about 4% of the pass** — and that ceiling
assumes the traversal itself is free, which is exactly the assumption `shaders/ssr_trace_hiz.hlsli`
already disproved (dependent per-mip reads and divergent loop counts, against a coarse loop whose
measured wins came from INDEPENDENT batched reads). The idea is sound in general; on this pass, in
this engine, it is chasing 15% and paying 11% of it up front. If the coarse march is to be attacked,
the honest target is steps 1–64 (43%), and the cheap shape is UE's own: one FIXED-mip probe used to
advance the stride, not a hierarchy walk.

**THE BUDGETS STAY COMPILE-TIME, AND THE MEASUREMENT IS WHY.** They were made runtime knobs first —
`SceneRenderSettings::ssrLogMarch`, constant-buffer fields, `--set=ssr.logSteps`/`ssr.logRefine`, two
developer-window sliders, the ocean pass following the same settings. It worked, and
`--set=ssr.logRefine:0` reproduced the 128×0 row of the table above exactly (**0.0535 ms** at runtime
versus **0.0530 ms** compiled). Then it was priced:

| budgets | `Pass_ReflectionSource` |
|---|---:|
| compiled in | 0.0710 / 0.0740 / 0.0740 / 0.0750 ms |
| in the constant buffer | 0.0770 / 0.0770 ms |

**+0.003 ms, about +4%**, repeatable to the digit — a dynamic loop bound costs the compiler the trip
count. So the whole runtime path was reverted: settings struct, CB fields, `--set` keys and sliders
all removed rather than left inert, because a knob nobody can turn is worse than no knob. A fixed
compile-time bound with a runtime `break` would not have recovered it either; the break defeats the
unrolling exactly as the dynamic bound does.

What survives is the useful half: `SSR_LOGMARCH_COARSE_STEPS` and `SSR_LOGMARCH_REFINE_STEPS`, stated
at their defaults in `SceneResourceBootstrapper`'s material description. Still compile-time, so the
budgets cost nothing, but the ablation above is reproducible by editing two numbers and rebuilding
rather than by rewriting the tracer.

**HARNESS BUG FOUND AND FIXED WHILE DOING THIS.** `--set=` was parsed with a single `strstr`, so only
the FIRST `--set=` flag on a command line was applied and any further ones were silently dropped —
the documented `--set=a:1;b:2` form worked, two separate flags did not. That is how the first attempt
at this measurement ended up profiling `Pass_RTReflections` (the level defaults to
`ReflectionSource::RT`) while believing it had forced SSR: the log said the settings that were TYPED,
not the ones that ran. `main.cpp` now loops over every occurrence. Any earlier measurement in this
document that passed two `--set=` flags should be re-read with that in mind.

---

### P15 — Volumetric fog: a UE-style froxel volume — NOT STARTED (queued 2026-08-19)

**Depends on:** P7 (the medium's parameters and their level plumbing already exist), P4 (exposure),
and the shadow work (VSM/CSM) because the whole point of the volume is that light in it is
*shadowed*. Independent of P8-P11; it does not block them and they do not block it.

**Goal:** light shafts through the palm canopy, shadowed haze, fog that local lights actually
illuminate, and a ground layer that has thickness instead of being a per-pixel function of distance.
P7 gives every pixel the same haze whatever is between it and the sun; this step is what makes the
air itself part of the lighting.

**Why it is a separate step and not "P7 done properly":** P7 is an analytic integral with no memory
of the scene, so it costs a handful of ALU in passes that already have depth bound. A froxel volume
is four new passes, three 3D targets and a temporal history. **The analytic fog does not get
retired** — it stays as the far field, exactly as in UE, where the volume covers the near range
(`r.VolumetricFog.Distance`, 6000 uu ≈ 60 m by default) and the exponential height fog carries
everything beyond it.

**Touch candidates:** new `shaders/volumetric_fog_*.hlsl` (four entry points), `RenderTargetManager`
(three volume targets + history), `SceneRenderer` (pass registration and the barrier declarations),
`AtmosphereSettings`/`AtmosphereSettingsJson`/inspector (the volume's own knobs), `compose_cs.hlsl`
and both ocean surfaces (the lookup), `LightManager` (local-light injection).

**Implement, in UE's own four stages** (`VolumetricFog.usf`; each of ours should keep the UE name in
a comment so the correspondence survives):

1. **Voxelize the medium — `MaterialSetupCS`.** Write `VBufferA` = scattering RGB + extinction A and
   `VBufferB` = emissive RGB + phase g, from the SAME `AtmosphereParams` the analytic model uses, so
   the two cannot describe different air. UE's froxel grid is `GridPixelSize` screen pixels wide
   (8-16) with `GridSizeZ` slices (64), distributed **logarithmically**:
   `ZSlice = log2(depth * GridZParams.x + GridZParams.y) * GridZParams.z` (`Common.ush:2379`). Do not
   invent a distribution — the log layout is what makes 64 slices enough.
2. **Inject shadowed local lights — `InjectShadowedLocalLightPS` + `WriteToBoundingSphereVS`.** Only
   the froxels inside a light's bounding sphere are touched, rasterised via that sphere rather than
   dispatched over the whole grid. Our spot/point shadows already exist (see
   [[spot-lights-refactor-plan]]); this is where they earn a second use.
3. **Light the froxels — `LightScatteringCS`.** Directional light with its shadow term, local lights,
   and the sky, each weighted by the **Henyey-Greenstein phase function**
   (`ParticipatingMediaCommon.ush:91`) against the froxel's view vector. UE reproject the previous
   frame's `LightScatteringHistory` here and super-sample only on a history miss
   (`HISTORY_MISS_SUPER_SAMPLE_COUNT`) — the volume is too coarse and too noisy to be believable
   without that, so temporal reprojection is part of the step, not a later optimisation.
4. **Integrate along Z — `FinalIntegrationCS`.** Front-to-back accumulation into
   `IntegratedLightScattering` (RGB = in-scattered light, A = transmittance), using Frostbite's
   energy-conserving form UE call out in the comment:
   `(S - S*T) / max(extinction, 1e-5)` rather than `S * stepLength`. Near fade-in is applied here
   (`VolumetricFogNearFadeInDistanceInv`), which is why the consumer's start-distance test can be a
   step function.

**Then the lookup, and this is the part that keeps P7 intact.** UE's `CombineVolumetricFog`
(`HeightFogCommon.ush:420`) is one line worth transcribing exactly:

```hlsl
return float4(VolFog.rgb + GlobalFog.rgb * VolFog.a, VolFog.a * GlobalFog.a);
```

The volume is the NEAR slab and the analytic fog is what lies beyond it, attenuated by the slab's
transmittance. Both consumers we already have — `compose_cs.hlsl` for opaque and both ocean surfaces
— gain the same 3D lookup at `ComputeVolumeUVFromNDC`'s UV, and the existing analytic block becomes
the `GlobalFog` term rather than being replaced.

**Interface contract:** with the volume disabled, the frame is screenshot-equivalent to P7 and **no
3D target is allocated** — same rule GTAO follows (off schedules no pass, rather than a pass that
writes a neutral value). Ships disabled.

**Done when:** the sun through the grove casts visible shafts that move correctly with the camera and
with wind-driven canopy motion; a spot light at night lights the air around it; and the P7 image is
reproduced exactly when the volume is off.

**Verify:** the three canonical `wind_test` views plus a night/spot-light view; camera inside and
above the layer (the 82 m case in P7's notes); native and DLSS; GPU timing of each of the four
passes separately; `--canonical-check` clean (this step adds resources and passes, so the full
barrier gate applies, unlike P7's shader-only edits).

**WHAT THIS PROJECT ALREADY KNOWS THAT CHANGES THE SPEC — read before starting:**

* **`skyBackScatter` is a stand-in for exactly what stage 3 does properly.** P7 added it because the
  analytic model has no phase function: it fakes the forward-peaked lobe by scaling the sky term with
  view-to-sun angle. Inside the volume the real HG phase applies per froxel, so the two must not both
  run — the volume's `PhaseG` is the honest parameter and `skyBackScatter` stays as the far field's
  approximation of it. Give them one authored control if the numbers can be made to agree.
* **`maxOpacity` has no business in the volume.** Its floor exists to keep distant shapes from
  vanishing in the analytic model, and P7's `AtmosphereMinTransmittance` already has to release it
  with depth to stop the horizon seaming. The volume's transmittance is a real integral; do not clip
  it.
* **The sky is never fogged, and that is still true.** The volume covers a near range and background
  pixels are not in it; the debug views must keep painting un-measured pixels black for the same
  reason P7's do.
* **Fix exposure before any A/B** (`--set=exposure.autoExposure:0 --dlss=off`). A volume changes
  average luminance more than P7 did, so metering will otherwise move the whole frame — including the
  sky — and the first measurement will look like a broken background test. This cost an hour on P7.
* **UE store `IntegratedLightScattering` PRE-EXPOSED** and divide it out at the lookup
  (`OneOverPreExposure`). Decide this deliberately given P4's exposure split; storing linear radiance
  in an FP16 volume is the thing pre-exposure exists to protect against.
* **Budget it before building it.** At 2560x1440 with a 16-pixel grid and 64 slices that is
  160x90x64 = 920k froxels; two RGBA16F volumes plus the integrated one plus history is roughly
  22 MB, and stages 1/3/4 each dispatch over the whole grid. If the measured cost lands far above
  P7's +0.003 ms on `Pass_Compose`, the grid size is the first dial, not the feature.

**Reference files needed from the drop** (all present in `ue_strip/Shaders/Private` as of
2026-08-19): `VolumetricFog.usf`, `VolumetricFogVoxelization.usf`, `VolumetricFogLightFunction.usf`,
`ParticipatingMediaCommon.ush`, and `HeightFogCommon.ush` (already used by P7). The C++ side is there too:
`Source/Runtime/Renderer/Private/VolumetricFog.cpp` carries the grid setup and the `GridZParams`
derivation, which the shader alone does not explain.

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

P8B and then P8C (convolution bloom, which is where flares come from) slot in after P8 whenever the
bloom look has been signed off; neither blocks P9-P11.

**P15 (volumetric fog) sits outside this ordering.** It is the largest single piece of new
infrastructure the plan carries — four passes, three volume targets, a temporal history — and it buys
light shafts and shadowed haze rather than correctness. Take it when the analytic look has been
tuned and found wanting, not before: P7's own knobs (density, height falloff, back-scatter, sky blur)
cover a lot of what "we need volumetrics" usually means, and they cost nothing.

Do not start final level grading before M2. The fastest route to the target image is to make exposure and environment lighting coherent first, then judge which finishing features are still visibly necessary.

---


**P12 and P13 (the sections above) are independent of the photographic order** and can
land whenever: P12.1 is a shadow-cull flow fix, P12.2 a mechanical buffer-state sweep. Neither
blocks P7-P11, and neither should be folded into one of them. P13 is the SSR tracer defect and
is likewise standalone -- the default reflection path does not depend on it.
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
| `PostProcessAmbientOcclusion.usf` + `.ush` + `.cpp` | **P6B GTAO**: horizon-based AO, the half-res + edge-aware upsample scheme, and the bilateral/temporal filtering | **have**, in `ue_ssao_ssgi/` |
| `SSRTDiffuseIndirect.usf`, `SSRTRayCast.ush`, `SSRTReflections.usf`, `ScreenSpaceRayTracing.cpp` | screen-space GI/reflection tracing — P9A's cheap bounce, and a cross-check for our SSR | **have**, in `ue_ssao_ssgi/` |
| the whole `SSD*` set + `ScreenSpaceDenoise.cpp` | the screen-space denoiser P6B needs — and the one the RT plan's S11 failed to hand-roll (1spp glossy + DLSS jitter = dancing noise) | **have**, in `ue_ssao_ssgi/` |
| `DiffuseIndirectComposite.usf`, `IndirectLightRendering.cpp`, `CompositionLighting.cpp` | how indirect diffuse/specular/AO are composited — the contract P6A, P6B and P9 all plug into | **have**, in `ue_ssao_ssgi/` |
| `ReflectionEnvironmentShaders.usf`, `SkyLightingShared.ush` | diffing F8's split-sum against the original. **Arrived 2026-08-17 and immediately earned its keep** — three real differences found (log vs linear roughness/mip mapping, cosine distribution at roughness>0.99, per-sample source mip from the solid-angle ratio) | **have**, in `ue_misc/` |
| `ReflectionEnvironmentShared.ush` | the body of `ComputeReflectionCaptureRoughnessFromMip` — the log roughness/mip mapping itself. Transcribed and shipped 2026-08-17 | **have**, in `ue_misc/` |
| `MaterialTemplate.ush` | `MaterialExpressionBlackBody`, the Planckian locus the sun's colour-temperature control is transcribed from | **have** |
| `PostProcessHistogramCommon.ush` | `CalculateLogLocalExposure` — P3B shipped WITHOUT this file, from the published algorithm plus measurement. Diff it against `shaders/local_exposure.hlsli` before extending P3B | **have**, `Shaders/Private/` |
| `PostProcessLocalExposure.usf` | the local-exposure apply pass and the exposure-fusion alternative | **have**, `Shaders/Private/` |
| `PostProcessHistogram.usf` | the bilateral-grid write inside the histogram pass — the P3B upgrade needs it | **have**, `Shaders/Private/` |
| `PostProcessLocalExposure.cpp` | bilateral grid construction parameters | **have**, `Source/Runtime/Renderer/Private/PostProcess/` |
| `VolumetricFog.usf` | **P15**: all four froxel stages — `MaterialSetupCS`, `InjectShadowedLocalLightPS`, `LightScatteringCS`, `FinalIntegrationCS`, plus the Frostbite energy-conserving integration | **have** |
| `VolumetricFogVoxelization.usf`, `VolumetricFogLightFunction.usf` | P15: voxelising fog volumes, and light functions inside the volume | **have** |
| `ParticipatingMediaCommon.ush` | P15: `HenyeyGreensteinPhase` — the real phase function `skyBackScatter` currently approximates | **have** |
| `GPUFastFourierTransform.usf`, `GPUFastFourierTransformCore.ush`, `GPUFastFourierTransform2DCore.ush` | **P8C**: the whole transform -- radix 2/3/4/8 butterflies, the group-shared single-dispatch convolution, the multi-pass decomposition for large sizes, the two-for-one real-channel trick, and `CopyWindowCS` for the zero pad | **have** |
| `Bloom/*.usf` (`BloomFindKernelCenter`, `BloomSurveyKernelCenterEnergy`, `BloomResizeKernel`, `BloomClampKernel`, `BloomPackKernelConstants`, ...) | P8C: turning a kernel image into a normalised, centred, resolution-scaled filter. Most of the work that is not the FFT | **have** |
| `PostProcessFFTBloom.cpp`, `GPUFastFourierTransform.cpp` | P8C: the size policy, the padding rule and the pass ordering. **Read, and the numbers are now in P8C itself** | **have**, `Source/Runtime/Renderer/Private/[PostProcess/]` |
| `PostProcessLensFlares.usf` | P8C's rejected alternative (bokeh scatter). Kept in case that trade is revisited | **have** |
| `LensDistortion.ush` | optional chromatic/edge treatment | **have** |
| `VolumetricFog.cpp` | P15: grid setup (`GridPixelSize`, `GridSizeZ`, `Distance`), the `GridZParams` derivation, and the cvar defaults. The shader alone does not say how the log Z distribution is built — read this before implementing | **have**, `Source/Runtime/Renderer/Private/` |

Everything is dropped in the **root** of `D:\Programming\ue_autoexposure\`, not under the
engine-relative subpaths — look there first.

**BEFORE MARKING ANYTHING "wanted", SEARCH BOTH TREES OF `ue_strip`.** It carries `Shaders/` AND
`Source/` (~24.5k C++ files under `Source/Runtime/Renderer` and `Source/Runtime/Engine`), at their
real engine-relative paths. Six rows in this table said "wanted" for files that were present all
along, because only `Shaders/` had been searched. `ue_strip/README.md` is the map.

**2026-08-17: `D:\Programming\ue_strip\` supersedes the three partial drops.** ~26.6k files: the
whole `Shaders/Private` tree (`Bloom/`, `PostProcessing/`, `SkyAtmosphere.usf`, `HeightFogCommon.ush`,
`Lumen/`, `ACES/`, `RayTracing/`, `Nanite/`, …) plus `Source/Runtime/Renderer` and
`Source/Runtime/Engine`. Engine-relative paths are intact, so a file wanted above can be located by
its UE path directly. It settled an open question the same day it arrived:
`CompositionLighting/PostProcessAmbientOcclusion.cpp` — absent from `ue_ssao_ssgi/` — carries
**`AmbientOcclusionTemporalBlendWeight = 0.1f`** (`Engine/Private/Scene.cpp:535`, clamped to
[0.01, 1], UI max 0.5) and `r.GTAO.FilterWidth = 5`. That replaced a guessed 0.15 in P6B item 4 and
confirmed the 5x5 kernel in item 3. **Look here first**; the older drops stay listed only because
paths elsewhere in this document already point at them. P7 (`SkyAtmosphere*`, `HeightFogCommon.ush`)
and P8 (`Bloom/`) are no longer blocked on a drop.



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


---

### P8D — The kernel is the diffraction pattern of the aperture — DONE (2026-08-19)

The convolution kernel was DRAWN: a radial core, plus N straight spokes, plus an anamorphic bar,
plus a chroma tweak. It is now COMPUTED, as `PSF = |FT{aperture}|^2` -- Fraunhofer diffraction --
which is what a lens actually does to a point source. The engine already owned the transform, so the
whole change is: draw the IRIS instead of the pattern, and transform it.

**The invariant that proves it, and that no analytic sum of spokes obeys by accident:** a polygonal
aperture gives N rays when N is EVEN and 2N when N is ODD, because opposite parallel edges throw
collinear spikes that overlap. Measured on `wind_test`: `blades = 5` -> 10 rays, `6` -> 6, `8` -> 8.
The rendered star also carries the sinc side-lobe BEADING along each spike, which is the visible
signature of transforming a hard-edged polygon and which the drawn kernel never had.

**What now follows from the shape instead of being authored beside it:** the number of rays, their
length, their relative brightness, the beading, and the shape of the core.

**Dispersion runs the opposite way to the old kernel, and the old one was wrong about it.** The
pattern scales with wavelength, so BLUE IS TIGHTER: `blueScale = 1 - 0.18 * chroma`, roughly
450/550nm at full strength. The previous kernel made blue WIDER, which is Rayleigh scattering -- a
real effect, but not one a diffraction pattern is made of.

**`anamorphic` is now an anamorphic APERTURE** (the iris is squeezed in x, so the pattern stretches
in x) rather than a bar added to the result. Same control, real mechanism.

**SIZE IS INVERSE and it is worth saying twice**: a wide aperture gives a TIGHT pattern. The first
zero of a circular aperture of radius `a` in a grid of N sits near N/(2a), so the aperture radius is
`N / (2 * kernelRadius)`. Getting that backwards still looks plausible on screen, which is exactly
why it is a comment in the shader.

**Cost: none in a normal frame.** Two extra transforms are needed (aperture -> FT -> |.|^2, once per
lane), but they run only on a kernel REBUILD, and the spectrum is cached by shape. Measured steady
state: `Pass_BloomConv` 0.223 / 0.222 ms across two runs, against 0.247 recorded before.

The buffer ordering is the only subtle part, and each grid is scratch exactly when the next stage
needs it free: `A <- aperture; A->B->A` is the transform, `B.xy <- |A|^2` (B was just emptied by the
vertical pass), then the blue lane scratches through the kernel grid instead (nothing is in it yet),
and finally `B -> A -> K` turns the packed PSF into the spectrum the multiply reads. Each lane is
written REAL, because the frequency-domain multiply is a per-channel convolution only while the
kernel is real.

**FOUR CONTROLS WERE REMOVED, not disabled** -- `convSpokeStrength`, `convSpokeLength`,
`convSpokeWidth`, `convFalloff`. They described a kernel that was DRAWN, and diffraction decides all
four. Leaving them was the "a control that lies" failure in its purest form: the user set
`convSpokeStrength: 0`, still saw an eight-pointed star, and had no way to learn that `blades` had
become the control that makes it. Removed from the settings struct, the JSON reader/writer, the
inspector, the dev window, `--set`, the shader constant buffer and the kernel cache key; the dead
`KernelValue()` and its stage went with them.

Everything they used to reach is still reachable, through the thing that actually causes it:
rays-how-many is `blades`, rays-how-far is `kernelRadius` (the whole pattern's scale),
rays-how-wide follows from the width of each flat, and the core's skirt is whatever a hard-edged
iris gives. **`blades = 0` is now the way to have no star at all**, and it is measured: bloom-only
angular contrast around the sun is 72:1 at `blades = 8` against 3.9:1 at `blades = 0`, where the
residual is Airy ring structure.

**WHAT UE ACTUALLY DO FOR GHOSTS, having now read `PostProcessLensFlares.usf/.cpp` end to end --
because the mip chain here was INVENTED and they have nothing of the kind.** Theirs is a SCATTER:
`LensFlareBlurVS` emits one quad per pixel of a downsampled scene (`TileSizeInPixels = 1`), reads
that pixel at `SampleLevel(..., 0)`, kills the quad when its luminance is under `Threshold` by
collapsing it to zero size, and `LensFlareBlurPS` multiplies the tile colour by a BOKEH SPRITE. The
softness is the sprite splat, not a mip. Then `AddLensFlaresPass` composites `LensFlareCount` copies
of that image, each scaled about the screen centre, where **the scale is encoded in the ALPHA of
`LensFlareTints[8]`**: `(A * (N-1) - (N-1)/2) * GuardBandScale`. With their defaults that is
+1.40, +0.42, -0.56, -1.54, -2.66, -3.22, -3.92, -4.90 -- two on the same side, six mirrored, and
most of them BIGGER than the source, which is why their ghosts are large clean discs. Photographic
reference agrees on the shape: ghosts are polygons of the iris, large and soft wide open, small and
sharp stopped down, strung along the line from the light through the frame centre. **The rework is
therefore: splat an aperture-shaped sprite per bright source pixel, then scale/tint copies of it --
which also deletes the ghost mip chain and the 0.116 ms it costs.** NOT done yet.

**SUPERSEDED -- the four controls listed below were removed as described above:** `convSpokeStrength`, `convSpokeLength`,
`convSpokeWidth` and `convFalloff`. Diffraction decides all four. Leaving them is exactly the
"controls must not lie" failure -- they still appear in the level, the inspector and `--set`, and
they now do nothing. Removing them touches the settings struct, the JSON reader/writer and the
inspector (the P8B fold path), and it changes levels that carry the values, so it is its own
increment rather than a silent edit here.

### P15B — The ocean clamped its own HDR output to 1.0 — DONE (2026-08-19)

`float4 outColor = float4(saturate(color), 1.0f)`, in both ocean surface variants, writing into
`R16G16B16A16_FLOAT`. The sky is written unclamped, so the water was structurally incapable of
matching the sky it reflects. Measured at the horizon with a fixed exposure, comparing mirrored rows
(sky at `hz-d` against water at `hz+d`):

| d | before | after |
|---|---|---|
| 20 | -66.0 / -69.9 / -69.7 | -12.5 / -6.6 / -1.0 |
| 120 | -36.4 / -58.6 / -71.9 | -1.2 / -2.5 / -1.8 |
| 300 | -9.5 / -32.5 / -63.6 | +2.3 / -0.2 / -1.1 |

Before the fix the water's blue channel sat at ~129 across the whole surface: not a blown highlight
(those go to 255) but a CEILING. **That is the tell, and it is worth writing down: a flat,
achromatic, direction-independent, mid-range value is a clamp downstream, not a bright value
upstream.** The cubemap, its mip chain, the prefilter, the roughness and the reflection direction
were all investigated first; none of them could have closed a gap that was being imposed after all
of them.

The same run also settled two changes made earlier on hypotheses that did not survive:
* `IblClampToSharp` (a blurred sky sample may not exceed the sharp sample in the same direction by
  more than 2x, shared by the fog and the water). With fog on, flat-slab pixels went 9904 -> 292 from
  the `saturate` fix alone, and 292 -> 18 from this bound. Kept, but it is a 16x reduction of a
  residual, not the fix it was written to be.
* The FP16 clamp in the importer stands on its own evidence (spec mip 0 at 524,711 against a 65,504
  ceiling) and is unrelated to this.

### P16 — Physical light units, in the only order they can be done — IN PROGRESS (2026-08-20)

**THE DEFECT, MEASURED.** On `wind_test`, open sand at EV 4, each term isolated:

| | screen value |
|---|---|
| sun alone, `sunIntensity = 2` (the level's value) | **7.51** |
| sun alone, `sunIntensity = 20` | 77.20 (x10.3 -- the sun is linear, nothing is clamping it) |
| **sky alone** | **76.94** |

**The sky lights the scene ten times harder than the sun.** Physically it should be about five times
WEAKER: a clear day is ~100,000 lx of direct sun against ~20,000 lx of skylight on a horizontal
surface. The scale is wrong by a factor of fifty, and no control fixes that -- with the sun at the
level's value the shadow of a palm frond measures BRIGHTER than the sand beside it (ratio 0.94), and
raising the sun alone only clips the highlights (21% of the frame saturated at `sunIntensity 256`).

**THE CAUSE IS THAT THE TWO ARE NOT ON A COMMON SCALE.** The sky's absolute level comes from the
importer's calibration, which pins its MEDIAN LUMINANCE to 0.18 -- a display-referred number,
"middle grey after exposure". The sun is a bare multiplier authored next to it. There is no physical
unit anywhere and no step that divides either by the exposure, so the ratio between them is an
accident of two unrelated conventions.

**WHAT TWO ENGINES DO, both checked in source rather than from memory.**

*Godot* (`scene/resources/camera_attributes.cpp:424`) computes one normalisation from the camera:

```
e = (aperture^2 * shutterSpeed) * (100 / ISO)
exposure_normalization = 1 / (e * 1.2)
```

and then multiplies EVERY light by it before rendering -- `sky.cpp:1088` does exactly that to the
sky, right after multiplying by the light's `LIGHT_PARAM_INTENSITY` in lux. Lightmaps, reflection
probes and SDFGI all carry the same factor. Physical values go in; numbers near 1 reach the shader.

*Unreal* does the same thing under a different name: `FViewInfo::UpdatePreExposure()`
(`PostProcess/PostProcessEyeAdaptation.cpp:1197`) sets `View.PreExposure` to the exposure the
tonemapper is about to apply, everything written to scene colour is scaled by it, and the tonemapper
divides it back out through `View.OneOverPreExposure`.

*Both* also keep the sky's LIGHTING contribution as a control separate from how bright the sky
LOOKS -- Godot's `ambient_light_energy` (documented starting range 0.3-0.8, "lower values produce
harder shadows") and `ambient_light_sky_contribution`; UE's `SkyLight.Intensity` in cd/m2. And both
OCCLUDE it (SDFGI/SSAO, DFAO/Lumen) rather than letting it fill shadows flat.

**THE ORDER IS FORCED, and it is the reverse of what this plan said yesterday.** Pre-exposure first,
units second. A 100,000 lx sun cannot be written to an `R16G16B16A16_FLOAT` target -- the ceiling is
65,504, and this project has already lost a day to exactly that wall with a sky whose peak was
524,711 (see the FP16 clamp in P15B's neighbourhood). Introducing physical units before the
normalisation exists would reproduce that failure by construction.

---

#### P16.1 — Pre-exposure: divide by the exposure before writing, multiply back at the tonemap

One factor, computed once per frame from the exposure the tonemap will apply, applied by every
producer of scene colour and undone by the tonemap. Nothing in the image changes; the numbers stored
in the FP16 target move from "raw radiance" to "around 1".

Touches, and every one has to agree or the frame comes out uniformly wrong:
* the base pass / lighting / sky / ocean / transparent writes -- every producer;
* `Pass_Tonemap`, which already computes the exposure and must not apply it twice;
* the bloom threshold, which measures in "the units the viewer sees" through `ExposureMultiplier()`
  in `bloom_cs.hlsl` and `bloom_conv_cs.hlsl` -- that helper and pre-exposure are two answers to the
  same question and must not both be live;
* `ExposureMetering`, which reads scene colour to compute the exposure it would then be scaled by.
  UE break the loop with the PREVIOUS frame's value and so must this;
* `DlssHandler.cpp:50`, `options_.preExposure = 1.0f` -- currently a literal telling DLSS our colour
  is unexposed. It becomes the real factor.

**Verification is an invariant, not a look**: with pre-exposure on, the maximum value ever written
to `scene` sits within a couple of stops of 1 for any sky. Instrument that FIRST -- it is also the
measurement that proves the old behaviour was broken.

**P16.1 IS TRANSPARENT AND VERIFIED (2026-08-20).** With the gate on, the whole frame differs by
**0.330 meanabs against a 0.34 run-to-run noise floor** -- i.e. not at all -- while the peak value
stored in scene colour falls from **2^10 to ~2^8.5**, which is the entire point. Turn it on with
`--set=render.preExposure:1`.

**THREE BUGS, and two of them were the same bug in different clothes.**

1. **The metering fed itself.** The histogram is built FROM scene colour, so once compose pre-exposed
   it the metering measured the scene times its own exposure, solved a shifted EV, and that EV became
   the next frame's factor. The instrument caught it as the peak going UP (2^10 -> 2^12). Both
   readers now divide the factor back out.
2. **THE SKY GOT IT TWICE.** The skybox draws BEFORE compose, and compose scales everything it
   writes -- including the sky pixels it passes through. 0.36 squared instead of 0.36 left the sky
   2.8x too dark. **THE RULE, and it is the whole design: compose applies the factor to everything it
   writes; only writers that run AFTER compose apply it themselves.** The skybox is not one of them.
3. **THE OCEAN GOT IT TWICE TOO, by READING.** It samples scene colour for refraction and the planar
   reflection -- already pre-exposed -- and then multiplies its whole result by the factor. Six reads
   across the two variants now divide it back out first. This was the last 20/255 on the near water.

**Cancellation is BY CONSTRUCTION, not by two numbers agreeing.** The first attempt had the writers
use a CPU factor from the exposure readback while the tonemap divided using the exposure it read from
the GPU buffer that frame -- the same quantity at different ages, which never cancelled. Now an
explicit `preExposureActive` flag says the writers already applied it and the tonemap applies
*nothing*; the gate-off path still reads the GPU value and stays byte-identical.

**One more consumer had to be told: LOCAL EXPOSURE.** It shifts its stored base by
`log2(exposureMultiplier)` so base and pixel share a space. The base is stored unexposed, so the
shift has to be the exposure the PIXEL ACTUALLY RECEIVED -- which, pre-exposed, is the writers'
factor and not what this pass applied. Using `exposureMultiplier` alone put them an exposure apart
and pulled the frame down by 10/255.

**GLASS, PARTICLES AND THE SSR HISTORY ARE NOW WIRED AND MEASURED (2026-08-20).** Finding the rest
of the list meant asking the question the other way round -- not "what draws late" but "what touches
scene colour at all":

* **glass** is BOTH halves at once. It draws in the transparent pass, so it applies the factor to
  what it writes; and its refraction tap reads `SceneOpaque`, a copy of scene colour taken at the
  top of that pass, so that read divides the factor back out first. Miss the divide and everything
  seen THROUGH the glass -- most of what glass shows -- is off by the factor.
* **particles** apply it to `rgb` only. Alpha is a blend weight against a destination that already
  carries the factor; scaling it would change the coverage, not the brightness.
* **SSR** is a READER, and the one that would have been missed: it resolves its hit colour from the
  PREVIOUS frame's scene colour, and its output goes into compose, which scales what it writes. So
  the hit has to come back to raw radiance first, using the factor the history was STORED with, not
  this frame's. That needs `prevPreExposure_` kept on the CPU. UE spell the same quantity
  `View.PrevSceneColorPreExposureCorrection`; the mechanism is not optional, it is what the design
  rule says out loud.
* **the two overlays** -- debug lines and the editor's selection outline -- write authored LDR
  colours into scene colour BEFORE the tonemap. Pre-exposure means the tonemap stops scaling them
  down, so they would blow out to white; both are pre-scaled on the CPU, no shader involved. These
  two are reasoned, not measured: neither is reachable from a headless capture.

The bloom threshold is handled: `exposureEnabled = 0` when the gate is on makes
`ExposureMultiplier()` return 1, which is right because its source is already in viewer units.

**HOW IT WAS MEASURED, and the two dead ends that shaped it.** `--sweep=render.preExposure:0,1`
gives the A/B inside ONE process; the noise floor is the same command with `0,0`.

The first run had a noise floor of **17.5 meanabs** -- the ocean moves between shots, so the metric
was measuring waves. `--wind-freeze` pins the ocean clock and drops it to 2.65; the rest is the
shore foam, so glass and particles are judged on a CROP over the thing under test, with the noise
floor measured on the SAME crop. Then the exposure is PINNED (`exposure.autoExposure:0` +
`exposure.manualEv100`) so the factor is a known constant rather than whatever the scene meters to
-- at the demo level's own EV the factor is 1.4, and a test point that close to 1 proves very little.

| view | factor | noise floor | gate 0 -> 1 |
|---|---|---|---|
| glass sphere crop, EV 3 | 0.18 | 0.527 | **0.517** |
| glass sphere crop, EV -2 | 5.76 | 0.413 | **0.441** |

### P8C-2 — The kernel becomes an IMAGE, and the ghosts become UE's — NOT STARTED (queued 2026-08-21)

**Depends on:** P8C (the FFT machinery it reuses unchanged) and P8D (whose aperture-PSF kernel it
RETIRES, and whose end-to-end read of `PostProcessLensFlares.usf/.cpp` it executes). Do not start
without re-reading both OUTCOME blocks — every trap listed there was hit once already.

**Why the current result looks the way it does — three diagnosed causes, none of them the FFT:**

1. **Resolution.** Our convolution runs on a 640x360 image inside a 1024x512 grid; UE's default is
   the FULL viewport (`r.Bloom.ScreenPercentage` default 100, `PostProcessFFTBloom.cpp:17`, clamped
   [0.1, max] in `GetFFTBloomResolutionFraction`). A 1-2 texel diffraction ray upscaled 4x per axis
   (then DLSS) is a dashed line of squares — the "ragged crown" IS this.
2. **Monochromatic diffraction.** The P8D kernel evaluates |FT{aperture}|^2 at effectively TWO
   wavelengths (R+iG share one kernel by the two-for-one packing; B has its own). A polygonal
   aperture's spike is sinc^2 interference WITH ZEROS — physically dashed at any single wavelength.
   Real photographic starbursts are smooth because they integrate the whole visible spectrum. No
   parameter fixes this; it is structural to a 2-sample spectrum.
3. **Ghost architecture.** Sprites placed at ghost positions of ONE source (`sunUV`) with an
   authored atlas is not what UE do at all — see P8D's read-out and the Implement below. The GPT
   atlas (grid lines, 0.3/255 black floor, additive leaks) was unrescuable content on top of the
   wrong mechanism.

**The asset that closes cause 2 exists and is already in the tree:**
`textures/DefaultBloomKernel.EXR` — 2048x2048, RGBA half, zip-compressed scanline EXR (header
parsed 2026-08-21). It is UE's shipped kernel: spectral integration, halo and rainbow rays are
baked in by the source photograph. NOTE `textures/` is NOT under git — copy it somewhere safe
before any import experiments, and every derived file written into `textures/` is a USER GATE.

**THE ONE REAL MATH CHANGE — a coloured kernel breaks the two-for-one packing.** `(R + iG)`
convolved by a single shared kernel is legal only while R and G share that kernel; a rainbow
kernel's R and G differ. The standard fix (UE's — it is why their frequency layout carries "+2
elements in the first transform direction"): Hermitian unpack at the spectral multiply. For a
packed transform Z of two real signals, `Fr(k) = (Z(k) + Z*(-k))/2` and `Fg(k) = (Z(k) - Z*(-k))/(2i)`;
unpack IMAGE and KERNEL at (k, -k), multiply per channel, repack. Memory unchanged, the multiply
reads mirrored texels (x2 reads). **Extend `scratchpad/fft_verify.py` and prove this in numpy
BEFORE touching the shader** — the P8C transform was verified to 5e-16 that way and it is the
reason the FFT itself has never been a suspect.

**WHAT UE'S GHOSTS ARE (verified in `PostProcessLensFlares.cpp/.usf`, 2026-08-21; P8D recorded the
same read):** input = a downsampled scene slice with a x2 guard band (off-screen sources still
throw flares) — NOT a light position. `LensFlareBlurVS/PS`: one instanced quad per input PIXEL
(`TileSizeInPixels = 1`), collapsed to zero size when `dot(rgb,1) < Threshold`, sized
`BokehSizePercent` of viewport width (default ~3), textured with a BOKEH SPRITE (the iris shape),
coloured by the pixel x `KernelAreaInverse`, additively rasterized. Then `LensFlareComposite`: N=8
copies of that blurred image, each one full-image quad scaled about the SCREEN CENTRE by
`(TintAlpha * (N-1) - (N-1)/2) * GuardBandScale` (+1.40 ... -4.90, negatives mirror), tinted, faded
by a double `DiscMask`, additively blended over a copy of the bloom. Ghost SHAPES are therefore the
actual defocused images of the actual bright sources — two suns give two chains for free, and no
atlas exists anywhere.

**Implement (in this order):**

1. **Kernel decode + import.** Zip-scanline RGBA-half EXR is ~50 lines of stdlib Python (zlib +
   struct + numpy half) — convert offline to an FP16 DDS the engine already loads, OR vendor tinyexr
   into the importer. Either way the write into `textures/` is presented to the user first. Sanity:
   dump the decoded kernel as PNG and LOOK at it (centre, rays, halo) before anything consumes it.
2. **Hermitian spectral multiply** (numpy first, then `bloom_fft_cs.hlsl`). Gate: the numpy port of
   the shader indexing must show per-channel convolution exact to float rounding, including the
   mirrored-index edge rows (k = 0 and Nyquist are their own mirrors — the classic off-by-one).
3. **Kernel pipeline stages.** Replace P8D's stages 3/4 (aperture draw + |FT|^2) with: resample the
   kernel texture into the grid at the size `convolutionSize` asks for (fraction of screen width,
   UE's `BloomConvolutionSize`), centre folded to the DC corner (P8C's fold comment), FFT once,
   cache per (kernel, size, resolution, frame-slot) — the P8C cache-key trap. Normalisation stays
   the free DC-term divide. `centerUV` default 0.5 (the EXR is centred; keep the control for a
   photographed kernel later).
3b. **The anamorphic streak is COMPOSITED INTO the kernel at build time** — the stock EXR is a
   spherical-lens kernel and carries none, so without this step "anamorphic" simply ceases to
   exist. During the resample stage, ADD a procedural streak lobe to the loaded image: a thin
   screen-horizontal line through the kernel centre, exponential falloff along x
   (`anamorphicLength` = its 1/e extent as a fraction of screen width), a couple of texels of
   Gaussian thickness in y, tinted with a gradient from near-white at the core to BLUE at the tips
   (the cyan-blue of real cylindrical-element coatings — and a coloured gradient is exactly what
   step 2's Hermitian multiply makes representable). `anamorphicIntensity` = the fraction of total
   kernel energy the streak carries, 0 = exact stock kernel. Three consequences that come free and
   should be asserted rather than assumed: the DC-divide keeps total bloom energy UNCHANGED as the
   streak is dialled (it redistributes, never brightens); the streak is screen-horizontal by
   construction (physically right — the streak is fixed to the LENS, not the world); and all three
   params join the kernel cache key, so they cost a kernel rebuild, not a per-frame term.
4. **Resolution.** Grid 1024x512 -> 2048x1024 (image ~1280x720). Decide the memory/precision pair
   ON MEASUREMENT: R32G32B32A32 grids go 25 -> 100 MB; half-float grids halve that but the FFT's
   dynamic range in half needs a verify pass (numpy: same transform in float16 vs float64 on a real
   HDR frame, look at the error). Expect `Pass_BloomConv` 0.12 -> ~0.5 ms; a `bloom.convPercent`
   control mirroring UE's ScreenPercentage keeps the old cost reachable.
5. **Ghosts, UE's way.** (a) The scatter: instanced quads over the thresholded half-res source
   (`GhostSource` exists), additive blend, bokeh sprite — the sprite is BAKED AT LOAD from the
   existing `ApertureMask` code into a small texture (a soft iris polygon is procedural-friendly;
   this is where P8D's blade controls keep living). Watch the cost: 1px tiles at half-res is ~1M
   quads with overdraw — UE gate this behind quality; start at QUARTER res and measure up. (b) The
   composite: ONE compute pass, 8 taps of the blurred texture at `centre + (uv-centre)/scale_i`
   (the transform is a fixed affine map, so a gather is legal here — it was the PER-SOURCE gather
   that could never work), UE's tint table (already transcribed), double DiscMask, into the bloom
   chain. (c) DELETE, not disable: the ghost atlas binding, `sunUV`/`sunOnScreen` plumbing, the
   per-ghost jitter hashes, and the atlas file itself from the level/settings — controls must not
   lie, and P8D's four-control removal is the precedent and the checklist for where they hide
   (settings struct, JSON, inspector, dev window, `--set`, CB, cache keys).
6. **Control cleanup after the kernel switch.** `blades`/`bladeRotation` move to the GHOST sprite
   only; `kernelRadius` is superseded by `convolutionSize`; `anamorphic` + `anamorphicLength` are
   RE-HOMED, not removed — they become step 3b's kernel-build parameters (`anamorphicIntensity` /
   `anamorphicLength`), applied on kernel rebuild like `blades` was, never per frame. What dies is
   the aperture-squeeze MECHANISM (it stretched the whole PSF, halo included, which is not what an
   anamorphic lens does to bloom) and `chroma` (the EXR carries real dispersion; the streak's blue
   gradient is authored in 3b). Same removal checklist as above for everything that goes. The
   aperture-PSF path itself is retired WITH its stages — git holds P8D if a content-free fallback
   is ever wanted again.

**Interface contract:** `bloom.method` semantics unchanged (pyramid default, convolution opt-in);
with the EXR kernel absent the convolution method refuses to enable (UE's own gate:
`IsFFTBloomEnabled` returns false without a kernel texture) rather than falling back to a
procedural kernel that no longer exists. Ghosts keep their own enable, default OFF until the visual
sign-off flips it.

**Done when:** `sun_glint` shows the EXR kernel's smooth rainbow starburst with no dashing at
native AND under DLSS; a two-source frame (sun + one bright specular) shows TWO ghost chains
through the screen centre; the wrap test still holds; the old "ragged crown" camera reads clean
at `bloom.convPercent` >= 50; and the anamorphic contract holds: `anamorphicIntensity 0` is
byte-identical to the stock kernel, raising it draws a screen-horizontal streak through every
glint with blue tips whose extent follows `anamorphicLength`, and the bloom's TOTAL brightness
stays within the noise floor as the streak is dialled (the DC-divide energy invariant, measured,
not assumed).

**Verify:** P8C's full gate list (wrap test, rotation series, exposure pinned, GPU timings split
transform/kernel/ghosts) plus: the numpy Hermitian check of step 2; a kernel-swap test (feed a
deliberately asymmetric test kernel and confirm the on-screen star rotates with it — proves the
image path, not the old procedural one, is live); and the P8C noise-floor discipline for every A/B
(`--wind-freeze`, same-crop floors).

**Traps already paid for, restated because every one of them will be met again:** the
`RecordComputeDispatch` /8 convention (P8C bug 1); per-frame-slot cache keys (P8C bug 2); UAV-chain
reads via `RWTexture2D` or GBV id=1358 (P8C bug 3); Karis-vs-box at setup (P8C bug 4); and
`textures/` is not under git — the EXR and anything derived from it must be backed up before import
experiments.
| particle plume crop, EV -2 | 5.76 | 4.63 | **4.97** |
| particle plume crop, EV 3 | 0.18 | 2.77 | **3.40** |
| full frame, SSR + UE march, EV -2 | 5.76 | 3.146 | **3.156** |

Both directions on purpose: a missing multiply shows up as `1/factor` too dark, a missing divide as
`factor` too bright, so a factor above AND below 1 catches either sign.

**The instrument lied by omission and was fixed.** `[p16] scene peak luminance` is built from the
metering histogram -- which now divides the pre-exposure back out, because it has to -- so it reports
the scene's RAW peak and cannot see the gate at all. It now prints the factor and the stored peak
next to the raw one, which is what makes a log line evidence that the gate was live. It caught a
second one immediately: the SSR A/B ran on the DEFAULT `LogMarch` technique, which never reads the
history, so the first "clean" SSR result was measured on a path where the changed line is not
executed. `ssr.technique:1` picks the UE march, and a second log line now states outright whether
the history is being read.

**P16.1 SUPERSEDED NOTE (kept for the record):** Wired and gated OFF; `--set=render.preExposure:1` turns it on.
Gate-off is verified INERT: 0.356 meanabs against a 0.337 run-to-run noise floor, i.e. unchanged.

Done: the instrument (scene peak from the metering histogram, **baseline 2^10 = 1024** on
`wind_test`); the factor, from the PREVIOUS frame's adapted EV, published as `render::g_preExposure`
because the writers are independent systems with no path to the scene renderer; and the wiring for
the skybox (folded into the multiplier it already had -- no shader change), compose, the tonemap
(divides it back out), and BOTH metering readers.

**The metering was a real trap and the instrument caught it.** With only compose pre-exposing, the
scene peak went UP (2^10 -> 2^12) and the frame shifted 28/255. The histogram is built FROM scene
colour, so it measured the scene times its own exposure, solved a shifted EV, and that EV became the
next frame's pre-exposure -- a loop. Taking the previous frame's value avoids a same-frame circular
dependency but not the drift; both readers now divide the factor back out.

Not done: the forward writers -- the OCEAN (both variants), glass, particles -- and the bloom
threshold's `ExposureMultiplier()`, which would double-apply once its source is pre-exposed. And the
multiply/divide pair does not yet cancel exactly for the paths that ARE wired (the sky moved by 50
of 255 with the gate on, where it should not move at all), so the CPU factor and the EV the tonemap
reads from the GPU buffer are not the same number yet. That is the next thing to chase, before
anything else is wired.

#### P16.2 — The sun in lux — DONE (2026-08-20)

`sunIlluminanceLux`, default 100,000 (UE's own figure for a sunny midday; 125,000 full bright sun,
20,000 heavy overcast, 400 sunrise, 0.25 full moon).

**THERE IS NO CONVERSION CONSTANT, AND THAT IS THE WHOLE POINT.** The field multiplies straight into
the light colour. Two things already pinned the unit down and neither is new:

* the shading has the right SHAPE -- `diffBRDF = kd * albedo * kInvPi` and the directional term is
  `diffBRDF * NdotL * lightRgb`, so `lightRgb` sits exactly where an illuminance goes and the
  product comes out as a luminance;
* the metering solves an **EV100**, and EV100 is defined against cd/m2 (`L = 0.125 * 2^EV` for the
  K=12.5 saturation convention this code uses -- read straight off `ExposureMultiplierFromEv100`).

So "one unit of light colour" has meant one lux since the exposure code landed. P16.2 writes that
down and puts it in the field's name. **The migration is therefore an assignment**: `sunIntensity`
and `sunIlluminanceLux` hold the same number, and a level keeps its pixels while learning that it
had been authoring lux all along. wind_test's sun reads **2 lux** — deep twilight, against a sky
delivering thirteen. That is not a mis-migration to be papered over with a fudge factor; it is the
defect P16 exists to fix, finally written in a unit that can be argued with.

**THE SKY'S IRRADIANCE, MEASURED FROM THE ASSET RATHER THAN THE SCREEN.** The `_diffuse.dds` beside
each imported sky is the cosine-convolved irradiance cube, `R16G16B16A16_FLOAT`, 32x32x6, and it is
plain FP16 — twenty lines of numpy read it with no engine involved. For `rustig_koppie_puresky_4k`
the +Y face centre is `(4.242, 4.203, 4.055)`, so the horizontal irradiance is `pi *` that =
**13.20 units**, luminance-weighted. wind_test scales it by `skyFillIntensity 0.7` → **9.24**.

That number also settles what the sky IS: the face maxima run +Z 6.29, +X 4.23, +Y 4.20 against
0.16–0.24 on the three opposite faces, a lobe pointing exactly where this level's sun points. **The
sun is baked into the irradiance cube.** The scene is lit by a sky that already contains a sun, and
then a second, separate sun is added on top at 2 lux — which is why the shadows do not read: the
shadowed sand still receives the sun, through the fill. P16.3/P16.4 inherit that.

**THE UNIT CLAIM IS FALSIFIABLE, AND IT WAS TESTED.** Raising the sun changes only the sun's share
of the illuminance, so the exposure that reproduces a given look must move by exactly the ratio of
the totals:

```
E(S lux) = S * 0.7957 (colour luminance) * 0.4741 (NdotL on flat sand) + 13.197 * 0.7 (sky)
E(2)      =      0.755 + 9.238 =      9.99
E(100000) = 37 730     + 9.238 = 37 739
ratio 3777  ->  11.883 stops  ->  baseline EV 2.0 must be matched at EV 13.883
```

Measured, on the open-sand crop of wind_test, sun pinned at 100,000 lx, `manualEv100` swept:

| manual EV100 | sand mean | vs baseline |
|---|---|---|
| 13.0 | 209.29 | +29.12 |
| 13.5 | 194.81 | +14.64 |
| **13.9** | **181.08** | **+0.91** |
| 14.3 | 165.64 | −14.53 |

Baseline (sun 2 lx, the level's own `manualEv100 2.0`) is 180.17. The crossing is at **EV ≈ 13.93
against a prediction of 13.88 — 0.05 stops**, with the neighbours ±14 grey levels away, so the match
is sharp rather than a flat region. One test, three claims: lux enters the light linearly, the 13.20
read out of the DDS is right (it is what makes the ratio 3,777 instead of 50,000), and the colour
luminance and cosine are applied as expected. As a sanity check on the ABSOLUTE, sunlit sand
correctly exposed at EV ~13.9 sits within a stop of sunny-16 (EV 14.6), which is where a real
photograph of that scene would be.

**PRE-EXPOSURE IS NOW ON BY DEFAULT**, which is what P16.1 was built for: with the sun at 100,000 lx
the instrument reads `scene peak ~2^14 = 16384 raw ... preExposure ~2^-11 ... stored ~2^3`. The raw
peak is a quarter of the FP16 ceiling before a single specular highlight is considered; stored, it
is 8. Turned back off with `--set=render.preExposure:0`.

**Also in this step, because the unit forced it:**

* **A new level is born with the photographic camera ON.** It has to be — a 100,000 lx sun with the
  camera off multiplies by exactly 1.0, and the first thing anyone would see is a white screen.
  Physical light units presuppose a metering camera; that is why Unreal and Godot both ship one.
  Existing levels without the section still default to `enabled = false` and do not move.
* **`light.legacySplit` was removed** — see the note under P4. It asserted an equivalence that F8
  and F9 had quietly invalidated, and re-running it measured 27.6 meanabs against a 0.81 floor.
* **A duplicated block in `ApplySweepValue`** — five light settings appearing twice, the second copy
  unreachable behind the first one's `return true` — was deleted.

**What P16.2 does NOT fix, by construction.** Typing the physical 100,000 lx does not yet give a
physically correct scene, because the sky it is being compared against is still display-referred
(and still contains a sun). That is P16.3. Point and spot lights keep their own arbitrary scale and
will need re-tuning once the sky moves — which is the "другие источники потом надо будет тюнить" this
step was asked for with.

**Verification recipe**, for anything that touches a light consumer later: run the level as
authored, then with `--set=light.sunIlluminanceLux:<the level's own value>`; the two must be
identical. Measured on wind_test at **1.00 meanabs against a 0.81 two-run noise floor**. The
pre-exposure default flip was checked the same way, `--set=render.preExposure:0` against the
default: **1.09**.

#### P16.3 — The sky: sun out of the fill (DONE 2026-08-20), then cd/m2 (next)

**P16.3a — THE SUN WAS INSIDE THE SKY'S IRRADIANCE CUBE, and that, not the units, is why the
shadows would not read.**

Measured by integrating the upper hemisphere of the source `.hdr` with real solid angles — a
Radiance decoder in numpy, no engine involved, because `imageio` and `cv2` are both absent here:

| | rustig_koppie_puresky_4k |
|---|---|
| peak luminance | 3.68e5 against a median of 0.09 |
| sun direction from the HDRI | `[0.5055, 0.4734, 0.7213]`, elevation 28.26° |
| **the level's own directional light** | **`[0.5047, 0.4741, 0.7215]`** — they agree to three decimals |
| horizontal illuminance, sun disc | 8.01 |
| horizontal illuminance, everything else | 0.73 |
| **the disc's share** | **91.7%** |

So the cube the engine calls a "sky fill" was 92% sun: a second sun, spread evenly over the
hemisphere, casting no shadow and occluded by nothing. Raising the directional light could never
make a shadow read, because the shadowed sand was still receiving the sun — through the fill. The
measured shadow/lit ratio on open sand was **1.007: the shadow was BRIGHTER than the sand beside
it.**

The disc's exact radius barely matters, which is worth knowing before arguing about it: 0.5° already
captures 91.0% and 10° only reaches 93.2%, so everything past the disc is genuine sky.

**The fix is the split every engine with a sky light already makes.** UE captures the SkyLight
without the sun and runs the sun as its own directional; Godot's `ambient_light_energy` is the sky's
contribution alone. Here there is a second reason on top: `lighting_cs` already adds an ANALYTIC sun
specular from the directional light, so a prefiltered radiance cube that also contains the disc
double-counts it on every glossy surface.

`ImportOptions::skyRemoveSunFromIbl` (default on, `--sky-keep-sun` to disable, checkbox in the
import dialog) builds the two LIGHTING derivatives from a copy of the calibrated cube with the disc
replaced by the **per-channel median of the annulus [R, 2R]** around it — local, colour-preserving,
and impossible to drag with one bright texel just outside the cut. The DISPLAY cube keeps its sun:
the sky still looks the same when you look up.

**Verified on the artefact, not the log.** Reading the shipped `_diffuse.dds` back (plain FP16 at
byte offset 148, so numpy reads it directly):

| face | before | after | ratio |
|---|---|---|---|
| +X | 4.232 | 0.614 | 6.9 |
| +Y (up) | 4.201 | 0.348 | 12.1 |
| +Z (toward the sun) | 6.245 | 0.810 | 7.7 |
| −X | 0.241 | 0.241 | **1.00** |
| −Y | 0.164 | 0.164 | **1.00** |
| −Z (away from the sun) | 0.220 | 0.220 | **1.00** |

**The three faces pointing away from the sun are unchanged to three decimals.** That is the evidence
that the strip took the sun and only the sun. Horizontal irradiance fell 13.20 → 1.09, and the up
face went from near-neutral `(4.24, 4.20, 4.06)` — sunlight — to `(0.23, 0.35, 0.65)`, which is
blue. The fill is finally skylight.

On screen, wind_test with the sun re-balanced to the new fill: shadow/lit on open sand
**1.007 → 0.750**, and by construction the linear ratio is 0.20, i.e. a 2.3-stop shadow, which is
what a clear day looks like.

**NOT EVERY SKY HAS A SUN, and one without must not be touched.** `citrus_orchard_puresky_4k`'s
brightest texel is 8.7 against rustig_koppie's 520,000 and its disc carries 0.02% of the hemisphere.
A disc under **1%** is logged and left alone; re-importing that sky produces a `_diffuse.dds` that is
**byte-identical** to its pre-P16.3 file, so the two levels using it did not move.

**`--sky-out=<textures root>`** was added at the same time: the headless import used to leave its
output beside the source and the four files had to be moved by hand, with the shared `brdf_lut.dds`
going somewhere different from the cube. It now applies the GUI panel's own rule —
`<root>/<name>/<name>.dds` plus the two siblings, `<root>/brdf_lut.dds` — so a CLI import and a
dialog import leave the tree in the same state and nothing is left in `import_staging`.

**P16.3b — THE SKY IN LUX. DONE (2026-08-20).**

The authored quantity is `skybox.illuminanceLux`: **how much light this sky puts on a horizontal
surface**. 12,000 for a clear sky with the sun about 30° up, 20,000 with it overhead, 10,000–20,000
for a heavy overcast where the sky IS the light. **0 = not authored**, which reproduces the previous
behaviour exactly — that is what keeps every unconverted level rendering what it renders today.

**The scale that realises it is DERIVED, never authored.** The engine reads this sky's own
irradiance cube at load — the `_diffuse.dds` +Y centre texel is E(up)/π by construction, so the
answer is π times its luminance — and divides. That is what makes the number portable: the same
12,000 lx means the same thing on a cube that came out of the importer bright and one that came out
dim. The engine's own measurement of rustig_koppie is **1.0924 units**, against **1.092** read
independently in numpy straight off the file.

**Runtime, not baked.** An FP16 cube cannot hold physical radiance anyway — a sunlit aureole is six
figures of cd/m2 and BC6H stops at 65,504 — while the runtime is protected by P16.1's pre-exposure.
It also means the calibration is a tuning decision rather than a bake, and that the ASSET is
separate from its CALIBRATION, which is what lets one sky serve two levels at two times of day.

**One scale for both roles**, folded into `Skybox::GetExposure()` so compose, the lighting pass, the
ocean and glass all pick it up with no new constant-buffer field. Lighting and appearance are the
same sky seen two ways; giving them independent scales is how you get a background that disagrees
with the fill it is supposed to be casting. (Separating them *deliberately*, for artistic reasons,
is what is left of P16.4.)

**wind_test is converted, and its exposure stays MANUAL.** The numbers are daylight-table values for
the sun this sky actually has — the HDRI's disc sits at 28.3° elevation, and the level's directional
light already pointed there to three decimals:

| | |
|---|---|
| `directionalLight.sunIlluminanceLux` | **85,000** (direct normal, clear day, sun ~30° up) |
| `skybox.illuminanceLux` | **12,000** (diffuse horizontal) |
| `skyFillIntensity` | 0.7 → **1.0** (it is a trim on top of the authored lux; 0.7 would mean the sky delivers 8,400 while the field says 12,000) |
| `cameraExposure.manualEv100` | 2.0 → **14.15**, `autoExposure` stays **false** |

The EV is derived, not guessed. P16.2's falsification test measured EV 13.93 as the exposure that
reproduces this level's own authored sand brightness at a total horizontal illuminance of 37,739.
Illuminance and exposure scale together, so `13.93 + log2(44,065 / 37,739) = 14.15`.

**Checked from five camera angles, one fixed exposure**, because a look change judged from one view
is not judged:

| view | mean | p99 | clipped | crushed |
|---|---|---|---|---|
| grove | 114.0 | 206 | **0.000%** | 0.009% |
| into_sun | 189.6 | 250 | **0.000%** | 0.002% |
| down_sun | 109.3 | 205 | **0.000%** | 0.011% |
| cross_light | 112.8 | 214 | **0.000%** | 0.017% |
| overhead_sand | 112.0 | 190 | **0.000%** | 0.000% |

Not one clipped pixel in any of them, including looking straight into the sun, on a fixed manual
exposure.

**THE OLD SKIES STILL WORK, verified rather than assumed.** `wind_test` before the field existed vs
after, unconverted: **0.53 meanabs, frame mean 48.13 → 48.13**. `roughness_sweep` and
`ssr_bronze_palms` (the overcast citrus_orchard sky) log `authored=0 lx -> physical scale x1.0` and
render clean; `demo` and `e3_particle_test` have no F7 derivatives at all and take the legacy sky
mip chain exactly as before.

**What is left of P16.4.** The sky's lighting and its appearance still share one number by design.
Splitting them is now an artistic choice rather than a units bug — and the remaining physical gap is
that **this HDRI's own sun:sky is about 11:1 where reality is nearer 3.5:1** (calibrating on its disc
instead of its sky would put the sky at ~4,300 lx). With the disc removed from the lighting and the
sky authored in lux, that discrepancy no longer reaches the render; it would only matter again if
the background were put on the same physical scale as the fill. Medium-scale occlusion of the fill —
the other half of P16.4 — is DONE and is written up in its own section below: GTAO now walks two
radii out of one dispatch, contact and sky-scale, and the sky fill takes the min of the two.

#### P16.12 — Ground bounce: the other half of the fill — DONE

**The complaint.** On a wide sunlit beach shot the shaded side of every palm read far too dark for
the day. GTAO was the obvious suspect and was wrong: measured at that camera on the darkest quartile
of a crop over the foreground palms, the ENTIRE GTAO pass was worth 0.174 stops and P16.4's half of
it 0.085. Pushing the distance fade from 80/120 m out to 300/600 m changed the frame by 0.005 stops
— the pass was not being faded out, it was looking at OPEN BEACH, where there is correctly nothing
to occlude. Two things that were worth a stop each: auto exposure metering a frame half full of
bright water (0.939 stops) and the subject of this section.

**The gap, in numbers.** Read out of the level's own irradiance cube
(`rustig_koppie_puresky_4k_diffuse.dds`, physical scale x10985 from `logs/ibl.log`):

| normal | E(N) |
|---|---|
| up | 14,073 lx |
| down | 9,643 lx |
| toward the sun (+X/+Z) | 19,700 – 23,600 lx |
| away from it (-X/-Z) | 7,700 – 9,000 lx |

With the sun at 85,000 lx and 28.3 degrees of elevation, level ground receives
85000·sin(28.3) + 14073 = **54,400 lx**, while a shaded vertical frond receives **~8,000 lx** — sky
only. Real sand at that illuminance and albedo 0.4 sends ~21,800 lx back up, of which a vertical
surface catches about half: **+10,900 lx, i.e. 1.2 stops**, on exactly the surfaces that looked
dead. The engine had no bounce term of any kind. The only fill was the sky cube, and the cube's
lower hemisphere is the HDRI's OWN ground, not the sunlit sand this scene is standing on.

**What shipped.** `GroundBounceOverPi` in `lighting_cs.hlsl`, added to the sky irradiance inside the
IBL branch before the albedo multiply:

    E_bounce(N) = groundAlbedo * E_ground * F(N),    F(N) = (1 - N.y) / 2

`F` is the exact view factor of an infinite flat plane of uniform radiance: an up-facing surface
sees NONE of the ground, a vertical one half, a down-facing one all of it. That direction dependence
is the whole reason to prefer it to a flat ambient add — it is what makes the result read as light
off the floor instead of a grey wash. `E_ground` is the sun's cosine on a level surface plus the
sky's own up-facing irradiance; it is deliberately a SCENE average and NOT shadowed, because a point
standing in a palm's shadow is still surrounded by lit sand. What localises it is the AO the caller
already applies to the whole indirect-diffuse term, which is what stops it lighting the inside of a
closed room.

The control is `directionalLight.groundAlbedo`, an RGB REFLECTANCE and not a light: it only ever
scales illuminance the scene already has, so it follows the sun automatically, cannot brighten a
night, and needs no second strength knob — black is off. Default 0.25 neutral (dry sand 0.4, dead
grass 0.3, concrete 0.25, green grass 0.2, asphalt 0.1, water 0.06); a tint belongs to the level
that has sand in it. **The same term is duplicated in `rt_reflections_cs.hlsl`'s off-screen
re-shade** — leaving it out there would have made every reflection of a shaded surface about a stop
darker than the surface seen directly, which is the identical half-a-pair defect P16.9 fixed for the
sky fill itself. DIFFUSE ONLY: a down-facing glossy surface really does reflect bright sand, but the
prefiltered sky cube has no such term either and adding one on this side only would break the
cancellation compose depends on.

**Verified against a known answer, not against taste.** The view factor makes a falsifiable
prediction — flat ground must not move at all. Sweeping the albedo with the exposure pinned
(stops vs albedo 0, default `wind_test` camera):

| region | 0.25 | 0.4 | 1.0 |
|---|---|---|---|
| flat sand, sunlit | +0.031 | +0.050 | +0.120 |
| flat sand, in palm shadow | +0.012 | +0.020 | +0.053 |
| palm crowns and trunks | **+0.164** | **+0.261** | **+0.609** |

The flat ground does move slightly, and correctly so: the sand carries a detail normal map and the
beach is duned, so its shading normals are not exactly vertical. The DIFFERENCE IMAGE settles it
outright — trunks and fronds light up, the sand, the ocean and the sky stay black, and on `demo` it
is the UNDERSIDES of the sphere grids that brighten. (A first attempt scored two rectangles labelled
"ocean" and "sky" that were actually sitting on foliage and reported both moving; the picture is
what caught it. Regions read off remembered coordinates are worth nothing.)

**Regression** on the darkest quartile, albedo 0 -> 0.25: `demo` +0.028 stops, `roughness_sweep`
+0.017, `d_emissive_test` +0.012, `ssr_bronze_palms` +0.238 — the last being the only one of the
four that is foliage over ground, which is the point.

**Cost**: `Pass_Lighting` 0.0350 -> 0.0360 ms, at the timer's resolution and inside the spread of
the off runs themselves (which spanned 0.0340-0.0360). One extra cube fetch at a constant direction.

**Gate**: 33/33 shaders, both configs (Debug caught a broken string literal Release never compiles),
`--gbv` on `wind_test` clean, `--scene-stress-gbv=8` CLEAN.

**Correction to an earlier claim in this session.** `ambient` / `ambientColor` were called dead on
any level with an IBL sky. They are dead only in the OPAQUE DEFERRED DIFFUSE path: `glass.hlsl` and
the RT fallback shading still read `ambientIntensity`. The keys must not be deleted from levels.

**Left open.** The bounce is a single scene-wide average, so it cannot know that the ground under
one tree is shadowed and under another is not, and it assumes a flat world. Real per-pixel bounce is
screen-space or ray-traced GI. It also does not reach the ocean or glass, which carry their own
ambient handling.

#### P16.13 — Exposure compensation is two knobs, not one — DONE

`compensationEv` was applied in BOTH branches of `exposure_solve_cs`, so a trim dialled while
looking at a metered shot silently re-trimmed every fixed-exposure shot in the same level, and back
again. They are different jobs: in auto the number offsets what the METER decided, in manual it
offsets an EV the author already solved by hand. `manualCompensationEv` is now its own field,
defaulting to 0 rather than -0.15 — a manual EV is an absolute the author dialled in, and a hidden
trim on top of it is a trap. Zero migration: both levels carrying a `cameraExposure` block author
`compensationEv: 0.0` explicitly.

Both UIs show only the field the current mode uses (showing both would move the confusion from the
value to the label), and the inspector's EV readout in manual now subtracts the trim, so the row
cannot disagree with the image.

**Verified with a known answer** — each key must move its own mode and be an exact no-op in the
other: manual +1 on `manualCompensationEv` = +0.836 stops and on `compensationEv` = +0.0002; auto +1
on `compensationEv` = +0.688 stops and on `manualCompensationEv` = +0.0002. (Less than a full stop
because the tone curve compresses; 0.0002 is the temporal noise floor.)

#### P16.16 — Clipmap shadow bias, done UE's way — and the streak was never a bias problem

Two earlier attempts (a slope-scaled depth push; sizing the normal offset by `T*sin/cos^2`, then
taking its direction from a depth-reconstructed geometric normal) were reverted at the user's call.
He then asked for the three points below, checked against the UE drop at every step.

**1. Per-tap receiver-plane depth bias** (`VsmSampleNDC`), from
`ComputeVirtualShadowMapOptimalSlopeBias`:

    depthSlopeUV = -planeUV.xy / planeUV.z          // plane transformed to shadow UV space
    bias = 2 * max(0, dot(depthSlopeUV, offsetUV))

`offsetUV` is the sub-texel offset to the texel centre PLUS the tap's own offset — the second term
is ours, because UE's SMRT walks single texels while this is a 3x3 PCF, so a corner tap now biases
about 1.4x the centre one where one value used to serve all nine. It is not an estimate of the
snapping error, it IS the error: zero on a texel centre, maximal at a corner.

`clipmapUvNormal` is built CPU-side exactly as UE build
`CalcTranslatedWorldToShadowUVNormalMatrix` — the transpose-inverse of world -> shadow UVZ. ONE
matrix serves all levels: the gradient is a ratio in which the level extent cancels.

**2. The per-level scaling** UE do as `bias *= 1 << (sampledLevel - requestedLevel)` falls out for
free: the offset is converted to UV with the dimensions of the level the fallback loop ACTUALLY
landed on, so a coarser level's larger texel widens it by the same factor.

**3. The normal offset is now UE's**, `max(0.02cm, NormalBias * distanceToCamera / cot(hFov/2))`
with `NormalBias` divided by 1000 on the CPU — so `g_clipmapNormalBias` is directly comparable to
`r.Shadow.Virtual.NormalBias` and defaults to their 0.5. It is referred to the world size of a
SCREEN PIXEL, not the shadow texel; the old form ignored the field of view and ran 3.9x larger.

**TWO TRANSCRIPTION BUGS, both caught by rendering.** The `/1000` was written into a comment and
never implemented, making the offset half the distance to the camera — the frame was solid acne.
And UE's `DepthSlopeUV` clamp of 0.05 DOES NOT TRANSFER: their clipmap level spans `ZRangeScale`
(1000) times its radius, ours spans 6x its extent (12x radius), so our gradients are ~83x theirs
and their clamp was crushing the real gradient (tan(theta)/6 = 0.31 for flat sand here) SIXFOLD —
producing less bias than the constant it replaced. The bound is now derived, not copied.

**AND THE STREAK WAS NEVER A BIAS PROBLEM.** With the receiver-plane bias in and verified, the
streak stayed. Amplifying it 10x only softened it while over-biasing the crown. The matrix was
cleared by rendering the same gradient derived analytically from the light basis: it differed by
0.654/255 against a same-shader-twice control of 0.499 — inside the noise floor, so the two agree.

The cause is `--vsm-lodbias`: **the shadow map renders the terrain at a coarser LOD than the
G-buffer shades**, so the receiver plane and the caster surface are genuinely different geometry.
No depth bias can fix that, which is exactly why every depth push failed and only a large LATERAL
normal offset ever "worked" — it walks past the mismatch. At `--vsm-lodbias=-2` the streak is GONE
with `normalBias 0`, on the receiver-plane bias alone.

**The cost of that**, three interleaved runs a side: `Pass_VsmPageRender` 1.333 -> 3.154 ms,
**+1.82 ms (+137%)**. So turning the LOD bias off globally is not the answer. The targeted fix is to
exclude the TERRAIN from the shadow caster LOD — it is the one mesh that is both the caster and the
receiver directly under the camera, while the palms the bias exists to speed up can keep it. Not
implemented; it is a separate change and a design call.

**Gate**: 33/33 shaders, both configs.

#### P16.17 — Terrain out of the shadow-LOD offset: the fix, and the measurement bug that hid it

The diagnosis was the user's and it is correct: **the terrain rasterized into the shadow map at a
coarser LOD than the G-buffer shades it, so the ground self-shadows.** Excluding
`RenderLayer::Terrain` meshes from the shadow LOD offset removes the streak completely — it matches
a reference render where the terrain casts nothing at all.

**IT TOOK TWO WRONG MEASUREMENTS TO GET THERE, AND BOTH WERE MINE.**

*The first* patched the per-(view, group) CPU table. That table feeds only the cull-clear/Legacy
path: the GPU picks the caster LOD PER VIEW — `vsm_page_setup_cs.hlsl:254`, `gViewLod[rung0View]`
indexes `gGroupLodMega[group * numLods + lod]`. The exemption never reached the drawn geometry and
measured as an exact no-op, inside the 0.5/255 run-to-run noise floor.

*The second* moved it into `gGroupLodMega` — but **that entry is filled in TWO separate loops**: the
lod-relative start and count in one, the mega-absolute start and base vertex in another. The
exemption went into the first only. The terrain therefore got **LOD 0's index count (248,832)
starting at LOD 1's mega offset**, reading 124,419 indices off the end of a 124,413-index slice into
whatever followed. It cost 4 ms, drew garbage, and left the streak in place — and I reported that as
"excluding the terrain does not fix it", and then built a whole wrong conclusion about foliage on
top of it. Both halves now go through one `effectiveLod(group, L)` helper so they cannot disagree
again.

**Corrected measurements** (`Pass_VsmPageRender`, three interleaved runs a side, `Pass_Compose`
steady at 0.030–0.033 as the control):

| variant | cost | streak |
|---|---|---|
| today | 1.325 ms | present |
| **terrain out of the LOD offset, every level** | **4.568 ms (+3.24, +245%)** | **GONE** |
| terrain out for the NEAR levels only (viewLod ≤ 1) | 3.518 ms (+2.18, +164%) | GONE, same image (1.0/255 from the above) |
| terrain casts nothing (reference) | — | GONE |

**The terrain is essentially the entire shadow-render cost.** `logs/shadow_lod_groups.log` (written
per caster rebuild) shows why: group 0 is the island, 199.6 m radius, **248,832 indices at LOD 1**,
rasterized into every page it touches; the palms are 336–7,776. So the LOD offset earns nearly all
of its keep on the one mesh that must not have it, which is the whole tension here.

Restricting the exemption to the near levels recovers a third of the cost for an identical picture,
and is the obvious next place to push: the far clipmap levels shade the ground coarsely too, so the
mismatch there is invisible while those levels are where a 248k-index mesh hits the most pages.
Beyond that the real answer is an error-bounded caster LOD keyed on the shadow texel — what Nanite
does for UE — rather than a per-mesh exemption.

**NOTHING FROM THIS SECTION IS IN THE TREE.** The mode switch, the terrain group flag, the
`effectiveLod` helper and the diagnostic log were measurement scaffolding and have been reverted;
`LodSelect.h`, `ShadowGpuData.h/.cpp` and `Scene.cpp` are back at the commit. What survives is the
knowledge: the diagnosis is confirmed, the fix is a per-mesh exemption from the shadow LOD offset,
it costs +3.24 ms (or +2.18 restricted to the near levels), and `gGroupLodMega` is assembled in two
loops that must be changed together.

#### P16.5 — Local lights in lumens, and every level converted — DONE (2026-08-20)

**THE FALLOFF HAD TO CHANGE WITH THE UNIT.** Point and spot lights were
`colour * intensity * (1 - d/r)^2`, which is not a falloff, it is a WINDOW: halving the distance
does not quadruple the illuminance, the number has no unit, and a light's brightness therefore moved
whenever someone dragged its radius. Relabelling that as lumens would have been a lie, so:

```
attenuation = saturate(1 - (d/r)^4)^2 / (d^2 + 1)      (Unreal's)
```

inverse-square where it matters, a window that reaches exactly zero at the range instead of clipping
a still-bright light, and a `+1` that keeps `d -> 0` finite — a real lamp has a size, and without it
a texel landing on the light's own position renders as an infinity.

**It lived in SEVEN places across six files** — the two compute passes, the two glass loops, the two
RT reflection evaluators and the legacy stencil pixel shader — which is exactly how a lighting model
drifts apart one pass at a time. It is now one function in `utils.hlsli`, which all six already
include. `pointlight_cs`, `spotlight_cs` and `rt_reflections_cs` were added to `check_shaders.py`
(the last needs SM 6.6 for `ResourceDescriptorHeap`, so the tool grew a second compute pass); 33
entries now, from 24 before this session.

**The unit is LUMENS**, `I [cd] = flux / (4*pi)`, and deliberately the same conversion for spot
lights: a spot with 1,000 lm has the same peak candela as a point with 1,000 lm, so narrowing the
cone does not silently brighten it. Candela times the falloff is **lux at the surface** — the same
unit the sun is in, which is the point of the exercise.

**The migration is NOT lossless and says so.** The falloff shape changed, so no single number
reproduces the old curve everywhere; it preserves the illuminance at **half the range**, where a
light does its work. Nearer than that a light is now brighter and further it falls off faster, which
is the correction.

#### Every level converted (2026-08-20)

Sun and sky come from a clear-day model evaluated at **each level's own sun elevation**, not one
number pasted everywhere:

```
sun_perp = 128000 * exp(-0.21 / sin(elev))       direct normal illuminance, lux
f        = 0.32 - 0.18 * sin(elev)               clear skies get proportionally less diffuse
sky_horiz = f/(1-f) * sun_perp * sin(elev)       diffuse horizontal illuminance, lux
```

At wind_test's 28.3° it returns **82,000 / 12,000** against the **85,000 / 12,000** that level was
set to by hand from daylight tables — under 0.05 stop apart, which is the check that the model is
not inventing anything.

**The sky reaches the scene by one of two paths and the conversion differs.** A sky WITH F7
derivatives gets `skybox.illuminanceLux`. A sky WITHOUT them lights through the flat fallback
`ambient * sunColour`, which is a FRACTION OF THE SUN'S OWN ILLUMINANCE — so it already scales with
the sun and only needs re-solving: `ambient = sky_horiz / (pi * sun_perp * colourLuminance)`.

**Every level's exposure is FIXED — `autoExposure: false` with a derived `manualEv100`.** Illuminance
and exposure scale together, so `EV_new = EV_old_effective + log2(after / before)`, where
`EV_old_effective` is the level's own manual EV, the settled value its auto exposure was sitting at
(measured, not guessed), or — for a level whose camera was OFF — `log2(1.44)`, the EV at which the
tonemap multiplier is exactly the 1.0 those levels were rendering with. Compensation is correctly
ignored: `exposure_solve_cs` uses `targetEv = manualEv100` in manual mode and never adds it.

**TWO THINGS THE FIRST PASS GOT WRONG, both caught by rendering it rather than by reasoning:**

1. **The sky background went black** on every level whose sky has no derivatives. Those cubes have
   nothing to measure, so `illuminanceLux` cannot calibrate them, and the background is a
   display-referred image nobody scaled — raising the exposure fifteen stops turned it off.
   `skybox.intensity` is exactly the control for that (it is the sky's APPEARANCE), set to the same
   factor the scene's illuminance rose by.
2. **`maxEv100` defaults to 16 and was silently clamping** the derived exposure on five levels — a
   control overruling the authored one without saying so. Raised per level to clear it.

Result, whole-frame mean against the pre-conversion render, **zero clipped pixels everywhere**:

| level | stops | | level | stops |
|---|---|---|---|---|
| e3_particle_test | −0.02 | | atoll | −0.14 |
| wind_shadow_test | −0.02 | | demo | −0.20 |
| e1_particles_test | −0.05 | | roughness_sweep | **−0.82** |
| new1 | −0.07 | | ssr_bronze_palms | **−0.83** |
| demo1 | −0.08 | | | |
| d_emissive_test | −0.13 | | | |

The two outliers are the two levels on the citrus_orchard sky, and the change is a CORRECTION: their
background is now consistent with the illuminance that sky is authored to deliver, where before it
was arbitrary. `skybox.intensity` will restore the old look if wanted, at the cost of the number
meaning what it says.

**Left as they are:** `demo_bckp` and `demo_orig` are explicit snapshots, and leaving them
unconverted is what makes them snapshots — they still take the legacy path and render exactly what
they did. The demo levels' local lights are now a nine-digit lumen figure — the same diagnosis the
sun's "2 lux" was, since they were authored against a scene lit tens of thousands of times below
physical. Re-authoring them as real fixtures is a tuning pass, not a migration.

#### The last eight levels moved off `skybox.dds` (2026-08-20)

`skybox.dds` predates the staging setup and **its source HDRI no longer exists**, so it can never be
given F7 derivatives — which means it can never be measured, which means `illuminanceLux` cannot
calibrate it, which is why those levels were left carrying a five-figure `skybox.intensity` as a
stand-in. Pointing them at `citrus_orchard_puresky_4k` replaces the stand-in with the real thing:

| | before | after |
|---|---|---|
| `skybox.intensity` | 23,332 – 126,953 | gone (neutral) |
| `skybox.illuminanceLux` | — | 10,000 / 17,000 lx, from the same model |

citrus_orchard is the right sky to paste onto eight levels whose suns point in three directions,
because **its disc carries 0.02% of its illuminance** — a hazy, low-sun sky with no directional cue
to contradict whatever the level's own directional light is doing.

**The exposure had to be re-measured, not carried over.** The horizontal illuminance is unchanged by
construction (the flat fallback was solved to equal the same `sky_horiz` the cube now delivers), so
the arithmetic said the EV was still right — and rendering it said otherwise, by −0.42 to −1.83
stops. Two things the horizontal-illuminance arithmetic cannot see: the fill stopped being
DIRECTIONLESS (the flat term applies the sun's colour equally from every direction; a real sky gives
upward faces more than downward ones and turns the shade blue), and the background is a different
photograph. So each level's own metering was asked for a settled value and that was pinned as
`manualEv100`, with `autoExposure` still false.

Final, whole-frame mean against the pre-P16 render, **zero clipped pixels on all eight**:

| level | stops | | level | stops |
|---|---|---|---|---|
| atoll | +0.10 | | new1 | +0.91 |
| wind_shadow_test | −0.17 | | e1_particles_test | +1.03 |
| demo | −0.28 | | d_emissive_test | +1.42 |
| demo1 | +0.24 | | | |
| e3_particle_test | +0.34 | | | |

The three that gained a stop or more were UNDEREXPOSED before (frame means of 43–65); the metering
has now put them where they belong. That is a change of look, and it is the point: swapping the sky
is not a migration, and there is no old exposure worth preserving across it.

#### P16.6 — Two things physical units broke, and the magic number they left behind — DONE (2026-08-20)

**THE SEA WENT GREEN, and it was not the sky swap.** Measured on demo's open water, the blue-to-green
ratio of the far field: original **1.31**, converted but still on the OLD sky **1.16**, converted plus
citrus **1.09**. Most of it happened at the UNIT conversion, with the sky image unchanged — and the
sky's own ratio was preserved across it (1.13 → 1.14), so the water was losing something of its own.

`Refraction()` builds the water's colour from three terms and **two of them had no light in them at
all**:

```hlsl
float3 color = DeepScatterColor(depthScale);                              // absolute
color += sssColor * saturate(sss.x + sss.y);                              // absolute
color += (ndotl*0.8 + 0.2) * li.mainLight.color * DiffuseColor(depth);    // lit
```

Those two are SCATTERED LIGHT written as absolute radiance. It worked only while the scene's light
happened to sit near 1; against a 76,000 lx sun the lit term is tens of thousands of times bigger and
the water's own blue simply vanishes, leaving the green diffuse. Multiplying both by the light that
does the scattering is the fix, in **both** ocean variants — the runup surface carries its own copy.
At the pre-P16 scale the sun colour was ~0.92, so this is under 0.13 stops for anything unconverted.
Measured after: B/G back to **1.46**.

This is the same defect as the flat `ambient` fill and the local lights: authored radiance that used
to sit at the scene's arbitrary scale and no longer does. It is worth stating as a rule —
**an authored colour that is ADDED to a lit result is a bug waiting for the units to change; a colour
is a tint on a light, not a light.**

**THE CAMERA STOPPED BEING A MAGIC NUMBER.** With the lights in lux, `manualEv100: 14.51` was the last
thing in the level nobody could source — nothing said where it came from or what to change it to. But
EV100 is not a free parameter:

```
EV100 = log2(N^2 / t) - log2(ISO / 100)
```

So the **aperture, shutter and ISO** are the authored controls and EV100 is derived and shown
read-only. `f/16, 1/125, ISO 100` is sunny-16 at EV 14.97, which is why a scene lit in real lux is
now correctly exposed **by the defaults, with nobody typing anything** — which is what the units were
for.

ONE control, not two: a level carrying `manualEv100` is MIGRATED (hold shutter and ISO, solve the
aperture) rather than given a second way to say the same thing. Every level was rewritten holding the
canonical f/16 and ISO 100 and solving the SHUTTER, because that is how a photographer meters a scene
— wind_test is `f/16, 1/71 s, ISO 100`, demo `1/128`, wind_shadow_test `1/256`. Verified transparent:
demo **0.44 meanabs, frame mean 104.44 → 104.45**. Nothing changed on the GPU; the solve shader still
receives one EV, computed on the CPU from the three.

**A new level now works out of the box**: the template points at a sky that HAS derivatives, with its
lighting in lux, and a fixed sunny-16 camera. It was still pointing at `skybox.dds` — no derivatives,
no source HDRI, so it can never be measured — which would have rendered a black background the moment
the exposure went physical.

**`--set=ocean.contactFoam:0`** turns the shore contact foam off for a capture. It is the thing that
litters a lighting comparison with moving white speckle, and turning it off in the level would mean
editing content that was tuned on purpose.

#### P16.6b — The sea brighter than the sky, and the two controls that were missing — DONE (2026-08-20)

**THE OCEAN IS NOT THE BUG.** Reported as "the ocean is far lighter than the sky, and the reflection
is correct". Measured on the reported camera, water luminance over sky luminance:

| | water/sky |
|---|---|
| `demo_bckp`, untouched | **0.46** |
| `demo`, converted, citrus sky | **0.75** |
| same, with the sun taken to 2,000 lx and the sky to 16,000 | **0.33** |

Three surfaces on one frame say where it actually moved: **sand/sky went 0.69 → 2.28**. The ground is
now over twice the brightness of the sky above it, and the water only followed. Nothing in the water
is wrong — **the level claims a 76,000 lx sun over a photograph of an overcast evening.**
citrus_orchard's own disc carries **0.0%** of its illuminance at 7.7° elevation; it is a picture of a
day that has no sun in it. Sunlit sand at 3,654 cd/m² against a stormy horizon band at roughly
600 cd/m² is a linear ratio of ~6, which is exactly the 2.28 that survives the tone curve.

The third row is the check: give that sky the sun it actually has and the water goes properly dark
against it. **This is a content mismatch, not a renderer defect, and it is the cost of pasting one
overcast sky onto eight levels that wanted sun.**

**EXPOSURE COMPENSATION NOW WORKS IN MANUAL MODE.** It was auto-only (`targetEv = manualEv100`
ignored it), which left a fixed-exposure level with no quick ± at all: changing the look meant
re-solving the shutter by hand, which is the arithmetic the camera controls exist to remove. It is a
row under the camera params. Verified on a fixed exposure: +1 EV → +0.45 stops of frame mean, −1 EV →
−0.56 (the tone curve compresses; the direction and magnitude are right). Because it was INERT in
manual before, the levels' inherited `-0.15` was zeroed so nothing moved.

**THE SKY'S LUX IS NOW RECOMMENDED, NOT GUESSED.** Switching skyboxes should not mean inventing a
four-figure number. The recommendation does not come from the sky asset at all — it comes from WHERE
THE SUN IS, through the same clear-day model the levels were converted with, so the hint and the
content agree by construction. The inspector prints "Sun is 24 deg up -> a clear sky delivers about
10000 lx" with a **Use it** button, and the help states the other case explicitly: an overcast sky IS
the light, so use 10,000–20,000 **and take the directional sun to near zero**. The import log already
distinguishes the two — `sky sun REMOVED ... the sun was 91.7%` versus `NOT REMOVED ... no sun to
take out`.

#### P16.7 — The rest of the authored-radiance family — DONE (2026-08-20), and one open defect

**THE RULE, now stated once because this is the sixth instance: AN AUTHORED COLOUR THAT IS ADDED TO
A LIT RESULT IS A BUG WAITING FOR THE UNITS TO CHANGE. A colour is a tint on a light, not a light.**
Every one of these worked only while the scene's light happened to sit near 1:

| | what it was | what it is |
|---|---|---|
| ocean scatter tints | absolute radiance added to a lit term | tint × the light that scatters (P16.6) |
| local lights | unitless `intensity`, non-physical falloff | lumens + inverse-square (P16.5) |
| flat ambient fill | fraction of the sun's colour | re-solved to the sky's lux |
| debug/selection overlays | authored LDR into scene colour | pre-scaled on the CPU (P16.1) |
| **particles** | authored colour ≈ 1 written into scene colour | **× `luminanceCdM2`** |
| **ocean foam's sky term** | measured irradiance × the legacy `ambient` fraction | **the irradiance itself** |
| **emissive** | authored 1–10, believed unitless | **documented as cd/m2, which it always was** |

**PARTICLES.** A particle writes straight into scene colour, so an authored 1.0 against a physical
sky is a thousandth of the frame — and because they are ALPHA-BLENDED that is not "invisible", it is
DARKER THAN THE BACKGROUND: the campfire's smoke came out as a black column subtracting from the
sky, and the flame vanished. `EmitterDesc::luminanceCdM2` says what the authored colour is worth;
`colorKeys` stays a hue and an alpha. Default 3,000 = smoke lit by a bright overcast, which is what
the presets were drawing. A wood fire is 20,000–50,000, a candle 10,000, an ember a few hundred.
Verified: the flame is back and the smoke is grey. Note it now reads DARKER than the sky behind it,
which is correct — backlit smoke is darker than the sky; white smoke was the unphysical version.

**THE OCEAN FOAM'S SKY TERM** was `SkyFillRadiance(n) * (li.ambient + ...)` — the measured irradiance
times the legacy fraction-of-the-sun knob. This is the exact mistake F8 documents about its own first
version ("multiplying an absolute measured irradiance by a level's 0.05 buries the fill about twenty
times too deep"), and it survived because foam is the one thing the ocean lights with the sky. The
directional term stays; it is a shape, not a strength. wind_test: frame mean 108.8 → 121.6, no
clipping.

**EMISSIVE NEEDED NO CODE.** `emissiveStrength` is a LUMINANCE in cd/m2 and always was — the product
goes straight into scene colour, which has been in cd/m2 since the lights went to lux. There is no
conversion to invent, exactly as with the sun. What changed is that the authored numbers are absurd:
`d_emissive_test`'s embers were **6.0 and 1.5 cd/m2**, which is a dim indicator LED. Relabelled the
control ("Emissive Luminance (cd/m2)", logarithmic to 10^6, with a reference line) and set the test
rig to 3,000 / 750, keeping its 4:1 ratio.

**STILL OPEN: THE BRONZE FLOOR'S REFLECTIONS.** Characterised, not fixed. What is established:

* **not the engine** — the pre-conversion level on the current binary is 0.0% black, the converted
  one 35.4%;
* **not pre-exposure** — the gate on and off are byte-identical;
* **not the sun, the fill or GTAO** — each pinned in turn, no change;
* **not a subtraction overshoot** — a BRIGHTER sky gives LESS black (47% at 4,000 lx → 35% at
  16,000 → 29% at 40,000);
* **not a scaling error** — crown/sky measures **0.747 seen directly and 0.726 in the mirror**;
* **it is in the reflection pass**: SkyOnly is clean, every tracer is not, and dumping the reflection
  buffer (debug slot 8) shows the failure IN THE BUFFER — along with the horizontal stripes.

The signature is BIMODAL: within one crown, ~35% of pixels are fully black and blurry while the rest
are correct and sharp. That is a per-pixel resolve failure, not a brightness error, and the stripes
in the same buffer are likely the same thing. The next step is an instrument inside the pass (output
alpha and the raw hit to a debug slot), not more black-box probing.

#### P16.10 — The bronze floor: an LDR reflection target — DONE (2026-08-20)

**`kReflectionFormat` was `R8G8B8A8_UNORM`.** An LDR target holding radiance. It worked only while
scene colour happened to sit near 1; once the lights were in lux **every non-zero reflected pixel
clamped to 1.0** and the buffer became a black-and-white MASK. Changed to `R16G16B16A16_FLOAT` —
RGBA, not R11G11B10, because the alpha carries the reflection's coverage and compose's blend needs
it. The blur scratch went with it, or it would have clamped the values straight back.

What it did downstream is worse than losing the colour: compose adds only the DIFFERENCE a
reflection makes, `reflectionRGB - skyCol * alpha`. With the hit clamped to 1 and the sky at several
thousand, that difference is hugely NEGATIVE — so a metal reflecting anything at all went black.

`frac<8` in the reflected crowns: **35.4% → 0.0%**, colours back to the pre-P16 reference
(66.6/54.8/34.8 against 69.0/54.4/35.9). The horizontal banding went with it — it was the mask's
quantisation, not a tracing artefact.

**HOW THIS WAS FOUND, AND WHY IT TOOK SO LONG.** The user opened the texture inspector and read the
format off the panel: `Resource: R8G8B8A8_UNORM`. Before that I had dumped the same buffer through
the debug blit — which NORMALISES — and spent an hour reasoning about its contents while the answer
was its type. **Check what a buffer IS before reasoning about what is in it**, and treat a
normalising viewer as unusable for magnitude. Two earlier conclusions were drawn on that bad
instrument: "the buffer is identical between levels" (it was not; the aggregate means coincided) and
"the hit is blown out" (it was clamped, which is not the same thing).

It also explains the one observation that should have been decisive on day one: **RT and SSR looked
identical.** They were not agreeing — they were both being flattened by the same target.

Fixed on the way, both real and both found by the audit rather than by the symptom:

* **`ssr_cs`** inverted its multi-ray compression on the mean of ALL rays including misses.
  `L/(1+L)` and `1/(1-L)` are inverses only for a single unweighted sample; with two of four rays
  hitting a 3,000 cd/m2 surface the old form returned 0.999 instead of ~1500 — and 0.33 instead of
  0.5 for years, which nobody could see at scene scale 1. Now inverted on the mean of the rays that
  HIT, in pre-exposed space where the Reinhard curve is conditioned.
* **`rt_reflections_cs`** re-shaded OFF-SCREEN hits with `albedo * ambient * lightRgb`, the legacy
  fraction knob — the one lighting path that never got F8's sky-irradiance branch. On
  `ssr_bronze_palms` that gave 1,592 against the 6,051 lx the same leaf underside receives on screen.
  It now samples the same irradiance cube `lighting_cs` does.
* **`glass.hlsl`** started its diffuse accumulator at `tint * ambientIntensity` — an authored 0.05
  with no light in it at all. `ambient` has always meant "this fraction of the SUN'S COLOUR", so it
  is multiplied by the sun now, exactly as `lighting_cs`'s flat branch does.

#### P16.11 — The banding: RT reflection rays hit their own floor — DONE (2026-08-20)

`TraceReflection` spawned its ray with **`TMin = 0`** and a fixed `P + N * 0.02` offset. Two things
make that fail on a large flat mirror:

* `P` is **reconstructed from the depth buffer**, so it carries an error that grows with distance and
  is QUANTISED by the depth format;
* at a grazing angle a normal offset of 0.02 buys only `0.02 / sin(theta)` of clearance ALONG the
  ray, which goes to nothing as the ray flattens out.

So the ray dips back under the plane and hits the floor it came from — and because the reconstruction
error alternates above/below the true plane in steps, the self-hits land in **evenly spaced,
perspective-converging bands**. The bronze was reflecting ITSELF in stripes.

**The evidence that separated it from everything else was difference imaging.** Subtracting the
SkyOnly render from the RT render, inside the floor, left the reflected crowns (dark) plus **bright
orange horizontal lines spanning the whole floor** — orange being the floor's own colour, which is
what named the mechanism. Row statistics alone could not do this: the floor carries a 3.85% baseline
ripple from the sky's own cloud layering, and the defect was only 0.57 points on top of it.

Fixed in two parts, because there are two failures: the normal offset now scales with view distance
(where the reconstruction error lives), and **TMin skips the distance the ray needs to clear that
offset at ITS OWN angle** — which is the part a normal offset alone cannot do:

```hlsl
const float viewDist = length(P - camPos);
const float nOffset  = max(0.02f, viewDist * 0.002f);
const float cosGraze = max(dot(N, R), 0.05f);
TraceReflection(P + N * nOffset, R, camPos, nOffset / cosGraze, radiance);
```

The RT shadow rays had the same `TMin = 0` with a fixed offset and got the same treatment, or
foliage and terrain would show acne at a grazing sun.

**Verified:** floor ripple **3.548 → 3.483 against a SkyOnly baseline of 3.482** — identical to three
decimals — and the difference image is clean of bands.

Two other things were established on the way and are worth keeping:

* **the soft striping present in EVERY path, including SkyOnly, is the SKY** — the source HDRI
  carries 8.7% row-to-row structure near the horizon, and a near-mirror shows it faithfully. Not a
  defect. Confirmed by the user hiding the meshes: it stayed.
* **the solid slabs across reflected crowns are a separate, still-open defect** — see below.

#### OPEN — RT reflections do not alpha-test masked geometry

The horizontal banding across reflected palm crowns. **Diagnosed, not fixed.**

Measured on `ssr_bronze_palms` at the reported camera, dominant vertical period and its strength
against the row-profile mean:

| path | period | strength |
|---|---|---|
| **default (RT)** | 12.5 px | **15.4** |
| SkyOnly (control) | — | 5.9 |
| SSR LogMarch | 11.1 px | 5.9 |
| SSR UE march | 18.8 px | 5.2 |

RT-only. **CORRECTION to a first reading of this:** the empty floor is NOT clean, and the claim
"only where crowns are reflected" was wrong -- it came from measuring PERIODICITY (an FFT peak) on a
region that was not actually empty. The right statistic is the row-to-row ripple, and on genuinely
empty floor it is **6.24 (RT), 6.26 (SkyOnly), 6.26 (SSR LogMarch), 6.25 (SSR UE)** -- identical,
including the path with no reflected geometry at all.

So there are TWO artefacts here, and they have different causes:

* **the stripes over EMPTY floor are the SKY**, not the reflection pass. The source HDRI carries
  **8.7% row-to-row structure near the horizon** (layered cloud), and a near-mirror at grazing
  angles compresses a large vertical span of sky into a few screen rows, tightening soft layers into
  hard stripes. Optics plus content, not a defect.
* **the solid slabs across reflected CROWNS are the real bug**, below.

The cause is in the code, not inferred: `AccelerationStructure.cpp:91` flags EVERY geometry
`D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE`, masked foliage included, and both ray queries in
`rt_reflections_cs.hlsl` use `RAY_FLAG_FORCE_OPAQUE`, which would override the flag even if it were
set correctly. So RT rays hit the FULL frond quads — the parts the rasteriser alpha-tests away — and
a crown reflects as a stack of solid slabs at the frond spacing. Those slabs then fail the
on-screen depth match (the depth buffer has no surface at those texels), so they also take the
darker re-shade path, which is what makes them read as dark bands rather than merely wrong ones.

**This is general: every masked surface is solid to RT reflections AND to RT shadows.**

The fix is a real feature, not a one-liner, but the machinery is already there — `GeometryInfo`
carries `albedoTexIndex`, `vbIndex/ibIndex`, `vertexStride` and `firstTri`, and `rt_geometry.hlsli`
has the bindless VB/IB loaders:

1. `AccelerationStructure.cpp`: flag masked submeshes `GEOMETRY_FLAG_NONE` instead of `OPAQUE`
   (needs the material's alpha mode at build time).
2. `rt_reflections_cs.hlsl`: drop `RAY_FLAG_FORCE_OPAQUE`, and in the `q.Proceed()` loop handle
   `CANDIDATE_NON_OPAQUE_TRIANGLE` — interpolate UV, sample the albedo's alpha, and
   `CommitNonOpaqueTriangleHit()` when it passes.
3. The same for `RtTraceShadow`, or foliage keeps casting solid shadows in the RT path.

#### P16.4 — Occlude the sky fill at a medium scale — DONE

The sky fill was occluded only by GTAO, whose radius is **0.75 m** — a contact term that knows
nothing about a palm tree twenty metres away. Measured with the sun pinned to 1 lx so only the sky
lit the scene, raising the sky 6x brightened ground under a DENSE CANOPY by 1.92 stops and open sand
by 1.88: the crown was worth 0.04 stops of shelter. The mechanism was not missing, the SCALE was.

**Raising the one radius is not the fix.** At 12 m open sand lost about a third of a stop as the
contact term started eating open ground, and the contact detail the pass exists for goes with it —
six steps spread over 12 m put the first tap two metres out, stepping clean over the 30 cm contact
at the base of a trunk. The two questions need two radii. UE answer them with SSAO + DFAO, Godot
with SSAO + SDFGI.

**What shipped.** `gtao_cs.hlsl` walks the horizon TWICE per direction and writes both answers to
one `R8G8_UNORM` target: `.x` at `worldRadius` (contact), `.y` at the new `skyRadius` (medium). The
depth fetch, the normal reconstruction, the direction set and the phase offset are shared, so only
the walk itself is paid for twice. `skyRadius <= worldRadius` skips the second walk and copies the
contact answer into both channels — an exact no-op, and the A/B switch. The whole chain (denoise,
temporal, upsample) carries two channels through the same weights, because those weights are
computed from depth and normal alone and are identical for both scales. **No new resource,
descriptor or barrier** — the format constant is the change.

**The combine at the consumer is a MIN, not a product** (`lighting_cs::CombinedAo`, mirrored in
`compose_cs`). The two are the SAME quantity at two scales, not two independent occluders: the wide
walk sees the trunk at your feet as well as the crown overhead, so a product would square that
trunk. `min` takes whichever walk found more, which is right when each is blind at the other's
scale. The product with `materialAo` stays a product — texture cavities really are independent of
what a depth buffer can see.

**The sky walk needs its own screen clamp, and this is the part that is easy to get wrong.** The
contact walk clamps its screen reach at 256 half-res texels. Left at 256 the new knob SATURATED:
25 m and 40 m produced the same image (-0.577 vs -0.583 stops) because both were clipped to the
same screen span and only the distance falloff still separated them. Note what that means — at any
normal viewing distance the *pixel* radius is pinned at the clamp for both walks, so what actually
separates a 12 m radius from a 40 m one is `attenFactor`, the falloff. Raising the sky clamp to
1024 restored the range (see the table). It costs no extra taps — `numSteps` is unchanged and the
steps simply land further apart — so it buys with cache locality, which is what `skyMipBias` (its
own, default 2) pays back: at mip 4 a 1024-texel span is 64 texels of that level. A knob whose top
half does nothing is a knob that lies.

**Measured**, `wind_test`, sun pinned to 1 lx, two cameras chosen from the level's own geometry —
one standing where 43 palms fall within 12 m, one where the nearest is 14 m away — same ground
height, same material, ground-only crop, `--wind-freeze`, one binary, `--sweep`:

| skyRadius | dense canopy (clamp 256) | open (256) | dense canopy (clamp 1024) | open (1024) |
|---|---|---|---|---|
| 0 (off) | +0.000 | +0.000 | +0.000 | +0.000 |
| 6 m | -0.111 | -0.000 | -0.056 | -0.000 |
| 12 m | -0.418 | -0.000 | -0.419 | -0.000 |
| 25 m | -0.577 | -0.001 | **-1.048** | -0.000 |
| 40 m | -0.583 | -0.001 | -1.157 | -0.001 |

**Open ground does not move at all** — 0.001 stops across the whole sweep, against the third of a
stop it cost to raise the single radius. That is the entire point of the split. The default is
**25 m**, where the curve bends: 40 m adds 0.11 stops for the same cost.

**Cost**: `Pass_Gtao` (the whole chain) 0.0720 -> 0.0875 ms, **+0.0155 ms (+22%)**. Three
interleaved runs per side, medians, first quarter of frames dropped, `Pass_Compose` as an unmoving
control at 0.0270 in all six — because two consecutive runs are NOT comparable on their own here,
the GPU clock drifts between them (a first attempt showed `Pass_Hzb`, which nothing touched, moving
26%).

**Regression**, whole-frame luminance at skyRadius 25 vs 0: `demo` -0.0005 stops,
`roughness_sweep` +0.0001, `d_emissive_test` -0.0000. Gate: 33/33 shaders, both configs,
`--gbv` on `wind_test`, and `--scene-stress-gbv=8` CLEAN across reload / level switch / resize /
DLSS mode / render scale / reflection scale / editor spawn-delete — the operations that recreate
the AO targets at the new format.

**Left for later.** `skyFillIntensity` and the sky's appearance still share one number by design;
that half of the original P16.4 note is unchanged and is a separate question from occlusion. And
the medium walk is still SCREEN-space: it cannot occlude from geometry off-screen or behind the
camera, which is what a distance-field or a voxel term would buy.

---

**Do not start P16.2 before P16.1 is verified.** Its failure mode is a uniform brightness error that
looks like a tuning problem, and it will be blamed on the level.
