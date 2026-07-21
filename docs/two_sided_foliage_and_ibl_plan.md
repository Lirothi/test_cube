# Two-Sided Foliage and Environment Specular — Implementation Plan (for AI executors)

This is an execution plan for adding a real thin-foliage shading model and fixing the
environment-specular path that currently makes rough palm leaves look silver/blue. It is written so
that an AI agent can pick up one **F-step**, implement it against a declared interface contract, and
verify it before the next step starts. Keep every landed step buildable and visually testable.

---

## 1. Goal and non-goals

**Goal:** Add a `TwoSidedFoliage` material shading model with wrap/transmission lighting while
preserving a visible direct-light specular highlight, and replace the leaking skybox fallback with a
controllable, physically coherent indirect-specular path.

The target result on `data/levels/atoll_a2_test.json` is:

- palm fronds receive plausible light on both visible sides;
- sunlight behind a leaf produces tinted transmission controlled by `subsurfaceColor`;
- the direct GGX sun highlight remains visible;
- disabling SSR and RT does not turn foliage into bright sky-colored cards;
- roughness controls reflection width without requiring foliage roughness to be forced to 1;
- DefaultLit, metals, emissive materials, shadows, instancing, and RT reflections do not regress.

**Non-goals for the first visible milestone:** full volumetric subsurface scattering, measured leaf
thickness, spectral transmission, multiple scattering through an entire canopy, Lumen-style GI,
reflection probes, or a general-purpose material graph. The initial foliage model is a thin-sheet
wrap/transmission approximation, matching the intent of Unreal's legacy Two Sided Foliage model.

---

## 2. Diagnosis — the reflection problem is not the two-sided BRDF

The current palm problem has two independent parts:

1. `twoSided` only disables back-face culling and flips the visible back-face normal in
   `shaders/gbuffer.hlsl`. There is no shading-model ID, subsurface payload, wrap lighting, or
   transmission. Foliage is otherwise shaded as ordinary DefaultLit GGX.
2. `ReflectionSource::Off` means **skybox specular only**, not "no reflections." The reflection target
   is cleared, then `shaders/compose_cs.hlsl` fills uncovered reflection with the sky cubemap and uses:

   ```hlsl
   spec = sky * FresnelSchlick(NoV, F0) * (1.0 - roughness);
   ```

   At a grazing angle Schlick Fresnel approaches 1. A leaf at roughness 0.6 can therefore receive up
   to 40% of the HDR sky radiance. Thin cards produce many grazing pixels, which explains the flowing
   silver/blue appearance when SSR/RT are disabled.

Verified foliage texture data (`models/coconut_palm/textures/Tree_2Mat_metallicRoughness.dds`):

- metallic is effectively zero;
- roughness median is approximately 0.541;
- roughness p90 is approximately 0.60.

This is not an accidental-metal content bug. The runtime is rendering a dielectric leaf with a strong,
unoccluded sky fallback.

The sky importer (`sources/assets/AssetImporter.cpp`, `ConvertSkyboxHdr`) currently calls ordinary
`GenerateMipMaps`. Those mips are image downsampling, not GGX prefiltering. Compose also has no BRDF
integration LUT, material specular/indirect-specular control, AO-based specular occlusion, bent normal,
or local reflection visibility.

**Key decision:** implement TwoSidedFoliage for the direct/body lighting, but fix indirect sky specular
separately. Adding only a foliage transmission lobe will not remove the sky leak.

---

## 3. Reference model and decisions locked for implementation

Unreal's public documentation describes Two Sided Foliage as a thin-surface model that transmits light
through the surface, with `Subsurface Color` controlling the transmitted tint/masks. Unreal Substrate
calls the legacy equivalent **Two-Sided Wrap**. It still exposes Roughness and Specular; the shading
model does not simply delete specular reflection.

Implementation decisions for this renderer:

1. **Separate reflection source from sky fallback.** Add real `None` and retain the old behavior as
   `SkyOnly`.
2. **Shading model is material-static.** JSON and the Material Editor select `DefaultLit` or
   `TwoSidedFoliage`; it is part of the slot PSO identity.
3. **Use a four-bit shading-model ID in `GBAux.b`.** Keep `GB1` as `R10G10B10A2_UNORM` so normal
   precision is unchanged. Encode IDs 0..15 as `id / 15` in the eight-bit blue channel.
4. **Use model-specific `GB2.rgb`.** DefaultLit stores emissive; TwoSidedFoliage stores its
   premultiplied subsurface/transmission color. Simultaneous foliage emissive is an accepted v1
   limitation and must be documented in the editor.
5. **Use one consolidated material auxiliary GBuffer.** `R8G8B8A8_UNORM`: R = material AO,
   G = indirect-specular scale, B = four-bit shading-model ID, A = reserved. In editor builds it is
   RT5 after the object-ID target; in runtime builds the editor-only target is absent and GBAux is RT4.
