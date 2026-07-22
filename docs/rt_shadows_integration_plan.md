# Ray-Traced Shadows Integration — Implementation Plan (for AI executors)

This is an execution plan for adding ray-traced (RT) shadows alongside the existing shadow-map
pipeline (Legacy CSM/atlas + Virtual Shadow Maps). It is written so an AI agent can pick up one
**R-step**, implement it against a declared interface contract, and verify it before the next step
starts. Keep every landed step buildable and visually testable. RT shadows are **added beside** the
shadow-map path, never a forced replacement.

---

## 1. Goal and non-goals

**Goal:** Provide correct, artifact-free ray-traced shadows for the directional sun first, then for
local (spot/point) lights, reusing the engine's existing DXR inline-RayQuery infrastructure. RT
shadows must eliminate the shadow-map artifact classes (peter-panning, acne, bias tuning, LOD/clipmap
popping, page-residency flicker, per-light page pressure from overlapping lights) while preserving
masked (alpha-tested) foliage cutouts and two-sided casters.

The target results:

- the sun casts pixel-exact contact shadows with no bias tuning and no cascade/clipmap seam;
- palm-frond shadows are alpha-cut, not solid blobs;
- many overlapping local lights cost flat memory (no per-light shadow pages) when RT is used;
- a runtime toggle selects shadow-map vs RT per shadow source, and hardware without RT falls back
  cleanly to the shadow-map path;
- DefaultLit, foliage, instancing, DLSS, SSR/RT reflections, and the shadow-map path do not regress.

**Non-goals for the first milestones:** full many-light importance sampling, ReSTIR, RT global
illumination, translucent/colored shadows, contact-hardening area lights beyond a single penumbra
model, or removing the shadow-map path. The shadow-map pipeline stays as the default and the
non-RT-hardware fallback.

---

## 2. Why RT shadows, and the honest trade

RT shadows remove the entire shadow-map artifact family this codebase has fought (peter-panning,
acne, bias tuning, wide-spot perspective-depth crush, clipmap/CSM seams, VSM page-residency flicker,
and the N× physical-page cost of overlapping local lights — a VSM page stores depth from one light's
viewpoint, so lights at different positions cannot share pages). RT replaces stored depth with a
per-pixel visibility ray, so those artifacts do not exist.

The trade the executor must respect:

- **Hard shadows** (1 ray) are exact and cheap. **Soft/penumbra** shadows need either multiple rays
  (noise) or a variable-blur denoiser — plan a denoiser, do not ship raw 1 spp soft shadows.
- **Masked foliage** shadow rays traverse non-opaque candidates and must alpha-test per hit
  (any-hit-style work in the inline RayQuery loop). This is the main per-ray cost; measure it.
- Cost scales with rays/pixel × lights and with BVH/any-hit work, not with shadow-map resolution.
  For the sun it is ~1 ray/pixel; for many overlapping locals it trades per-light memory for
  per-light ray cost. Do not assume RT is universally faster — **measure per scene**.

**Locked decision:** the sun (directional) lands first via a dedicated, denoisable full-screen
visibility mask. Local lights come later and trace inline in their existing compute loops. The
shadow-map path remains and is the fallback.

---

## 3. Reference model and decisions locked for implementation

1. **Inline RayQuery in compute**, reusing the RT-reflections architecture — no separate ray-gen
   shader / shader binding table. SM6.6 `ResourceDescriptorHeap`, `RayQuery<>` over the existing TLAS.
2. **Sun visibility is a decoupled pass.** A full-screen RT pass writes a per-pixel sun visibility
   value (0..1) to its own target; `lighting_cs.hlsl` samples that instead of `SampleShadowCSM` /
   `VsmClipmapShadow` when the sun is in RT mode. Decoupling makes the result denoisable and keeps the
   light shader simple.
3. **Local lights trace inline.** `pointlight_cs.hlsl` / `spotlight_cs.hlsl` trace a shadow ray per
   shadowed light inside their existing per-light loop when that light is flagged RT. No per-light
   full-screen mask (would be N passes).
