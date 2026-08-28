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
- **Vertex-animated casters do not exist in the BVH.** Wind sway in this engine is a pure vertex-
  shader displacement (`shaders/wind.hlsli`), and a BLAS is built once from the static vertex buffer
  and cached per `Mesh*`. Every RT shadow of a swaying plant is therefore a shadow of its **rest
  pose** until a deform+refit path exists (step **RW**). This is the single largest scope item this
  plan gained after the readiness review — it is not a tuning detail, it is missing geometry.

### Why the cost curves differ (the structural argument)

The shadow-map cost is *world-space*: pages × geometry rasterized per page. It grows when the camera
approaches casters (finer clipmap levels become resident) and it is traded against texel size — which
is exactly the `clipmapBaseExtent` quality-vs-cost dial that has no good setting on the atoll. The RT
cost is *screen-space*: ~1 ray per render pixel, independent of clipmap extent, shadow-map resolution
and camera proximity. RT is the structurally right answer for a scene whose shadow cost is dominated
by dense masked foliage near the camera. That is the case for choosing it — not a raw ms ratio, which
is expected to be roughly 2× rather than an order of magnitude (see §4.1).

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

## 4. Current engine state (verified 2026-07-22, code re-audited 2026-07-23; confirm before relying on any item)

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

### 4.1 Measured baseline — what RT is actually competing against (atoll, 2026-07-23)

The atoll is the scene that motivates this work, so its numbers are the acceptance target. Captured
in Release with VSM active, camera at ground level under the palm cluster, DLSS render scale 0.58:

| | |
|---|---|
| `Pass_VsmPageRender` | **1.46 ms GPU** avg / 2.05 max (0.28 ms CPU) |
| `Pass_ShadowCull` + `Pass_VsmPageRequest` | 0.05 + 0.04 ms GPU |
| `GPU.Frame` | 2.55 ms — i.e. VSM is **~60 % of the whole frame** |
| `Pass_RTReflections` (cost-of-a-ray reference) | 0.08 ms at half display res, incl. hit shading **and** a secondary shadow ray |

**All of that VSM cost is the sun.** `data/levels/atoll.json` has `pointLights: []` and
`spotLights: []`, so the local-light half of the VSM pipeline is idle and the directional clipmap is
100 % of `Pass_VsmPageRender`. R2 therefore does not shave a slice off this pass — it retires the
pass, the shadow cull and the page request outright for this scene. That makes the atoll the cleanest
possible A/B and it should be the primary benchmark for R2/R3.

Scene shape driving the cost: 100 palms (34 coconut / 33 date / 33 curly) of 6630 / 9941 / 4490 tris
at LOD 0, plus island, lagoon floor and 4 rocks — ~107 CPU-placed instances, all already present in
the TLAS. 281 resident pages × 128² = **4.6 Mtexel of alpha-tested foliage rasterization per frame**,
versus ~0.7 Mpixel of render-resolution rays that would replace it.

`clipmapBaseExtent = 12.0` over `kVirtualRes = 2048` is **5.9 mm per texel** at the finest level.
Raising the extent is the only shadow-map lever that meaningfully cuts the page count, and it costs
quality on two axes at once: texel size, and — because `VsmClipmapShadow` derives its world-space
normal bias from `max(dist, 0.5*baseExtent) / (0.5*VSM_VIRTUAL_RES)` (`shaders/vsm_sample.hlsli:141`)
— a proportionally larger bias push, i.e. peter-panning. This coupling is why the extent dial feels
worse than a pure resolution trade. Any RT-vs-VSM comparison must be made against a *fairly tuned*
VSM baseline, so re-tune `clipmapNormalBias` whenever the extent is changed for a measurement.

### 4.2 RT-readiness audit findings (pre-answers R0 — verify, do not re-derive)

Read from the code on 2026-07-23. These are gaps R0 must confirm and later steps must close; they are
recorded here so no executor plans around a BVH that does not contain what they think it does.