6. **Do not silently grow `render::InstancePerObject`.** It is currently 208 bytes and mirrored by
   GBuffer and shadow paths. Material-static foliage parameters should travel through a dedicated
   per-slot surface-parameter CB, or through the existing multi-slot CB where that already occupies
   `b2`. If the executor chooses a different transport, it must preserve every C++/HLSL mirror and
   prove shadow/instancing parity in the same step.
7. **Keep direct and indirect specular independently controllable.** `indirectSpecularScale` affects
   sky/SSR/RT reflection composition, not analytic sun/point/spot GGX.
8. **Correct IBL is a later, independent milestone.** The targeted foliage control lands first so the
   scene can be fixed and validated before the importer/runtime IBL conversion.

Official references:

- <https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine>
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-substrate-materials-in-unreal-engine>
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/reflections-captures-in-unreal-engine>
- <https://dev.epicgames.com/documentation/unreal-engine/distance-field-ambient-occlusion-in-unreal-engine>
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/composite-texture>

---

## 4. Current engine state (verified 2026-07-21)

- Material files are flat schema-v2 JSON objects in `data/materials/*.json`, parsed by
  `MaterialDataManager.cpp::ParseMaterialPresetJson` and edited by `MaterialEditorPanel`.
- `MaterialData::ConfigureDefinesForGBuffer` already adds per-material sampling defines and is the
  correct place to add `SHADING_MODEL_ID`.
- Slot PSOs already differ for alpha-test and two-sided culling in
  `GBufferRenderable::ApplySlotPipelineOverrides`; auto-instancing groups use slot-specific PSOs.
- `shaders/gbuffer_common.hlsl::FinalizeGBuffer` is shared by per-object, instanced, and instanced-CB
  GBuffer shaders. It writes:
  - RT0: albedo + packed roughness/metal;
  - RT1: world normal + constant alpha 1;
  - RT2: emissive;
  - RT3: motion;
  - editor: RT4 object ID, RT5 GBAux;
  - runtime: RT4 GBAux.
- Deferred formats are declared in `sources/rendering/core/RenderConstants.h`; targets/descriptors are
  owned by `RenderTargetManager`. Object-ID allocation, readback, render-graph states, and MRT slots are
  compiled only with `WITH_EDITOR`; runtime binds five GBuffer MRTs while editor binds six.
- Directional, point, and spot light compute shaders all use `EvalBRDF` from `shaders/utils.hlsl`.
  Directional ambient is a flat `albedo * (1-metal) * ambientIntensity` term.
- Compose blends premultiplied SSR/RT coverage over the sky fallback:
  `reflection.rgb + sky * (1-reflection.a)`.
- `ReflectionSource` lives in `sources/app/scene/SceneFrameData.h`; cycling is in
  `sources/app/AppController.cpp`; labels/settings are in `sources/app/ui/DeveloperWindow.cpp`.
- Sky HDR import already produces a cubemap DDS with a full ordinary mip chain. `TextureCube` can load
  cubemap mip chains, so the resource loader is usable for a GGX-prefiltered derivative.
- Shaders compile at runtime. A successful C++ build is not sufficient; every step touching HLSL must
  run a scene that exercises the changed pass.
- The executable supports screenshot capture:
  `test_cube.exe --level=<level> --shot=<out.png> --shot-delay=<seconds>`.

---

## 5. Executor conventions

- **One logical F-step per commit**, but only create commits when the human explicitly asks.
- Build from PowerShell with MSBuild. At minimum build Debug x64 after every step; build Release and
  `Release_Editor` at milestone boundaries.
- C++/HLSL/project files use CRLF. Markdown uses LF. Verify touched files have no mixed endings.
- Preserve unrelated worktree changes. Do not rewrite palm material files before F12.
- Treat `MaterialData` replacement/live apply as a descriptor-lifetime hazard. After material-editor
  tests, move the camera and exercise VSM so stale CPU SRV handles are caught.
- Keep defaults neutral:
  - DefaultLit;
  - AO = 1;
  - indirect specular scale = 1;
  - transmission strength = 0.
- Every dormant/plumbing step must be visually byte-equivalent or screenshot-equivalent to its
  baseline. Do not combine plumbing and the visual flip.
- Use `data/levels/atoll_a2_test.json` for foliage checks. Also test at least one smooth metal,
  rough dielectric, emissive object, and a multi-slot/instanced mesh for regression coverage.
- Run with the D3D12 debug layer/GPU validation for descriptor, root-signature, MRT-format, and resource
  state changes.

---

## 6. Material and GBuffer contracts

### Material JSON (final schema)

```json
{
  "shadingModel": "twoSidedFoliage",
  "twoSided": true,
  "subsurfaceColor": [0.22, 0.50, 0.10],
  "transmissionStrength": 0.65,
  "indirectSpecularScale": 0.35,
  "ambientOcclusion": 1.0
}
```