4. **Shadow mode is a selectable source, not on/off.** Add `SunShadowMode { ShadowMap, RayTraced }`
   (mirror the existing `ReflectionSource` pattern and its F-key cycling), plus a per-local-light
   `rtShadow` flag. `ShadowMap` = the current Legacy/VSM path unchanged.
5. **Reuse the reflections TLAS.** RT shadows consume the acceleration structure already built for RT
   reflections (`Pass_BuildAS`). If shadow casters are not fully represented there (masked, two-sided,
   instanced), extend the AS build in a dedicated step rather than duplicating it.
6. **Masked shadows via non-opaque candidates.** The inline RayQuery accepts
   `CANDIDATE_NON_OPAQUE_TRIANGLE`, fetches the hit triangle's albedo alpha through the existing
   bindless per-submesh material records, applies the material alpha cutoff, and commits only if
   opaque enough. Opaque geometry uses the fast committed path.
7. **Denoise is explicit and staged.** Hard first (R2/R3), then temporal+spatial (R4), then soft
   penumbra (R5). Consider DLSS Ray Reconstruction only after a hand-rolled denoiser is understood;
   the RT-reflections work already found 1 spp + DLSS jitter needs real reconstruction.
8. **Fallback is mandatory.** No RT hardware, or AS not built → the light shaders use the shadow-map
   path. Never bind a null AS or trace against an invalid TLAS.

Official references (background only; do not copy private constants):

- <https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html> (inline RayQuery, any-hit semantics)
- <https://developer.nvidia.com/rtx/ray-tracing/rt-denoisers> (shadow denoising background)
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-settings-in-unreal-engine> (hybrid RT-shadow practice)

---

## 4. Current engine state (verified 2026-07-22, confirm before relying on any item)

- **Shadow modes** live in `sources/rendering/renderables/InstanceTypes.h`: `render::ShadowMode
  { Legacy = 0, VSM = 1 }`, `render::g_shadowMode` (default **VSM**), `render::VsmActive()`. Legacy =
  CSM directional + spot/point/glass atlas; VSM = the virtual page pool.
- **Light accumulation is per-pass compute**, each writing/adding to `LightTarget` (u0):
  - `shaders/lighting_cs.hlsl` — directional sun + flat ambient. Already has `sunDirWS`,
    `sunAngularSize` CB fields, `camPosWS`, `invView`/`invProj`, and samples GB0/GB1/GB2/Depth/GBAux.
    Shadow via `SampleShadowCSM` (Legacy) or `VsmClipmapShadow` (VSM), wrapped in a local
    `SampleSunShadow` helper.
  - `shaders/pointlight_cs.hlsl` — loops point lights; shadow via `PointShadowFactor` (atlas cube or
    `VsmPointShadow`).
  - `shaders/spotlight_cs.hlsl` — loops spot lights; shadow via `ComputeSpotShadow` (atlas or
    `VsmSpotShadow`). Bias for the VSM path is texel-sized world-space (see the shadow-bias work).
- **CB plumbing:** each light pass has a C++ constants struct in `SceneResourceBootstrapper.h`
  (`PointLightPassConstants`, `SpotLightPassConstants`, and the directional lighting constants) mirror-
  ed by an HLSL cbuffer, written field-by-field by named `ComputeCB0FieldHandle` handles
  (`WriteSpotLightConstants` etc.). New CB fields require a handle in the `*CBHandles::Populate` and a
  write — an unwritten field reads garbage (per-frame ring memory) and flickers.
- **DXR / RT reflections already exist** (Tier 1, user-confirmed): `Pass_BuildAS` builds BLAS/TLAS
  (`sources/rendering/.../AccelerationStructure.cpp`), a bindless table with per-(instance,submesh)
  material records (`BindlessTable`, albedo/MR/params + `firstTri`), and an inline-RayQuery compute
  shader `shaders/rt_reflections_cs.hlsl` that binds a `RAYTRACING_ACCELERATION_STRUCTURE` SRV via the
  normal compute descriptor path and shades off-screen hits (sun + ambient + shadow ray + real albedo
  textures). Gotchas recorded: AS scratch is COMMON not UAV; AS bypasses the resource-state tracker;
  instance transform is transpose of the upper 3×4; AS-SRV uses a null resource + Location.