1. **Every BLAS geometry is forced opaque.** ~~`AccelerationStructure.cpp:90` sets
   `D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE` unconditionally. Masked foliage is a solid quad in the BVH
   today (already visible in RT reflections). Without R3 an RT sun shadow of a palm is a **black
   rectangle**, not a cut-out crown. R3 is not an enhancement here, it is a correctness prerequisite —
   R2 sign-off on the atoll is meaningless without it.~~ **CLOSED 2026-08-28.** Masked submeshes now
   build their BLAS geometry without the OPAQUE flag (`InstanceEntry.nonOpaqueSlots`, derived from the
   same per-slot `alphaCutoff` the raster clip uses), the bindless record carries the cutoff, and every
   RayQuery loop (reflection, off-screen shadow ray, debug view) alpha-tests
   `CANDIDATE_NON_OPAQUE_TRIANGLE` via `RtAlphaCandidatePasses` in `rt_geometry.hlsli` — UE's
   masked-segment any-hit policy (`RayTracingInstanceMask.cpp:219`), at per-geometry granularity.
   Verified on a mirror sphere in the wind_test grove: reflected crowns are cut-out fronds with sky in
   the gaps. An RT shadow pass inherits this for free.
2. **BLAS is static and cached per mesh, and cannot be refit.** `AccelerationStructure.h:17-24`
   ("Geometry in this engine is static, so a BLAS is built once and cached per mesh"), built with
   `PREFER_FAST_TRACE` only — no `ALLOW_UPDATE` (`AccelerationStructure.cpp:109`), so `PERFORM_UPDATE`
   is not even legal on it. Wind sway is invisible to RT. See step **RW**.
3. **Wind phase is per-instance, so BLAS sharing dies under deformation.** The per-plant phase is
   `dot(worldOrigin.xz, float2(0.13, 0.17))` (`shaders/wind.hlsli:114`), unique per palm. 100 palms
   share 3 BLASes today; deformed, they need 100. This is the cost driver of RW, not the triangle
   count.
4. **TLAS is a non-issue.** 107 instances, transforms only; in-place refit already implemented and
   shipping (`AccelerationStructure.h:104-109`, S12), with a periodic full rebuild to bound BVH-quality
   drift. Rebuilding it every frame is microseconds. Do not spend effort here — sway is not a
   transform, so no amount of TLAS work addresses item 2.
5. **Caster coverage is otherwise good but not total.** `SceneRenderer.cpp:1030-1098` gathers opaque,
   single-mesh, CPU-placed visible instances plus GPU-instanced models, with per-slot material records
   (so palms already carry bark/frond materials for hit shading). Excluded by design: ocean
   (GPU-displaced, planar path), transparent/glass. Confirm the exclusions are acceptable as shadow
   casters before R2 sign-off — an unshadowed ocean is fine, an unshadowed glass object may not be.

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
                                                              │     └─> RW wind-deformed BLAS
                                                              └─> R4 sun denoise ─> R5 soft penumbra