Backward-compatible defaults:

```text
shadingModel           = DefaultLit
subsurfaceColor        = (1,1,1)
transmissionStrength   = 0
indirectSpecularScale  = 1
ambientOcclusion       = 1
```

`TwoSidedFoliage` implies `twoSided=true`. The editor may keep the explicit JSON key for clarity, but
must not produce a foliage material whose rasterizer culls the back face.

### Shading-model encoding

```text
0 = DefaultLit
1 = TwoSidedFoliage
2..15 = reserved
```

HLSL contract:

```hlsl
float EncodeShadingModel(uint id) { return float(id & 15u) / 15.0f; }
uint  DecodeShadingModel(float b) { return (uint)round(saturate(b) * 15.0f); }
```

### Deferred payload

```text
GB0  R8G8B8A8_UNORM   albedo.rgb, packed roughness/metal in a (unchanged)
GB1  R10G10B10A2      normal.rgb, unused a
GB2  R11G11B10_FLOAT  DefaultLit: emissive; Foliage: subsurfaceColor*transmissionStrength
GBAux R8G8B8A8_UNORM  materialAO, indirectSpecularScale, shadingModelId/15, reserved
```

Do not steal bits from packed roughness/metal in GB0. Do not change normal precision in GB1.

---

## 7. Dependency graph and milestones

```text
F0 baseline ─┬─> F1 real None/SkyOnly ───────────────┐
             └─> F2 shading-model plumbing ─> F3 payload/MRT ─> F4 sun foliage ─> F5 local/ambient
                                                   └────────────> F6 indirect foliage control

F7 offline GGX assets ─> F8 runtime split-sum IBL ─> F9 specular occlusion
F7 ────────────────────────────────────────────────> F10 normal-variance roughness mips

F5 + F6 + F8 + F9 ─> F11 RT/instancing/shadow parity ─> F12 palm migration and sign-off
```

`F1`, `F2`, and the design/prototype portion of `F7` may start independently. Serialize changes that
touch `AssetImporter.cpp` (`F7` and `F10`) to avoid a merge conflict.

Milestones:

- **M0 — diagnostic control:** F0-F1. A true reflection-off mode proves the source of the artifact.
- **M1 — useful foliage:** F2-F6. Two-sided transmission plus foliage indirect-specular control; the
  atoll should already be artistically usable.
- **M2 — correct environment foundation:** F7-F10. GGX IBL, occlusion, and stable roughness mips.
- **M3 — production parity:** F11-F12. All renderer paths agree and the palm content is migrated.

---

## 8. Step template

Each step below declares **Depends**, **Goal**, **Touch**, **Implement**, **Interface contract**,
**Done-when**, and **Verify**. If a step cannot meet its acceptance without pulling work from a later
step, stop and report the dependency instead of silently widening the change.

---

### F0 — Baseline capture and source isolation — DONE (2026-07-21)

- **Depends:** none.
- **Goal:** Establish a reproducible comparison and prove that the artifact comes from sky specular.
- **Touch:** no permanent source changes; optional temporary local shader edit must be reverted.
- **Implement:**
  - Use one saved camera in `atoll_a2_test` close to the central palm, including top-facing and
    grazing fronds.
  - Capture current `Off`, SSR, and RT results.
  - Record sky exposure, directional ambient/exposure, reflection resolution, and glossy blur.
  - Temporarily zero only the compose sky-specular contribution while leaving the sky background and
    direct lights active; capture the result, then revert the probe.
- **Interface contract:** store capture names/settings in the implementation report; no code contract.
- **Done-when:** the no-sky-specular capture removes the silver/blue wash while direct sun specular
  remains visible.
- **Verify:** fixed-camera PNG comparison; Debug build unchanged after reverting the probe.

**Result:** The supplied atoll capture plus code-path inspection isolated the sky fallback: old `Off`
cleared traced/screen reflections but compose still injected the cubemap. A pre-change RT screenshot was
captured to `C:\tmp\foliage_F0_before_rt.png`; the leaf MR texture was also checked numerically (metallic
effectively 0, roughness median ~0.541, p90 ~0.60), ruling out accidental metallic content.

---

### F1 — Split reflection `None` from `SkyOnly` — DONE (2026-07-21)

- **Depends:** F0.
- **Goal:** Make reflection source names truthful and provide a permanent diagnostic/artist control.
- **Touch:** `sources/app/scene/SceneFrameData.h`, `sources/app/AppController.cpp`,
  `sources/app/ui/DeveloperWindow.cpp`, compose CB plumbing in `SceneResourceBootstrapper.*` and
  `SceneRenderer.cpp`, `shaders/compose_cs.hlsl`, and the independent glass fallback in
  `shaders/glass.hlsl`/`BuildGlassViewCB`.