- **Reflection source** is `ReflectionSource { None, SkyOnly, SSR, RT }` in
  `sources/app/scene/SceneFrameData.h`, cycled in `AppController.cpp`, labeled in `DeveloperWindow.cpp`
  — the pattern to mirror for a shadow-source toggle.
- **Masked / two-sided casters:** materials carry `alphaTest` + per-slot `alphaCutoff` and
  `twoSided`; the shadow-map path has a masked SHADOW variant (C2). RT hit shading already reads
  per-submesh albedo, so alpha data for shadow rays is available through the same records.
- **Motion vectors** exist (GBVelocity, GBuffer RT3) and DLSS/Streamline is integrated — both needed
  by a temporal shadow denoiser.
- Shaders compile at runtime; a C++ build is not sufficient — every step touching HLSL must run a
  scene that exercises the changed pass.
- Screenshot capture: `test_cube.exe --level=<level> --shot=<out.png> --shot-delay=<seconds>` reads
  back the last-presented backbuffer (reliable; external screen-grab of the flip-model swapchain is
  not).

---

## 5. Executor conventions

- **One logical R-step per commit**, but only create commits when the human explicitly asks.
- Build from PowerShell with MSBuild. Build Debug x64 after every step; build Release **and**
  `Release_Editor` at milestone boundaries. `WITH_EDITOR` is Debug/Release_Editor only — editor UI
  (toggles, developer window) must be built in one of those, not Release.
- C++/HLSL/project files are CRLF; Markdown is LF. Run `git diff --check` and verify no mixed endings.
- New CB fields: add the handle in `Populate`, write it in the `Write*Constants`, and mirror the HLSL
  cbuffer layout with **scalar** padding (never `float2/float3` pads — they straddle-mismatch the
  16-byte HLSL rows). An unwritten CB field flickers.
- Every dormant/plumbing step must be visually byte- or screenshot-equivalent to its baseline. Do not
  combine plumbing and the visual flip in one step.
- Keep defaults on the shadow-map path: RT shadows are opt-in per source until M3 sign-off.
- Run the D3D12 debug layer / GPU validation for any AS-binding, descriptor, or resource-state change.
  Exercise `--scene-stress` with light/level churn for AS lifetime.
- Verify visual results with the user before declaring a shadow feature correct (own-screenshot reads
  are not trusted for shadow acne/flicker, which are motion-dependent).

---

## 6. Interface contracts

### Shadow-ray helper (HLSL)

A single shared include (e.g. `shaders/rt_shadow.hlsli`) exposing:

```hlsl
// Returns visibility in [0,1]: 1 = fully lit, 0 = fully occluded.
// origin: receiver world pos (already offset off the surface by a small epsilon along N).
// dir:    normalized direction toward the light.
// tMax:   distance to the light (or a large value for the sun).
// evalAlpha: if true, non-opaque candidates are alpha-tested via bindless records; else force-opaque.
float TraceShadowVisibility(RaytracingAccelerationStructure tlas, float3 origin, float3 dir,
                            float tMin, float tMax, bool evalAlpha);
```

- Uses `RayQuery<RAY_FLAG_CULL_...>`; committed opaque hit => occluded. Masked handling per Section 3.6.
- Deterministic given inputs; no per-frame randomness except an explicit sample index passed in for
  soft shadows.

### Sun visibility target

- A dedicated render target (R8_UNORM or R16_FLOAT), render-resolution, holding sun visibility.
- `lighting_cs.hlsl` samples it (point sample) in place of the analytic shadow term when
  `sunShadowMode == RayTraced`; `ShadowMap` mode never allocates/reads it.