R2 + R3 ─> R6 local (spot/point) RT ─> R7 hybrid policy + perf
R3 + R5 + R6 + RW ─> R8 parity/fallback/hardening/sign-off
```

Milestones:

- **M0 — RT-shadow readiness:** R0-R1. AS proven to cover casters; a shadow ray traces correctly;
  toggle plumbed; nothing visually changed yet.
- **M1 — usable RT sun:** R2-R4 (+ RW for any wind scene). Opaque + masked sun shadows with a
  denoiser; the atoll/demo sun reads cleanly with no bias artifacts. **R2 alone is a measurement, not
  a shippable state** on the atoll: without R3 the crowns are solid, without RW they do not sway.
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

### RW — Wind-deformed casters in the BVH (deform pass + per-instance updatable BLAS)

**STATUS 2026-08-28 — NEAR-TIER SUBSET IMPLEMENTED (for RT reflections; an RT shadow pass will
inherit it).** What shipped: `shaders/rt_wind_deform_cs.hlsl` (includes `wind.hlsli`, calls
`WindOffset` with the gbuffer VS's exact argument sourcing, writes a packed float3 position
stream in OBJECT space so the TLAS transform applies once); a 24-slot dynamic-BLAS pool in
`AccelerationStructureManager` (ALLOW_UPDATE + PREFER_FAST_BUILD, per-slot persistent scratch,
staggered full rebuild every 16 frames, **LOD 1 index buffer** — LODs share the base VB so the
deformed positions apply unchanged); stable owner->slot binding + nearest-first selection within
40 m in `RtSceneAs` (CPU-placed single-instance casters only; GPU-instanced clouds stay
rest-pose). Hit shading keeps reading the STATIC vertex buffer through the bindless table — UVs
do not move with sway and stale normals on a bent frond are invisible at reflection res.
Phase bucketing and the mid tier are still TODO for grove-scale RT shadows.

**Measured (ssr_bronze_palms wind bench, 66 palms, 24 animated, Release):** whole frame 1.40 ms
vs 1.11 ms with the AS pass off — **~0.29 ms for deform + 24 BLAS refits + trace delta**, inside
this plan's budgeted model. Two costs the model undersold, both fixed en route: (1) interleaving
each build with its own barrier SERIALIZED the GPU at ~34 us/palm — batching all stream
transitions before the builds and all AS-read barriers after them cut 0.60 ms; (2) LOD 0 refits
were fine but pointless — LOD 1 is free quality-wise at reflection res. Reflected crowns sway in
lockstep with the raster (live series: direct-crown frame diff 4.8/3.8/5.1 vs reflections
3.4/3.1/3.9 — the reflection tracks at ~75% through the half-res blur chain).


- **Depends:** R3 (a rest-pose RT shadow that is already alpha-correct, so this step's only variable is
  the pose).
- **Goal:** Make RT shadows track the wind sway, i.e. put the deformed geometry into the BVH. Without
  this every plant casts the shadow of its rest pose: the trunk survives (bend ≈ 0 at the base) but
  frond shadows stand still under fronds that visibly move — a hard blocker for the atoll.
- **Touch:** `sources/rendering/rt/AccelerationStructure.{h,cpp}` (per-instance BLAS keyed by
  (mesh, instance) rather than `Mesh*`, `ALLOW_UPDATE` build flag, a refit entry point); a new compute
  deform pass writing per-instance deformed position buffers; `SceneRenderer.cpp` AS gather (mark
  wind casters, drive the deform + refit); `shaders/wind.hlsli` (reused verbatim — see the invariant
  below).
- **Why this is not "just refit the TLAS":** sway is a per-vertex displacement, not a transform. The
  TLAS holds transforms and is already refit-capable; it can never represent this. The work is
  entirely at BLAS level.
- **Implement:**
  - A compute pass evaluates `WindOffset()` per vertex into a per-instance deformed position buffer.
    **The shader must include `shaders/wind.hlsli` and call `WindOffset` with identical arguments** —
    the same invariant the gbuffer VS and the depth-only shadow VS already obey. A re-implementation
    drifts and the RT shadow detaches from the tree exactly the way the raster shadow would.
  - Build those instances' BLASes with `ALLOW_UPDATE` and `PERFORM_UPDATE` them per frame from the
    deformed buffer. Keep the shared static per-`Mesh*` BLAS cache for everything that does not sway —
    only wind casters pay.
  - Round-robin a full rebuild across wind instances (N per frame). Frond tips move on the order of a
    metre, which is a large deformation relative to the mesh; pure refit degrades traversal quality.
  - Budget controls, all three expected to be needed:
    - **Deform only near casters.** Distant plants keep the shared rest-pose BLAS — their shadow sway
      is sub-pixel. A distance/screen-size threshold, mirroring the shadow LOD selection.
    - **Deform a shadow LOD.** The palms ship 3-4 LODs with identical UVs, so the alpha cutout still
      works; LOD 1-2 cuts the refit triangle count 4-8×.
    - **Amortize.** `swayFrequency` is ~1 rad/s; a one-frame-stale pose is invisible at these frame
      rates, so refitting every other frame is free quality-wise.
- **Interface contract:** a wind caster's RT shadow matches its rasterized silhouette within one
  frame of sway; non-wind casters keep the shared cached BLAS and are byte-identical to R3; with the
  deform disabled the behaviour degrades to the R3 rest-pose result, never to a missing shadow.
- **Cost model — naive per-instance refit DOES NOT SCALE (recomputed 2026-07-23 on the real
  `wind_test` grove: 610 palms, 222 coconut / 217 date / 171 curly):**

  | Item | Quantity | Estimate |
  |---|---|---|
  | Deform compute | 610 plants = **3.29M verts** | ~0.1-0.2 ms (fine) |
  | BLAS refit @LOD0 | **4.40M tris**, **610 separate refit calls** | **~2-5 ms** + per-call overhead |
  | BLAS + deformed-VB memory | | **~150-250 MB** + ~40 MB |

  That is **worse than the ~1.5 ms of VSM it is meant to replace**, and it grows linearly with plant
  count. The earlier version of this model was written for 100 palms and is wrong at grove scale. The
  budget controls below are therefore **mandatory, not optional** — an unbudgeted RW is a regression.

- **Phase bucketing is the load-bearing idea (do this first, not last).** The refit bill is a function
  of how much geometry is *deformed*, which is a design choice, not a scene property. The per-plant
  wind phase is `dot(worldOrigin.xz, float2(0.13, 0.17))` (`shaders/wind.hlsli`) — continuous, hence
  610 unique poses. But a grove does not need 610 distinct phases: **quantize the phase into N buckets
  so a deformed BLAS is keyed by (mesh, phaseBucket), not by instance.** 3 mesh types × 12 buckets =
  **36 BLASes instead of 610**. Combine with a coarse BLAS LOD (the same lever as the shadow LOD work
  — note `models/` LOD chains vary: coconut 6630→792 is 8.4×, date 9941→4507 only 2.2×) and near-only
  exact deform:

  | Tier | Treatment | Rough cost |
  |---|---|---|
  | Nearest ~20 plants | own deformed BLAS, exact phase, LOD 1-2 | ~40k tris |
  | Mid | bucketed BLAS (mesh, phase), coarse LOD | ~70k tris |
  | Far | shared rest-pose BLAS, no deform | 0 |

  ≈ **0.1 ms and a few MB** — and, the property that actually matters, **capped**: adding 2000 more
  distant plants adds *nothing* to refit, whereas the VSM per-page cull it replaces grows linearly
  with caster count (measured in §11).

- **Bucketing caveat that must be honoured:** the gbuffer draws each plant at its *exact* phase while
  a bucketed shadow uses the bucket's phase, so the shadow desynchronises from its tree by up to half
  a bucket (≈15° at 12 buckets ≈ 0.3 m of crown displacement at `foliageSwayMeters` 1.1). Invisible at
  distance, visible up close — which is exactly why the near tier keeps an exact per-instance BLAS.
  Bucket assignment should also be spatially dithered so two adjacent plants rarely share a bucket.
  If the plant's world rotation varies, either fold yaw into the bucket key or deform in a canonical
  frame — otherwise a shared pose leans the wrong way relative to world wind.

  If the measured refit cost lands far above this model, report it — it changes the RT-vs-VSM verdict
  and is the one number in this plan most likely to be wrong.
- **Done-when:** palm frond shadows sway in lockstep with the fronds; no per-frame popping from the
  round-robin rebuild; the cost sits inside the budget established in §4.1 (the whole RT sun path,
  including RW, must stay under the 1.55 ms of VSM passes it replaces).
- **Verify:** atoll under wind, camera static, side-by-side raster vs shadow; a high-`swayFrequency`
  stress (the wind system's own DLSS smear test setting) to expose staleness; GPU validation over the
  new AS lifetime; `--scene-stress` for BLAS churn on level switch.

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
  R2. **Three gaps are already known** (§4.2): forced-opaque geometry flags, static per-mesh BLASes,
  and vertex-animated casters absent from the BVH entirely.
- **Vertex animation is invisible to RT (the big one):** any future GPU-side vertex work — wind,
  skinning, GPU-displaced water, morphs — does not exist in the BVH unless a deform+refit path feeds
  it. RW covers wind; anything added later must either join that path or be documented as
  RT-shadow-exempt. A silently rest-pose shadow is worse than no shadow because it looks correct in a
  screenshot and wrong only in motion — exactly the failure mode screenshots do not catch.
- **Per-instance BLAS memory/CPU blowup (measured, not hypothetical):** deformation kills BLAS sharing
  (unique sway phase per plant). On the real 610-palm grove the naive path is 4.40M tris / 610 refit
  calls / ~150-250 MB ≈ 2-5 ms — *worse than the VSM it replaces*. Phase bucketing + coarse BLAS LOD +
  near-only exact deform (step RW) are mandatory. Cap the deformed-caster set explicitly; never let it
  be unbounded, and re-measure whenever the cap changes.
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

R0 -> R1 -> R2 -> R3 -> RW -> R4 gives the shortest route to a user-visible, correct RT **sun** shadow
that removes the artifact classes this codebase has fought. Then R5 (soft) and R6 (locals, the
overlapping-light memory win), R7 hybrid/measurement, and R8 parity/fallback/sign-off. Do not remove
or regress the shadow-map path at any point — RT is added beside it and defaults off until M3.

**Measure at two gates before committing to the remaining scope.** The whole plan rests on an
estimate — that ~0.7 Mpixel of rays beats 4.6 Mtexel of foliage rasterization — and two steps can
falsify it cheaply:

- **After R2** (rest-pose, opaque): the shadows are wrong on purpose (solid crowns, no sway), but the
  pass timing is the honest cost of a sun ray in this scene. Compare against the §4.1 baseline.
- **After R3** (alpha-tested rays): this is where the estimate is most likely to break. Foliage
  any-hit is the dominant per-ray cost and the atoll is nothing but foliage. If R3 lands far above
  budget, stop and reconsider before paying for RW.

Do not skip straight to RW to "see the trees sway" — it is the most expensive step and the least
informative about whether RT is the right answer.

## 11. Relationship to the shadow-map work (measured 2026-07-23 — do not double-invest blindly)

RT is not the only lever on the §4.1 number, and the competing VSM work is cheaper per unit of effort.
The two are not mutually exclusive — locals will keep the shadow-map path for a long time, so VSM
improvements are not wasted if RT wins for the sun — but the *ordering* matters. A headless sweep on
`wind_test.json` (10 clustered palms, camera under them; `Pass_VsmPageRender` GPU, healthy-clock
runs) pins down what actually moves it:

| lever | result | verdict |
|---|---|---|
| `clipmapBaseExtent` 12 → 24 → 36 | **1.60 → 1.18 → 0.86 ms** | the real lever; render clean at 24 (screenshot-verified) |
| `g_residentIterOnly` (skip free pages) | 1.63 vs 1.60 ms | **no effect** — measured, not theorized |
| `Pass_VsmPageRequest` | 0.05–0.16 ms in every config | not worth touching |

### What `Pass_VsmPageRender` is actually made of (610-palm grove, camera inside it, 2026-07-23)

Once the grove got dense (610 palms) the earlier levers saturated, so the pass was split with a
`VsmPageRender.Setup` GPU sub-scope (the per-page cull dispatch) to see where the time really goes:

| config | PageRender | Setup (per-page cull) | rasterization |
|---|---|---|---|
| shadow LOD bias 0 (default) | 1.50 ms | 0.40 | **1.10** |
| bias +3 (coarsest geometry) | 1.27 ms | 0.47 | **0.79** |
| `clipmapBaseExtent` 24 | 1.28 ms | 0.43 | 0.84 |
| `g_residentIterOnly` on | 1.51 ms | 0.41 | 1.10 (CPU 0.27→0.13) |

Three conclusions, each of which kills a candidate optimization:

1. **The geometry lever is exhausted.** Coarsening every caster to its coarsest LOD removes only
   ~0.3 ms of the 1.10 ms raster. Alpha-tested frond *fill* does not shrink with LOD — the cards cover
   the same shadow texels at any triangle count. The remaining ~0.79 ms is LOD-immune by construction.
2. **The per-page cull is O(pages × casters) and grows linearly with plant count** — 1024 pages × 610
   objects × 2 passes ≈ 1.25M AABB tests for 0.40 ms. It is the term that will dominate as the scene
   grows, and it is unmoved by LOD (as expected).
3. **Free pages are not a GPU cost.** `g_residentIterOnly` is a GPU no-op (1.51 vs 1.50) and only
   halves the *CPU* submission (0.27→0.13 ms). The GPU time is resident pages doing real work.

So the VSM path is near the ceiling of "for every page, test every caster, then issue a draw per
group": both remaining terms are fixed relative to how much geometry actually lands in a page. The
structural moves left are (a) invert/scatter the cull so casters write into the pages they cover
instead of every page testing everything, and (b) attack alpha-test fill (fewer pages via extent, or
a cheaper masked shadow PS). This is also precisely the regime where RT wins structurally: ray cost
scales with screen pixels, not with caster count or crown overlap — provided RW stays budgeted.

The measured facts that redirect effort:

- **The per-page CPU loop is NOT the bottleneck.** `RecordPageRender` iterates all `kPoolPageCount`
  = 1024 pool pages issuing a viewport + scissor + root CBV + `ExecuteIndirect(maxCount=groups)` per
  page while only ~281 are resident, so a compacted resident-page list looks tempting — but
  `g_residentIterOnly`, which does exactly that skip, changes the GPU time by ~2 % (noise). The empty
  pages' zero-instance `ExecuteIndirect`s are effectively free on the GPU. **Do not build the
  resident-list compaction; it was measured to do nothing here.** The cost is the real rasterization
  of the resident pages — dense alpha-tested foliage.
- **So the two levers that move it are page COUNT and geometry-PER-page**, not loop overhead:
  - **Per-view shadow LOD** — each shadow view (CSM cascade / VSM clipmap level / local light) picks a
    base mesh LOD from its tier (near = fine, far = coarse; `render::ShadowTierBaseLod`), plus an
    additive `render::g_shadowLodBias` ("Shadow LOD bias" dev-window slider, default **0**). The
    GPU-driven caster mega-buffer holds ALL LODs per mesh; the VSM setup shader picks each page's LOD
    from its view, and the Legacy per-view path binds per-cascade. **Implemented + measured 2026-07-23:**
    wind_test `Pass_VsmPageRender` GPU, default aggressive curve (bias 0) **1.29 → 0.51 ms (~2.5×)** vs
    near-sharp; slider spans 1.29 ms (bias −2, near ≈ LOD0) → 0.31 ms (bias +3, max coarse), a 4.1×
    range, all screenshot-clean (shadows don't resolve fine geometry; visible meshes keep their camera
    LOD, only casters coarsen). A bias change triggers a GPU-idle caster rebuild
    (Scene::ReconcileShadowLodBias). The single biggest win found, at no visible cost.
  - `clipmapBaseExtent` up (fewer, coarser pages): ~1/extent^0.6 here, palm shadows still read clean
    at extent 24. Re-tune `clipmapNormalBias` down when raising it — the two are coupled through
    `VsmClipmapShadow` (§4.1), which is why the extent dial *feels* like a bigger quality loss than
    the texel-size change alone. Stacks with the LOD bias.

Run the extent re-tune first if the goal is "the atoll frame is too slow today" — it is one slider and
it is already known to work. Run this plan if the goal is "remove the shadow-map artifact classes and
stop trading texel size against cost" — that is what the extent dial keeps forcing. Both are valid and
different goals; do not confuse them in a perf review.

> Measurement caveat: this dev machine downclocks its GPU aggressively when apps are launched
> back-to-back (a run that sustains ~1500 frames/14 s instead of ~5000 is throttled; its absolute ms
> are 2-4× inflated). The numbers above are from healthy-clock runs (>2500 frames), and the *ordering*
> held in every run regardless of throttle. Space perf runs out (cooldown) or trust only the
> high-frame-count samples. The harness (`--profdump`, `--vsm-extent`, `--vsm-resident`) is uncommitted
> temporary instrumentation.