- **Implement:**
  - Replace old `Off` semantics with explicit `None`, `SkyOnly`, `SSR`, `RT` values.
  - Preserve RT-to-SSR fallback on hardware without RT support.
  - Add a compose CB flag that gates **surface sky specular only**. Do not suppress the rendered sky
    background or alter sky exposure.
  - `None`: clear/ignore the screen/RT reflection buffer and skip sky fallback.
  - `SkyOnly`: old `Off` behavior.
  - Apply the same distinction to forward glass: `None` keeps refraction/direct light but suppresses
    the cubemap; `SkyOnly` keeps the old cubemap-only glass reflection.
  - Update input cycling order and developer-window labels.
- **Interface contract:** `SceneRenderSettings::reflectionSource` exposes all four states; compose has
  an explicit `enableSkySpecular`/equivalent field.
- **Done-when:** `None` shows the sky background and direct highlights but no indirect reflection;
  `SkyOnly` matches the old `Off` capture.
- **Verify:** fixed-camera screenshots in all four modes; cycle action wraps correctly; runtime shader
  compilation clean.

**Result:** Added `None`, `SkyOnly`, `SSR`, and `RT`; F5 cycles them in that order and skips RT on
unsupported hardware. Compose carries an explicit `enableSkySpecular` flag. Forward glass uses an
independent sky-fallback flag, so `None` preserves refraction/direct light without a cubemap while
`SkyOnly` retains old behavior. Automated hidden-window captures exercised all four modes on
`atoll_a2_test` and compared `None`/`SkyOnly` on `demo_orig` (including glass). Debug, Release, and
Release_Editor builds passed; every capture exited 0.

---

### F2 — Shading-model identity plumbing (dormant) — DONE (2026-07-21)

- **Depends:** F0.
- **Goal:** Carry a material shading model through JSON, PSO identity, and GBAux without changing light.
- **Touch:** `MaterialData.h/.cpp`, `MaterialDataManager.h/.cpp`, `MaterialEditorPanel.*`,
  `GBufferRenderable.cpp`, `shaders/gbuffer_common.hlsl`, all GBuffer variants, render-target lifecycle,
  shared HLSL decode helpers, texture-debug UI.
- **Implement:**
  - Add the C++ `ShadingModel` enum and strict string parse/serialize helpers.
  - Unknown/missing strings fall back to DefaultLit with a warning for unknown values.
  - Add a Material Editor combo. Selecting foliage enables `twoSided`; attempting to disable it either
    re-enables it or presents a clear validation error.
  - `MaterialData::ConfigureDefinesForGBuffer` writes `SHADING_MODEL_ID` into the slot PSO desc.
  - Append neutral `GBAux` (`R8G8B8A8_UNORM`). It is RT5 with editor object picking and compacted to
    RT4 when `WITH_EDITOR` is disabled.
  - `FinalizeGBuffer` writes the encoded compile-time ID to `GBAux.b`; default macro value is 0.
  - Preserve GB1 RGB10 normal precision; GB1 alpha is not used for material identity.
  - Add a debug visualization for decoded IDs.
  - Lighting and compose may decode the ID in this step, but must not branch on it yet.
- **Interface contract:** C++ enum values and HLSL IDs are exactly the table in Section 6.
- **Done-when:** old materials are ID 0, a test foliage material is ID 1, and the lit result is unchanged.
- **Verify:** Debug + `Release_Editor`; Material Editor save/reload/live-apply; per-object and instanced
  GBuffer paths; debug-ID screenshot.

**Result:** Added the exact `DefaultLit=0` / `TwoSidedFoliage=1` C++ contract within a four-bit
0..15 HLSL encoding, strict JSON string helpers, and PSO identity via `SHADING_MODEL_ID`. The ID lives
in `GBAux.b` as `id/15`; GB1 remains RGB10 and no normal precision is lost. `GBAux` is a consolidated
`R8G8B8A8_UNORM` target initialized to neutral AO/specular values `(1,1,0,0)`. Editor builds keep it at
RT5 after object ID; runtime builds omit ObjectID entirely and bind GBAux at RT4.
The Material Editor exposes both models and makes `twoSided` mandatory for foliage; new and generated
materials serialize `defaultLit`, while omitted legacy fields remain ID 0. Texture Inspector now has a
dedicated `Shading Model ID` view. A temporary isolated atoll fixture rendered foliage as ID 1 beside
legacy ID 0 surfaces in `C:\tmp\foliage_F2_4bit_shading_model_id.png`; the fixture was then removed without
changing the palm materials or level. Debug and `Release_Editor` builds passed, final runtime shader
compilation and `--scene-stress=5` exited 0, and lighting/compose still ignore the new ID.

---

### F3 — Surface payload CB and `GBAux` MRT (neutral plumbing)