### Toggle

- `SunShadowMode { ShadowMap, RayTraced }` in `SceneFrameData.h`; cycled by an input action; labeled in
  the developer window. Per-local-light `bool rtShadow` (JSON + light desc + GPU flag).

---

## 7. Dependency graph and milestones

```text
R0 baseline/readiness ─> R1 helper+TLAS binding (dormant) ─> R2 sun RT (opaque, hard)
                                                              ├─> R3 masked/two-sided rays
                                                              └─> R4 sun denoise ─> R5 soft penumbra
R2 + R3 ─> R6 local (spot/point) RT ─> R7 hybrid policy + perf
R3 + R5 + R6 ─> R8 parity/fallback/hardening/sign-off
```

Milestones:

- **M0 — RT-shadow readiness:** R0-R1. AS proven to cover casters; a shadow ray traces correctly;
  toggle plumbed; nothing visually changed yet.
- **M1 — usable RT sun:** R2-R4. Opaque + masked sun shadows with a denoiser; the atoll/demo sun reads
  cleanly with no bias artifacts.
- **M2 — soft + locals:** R5-R6. Penumbra for the sun; RT for spot/point to kill the overlapping-light
  page pressure.
- **M3 — production:** R7-R8. Hybrid selection, measured budgets, hardware fallback, full parity.

---

## 8. Step template

Each step declares **Depends**, **Goal**, **Touch**, **Implement**, **Interface contract**,
**Done-when**, and **Verify**. If a step cannot meet acceptance without pulling later work, stop and
report the dependency instead of silently widening the change.

---

### R0 — Baseline capture and RT-readiness audit

- **Depends:** none.
- **Goal:** Prove the AS covers all shadow casters and a shadow ray can be traced from the light-pass
  context, and capture shadow-map references for later comparison.
- **Touch:** no permanent source changes; a temporary scratch probe that must be reverted.
- **Implement:**
  - Record the fixed cameras/levels for later A/B (atoll sun; a low point/spot scene; a masked-foliage
    scene). Capture current Legacy-CSM and VSM sun shadows and local shadows.
  - Confirm `Pass_BuildAS` includes opaque, masked, two-sided, and instanced casters (inspect the BLAS
    geometry set and instance list). Note any caster type missing from the AS.
  - Temporarily, in a scratch compute pass or a guarded probe in `rt_reflections_cs.hlsl`, trace a
    single receiver→sun ray and visualize hit/miss; confirm the `RAYTRACING_ACCELERATION_STRUCTURE`
    SRV binds and the ray returns sane occlusion. Revert the probe.
- **Interface contract:** none (audit only). Findings recorded in the results section.
- **Done-when:** a documented list of which caster types are/aren't in the AS, and a confirmed
  hit/miss visualization from a sun ray.
- **Verify:** fixed-camera reference PNGs; the probe visualization; Debug build unchanged after revert.

---

### R1 — Shadow-ray helper + TLAS binding into light passes (dormant)

- **Depends:** R0.
- **Goal:** Land the reusable `TraceShadowVisibility` helper and bind the TLAS SRV into the light-pass
  descriptor tables without changing any pixel.
- **Touch:** new `shaders/rt_shadow.hlsli`; `lighting_cs.hlsl` / `pointlight_cs.hlsl` /
  `spotlight_cs.hlsl` (bind + include, no calls yet); light-pass root signatures/descriptor staging in
  `SceneRenderer.cpp` / `SceneResourceBootstrapper.*`; `SceneFrameData.h` (`SunShadowMode`), input
  cycling, developer-window label.
- **Implement:**
  - Author the helper per the Section 6 contract (opaque path first; masked path is R3, guarded off).
  - Append the AS SRV to each light pass's descriptor table (do not renumber existing registers).
    Guard the binding: if the AS is absent or RT unsupported, bind the documented null-AS and keep
    `SunShadowMode == ShadowMap`.
  - Add the `SunShadowMode` enum + toggle + per-light `rtShadow` flag (parsed, stored, uploaded as a
    GPU flag), all defaulting to the shadow-map path.