- **Depends:** F2.
- **Goal:** Carry foliage data and indirect controls without changing the default rendered result.
- **Touch:** material preset/data/editor files; `shaders/gbuffer_common.hlsl`, `gbuffer.hlsl`,
  `gbuffer_inst.hlsl`, `gbuffer_instcb.hlsl`; `InstanceTypes.h` only if unavoidable;
  `RenderConstants.h`, `RenderTargetManager.*`, `Renderer.*`, `GBufferRenderable.cpp`, render-graph
  declarations, `TextureDebugViewer.*`, directional/local/compose descriptor tables.
- **Implement:**
  - Parse/edit/store `subsurfaceColor`, `transmissionStrength`, `indirectSpecularScale`, and scalar
    `ambientOcclusion` with the neutral defaults from Section 6.
  - Add a material-static GPU surface-parameter block. Use free `b2` on ordinary/single-slot paths;
    extend the existing `b2` slot block for the multi-slot instanced variant. Keep binding contracts
    explicit per shader variant.
  - Do **not** grow the shared 208-byte `render::InstancePerObject` as a shortcut. If transport design
    forces a growth, update all C++ and HLSL mirrors, shadow structured-buffer strides, upload sizes,
    and static asserts in this same step and explain why the dedicated CB was impossible.
  - Reuse the neutral `GBAux` target landed in F2; populate R/G from the new material parameters while
    preserving B shading-model identity and reserved A.
  - Keep AO=1 and indirectSpecularScale=1 for all old materials.
  - DefaultLit writes emissive to GB2. Foliage writes
    `subsurfaceColor * transmissionStrength` to GB2.
  - Compose must stop adding GB2 as emissive when shading model is foliage, but no new foliage light is
    added yet.
  - Append new descriptors to pass tables instead of renumbering established registers where practical.
- **Interface contract:** exact GBuffer layout from Section 6; neutral old-material output.
- **Done-when:** GBAux debug view reads `(1,1)` on old materials; emissive DefaultLit still works;
  foliage GB2 shows its payload and is not self-emissive.
- **Verify:** Debug + `Release_Editor`; resize/render-scale changes; DLSS on/off; material live-edit then
  camera movement; GPU validation; instanced single-slot and multi-slot rendering.

---

### F4 — Directional-light TwoSidedFoliage (first visual flip)

- **Depends:** F3.
- **Goal:** Add thin-sheet front diffuse, back transmission, and unchanged direct GGX sun specular.
- **Touch:** `shaders/utils.hlsl` (or new `shaders/foliage_lighting.hlsli`),
  `shaders/lighting_cs.hlsl`.
- **Implement:**
  - Keep `EvalBRDF` unchanged for DefaultLit.
  - Add a foliage helper that receives albedo, roughness, metallic, visible-side normal, V/L,
    subsurface payload, and shadow visibility.
  - Front side: normal dielectric diffuse + the existing GGX direct specular.
  - Back side: a wrapped response based on `-dot(N,L)`, tinted by GB2 subsurface payload. Start with a
    documented constant wrap width; do not claim exact UE private constants.
  - Front and back diffuse/transmission should not double-count at full strength. Keep their weights
    bounded and expose only physically useful material controls.
  - Reuse the directional shadow visibility initially. If leaf self-shadow erases all transmission,
    add a narrowly scoped transmission shadow bias rather than disabling shadows.
  - Preserve the normal-facing convention from `SV_IsFrontFace`; do not flip the normal a second time
    in compute lighting.
- **Interface contract:** helper returns separate diffuse/transmission/specular terms so local lights and
  RT hit shading can reuse the same math.
- **Done-when:** front-lit foliage remains stable; a back-lit leaf gains tinted transmission; direct sun
  highlight remains visible; DefaultLit is unchanged.
- **Verify:** fixed-camera front/back-sun captures, Legacy CSM and VSM A/B, shadow-debug validation.

---

### F5 — Point/spot and environment-diffuse foliage parity

- **Depends:** F4.
- **Goal:** Remove shading-model differences between light types and avoid black undersides in ambient.
- **Touch:** `shaders/pointlight_cs.hlsl`, `shaders/spotlight_cs.hlsl`, shared foliage helper,
  `shaders/lighting_cs.hlsl` ambient branch.
- **Implement:**
  - Reuse the F4 helper for point and spot lights; do not duplicate a slightly different BRDF.
  - Apply each light's attenuation and shadow visibility to transmission.
  - For the existing flat ambient, add a conservative two-sided foliage response so the visible back
    side does not go black. Do not turn ambient into emissive.
  - Defer irradiance-cubemap ambient to F8; this step must work with the current renderer.
- **Interface contract:** all analytic light shaders select the same shading-model ID and helper.
- **Done-when:** directional/point/spot front and back lighting agree; light radius/cone falloff affects
  transmission correctly; no DefaultLit delta.