- **Interface contract:** helper signature and target formats exactly as Section 6; toggle defaults to
  `ShadowMap`.
- **Done-when:** builds clean; the AS SRV is bound in all three light passes; toggling the (unused)
  mode changes nothing yet; GPU validation clean.
- **Verify:** Debug + `Release_Editor`; before/after screenshots byte/jitter-equivalent; GPU
  validation shows the new SRV bound and valid.

---

### R2 — RT sun shadow, opaque, hard (first visual flip)

- **Depends:** R1.
- **Goal:** Replace the directional sun shadow term with a traced ray against opaque geometry when
  `SunShadowMode == RayTraced`.
- **Touch:** new sun-visibility RT pass (compute) + its target in `RenderTargetManager` and the render
  graph; `lighting_cs.hlsl` (sample the mask when RT); `SceneRenderer.cpp` pass wiring.
- **Implement:**
  - Add a full-screen compute pass: reconstruct world pos from depth, offset off the surface along N by
    a small epsilon, trace toward `-sunDirWS` with `tMax` large, `evalAlpha=false` (force opaque),
    write visibility to the sun-visibility target.
  - `lighting_cs.hlsl`: when RT, multiply the sun's direct term by the sampled visibility instead of
    `SampleSunShadow`. Ambient and analytic BRDF unchanged. Sky background unchanged.
  - Skip the VSM/CSM directional shadow work when the sun is RT (do not pay for both), but keep it
    intact for `ShadowMap` mode.
- **Interface contract:** `SunShadowMode` selects the source; `ShadowMap` is byte-identical to today.
- **Done-when:** RT sun gives exact contact with no peter-panning/acne and no bias knobs; opaque
  silhouettes match the shadow-map result; DefaultLit unchanged in `ShadowMap` mode.
- **Verify:** fixed-camera A/B (ShadowMap vs RT) on opaque geometry; flying camera shows no
  distance-dependent detachment; GPU validation; runtime shader compile clean.

---

### R3 — Masked (alpha-tested) and two-sided shadow rays

- **Depends:** R2.
- **Goal:** Make foliage shadows cut-out, not solid, and keep two-sided casters correct.
- **Touch:** `rt_shadow.hlsli` (non-opaque candidate handling); bindless material-record access in the
  shadow context; the sun-visibility pass (enable `evalAlpha`).
- **Implement:**
  - In the RayQuery loop, on `CANDIDATE_NON_OPAQUE_TRIANGLE`, fetch the hit's albedo alpha via the
    per-(instance,submesh) records (barycentric UV interpolation + bindless albedo sample) and commit
    only if `alpha >= cutoff`; else continue traversal.
  - Confirm two-sided/foliage casters are not back-face-culled out of the shadow ray.
  - Keep an opaque fast path (force-opaque) for materials without alpha test to avoid the any-hit cost.
- **Interface contract:** masked geometry casts cut-out shadows identical in coverage to the masked
  shadow-map variant; opaque unaffected.
- **Done-when:** palm-frond shadows show sky through the cutouts; no solid-blob frond shadow; two-sided
  cards cast from both faces.
- **Verify:** masked-foliage scene A/B vs the masked shadow-map path; measure the added per-ray cost of
  the alpha path; GPU validation.

---

### R4 — Sun shadow denoise (temporal + spatial)

- **Depends:** R2 (R3 for masked content).
- **Goal:** Turn the 1 spp sun visibility into a stable, slightly soft result under motion.
- **Touch:** a denoise pass (temporal reprojection using GBVelocity + a spatial filter), history
  targets, `RenderTargetManager`, render graph; optional DLSS Ray Reconstruction evaluation.
- **Implement:**
  - Temporally accumulate visibility reprojected by motion vectors with disocclusion rejection; add a
    small edge-aware spatial blur (depth/normal guided).
  - Keep the hard result available (denoise strength = artist/dev tunable) so contact stays crisp.
- **Interface contract:** denoised visibility ∈ [0,1]; disabling the denoiser returns the raw 1 spp
  result.
- **Done-when:** no shimmer on static or slowly moving shadows; contact edges stay attached; no
  smearing ghost trails behind moving casters beyond a documented tolerance.
- **Verify:** moving-camera run with DLSS on/off; disocclusion test (fast pan); before/after video
  frames; the user confirms motion stability.

---

### R5 — Soft sun penumbra

- **Depends:** R4.
- **Goal:** Physically-plausible penumbra scaled by the sun's angular size, without heavy noise.
- **Touch:** the sun-visibility pass (cone sampling), the denoiser (variable blur radius), sun angular
  params (already present as `sunAngularSize`).
- **Implement:**
  - Sample the sun's angular disk: either N stratified rays per pixel, or 1 jittered ray + a
    penumbra-width estimate feeding a variable-radius denoiser. Start with the 1-ray + variable-blur
    approach (cheapest) and only add rays if quality demands.
  - Penumbra widens with `sunAngularSize`; hard when angular size → 0.
- **Interface contract:** `sunAngularSize` drives penumbra continuously; 0 = hard.
- **Done-when:** shadow edges soften with distance from the contact and with angular size, no dancing
  noise, contact still sharp.
- **Verify:** angular-size sweep; moving run; A/B vs shadow-map PCF softness.

---

### R6 — RT local (spot/point) shadows

- **Depends:** R2, R3.
- **Goal:** Trace shadow rays for flagged spot/point lights inline, solving the overlapping-light page
  pressure (flat memory, exact contact).
- **Touch:** `pointlight_cs.hlsl` / `spotlight_cs.hlsl` (trace inside the per-light loop when
  `rtShadow`), the per-light GPU flag, light desc/JSON/editor for the flag.
- **Implement:**
  - When a light is RT-flagged, replace `PointShadowFactor` / `ComputeSpotShadow` with a ray from the
    receiver toward the light (`tMax` = distance to light), `evalAlpha` per the material set present.
  - Keep attenuation/cone falloff unchanged; multiply by the traced visibility.
  - Do not allocate VSM/atlas pages for RT-flagged lights (that is the memory win). Non-flagged lights
    keep the shadow-map path.
- **Interface contract:** per-light `rtShadow` selects RT vs shadow-map; falls back to shadow-map on
  no-RT hardware.
- **Done-when:** 8 overlapping spots render correct contact shadows with **no** growth in VSM pool
  usage; foliage under local lights is masked.
- **Verify:** the 8-overlapping-spot scene (memory/pool stats flat vs the VSM case); front/back and
  grazing; scene-stress with GPU validation.

---

### R7 — Hybrid selection policy and performance pass

- **Depends:** R5, R6.
- **Goal:** Decide per frame which lights use RT vs shadow-map, and measure the real cost.
- **Touch:** light selection (`LightManager`), a policy (overlap/importance/artist opt-in), developer-
  window budget readouts.
- **Implement:**
  - A documented policy: sun RT when enabled; locals RT when their overlap/count would oversubscribe
    the VSM pool or when artist-flagged; else shadow-map.
  - Measure GPU cost: sun ray pass, masked any-hit overhead, local ray cost vs the VSM page-render cost
    they replace. Log a per-mode breakdown.
  - Expose the toggles and the measured numbers in the developer window.
- **Interface contract:** the policy is deterministic and inspectable; toggles override it.
- **Done-when:** a scene with mixed lights picks sensible modes automatically and the cost breakdown is
  visible; no worse than the shadow-map path in the shadow-map-favourable case (few lights, no
  overlap) because RT is then not selected.
- **Verify:** perf capture on atoll + demo + the overlap scene; A/B RT-forced vs auto vs shadow-map.

---

### R8 — Parity, hardware fallback, hardening, and sign-off