- **Verify:** small test arrangement or editor-created point/spot lights around the palm; shadows on/off;
  GPU validation and runtime shader compile.

---

### F6 — Per-material indirect-specular control (targeted scene fix)

- **Depends:** F1, F3.
- **Goal:** Stop sky/SSR/RT indirect reflection from overpowering foliage without removing direct spec.
- **Touch:** `shaders/compose_cs.hlsl`, compose descriptor staging/root signature, Material Editor labels
  and tooltips if needed.
- **Implement:**
  - Sample `GBAux.g` and multiply the composed indirect reflection by `indirectSpecularScale`.
  - The scale applies after SSR/RT-premultiplied coverage is combined with sky fallback, so the material
    control behaves consistently across `SkyOnly`, SSR, and RT.
  - Do not apply it to LightTarget's analytic specular, foliage transmission, emissive, or diffuse.
  - Clamp serialized/editor input to [0,1] for v1. Default is 1.
- **Interface contract:** `indirectSpecularScale=1` is exact old intensity; 0 removes indirect reflection
  from the receiver while preserving direct light.
- **Done-when:** foliage around 0.25-0.4 loses the sky wash but retains a readable sun highlight;
  DefaultLit at 1 is unchanged.
- **Verify:** one camera across `None`, `SkyOnly`, SSR, RT; direct spec visible at scale 0; compare rough
  metal and dielectric controls.

---

### F7 — Offline GGX sky derivatives and BRDF LUT

- **Depends:** F0; serialize with F10 because both touch the importer.
- **Goal:** Produce the resources required for split-sum IBL without changing runtime shading yet.
- **Touch:** `sources/assets/AssetImporter.cpp/.h`, importer UI/logging if exposed, generated texture
  naming conventions, optional small CPU reference/test utility.
- **Implement:**
  - Preserve the display/radiance cube as `<stem>.dds`.
  - Generate `<stem>_spec.dds`: GGX-prefiltered radiance, one roughness per mip, cubemap seam-safe.
  - Generate `<stem>_diffuse.dds`: low-resolution cosine-convolved irradiance cube.
  - Generate/load a reusable `textures/brdf_lut.dds`, preferably `RG16_FLOAT`, indexed by NoV and
    perceptual roughness.
  - Use deterministic sampling and log face size, mip count, sample count, format, and output paths.
  - Do not label ordinary `GenerateMipMaps` output as prefiltered IBL.
  - Keep BC6H for non-negative HDR cubes with RGBA16F fallback, matching existing importer behavior.
- **Interface contract:** sibling naming above; mip 0 of the specular cube corresponds to roughness 0,
  final mip to roughness 1; loader can discover derivatives from the level's existing skybox path.
- **Done-when:** importer outputs all resources, reloads metadata successfully, and a debug cube/LUT view
  shows finite values without face seams or NaNs.
- **Verify:** import the current HDR source, inspect every cube face/mip, rerun import for deterministic
  output metadata, build Release.

---

### F8 — Runtime split-sum IBL and irradiance ambient

- **Depends:** F1, F7.
- **Goal:** Replace ordinary sky mips and `Fresnel*(1-roughness)` with proper prefiltered specular IBL.
- **Touch:** skybox/resource loading (`TextureCube`, `Skybox`, scene resource bootstrap), compose root
  signature/descriptors/CB, `shaders/compose_cs.hlsl`, directional ambient plumbing and shader.
- **Implement:**
  - Load specular/irradiance siblings and BRDF LUT with graceful fallback to current resources.
  - Expose actual specular cube mip count; remove hard-coded `kSkyRoughMaxMip=5`.
  - Sample GGX-prefiltered radiance by perceptual roughness and evaluate split-sum:

    ```hlsl
    indirectSpecular = prefilteredRadiance * (F0 * brdf.x + brdf.y);
    ```

  - Remove the extra `(1-roughness)` multiplier from the IBL path.
  - Preserve premultiplied reflection coverage:
    `reflection.rgb + skyIBL * (1-reflection.a)`.
  - Preserve `None`: no indirect specular even though the background sky remains visible.
  - Replace flat ambient with irradiance-cube diffuse in a contained substep/commit if the visual delta
    is too large to verify together. Keep metal diffuse gated by `(1-metal)`.
  - Foliage diffuse irradiance uses the same two-sided intent as F5; do not add transmission twice.
- **Interface contract:** old skyboxes without derivatives still load through a clearly logged fallback;
  new skyboxes use the split-sum path.
- **Done-when:** roughness smoothly broadens reflection; high roughness has no sharp horizon; metals keep
  plausible energy; no hard transition between SSR/RT coverage and sky fallback.
- **Verify:** roughness sweep on dielectric and metal spheres, palm capture in all reflection modes,
  skybox derivative missing-file test, runtime shader compilation, GPU validation.

---

### F9 — Material AO and roughness-aware specular occlusion

- **Depends:** F3, F8.
- **Goal:** Reduce indirect-sky leaking in locally occluded areas without suppressing direct highlights.
- **Touch:** material schema/editor for AO source if added, GBuffer sampling/write, `GBAux.r`, shared IBL
  helper/`compose_cs.hlsl`; optional later SSAO/GTAO pass is a separate follow-up.
- **Implement:**
  - First land scalar material AO through `GBAux.r`; default 1.
  - Add a documented roughness/NoV-aware specular-occlusion approximation for sky/capture fallback.
  - Apply material AO to diffuse irradiance separately and conservatively.
  - Do not multiply analytic sun/point/spot specular by ambient occlusion.
  - Do not blindly darken valid RT/SSR hits in v1. Apply the new visibility term to the fallback sky
    first; expansion to all indirect methods requires its own A/B.
  - If an AO texture is added, prefer an explicit, backward-compatible packing contract. Existing MR
    DDS files have no valid AO contract in B, so never interpret B as AO without a material/import flag.
- **Interface contract:** AO=1 is neutral; AO=0 removes fallback skylight visibility but does not remove
  direct lighting.
- **Done-when:** closed canopy/intersection regions leak less sky, open silhouette leaves are not crushed,
  and there are no screen-space halos.
- **Verify:** AO sweep captures, sun-only versus sky-only comparison, SSR/RT coverage edges, DefaultLit
  cavity material regression.

---

### F10 — Normal-variance roughness mip generation (specular AA)

- **Depends:** F7 for sequencing only; the algorithm is otherwise independent.
- **Goal:** Stop distant foliage normal detail from turning into shimmering/swimming specular.
- **Touch:** `sources/assets/AssetImporter.cpp`, texture-set/MR import helpers, importer logs/tests;
  regenerated palm normal/MR DDS files only after explicit content approval.
- **Implement:**
  - During paired normal/MR mip generation, measure filtered normal-vector length/variance per mip.
  - Convert that variance into additional GGX roughness using a documented Toksvig-style or equivalent
    formulation.
  - Mip 0 remains content-authored. Corrected roughness must never be below source roughness.
  - Preserve engine MR layout: R=metal, G=roughness. Do not corrupt alpha-test coverage or color-space
    treatment.
  - Make the behavior opt-in/versioned at first so the importer does not silently rewrite every asset.
  - Log whether a texture set received normal-variance roughness adjustment.
- **Interface contract:** deterministic output; no correction without a paired normal map; current assets
  remain loadable.
- **Done-when:** moving-camera distant fronds shimmer less, close-up mip 0 is unchanged, and roughness
  does not jump visibly at mip transitions.
- **Verify:** moving run with DLSS/TAA, fixed-distance mip sweep, numeric inspection of generated MR mips,
  before/after screenshots or video frames.

---

### F11 — RT, instancing, alpha, and shadow parity

- **Depends:** F5, F6, F8, F9.
- **Goal:** Ensure all paths agree on material identity and foliage response.
- **Touch:** `gbuffer_inst.hlsl`, `gbuffer_instcb.hlsl`, instancing compatibility/bindings,
  `shaders/rt_reflections_cs.hlsl`, RT material records/table, alpha-hit handling, shadow variants and
  any material refresh hooks affected by new descriptors.
- **Implement:**
  - Confirm single-slot and multi-slot instanced GBuffer draws bind the correct surface-parameter block
    and write the same GB1/GB2/GBAux values as per-object draws.
  - Include shading model and foliage payload in RT material records used for off-screen hit shading.
  - Reuse the shared foliage direct-light helper or a mathematically identical include in RT hit
    shading; avoid a third hand-copied approximation.
  - Preserve masked alpha hit rejection and two-sided normal orientation in RT.
  - Confirm CSM/VSM still render the alpha mask and two-sided caster. Shading-model payload must not
    alter the shadow PSO except for the already-required two-sided raster state.
  - Exercise Material Editor live apply. Rebuilt materials must refresh shadow and RT descriptor tables;
    stale CPU descriptor handles are a hard failure.
- **Interface contract:** raster per-object, raster instanced, reflected RT hit, CSM, and VSM agree on
  foliage material identity.
- **Done-when:** no visible model switch in reflections or instanced palms; shadows remain attached and
  masked; no invalid descriptor or resource-state messages.
- **Verify:** auto-instancing on/off A/B, RT/SSR A/B, VSM/Legacy A/B, edit foliage then move camera,
  `--scene-stress` with GPU validation.

---

### F12 — Palm migration, tuning, and final sign-off

- **Depends:** F11 and the desired M2 features (F7-F10).
- **Goal:** Opt the actual palm leaves into the model and establish production defaults.
- **Touch:** only confirmed leaf material files under `data/materials/`; optional importer-generated DDS
  updates after user approval; this document's results section.