- **Depends:** R3, R5, R6.
- **Goal:** All paths agree; no-RT hardware works; production defaults set.
- **Touch:** RT-capability query + fallback, instancing/foliage/two-sided parity checks, material
  live-apply/AS-refresh hooks, final results section.
- **Implement:**
  - On hardware without DXR (or AS not built), force `ShadowMap` for all sources; the toggle is
    disabled/annotated. Never bind an invalid AS.
  - Confirm raster instanced draws, masked alpha, and two-sided casters produce the same RT shadow as
    per-object draws; material live edit refreshes AS/bindless records (stale descriptors are a hard
    failure).
  - Establish production defaults (which sources default to RT once signed off) and document measured
    GPU cost.
- **Interface contract:** RT and shadow-map agree on silhouette/coverage; older content still renders
  on non-RT hardware.
- **Done-when:** the user confirms the RT shadow look; no invalid-descriptor/resource-state messages;
  `--scene-stress` clean; DefaultLit/foliage/instancing/DLSS/SSR/RT-reflections regression-clean.
- **Verify:** Debug, Release, `Release_Editor`; fixed-camera matrix (ShadowMap vs RT; sun/local;
  masked; instanced; DLSS motion); GPU validation; `git diff --check`; line-ending audit.

---

## 9. Global acceptance criteria

- RT sun shadow has pixel-exact contact with **no** bias tuning and no cascade/clipmap seam.
- Masked foliage casts alpha-cut RT shadows; two-sided casters cast from both faces.
- Overlapping local lights on RT cost flat shadow memory (no per-light pages).
- Soft shadows scale with angular size and are stable under motion (denoised, no dancing noise).
- `SunShadowMode == ShadowMap` and non-RT hardware are byte/behaviour-identical to today's shadow-map
  pipeline.
- DefaultLit, foliage, metals, motion vectors, object picking, DLSS, SSR, RT reflections, CSM, VSM,
  and auto-instancing pass regression checks.
- No mixed line endings; C++/HLSL are CRLF and this Markdown file is LF.

---

## 10. Risks and rollback notes

- **AS coverage gap:** if `Pass_BuildAS` omits masked/two-sided/instanced casters, RT shadows will be
  wrong for exactly the content that matters. R0 must audit this; extend the AS build if needed before
  R2.
- **Any-hit cost:** alpha-tested shadow rays (foliage) are the dominant per-ray cost. Keep an opaque
  fast path; measure; consider limiting alpha evaluation range or LOD.
- **Denoiser artifacts:** 1 spp soft shadows without a denoiser dance; over-strong denoise smears
  contact or ghosts moving casters. Stage denoise (R4) before soft (R5); keep the hard result
  available.
- **CB flicker:** any new per-light/per-frame flag added to a light CB must be registered as a named
  handle and written every frame (an unwritten field reads per-frame garbage and flickers — a bug this
  codebase has already hit).
- **Double-cost:** do not run both the shadow-map render and the RT trace for the same light; select
  one per source/light.
- **Descriptor/state hazards:** the AS bypasses the resource-state tracker and its scratch is COMMON,
  not UAV; material live-apply must refresh AS/bindless records or stale CPU descriptors crash. Run GPU
  validation on every AS/descriptor change.
- **Fallback:** every RT step must degrade to the shadow-map path on no-RT hardware — never a black or
  unshadowed scene. RT reflections' capability query is the model.
- **Rollback:** R1 is dormant; R2 is gated behind `SunShadowMode`; R6 is gated per-light; reverting the
  toggle default restores the shadow-map pipeline entirely. The shadow-map path is never removed.

## Suggested execution order

R0 -> R1 -> R2 -> R3 -> R4 gives the shortest route to a user-visible, correct RT **sun** shadow that
removes the artifact classes this codebase has fought. Then R5 (soft) and R6 (locals, the
overlapping-light memory win), R7 hybrid/measurement, and R8 parity/fallback/sign-off. Do not remove
or regress the shadow-map path at any point — RT is added beside it and defaults off until M3.