- **Implement:**
  - Identify leaf slots by alpha-test/two-sided usage and mesh assignment. Do not mark trunk/coconut
    materials as foliage by filename guess alone.
  - Start the leaf material near:

    ```json
    "shadingModel": "twoSidedFoliage",
    "twoSided": true,
    "subsurfaceColor": [0.22, 0.50, 0.10],
    "transmissionStrength": 0.60,
    "indirectSpecularScale": 0.35,
    "ambientOcclusion": 1.0
    ```

  - Tune with authored roughness intact first. Do not hide renderer defects by forcing roughness to 1,
    globally lowering sky exposure, or zeroing all specular.
  - Capture the final matrix: None/SkyOnly/SSR/RT; front/back sun; camera above/below/grazing; near/far
    palm; instancing on/off; VSM/Legacy; DLSS/TAA movement.
  - Record measured GPU cost for added GBuffer bandwidth and IBL samples.
- **Interface contract:** material JSON round-trips through the editor and older DefaultLit material
  files remain valid without migration.
- **Done-when:** the user confirms the foliage look; direct specular reads, environment does not flow
  across cards, transmission reads naturally, and no unrelated material regression remains.
- **Verify:** Debug, Release, `Release_Editor`; fixed-camera PNG set; short moving run; GPU validation;
  `git diff --check`; line-ending audit.

---

## 9. Global acceptance criteria

- `ReflectionSource::None` is genuinely free of surface reflections while the sky background remains.
- TwoSidedFoliage is a shading model, not an alias for `twoSided` culling.
- Back-lit foliage receives tinted transmission; visible undersides do not collapse to black.
- Direct sun/point/spot GGX specular remains visible and is not controlled by ambient occlusion or
  `indirectSpecularScale`.
- Sky-only fallback no longer produces the original silver/blue palm artifact at roughness ~0.55-0.60.
- New GGX sky resources use prefiltered radiance + BRDF LUT, not ordinary image mips presented as IBL.
- DefaultLit screenshots are unchanged through M1 except where the user deliberately selects new
  reflection semantics.
- Emissive DefaultLit, metals, masked alpha, motion vectors, object picking, DLSS, SSR, RT, CSM, VSM,
  and auto-instancing pass regression checks.
- Material live edit followed by camera movement produces no D3D12 descriptor validation failure.
- No mixed line endings; C++/HLSL are CRLF and this Markdown file is LF.

---

## 10. Risks and rollback notes

- **MRT/PSO mismatch:** adding GBAux changes the GBuffer PSO render-target count and
  `OMSetRenderTargets`. Update every GBuffer PSO variant and binding atomically in F3. A format/count
  mismatch can fail PSO creation before a shader error is visible.
- **Four-bit shading-model quantization:** write exact `id/15.0` to `GBAux.b` and decode with round.
  Keep the C++/HLSL ID range at 0..15 and do not compare sampled values to ad-hoc thresholds.
- **GB2 interpretation:** compose must branch before adding emissive. Otherwise foliage payload becomes
  self-illumination.
- **Shared instance stride:** `render::InstancePerObject` is consumed outside the main GBuffer path.
  Silent growth corrupts shadow reads. Prefer the material-static surface CB.
- **Root-register collision:** multi-slot `gbuffer_instcb` already uses `b2`. Extend that block or choose
  a documented variant-specific binding; never bind two meanings to the same root slot.
- **Reflection double filtering:** the screen/RT reflection buffer already has a roughness-driven blur.
  GGX-prefilter the sky fallback, but do not accidentally blur it again through that pass.
- **AO misuse:** AO is indirect visibility, not a direct-light shadow. Multiplying all lighting by AO
  will remove the direct leaf highlight the feature is meant to retain.
- **Transmission self-shadow:** reusing opaque shadow visibility may suppress the light that should pass
  through the leaf. Solve with a scoped transmission bias/visibility policy, not shadow disablement.
- **Importer cost:** high-sample GGX cubemap convolution is offline work. Keep it deterministic, logged,
  and bounded; do not move Monte Carlo convolution onto the frame hot path.
- **Fallback compatibility:** missing `_spec`/`_diffuse`/BRDF LUT resources must log and use a safe old
  path, not bind null descriptors.
- **Rollback:** F1 is independently revertible; F2 is dormant; reverting F6 restores old indirect
  intensity; F7 only adds derivative assets; F8 can fall back to the old cube; content migration is
  isolated in F12.

## Suggested execution order

F0 -> F1 -> F2 -> F3 -> F4 -> F5 -> F6 gives the shortest route to a user-visible, controllable foliage
fix. Then F7 -> F8 -> F9 -> F10 establishes the correct environment foundation. Finish with F11 parity
and F12 content migration/sign-off. Do not start by globally increasing palm roughness; that hides the
IBL leak and makes later validation ambiguous.
