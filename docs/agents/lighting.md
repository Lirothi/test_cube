# Lighting -- Reflections, Sky Visibility, GI, Glass

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## The Water Reflection -- Its Resolve Runs Only Without DLSS

`Main_WaterReflection` traces the water's SSR (with `ssr.waterSurfaceNormal` from GB1, the wave normal of
`waterNormalCascades` cascades) and may resolve it in time (`ssr_temporal_cs`, `waterReflectionTemporal`). **That resolve
runs only while no temporal upscaler accumulates the frame** (2026-11-02, owner at cam 41.47 5.51 8.46, 14:00: "with the
temporal resolve the water's reflections crawl in big pixels; without it noisier but more real") -- UE's rule:
`IsSSRTemporalPassRequired` (ScreenSpaceRayTracing.cpp:213-222, asked for the water by SingleLayerWaterRendering.cpp:1748)
skips the SSR temporal pass under TAA / TSR / DLSS unless `r.SSR.Temporal` (default 0) forces it. Two accumulators in a row
turned the trace's grain into blotches: on that view (DLSS Balanced, the owner's 3 cascades, 1.0 resolution) the
reflection band's coarse detail (9-px box minus a 31-px trend) 5.4-5.8 -> 7.8-7.9 with the resolve, its fine grain not
lower (5.3 -> 6.1); without DLSS the resolve takes the grain 23 -> 14 and is what calms the raw trace. Developer Controls
"Water reflection temporal resolve (without DLSS)" shows "idle" under DLSS; `--set ssr.waterTemporalUnderDlss:1` = before
(UE's r.SSR.Temporal 1, the A/B; transient). The deferred SSR's own resolve is unchanged (it was measured useful there).

**The grain in shaded water is the reflection itself** (same day, the owner at cam 4.37 1.92 -38.31, 14:00, the palms'
shadow on the water: "without the resolve, with DLSS, still noisy"). The sun's shadow on the water (below) took the lit
body away there and left the reflection on top: the shore, the sky between the crowns and the palms, mirrored by ripples
smaller than a pixel at 15-40 m, one sample a pixel. Not the shadow: sparks (> 40 levels over their 7x7 box) in the shaded
water 2.50 / 2.42 / 2.55 % at 1 / 8 rays / the single tap, 2.65 % with the sun glint off, 1.55 % with the water's shadow off
(the bright body masks it), 5.4 % without the SSR hit (the sky alone sparkles more); frame-to-frame std with the wind held
0.91 / 0.71 at 1 / 8 rays. Caustics and the surface refraction do not move it either (pixel boil 3.41 / 3.30 with the
refraction at 0). Developer Controls > Reflections "Water reflection normal filter (mips)" (graphics_settings.json
`reflections.waterNormalFilter`, `--set ocean.reflectionNormalFilter`, 0-4; default 0 = the frame bit for bit as before;
SceneRenderSettings -> `OceanRenderable::SetReflectionNormalFilter` every frame, like the cascades) takes the
reflection's DIRECTION -- the sky's and GB1's -- from the derivatives that many mips coarser (`LightingInput::
reflectionNormal`, surf-sim surface; the run-up one has its macro normal) and adds the slope variance it drops to the sky's
blur by Bruneton's distance law at 2^w the distance; the Fresnel weight keeps the full normal (from the filtered one the
sun-lit lagoon turned bluer: at a grazing view the mean of F over the ripples is below F of their mean normal). Measured
with the wind held: the shaded water's fine grain 8.51 / 8.37 / 8.32 / 7.56 at 0 / 1 / 2 / 3 mips, its pattern coherent
ripples instead of salt and pepper; in motion the pixel-scale boil the same (3.26 -> 3.14 at 2, within run noise), and on
sun-lit water 2-3 show the swell's sky reflection as coherent bands where 0 averages it to an even turquoise. `Ocean.Surface`
+0.007-0.009 ms (the derivatives read once more, up to 4 cascades). Left at 0 for the owner's pick; `ssr.waterNormalCascades:2` (the swell only in
the ray) gave the most coherent shore reflection on that view. GBV (`Transparent,Transparent.Water,WaterReflection,Skybox`
+ the Skybox self-test, that view, the resolve on, the filter at 2): clean.

**The stretched pixel zones by the shore are the reflection's undersampling, and a prefilter in space takes them out**
(same day, the owner at cam -15.63 3.52 -39.53 / 10.97 0.96 -47.95, 14:00, his reflection settings: "zones of stretched
pixels, even without DLSS -- is it the SSR, the sampling, something else?"). Not the tracer: the water's hit mask (white =
the ray hit, `debug.tex:12` with `debug.texMode:1`) near the shore was the same at 8 and 64 steps, starting the march at
the HZB's mip 1 (UE's, SSRTRayCast.ush:660; our HZB's mip 0 is half the render resolution) or 0, with UE High's GGX rays or
one mirror ray, and at 0.5 resolution only coarser; it is hits on the shaded beach and the trunks broken by horizontal runs
of misses (the sky between the crowns) along the ripples. Each water pixel casts ONE mirror ray, from ripples finer than the
pixel at a grazing view (horizontal slivers a pixel tall), into a scene of high contrast -- one sample per pixel of a
reflection that varies faster than the pixels. With the waves HELD, DLSS adds its jittered frames up to a clean, coherent
rippled reflection: the reflection is right, it is undersampled. With them running, nothing can follow the ripples (they run
at their phase speed, no surface point carries them, so no motion vector does): without DLSS the raw runs show, under DLSS
its clamped history smears them -- the stretched zones. `water_composite_cs` now reconstructs the pixel in space, as a TAA
resolve's spatial kernel does: the reflected radiance (the hit, and the sky where the ray missed: GB2 / (W x
preExposure)) averaged over the neighbouring water by a Gaussian of `waterReflectionPrefilter` render pixels across the
columns and half that along them (the ripples are horizontal slivers; a tall kernel merged crests), radius 2 sigma capped
at 3 x 2. Not gated: the reflected ray steps tens of the reflected image's pixels between neighbours on near and far water
alike, and the second difference of those steps (a smooth sweep against a scatter) lit the near crests as much as the far
grain. Developer Controls > Reflections "Water reflection prefilter (px)" (graphics_settings.json `reflections.waterPrefilter`,
`--set ocean.reflectionPrefilter`, 0-3; **default 1**, 0 = before). Sigma 1.5-2 already softened the near shore; 1 turned the
grain into coherent ripples on both views with DLSS, the waves running (the zones' fine grain 3.96 -> 3.54 and 4.94 -> 4.12),
and halved it without DLSS. The near water is unchanged (refraction and glints, not the reflection). `WaterReflection.Composite`
0.012 -> 0.027-0.030 ms. The very grazing band at the waterline (view 2) keeps some horizontal streaking. GBV (the same passes,
the resolve on, the prefilter at its default): clean.

## Glass -- the thinTranslucent Shading Model

Glass is a material SLOT's shading model, not an object type: `"shadingModel": "thinTranslucent"` (UE
MSM_ThinTranslucent, glTF KHR_materials_transmission) with `"transmission"` (factor), `"transmissionMap"` (its R
multiplies it) and `"ior"` -- not the foliage's `transmissionStrength`. `TransparentStaticMesh` (the `glass.hlsl`
panes) stays as it was. A glass slot draws in Main_Translucent through `glass.hlsl`'s GLASS_MATERIAL permutation
(`GBufferRenderable::RenderTranslucent`, reached because `Scene::PrepareViewQueue` adds every visible object with
`HasTranslucentSlots()` to `translucentComplex` and RenderObjectBatch calls `RenderTranslucent` in that pass).

- **A slot with an alpha test SPLITS**: the texels its cutout keeps are opaque and take every opaque path as before
  (written DefaultLit); the ones it removes are glass. **A slot without one is glass only**
  (`MaterialData::IsGlassOnly`) and leaves every opaque path: `Render` / `RenderShadow` skip it, instancing refuses
  the object, its indirect PSO is null and ShadowGpuData zeroes its group's index count (camera and every shadow
  view), an object of nothing else is not in the TLAS. Any glass slot refuses instancing (the instanced shaders read
  one cutoff for the G-buffer and the shadow).
- **glTF BLEND alpha is coverage on top of the transmission model** (three.js and Blender do the same): Sketchfab's
  exporter writes a pane's opacity as BOTH the alpha and 1 - transmission. Left out, the_bathroom_free's shelf was a
  full sheet of blue glass. The share the alpha leaves open is still that glass seen through: it takes the same bend
  as the transmission (sharp, untinted -- Sketchfab's refraction opacity mixes the BENT scene with the surface).
  Until 2026-10-24 it showed the scene straight, and the tumblers (alpha 0.498) passed half their background
  unbent: no wall thickness, not even solid glass, changed the picture. A pane does not bend, so the shelf is as it was.
- **Refraction through a glass shell** (owner 2026-10-24, "no distortion", then "like Sketchfab"): `glass.hlsl
  GlassRefractedUv` takes the surface locally as a quadric with BOTH principal curvatures of the interpolated
  GEOMETRIC normal (the shape operator from the pixel quad): an ellipsoid with semi-axes R, R/s, R (R = 1/k1,
  s = sqrt(k2/k1)) -- a sphere where the surface is round both ways, a cylinder up a tumbler's side (a sphere of
  the larger curvature bent a tumbler's background into a dome). `glass.wall` m of glass lie under the surface:
  thinner than the shape is round = HOLLOW, refracted through both walls exactly; at least that thick = SOLID.
  The view ray then lands on the opaque scene by HDRP's proxy raycast (the depth copy at t18, one refinement; a
  landing in front of the glass keeps the straight sample). A pane or a concave face is not bent. **The default
  is solid (`glass.wall` 0.5)**: the owner picked Sketchfab's preview, whose screen-space refraction bends every
  cup like a magnifying block, over what the_bathroom_free models -- hollow tumblers with a 4.3 mm wall (outer
  and inner surface), `glass.wall:0.004`. 0 = straight through. **A material says its own**: `"glassWall"` (m;
  absent = the frame's glass.wall; Material Editor "Own glass wall"; `MaterialData::glassWall` -> the slot CB's
  matGlass.z, `glassRefraction.y` = the depth copy bound). small_yacht's windshield (`small_yacht_7`) is a pane, 0
  (2026-11-09, owner at its bow: "artefacts on the glass"): as the frame's 0.5 m block its wrapped corners (radius
  under 0.5 m: solid) bent the cockpit like a bottle and its flatter middle read as half a metre of glass, the
  curvature from the pixel quads jumping between its triangles -- torn, stepped blocks; 0 and 5 mm were both
  clean there. A window, a windshield, any open sheet of glass is a pane: the shell model is for closed shapes
  (a cup's far wall). The tumblers keep the frame's; the corner shelf's panes measure the same under every
  value (0.4 % of pixels, noise). Roughness blurs what is behind (`SampleBehindGlass`, SceneOpaque has one mip). Its reflection is the
  sky's (a slot is not in the glass-reflection G-buffer) -- **occluded by plan S**: the glass has no bent normal of
  its own, so the opaque surface behind it lends its RT sky visibility (t19, `glassSkyVis` = lighting's
  skyVisParams) to the sky fill and the sky reflection (2026-10-24, owner: "why are the transparent objects so
  blue" -- a tumbler in a closed room reflected the whole night sky x nightBrightness).
- It blends OVER the target (ONE / INV_SRC_ALPHA) so glass behind glass shows, after a **depth prepass**
  (`PSDepthOnly`): a concave cup has two front-facing surfaces per pixel, and in index order the far inner wall
  blended over the near one -- the owner's "стаканы вывернуты". The prepass's depth stays: a particle behind glass
  is hidden by it.
- **Shadow**: a split slot's SHADOW cutoff is raised by `kGlassShadowCutoff` (CPU shadow CB, ShadowGpuData's group
  mask -- never the G-buffer's); its glass texels then cast a dithered share equal to their coverage
  (`shadow_depth_common.hlsli ShadowAlphaClip`), which the shadow filters average into a partial shadow. A glass-only
  slot casts nothing.
- The importer decides per NODE (`materialgen::ClassifyGlass`: the share of the surface whose transmission is over
  `kGlassTransmissionMin`; a split needs `kGlassOpaqueShareMin` of it opaque): one glTF material is often an atlas
  for mugs and tumblers alike. `intent_regression` holds it on the staged bathroom.

## Two-Sided Surfaces -- the Back Face and the Sun

A two-sided material's back face (`SV_IsFrontFace` false) takes its normal map in the FRONT face's frame and only then
flips the result -- UE's order (`MaterialTemplate.ush:4369-4380`, `WorldNormal *= TwoSidedSign` after the tangent-space
transform); every G-buffer shader, glass and the editor preview do it. Flipping the vertex normal first left the
tangent unflipped and gave the back face (x, -y, -z) of the map instead of -(x, y, z).

The sun's direct light also needs the visible side's GEOMETRIC normal to face it (`lighting_cs.hlsl
SunGeometricTerminator`, rebuilt from the depth buffer, a ramp over N.L 0 -> 0.05; an edge pixel -- the nearer
neighbour farther than 3 % of the distance -- is not gated). Why (2026-10-28, owner: "light leaking" inside the tent,
cam 70.85 2.24 41.52 at 16:00): the cloth's folds are mostly its normal map, and on the inside the map tilted pixels
toward a sun that lay behind the cloth. No shadow map catches that -- the receiver is its own caster; Legacy, SDSM and
VSM leaked alike with every bias at zero and the LODs off. Traced on the staged glTF from that camera (scratchpad
`tent_rays.py`): 99.5 % of the lit pixels sat on triangles facing away from the sun (N.L median -0.23), the vertex
normals the same; the tent's normal strength at 0 made the leak go. Measured with the exposure pinned: the tent -5.9 %,
the bathroom -1.1 % (0.07 % of pixels over 8 levels), the grove -0.13 % (1.4 %, under its 1.7 % frame-to-frame floor),
Pass_Lighting +0.01-0.015 ms. `--set light.geometricTerminator:0` = before. Local lights are not gated (yet).
**Only where the VERTEX normal is behind the light too** (2026-11-02, owner at cam -5.44 5.72 78.10, 14:00 on Nov 1, the
sun 24 degrees up: "the shadow triangles in VSM and SDSM"). Gated on the facet alone, a smooth-shaded low-poly dune lost
the sun on every triangle of its lee slope that leaned past the light while its vertex normals still faced it: dark
teeth on the sand, the same in VSM, SDSM and Legacy, untouched by tessy.lod / tessy.enabled / the shadow LOD bias / LOD0
(0-0.4 % of the teeth moved), nothing between the sand and the sun (MCP `lookingAt` from the sand toward it: no hit), gone
with the gate off. The tent's normals faced away as well, so the G-buffer now writes GB1.a = whether the visible side's
interpolated vertex normal faces the frame's light (`gbuffer_common.hlsli FinalizeGBuffer`, the light in PerView's new
tail `gbLightToward`, PerViewCB 240 -> 256 bytes -- SDSM's per-partition blocks were 256 already and do not write it),
and the facet gate runs only on the 0s (the ocean writes 0: its gate as it was). The teeth box's mean 85 -> 99.5 (gate off
99.6), all three modes; the tent's leak held -- the new gate against the old one 0.88 % of the frame moved > 8 levels, the
old against itself 0.96 %, no gate 1.70 %. `light.geometricTerminator` 0 / 1 / 2 = none / this / the facet alone
(2026-10-28 .. 11-02). GBV (`GBuffer,Lighting,Skybox` + the Skybox self-test, that view): clean.

**The sun's shadow is looked up from the SMOOTH surface: Hanika's terminator offset** (2026-11-02, owner at cam -11.6 4.17
73.6, rot -0.1281 0.8834 -0.2052 -0.4086, 14:00 on Nov 1: "what is this staircase then? -- the offset after Hanika"). A
smooth-shaded low-poly surface is SHADED by its interpolated normals but SHADOWED by its flat triangles: under a grazing sun
a facet of the island's convex dune fell into its neighbours' shadow while the shading said lit, and the shadow's edge
followed the triangles -- the classic shadow terminator problem. Sharpest under VSM's SMRT (precise rays), blurred by SDSM's
and Legacy's filters; untouched by the gate (0.1 %), tessy.lod / tessy.enabled / the shadow LOD bias / LOD0 (0 %: the
island's own geometry casts), the SMRT knobs (the screen ray 0.015, dither, extrapolation, source angle) and the normal bias
(it only moved the edge). The practice: Hanika, "Hacking the Shadow Terminator" (Ray Tracing Gems II ch. 4; Blender Cycles'
Shadow Terminator Geometry Offset is the same idea). `gbuffer_common.hlsli GBufferTerminatorHeight`: the G-buffer PS reads
its triangle's three vertices (`GetAttributeAtVertex` on VSOut's new `nointerpolation vtxPosWS / vtxNWS`, `SV_Barycentrics`;
every G-buffer PS and the Tessy mesh-shader twins), lifts the point by each vertex's tangent plane it lies under, blended by
the barycentrics, and stores the lift's length in GBAux.a (sqrt-encoded over 2 m; foliage keeps its transmission normal
weight there, water its reflection weight); lighting_cs looks the SUN's shadow up that far above the pixel along the
triangle's normal rebuilt from depth (`Pshadow`; the shading normal's map tilt would jitter it). Flat-shaded and concave
triangles get 0. On the owner's slope the staircase is gone in VSM, SDSM and Legacy -- the lee slope now darkens smoothly
by its normals; low over the sand at 15:00 the striped self-shadow on the near sand went with it (28 % of that frame
brighter); the grove 8.9 % moved (its frame-to-frame floor with the wind held ~1.5-4 %), the bathroom 0.2 %, the tent 0.5 %,
the island from above 0.7 %, the trunks' and palms' shadows where they were. Cost: GBuffer_Indirect 0.152-0.153 ms on the
grove (0.155-0.163 documented before), Pass_Lighting on / off the same (0.39 / 0.39, 0.41 / 0.41). `--set
light.terminatorOffset:0` = before (the A/B; a scale, 1 = the smooth surface). The lamps' shadows do not take it (yet).

**The normal map's bumps do not light a surface the sun has left: the bump terminator** (2026-11-03, owner at cam -55.21
4.85 29.13, rot -0.0525 0.9330 -0.1521 -0.3220, 18:00, the sun 10.7 degrees up: "what is this shadow's silhouette?", then
"why so sharp -- a crease?", then "make the edge smooth"). Not a crease: the beach there slopes at the sun's elevation, the
island's facets' N.L runs -0.016 .. +0.029 across it (exact against the OBJ, scratchpad `island_shadow.py`), and the
island's own shadow (UE's too, the same mesh) ends where the facets turn to the sun. It was SHARP because the sand's normal
map kept the shading N.L above 0 there -- the sun-only probe stepped 66 -> 237 across the edge; with the map off it ramped
50 -> 69 -> 87 .. 205. `utils.hlsli SunBumpShadowing` (Chiang, Li, Burley 2019, "Taming the Shadow Terminator"; Blender
Cycles' bump_shadowing_term): g = (Ng.L) / ((Ns.L) (Ng.Ns)), Ng the interpolated vertex normal, the cubic -g^3 + g^2 + g,
computed in FinalizeGBuffer for the frame's light (PerView gbLightToward) and stored as 1 - G in GBAux.b's HIGH four bits
(the shading model's ID keeps the low four: `EncodeShadingModel` is n/255 now, `DecodeShadingModel` masks; a writer that
does not know -- the ocean -- leaves 0 = none), dithered by a hash of the mapped normal; lighting_cs multiplies the direct
sun by it (opaque and the foliage's front; not the transmission). After: the edge ramps 51 -> 57 -> 70 .. 200, the full
frame 425 -> 415 -> 422 .. 539 (was 446 -> 564). The price is the grazing-lit sand's relief: bumps no longer light up where
the smooth surface barely faces the sun (that view 9.8 % of the frame > 8 levels, mean -7 %); the beach at 14:00 0.79 %
(floor 0.84), the grove at 15:00 1.30 % (floor 0.97). UE has no such term (its edge is as sharp as ours was). `--set
light.bumpShadowing:0` = before (the A/B).

**A leaf passes the sky behind it, not only the sun** (2026-10-31, owner on the island against the sun at 15:03, cam
71.70 6.95 114.59: "isn't it too black now?"). A two-sided foliage pixel adds `subsurface payload x (sky irradiance(-N) +
GroundBounceOverPi(-N))` to its fill, under the front's occlusion (`lighting_cs.hlsl`, after the sky fill) -- UE's
`GetEffectiveSkySHDiffuse(-WorldNormal) * SkyVisibility` times the subsurface colour (BasePassPixelShader.usf:530-536,
:1377). Before, the only light through a leaf was the direct sun, and inside a crown the sun is mostly blocked by other
fronds, so a crown against the sun took just the darker sky on the camera's side while the brightest sky (the aureole) sat
behind it. The GI probes are not asked for the back. Measured at that view (exposure the same, EV 15.99): the crowns' dark
pixels x1.41 linear (median; p25 1.20, p75 1.75), display 7 -> 12, trunks and sand unchanged; the front-lit grove at
15:00 x1.08 (9 % of the frame moved > 8 levels, all foliage); `Pass_Lighting` unchanged (0.231-0.234 / 0.109-0.112 ms,
alternated twice). `--set light.foliageBackSky:0` = before (the A/B). The rest of that view's darkness is the grade
(local exposure off, contrast 1.3 -- 9f88f5e7): the morning's grade puts its trunks at 42 of 255 instead of 24.

Object-level surface keys (`normalStrength`, `metalRough`, `useMR`, `useAlbedo`, `baseColor`, `texOffsScale`, the
emissive pair) reach EVERY material slot -- at load (`GBufferRenderable::MergeMaterialDefaults`) and live
(`SetObject*`: the Inspector, MCP `setSurface`); they used to reach slot 0 alone, so on the tent (sticks, ropes, cloth)
`setSurface` changed the sticks and reported success. No level carried such a key on a multi-slot mesh (checked
over data/levels), so no saved look moved.

## RT Sky Visibility -- the Sky Stops Lighting What It Cannot See

`Main_SkyVis` (`shaders/rt_sky_visibility_cs.hlsl`) is UE's Distance Field AO bent normal traced with inline
RayQuery on the TLAS: half resolution, `skyVis.rays` (8) cosine rays per pixel against the SHADOW casters' geometry
(a cutout at its mesh's opacity, below), out to `skyVis.maxDistance` (15 m). Two entry points, in UE DFAO's order
(since 2026-10-27, below): `CSTrace` writes this frame's rays alone into `skyVisRaw`; `CSAccumulate` filters THEM in
space (UE DFAO's spatial filter: a 9x9 half-resolution gather weighted by relative depth `skyVis.filterDepth` 0.05 and
normal agreement^`skyVis.filterNormalPower` 8; `skyVis.filter:0` = the rays as they are) and only then blends the
result into the history (`skyVis.history` 0.995, reprojected through the G-buffer velocity) -> `skyVisAccum` (next
frame's history) and `skyVis`, the target every consumer reads. Only each frame's NEW value is filtered, never the
history (a filtered history widens the blur every frame). Its length is how much sky a point sees, its
direction where from. `shaders/sky_visibility.hlsli` applies it with UE's formulas, file and line cited there:
the sky diffuse takes min() with the AO (UE's Minimum combine) and a bent lookup normal, the sky specular a cone-cone
occlusion -- in lighting_cs AND compose_cs, the same line, because compose subtracts the sky specular lighting
added. Needs RT hardware (`decisions_.skyVis` also builds the TLAS); off = the open sky, the frame before it.

- Bindless scene slots 26-31 are this pass's (its own TLAS/GB1/depth copies -- the reflection trace that writes
  0/2/3 does not run unless reflections are RT).
- The body writes the target on EVERY path: traced, or cleared to the open sky while the TLAS is not ready --
  lighting and compose read it on the strength of `decisions_.skyVisTraced`, set by the builder.
- Cost is the ray count: 4 rays 0.28-0.34 ms on wind_test's grove, 8 rays 0.63, 2 rays 0.18. Split (2026-10-28, 8 rays,
  the nearest-hit query): ~0.053 ms a ray on the grove, 0.021 in the tunnel, plus ~0.2 / 0.15 that does not follow the
  rays (the G-buffer reads and the reprojection at 743x418; the 9x9 filter is 0.01-0.05 of it). A ray's price is its
  traversal: on the grove the reach 15 -> 5 m saved 0.24 ms and solid cards 0.2 -- which the two any-hit queries
  (below, "A tree's leaf card") now save too.
- Noise (2026-10-24, owner: "the wall is noisy"): a wall lit by a window that fills a few % of its hemisphere gets
  the sky from a few of its rays now and then. wind_test's second room at 24:00 (cam -21.92,1.07,89.75), per-pixel
  temporal std over 12 clean shots (scratchpad noise_map.py): 0.9 and no filter 9.4 % median (p90 18 %), 0.95 +
  the filter 3.2 % (5.7 %), 0.97 2.8 %, 0.97 + 8 rays 2.5 % (+0.075 ms); SkyVis off 0.35 %. Bindless slots 40-41
  and 52-53 are the second dispatch's (this frame's rays, the target; the rays' UAV, the A/B's accumulation SRV).
- 8 rays and 0.98 are the defaults since 2026-10-26 (owner's tunnel of boxes, cam -58.63,1.72,65.12 at 15:00, whose
  floor sees the sky only through the mouth; per-pixel temporal std over 16 shots, scratchpad tunnel_variants.py):
  4 rays / 0.95 8.6 % median (p90 12.3 %), 8 rays 5.6 %, 0.98 5.2 %, both 3.5 % (5.1 %); SkyVis off 0.2 %.
  Pass_SkyVis 0.14 -> 0.25 ms there. **0.995 since 2026-10-27** (below, "Dancing"); the range tops out there.
- **A texel without history is SEEDED** (UE DFAO `FilterHistoryPS`, DistanceFieldLightingPost.usf:297-361; 2026-10-26,
  owner: "flying backwards down the tunnel, noise artifacts"): disoccluded or come in over the screen's edge, it takes
  `lerp(own rays, mean of the neighbours within 4 texels that kept their history, skyVis.history)` -- each neighbour's
  own reprojected history, weighted as the spatial filter weighs (`skyVis.filterDepth` / `filterNormalPower`; UE's
  exp2(-1000 x depth step) drops the next row of a floor seen at 30 degrees). Before, the few rays of that first frame
  went into every later blend at 0.98^n: a camera flying backwards, the floor coming in under the bottom edge every
  frame, wore a band of grain ~50 half-res rows deep. Owner's view (cam -57.89,2.27,65.68 yaw 57.8, 15:00) flying
  back at 3.8 m/s, the floor's bottom band (std of luminance minus an 8-px blur / mean, scratchpad band_noise.py; the
  same frame still = 10.7 %): 300 fps 16.0-17.7 % -> 9.3-9.9 %, 60 fps 22.7-23.5 % -> 9.6-10.2 %; Pass_SkyVis
  0.230-0.239 -> 0.237-0.249 ms. `skyVis.seed:0` = before (the A/B); `skyVis.debug:1` paints the texels that lost
  their history this frame as the open sky (the accumulation's .w is negative there, UE's sign bit -- the target's
  stays positive). A wholly new region (no neighbour kept anything) still starts from its own rays, as in UE.
- **No RT ray but a reflection's alpha-tests** (owner, 2026-10-26): this pass, the GI (probe rays and their
  shadow/sky rays) and the shadow rays of what a reflection hits see the GEOMETRY, a leaf card solid (a tree's partly
  open since 2026-10-27, next bullet) --
  `rt_geometry.hlsli RtVisibilityBlocked` (FORCE_OPAQUE + ACCEPT_FIRST_HIT_AND_END_SEARCH) and ddgi_trace's
  FORCE_OPAQUE closest hit. `rt.alphaMode` / Reflections "RT foliage alpha" is the reflection rays' alone. Why
  (owner: "SkyVis dances inside the house"): they followed the reflections' mode, and its mode 1 alpha-tested the hit
  the traversal met FIRST -- the BVH's order, not the ray's; the palms' wind BLAS are rebuilt every frame, so a crown
  went "blocked" / "open" for every ray through it at once and the bathroom, lit through one window with palms
  outside, pulsed. Owner's view (cam -24.51,1.36,87.74, 15:00), 11 headless shots 0.5 s apart, per-pixel std (scratchpad
  house_series.ps1 + series_noise.py): median 5.08 % / p90 19.9 % -> 1.82-1.88 % / 3.1-3.2 % (twice); the wind frozen
  1.84 %, SkyVis off 0.70 %. Cost (editor closed, alternated x3): SkyVis 0.31 ms in the bathroom, 0.48 on the grove,
  whatever the reflections' mode -- the old mode 1's own traversal. Tried first, dropped: the NEAREST hit's texel
  deciding (deterministic too, but +0.10 / +0.23 ms SkyVis and a ray through a wall behind a transparent texel), and
  closing that leak (1.49 / 1.94 ms on the grove); mode 2 throughout costs the grove's SkyVis 2.0 ms.
- **A tree's leaf card lets part of a visibility ray through: mesh.json `"rtCutoutOpacity"`** (2026-10-27, owner:
  "make the leaves partly transparent for RT rays -- UE has the like", then "a per-MESH setting, only the trees; walls
  must block fully", then "it hit a leaf, the leaf lets its share through, that's it -- the ray goes nowhere after").
  As solid cards a palm crown shut out far more sky and sun than its fronds do (they cut 35-68 % of their cards away):
  the sand under the crowns lay in full GI shade, a leaf's underside saw no sky through the leaves above. Every
  visibility ray -- this pass, the GI's probe, shadow and sky rays, the fog's sky and sun rays -- asks the NEAREST
  surface along it on the hardware's fast path (`rt_geometry.hlsli RtVisibilityTransmittance`: FORCE_OPAQUE, one
  query, no candidate loop): a cutout of such a mesh lets `1 - opacity` through and the ray stops there; anything else
  blocks. The GI probe's own ray stops on the card too and takes `opacity` of its radiance (the rest would have come from
  behind it, not traced: no bounce). The value is the ASSET's (`RenderableObject::SetRtCutoutOpacity`, folded in by
  ResolveMeshAsset like every mesh.json key; Mesh Editor > Ray tracing > "Cutout opacity"); absent = 1, every card solid
  -- the five `*_palm` assets say 0.8 (20 % through; four of them sat at 0.2 for a while), fern and grass nothing. It rides the
  TLAS instance's `InstanceContributionToHitGroupIndex` (inline RayQuery has no hit groups; AccelerationStructure.cpp):
  bits 0-7 = 255 x (1 - opacity), bits 8-23 = which of the mesh's first 16 submesh slots are cutouts, so a palm's TRUNK
  blocks fully -- and the fog, which has no geometry records, reads it as the others do. The first version was ONE global
  0.8, and the bathroom's door, curtains, towels, carpets, cups and window wall are alpha-tested too (Sketchfab): a fifth
  of the sky came through them. `rt.meshOpacity:0` = every card solid, the A/B; `rt.cutoutOpacity:<v>` (transient,
  `--set` / MCP; < 0 off) stands in for every mesh that says < 1, to try a value live. The reflections keep `rt.alphaMode`.
  - **Before (until 2026-10-27): a fixed share of a cutout's TRIANGLES blocked** (a hash of their ids) and the ray traced
    on through the rest -- layers multiplied (0.8 per card: a crown of 3-5 cards passed 1 % or less), but without
    FORCE_OPAQUE every alpha-tested triangle in the scene, a solid mesh's too, was a candidate the RT core handed back
    to a Proceed() loop: ~1.2 ms of the grove's 5 ms frame. The number first written here (+0.01-0.03 ms) was wrong.
  - **The NEAREST hit, not the traversal's first**: ACCEPT_FIRST_HIT (tried first) returns whichever hit the BVH finds
    first -- a ray up from the bathroom found the palm crowns BEYOND its ceiling before the ceiling, and the closed room
    saw the sky through its roof (sky visibility mean 66 -> 117 of 255 with the crowns at 0.2). Nearest: 65.8 / 65.8 / 65.7
    with solid cards / 0.8 / 0.2. Solid cards (`rt.meshOpacity:0`) keep the any-hit query: there every hit answers the same.
  - **Cost** (the live editor, panels closed, the grove camera [34,3.5,68] yaw 80 pitch 8 at 15:00, wind frozen, arms
    alternated twice): GPU frame 4.07-4.09 ms vs 3.85-3.86 with solid cards (the triangle pick: 4.97-5.35) -- SkyVis
    0.67 vs 0.51 (1.21-1.30), GI trace 0.29 vs 0.26 (0.43-0.47), far 0.12-0.13 vs 0.11 (0.20-0.22), fog scatter
    0.25-0.28 vs 0.23 (0.44-0.47). GBV (`SkyVis,GiTrace,VolumetricFog,Skybox` + the Skybox self-test): clean.
  - **The look** (the same camera, the exposure pinned to the solid-card frame, scratchpad `grove_opacity.py`): mean sRGB
    luminance against solid cards, frame / sand / crowns -- 0.95 +2.8 / +2.9 / +3.9 %, 0.8 +9.0 / +9.8 / +12 %, 0.6 +15 /
    +18 / +18 %, 0.4 +20 / +24 / +24 %, 0.2 +26 / +32 / +29 %, 0 +30 / +38 / +33 %. The sunlit sand and trunks brighten
    (the sky and the bounce through the crowns); the sun's own shadows are SDSM's, not RT, and stay. The triangle pick at
    0.8 measured +2.0 % (layers multiplied): one card now passes what a crown used to.
  - **Two any-hit queries, not one nearest** (2026-10-28, `rt.meshOpacity` 1; 2 = the one nearest-hit query above, the
    A/B; 0 = solid cards): the shadow bit of the TLAS mask is split -- a caster whose mesh says rtCutoutOpacity < 1
    answers `rt::kInstanceMaskShadowTransmissive` (0x04), every other caster `kInstanceMaskShadowSolid` (0x02), the
    visibility rays trace both (`kRtMaskShadow`). Anything solid in the way blocks wherever it lies (any hit ends it --
    still the ceiling between the bathroom and the crowns); a ray that met nothing solid takes the first transmissive
    surface the traversal finds: a cutout passes 1 - opacity, a trunk nothing. A leaf in front of something solid now
    passes nothing; a ray through a frond AND a trunk takes either (the wind BLAS's order: one of 8 rays, 0 or 0.2, under
    the 9x9 filter and the 0.995 history -- the grove's visibility moved alike frame to frame in all modes, 48 frames, wind
    on). Live editor, wind frozen, alternated twice, against the nearest query: SkyVis grove 0.505-0.508 vs 0.660-0.663
    ms (solid cards 0.49-0.51), bathroom 0.31-0.32 vs 0.43-0.44, tunnel the same (0.34); GI trace grove 0.24 vs 0.27; fog
    scatter grove 0.15-0.22 vs 0.25-0.26. The look (exposure pinned): grove -0.10 %, |d| > 8 on 1.7 % of the frame (the
    crowns and trunks only), bathroom +0.4 %; solid cards stay -8 % on the grove. Tried first: the nearest surface among
    the transmissive meshes in the second query -- 0.63-0.66 ms on the grove, the rays up through the crowns meet nothing
    solid. GBV (`SkyVis,GiTrace,VolumetricFog,Tonemap,Skybox` + the Skybox self-test): clean.
- **In motion the history is read as UE and Lumen read theirs** (2026-10-26, owner: "SkyVis is very noisy and trails
  when the camera moves or turns fast"). Three things, each cited in `rt_sky_visibility_cs.hlsl SkyVisHistoryAt`:
  (1) the four texels around the reprojected point, bilinearly, each kept only if the point its own depth puts there
  last frame lies on this point's PLANE within 3 % of the view depth (UE UpdateHistoryDepthRejectionPS's manual
  resample with a weight per corner; the plane test is UE GeometryAwareUpsample's -- the 3D distance of the history's
  own test threw history away on walls at grazing angles under DLSS jitter). It was ONE point sample, whose rounding
  in a steady turn slid the history the same fraction of a texel every frame, and a 10 % device-depth test.
  (2) Each texel counts the frames it has accumulated (`skyVisCount`, R8_UNORM per Deferred set, bindless 50-51,
  `kScenePerFrame` 54) and blends the new rays at 1 / (1 + count) up to `skyVis.history`'s own share (0.98 = 49
  frames): Lumen's NumFramesAccumulated (LumenScreenProbeGatherTemporal.usf:481-527). A fixed 0.98 kept a texel's
  FIRST frame at 0.98^n -- 82 % after 10 frames -- and a 90 degree/s turn at 120 fps brings ~10 new half-res columns a
  frame, out of the seed's reach. (3) A seeded texel counts as UE DFAO's history (0.85 = 5.67 frames), not 49.
  Owner's tunnel (cam -57.89,2.27,65.68 yaw 57.8, 15:00), moving frame vs its still retake, one launch, old/new:
  turning 90 degrees/s at 120 fps 6.86 -> 2.26 mean |d| native (DLSS Balanced + bloom 7.27 -> 2.30; SkyVis off 0.32 /
  0.50); flying 4 m/s 3.78 -> 3.17, grain 1.32 -> 1.05; flying backwards (band_noise.py) bottom band 19-21 % -> 18 %.
  Still: the same (floor 11.9-12.4 % per-pixel temporal std both, the whole frame 2.9-3.2 %). Pass_SkyVis +0.004 ms
  still, +0.007 turning. What is left in a turn is the young texels' own noise at the leading edge and the
  reflections' temporal resolve (below, "RT Reflections"): the metal walls' sky reflection is where the sky
  visibility shows most.
- **Rays are point samples, a distance-field cone is not** (2026-10-26, owner: "SkyVis noises like a bastard" in his
  tunnel; "think what the principal difference from DF tracing is"). UE's DFAO cone measures what SHARE of its own
  section the geometry closes -- deterministic, smooth, blurred. A ray says 0 or 1, so where the sky is a share p of
  the hemisphere (the walls by a tunnel's mouth, p a few per cent) 8 rays and the history leave ~1 / sqrt(p x samples)
  of noise per texel: the grain. What rays can do about it, measured on the owner's tunnel with `skyVis.debug:2`
  (the visibility alone, x4, no textures; scratchpad `tunnel_burst.py`, 24 still frames): STRATIFY across texels --
  UE's interleaved gradient noise rotates each texel's rays (`skyVis.sampling` 1; 0 = the old white noise), so the
  spatial filter averages rays that cover the hemisphere between each other: grain median 6.7 -> 4.7 %, flicker p90
  9.0 -> 5.9 %, free -- and WIDEN the spatial filter (`skyVis.filterRadius` 4 = 9x9 half-resolution, was UE's 5x5):
  2.3 % / 3.5 %, +0.01-0.04 ms, the diagonal hatching in flight gone (CSAccumulate's comment has the numbers). 16 rays alone:
  3.8 % for +0.12-0.27 ms. Tried and DROPPED: half the rays into a cone around the history's bent normal (Lumen-style
  guiding, balance-heuristic weights) -- grainier and, interleaved, 12 % darker. The means agree within 1 % across
  every kept arm. Not tried yet: screen-space probes with many rays each (Lumen's gather) -- a redesign.
- **The spatial filter runs BEFORE the history: UE's order** (2026-10-27, owner: "what trembles here?" -- the tent's
  fabric where it meets the floor, cam 71.17 2.11 41.16, 15:00). The fabric's fold (visibility 0: every ray into the
  floor or the fabric over it) beside the wall's bright bottom trembled 4-7 % frame to frame, 0.03 levels with
  `dlss.mode:0`: the filter ran AFTER the accumulation, weighted by this frame's G-buffer under each texel, which the
  DLSS jitter moves across a crease every frame, and nothing averaged those weights. UE DFAO's
  UpdateHistoryDepthRejectionPS runs its GeometryAwareUpsample on the NEW value and then blends
  (DistanceFieldLightingPost.usf:161-240); `CSAccumulate` does the same with our gather. A/B in one launch (scratchpad
  seam_bisect.py: the hem's 97th-percentile band as the mask, 32 consecutive frames, wind frozen, three pairs): the hem
  4.5-6.5 % -> 0.94-1.02 %, the whole frame 0.3 % both. The tunnel's visibility grain (24 still frames, tunnel_burst.py):
  2.27-2.33 % / p90 6.45-6.67 % -> 2.28 % / 6.67 %, the lit frame the same; in motion (motion_err.py, moving frame vs
  its still retake) turning 90 degrees/s 1.95-2.00 / 1.94-2.02 mean |d|, flying 4 m/s 2.82-3.32 / 3.27-3.36 -- the same.
  Pass_SkyVis 0.53-0.59 ms on the grove (the A/B's three dispatches 0.57-0.60), 0.30-0.31 in the tunnel.
  `skyVis.filterFirst:0` = the filter after the history (a third dispatch, `CSFilterHistory`), the A/B. Tried first,
  no help: UE GeometryAwareUpsample's normal weights in the lighting upsample, the tap's plane distance instead of the
  depth step (the fold is coplanar with the floor), normal power 1-2, a ray origin on the eye's side of the normal;
  `skyVis.filterDepth:0.01` calmed the hem and doubled the tunnel's grain. Also dropped: the IGN unshifted plus each
  texel's R2 sequence in time (grain 3.7 % / p90 11.8 % with the gather first). `skyVis.filterNormalPower:0` was NaN
  (pow(0, 0)) -- guarded. `skyVis.debug:1` (and :2, which sets it too) paints texels without history as the open sky.
- **Dancing: the history is 0.995** (2026-10-27, owner: "remove SkyVis's dancing", in his tent, cam 71.17 2.11 41.16,
  15:00). In a closed space the sky comes through small openings, and with the frame-count blend a still texel's
  estimate drifts by 1 / (1 + frames) of a few new rays a frame; the 9x9 filter turns that into crawling blotches.
  Not the wind: the visibility alone (`skyVis.debug:2`) measured the same with the wind on, frozen, and with the palms'
  deformed BLAS off (`rt.windBlas:0`), within the spread of two runs of one arm. 60 consecutive frames, wind on, two
  runs each (scratchpad `dance.py`): the fabric's frame-to-frame std median 1.5 / 0.67 / 0.40 % at 0.98 / 0.99 /
  0.995, p90 3.1 / 1.45 / 0.98 %; the floor 1.4 / 0.64 / 0.33 %; 16 rays at 0.98 only 0.94-1.18 % (+0.1-0.27 ms).
  Free. A moving camera is unaffected (a rejected texel counts from zero); a surface whose sky a MOVED object changes
  follows it over up to 199 frames instead of 49. The settings file's `skyVis.history` overrides it (GI tab "History").
- Developer Controls > GI "Sky visibility": on/off, rays, history, reach, diffuse, specular, spatial filter, its radius,
  the seed -- persisted in graphics_settings.json `skyVis` (`--set=skyVis.*` the same knobs, transient); "Reset GI to
  defaults" resets them with the probes. The filter's depth/normal weights stay `--set` only.
- **Where its time goes, and the seed searched near first** (2026-11-09, the owner's GPU pass on his 18:00 view, cam
  37.93 4.63 71.59, a grove of fronds; GPU sub-scopes `SkyVis.Trace` / `SkyVis.Accumulate`, alternated in one process):
  the trace 0.31 ms (8 rays, two any-hit queries each; 6 rays 0.23), the accumulate 0.17 -- of which the SEED 0.11:
  standing still, texels on the fronds' edges lose their history under the DLSS jitter every frame, and a texel to seed
  reprojected the history of each of 80 neighbours (velocity, four corners, four counts) while its wave's 63 other
  lanes waited. Now it searches the 5x5 first (UE's own reach, HALF_HISTORY_FILL_KERNEL_SIZE 2) and the ring out to 9x9
  only when that holds nothing kept -- the rows a camera flying backwards brings in still reach 4. The accumulate 0.17 ->
  0.115 ms; the still frame the same (the 18:00 view 1.0-1.4 % of pixels over 8 levels either way, the same arm twice
  1.0 %; the bathroom by day within its own drift). `--set skyVis.seedNearFirst:0` = the one 9x9 search (transient).
  The filter's radius (4 / 3 / 2: ~0.025 ms a step) and the seed (off: -0.04 ms, the flying band) are in the UI.
  Tried and DROPPED, each measured against the same arm: the visibility query asking both masks at once (an any-hit
  over solid and transmissive, the solids alone only after a cutout) -- the trace 0.32 -> 0.34 ms, the fog -0.01, GI the
  same; and the group's 16x16 tile of rays and normals in groupshared for the 9x9 gather and the seed -- the seed got
  cheaper, the gather slower than the texture cache's (0.061 -> 0.108 ms without the seed): the sum the same.
- A/B it with the exposure PINNED (`exposure.autoExposure:0` after metering): the metering brightens the whole
  frame to make up for the darker shade, which reads as "the sky got brighter".
- **Without RT: the sky from above** (`skyVis.fallback`, default 1; 2026-11-09, owner with skyVis and GI off for the frame
  time, the island at 18:00, cam 88.11 4.35 61.76: "why is everything so pale without skyVis -- make it look right too, but
  the skyVis frame must not get worse"). Off, the sky lit every point as if it saw all of it: 95 % of the difference to the
  RT frame was the sky DIFFUSE (crowns +23 of 255, trunks and the camp +32, the sand under the palms +24; the sky specular
  7 %, the fog's air 2.5 %), and GTAO's sky walk at 25 m (`gtao.skyIntensity`, 0 in wind_test) closed 15 % of it -- from the
  screen it cannot see the crown over a trunk. Now, on a frame the RT trace does not run (skyVis off, or no RT hardware),
  `Main_SkyVisFallback` writes the SAME target the trace writes -- UE DFAO's bent normal, .w the depth -- so lighting,
  compose, glass, the RT resolve and the night camera's open-sky share read it unchanged (`SkyVisApplied()`). What it
  traces against is the world from above (`app/scene/SkyOcclusionMap`, `shaders/sky_occlusion_cs.hlsl`): the casters drawn
  top-down and bottom-up and the terrain top-down (D16, 4 depth texels a map texel, +-500 m of height, CPU `RenderShadow`
  at LOD 2, Pass_ShoreDepth's road; `Main_SkyOcclusionMap`), reduced into levels of 1 and 4 map texels (1 m and 4 m by
  default) of (coverage, coverage x top, coverage x bottom, ground) (`Main_SkyOcclusionReduce`; premultiplied, so a
  bilinear read halves a slab's coverage instead of inventing one at half its height). A texel is a slab of air from its bottom to its top that passes 1 - coverage straight
  down; 12 cosine directions (the same at every pixel: no noise, no history) march 8 quadratic steps out to
  `skyVis.maxDistance`, losing exp(-sigma x the length inside a slab) and ending under the ground. Measured on the owner's
  view, the exposure pinned to the RT frame's: pixels over 8 levels off the RT frame 29.4 % -> 8.8 %, crowns +23 -> +3.5,
  trunks +32 -> +5, the shaded sand +24 -> +3; the grove at 15:00 [34, 3.5, 68] 6.5 % of the gap left. Cost: the pass
  0.056 ms, the frame +0.075 ms (the RT trace +0.24 ms over that); a redraw 0.37 ms GPU + 0.45 ms of recording, on a 16 m
  snap step of the camera, a change of the caster set, or a static caster moved (an editor drag; at most every 30 frames,
  never for physics, buoyant or animated movers). With the RT trace on the fallback never runs: the RT frame with
  `skyVis.fallback` 1 / 0 differs by 0.39-0.41 % of pixels, the same arm twice 0.37 % (wind at one clock, clouds off).
  THE LIMIT: from above it sees a crown, a roof or a rock, not a wall under a roof (hidden from above by the roof, from below
  by the ceiling) -- a room's horizontal directions see the open sky: the bathroom by day (cam -24.90 1.35 89.34, 15:00)
  meters 1.15 stops brighter than with RT, at the RT frame's exposure +55 of 255 over it (without the fallback +179, white),
  its cabinet's side walls lit blue; the tent likewise. Not the fog's air (`skyVis.fogRays`
  stays RT-only) and not GI. Knobs: Developer Controls > GI "Without RT: sky from above" (graphics_settings.json
  `skyVis.fallback`), `--set skyVis.fallbackStart` (0.5 m: nearer, no slab -- the receiver's own texel is one; against the
  RT frame 1 m 12.8 %, 0.5 m 8.8 %, 0 7.9 % with more self-shadowing of objects), `skyVis.fallbackOpacity` (1; 0.8 was
  worse everywhere). `skyVis.debug:3` = the visibility at one times it (2 is four times). Debug with skyVis off asserted
  on every frame before this: the volumetric fog's mtDep list held `fogTracesSky ? pBuildAS : -1` (Release dropped the -1
  silently); a run-time list now, and an index past the graph names its pass as a Fatal before the assert.
  **Its size and its edge** (same evening, owner: "at the texture's edge the transition shows strongly -- blur it, and a
  setting for the texture's resolution and the texel's size in the world"). The first cut covered 256 m, and the back of
  the grove from the owner's view (120-160 m off) lay outside it: a pale band behind the palms where the march left the map
  and the open sky came in at once. `skyVis.fallbackResolution` (the map's texels a side, a power of two 64-1024, default
  512) and `skyVis.fallbackTexel` (m, 0.125-4, default 1) -- the map covers their product, 512 m now (26 MB: 3 x 8 MB of
  D16, the levels 2 MB); Developer Controls > GI "Map resolution" / "Map texel", graphics_settings.json `skyVis.*`, `--set`.
  The texel only redraws; the resolution re-creates the textures at a GPU-idle point before PrepareViews
  (`Scene::ReconcileSkyOcclusion` -> `SkyOcclusionMap::Resize`, which also frees them while the map is not wanted -- the
  RT trace on). And the edge fades (`skyVis.fallbackFade`, "Edge fade", default 0.25): the occlusion gives way to the open
  sky with the horizontal distance from the CAMERA, over that share of the radius the map traces in full whatever the
  camera's place in its snap cell (half the width, less half a snap step and the reach: 233 m at the default; 175 -> 233 m)
  -- a gradient round the camera instead of the map's square in the world. Pixels over 8 levels off the RT frame, owner's
  view / [-75, 28, 40] toward [10, 0, 90] / 110 m up: 256 m hard 6.2 / 7.3 / 2.8 %, 512 m hard 5.0 / 4.6 / 2.6 %, 512 m and
  0.25 4.7 / 4.5 / 2.5 % (0.5, fading from 116 m, brought part of the band back: 5.7 %). 256 x 2 m covers the same 512 m
  for 6.5 MB and matches outdoors (5.0 / 5.1 / 2.5 %), but spreads a roof: the bathroom by day +89 of 255 over the RT frame
  against +52 at 1 m; 0.5 m keeps less of a crown's column (the grove 34 % of pixels off the RT frame, 1 m 17 %). The
  height was +-100 m round the camera: from 110 m up nothing was in the map (10.3 % off the RT frame, the fallback off
  10.5 %); +-500 m costs the depth 1.5 cm a step. Cost: a redraw ~0.39 ms GPU at 256 and 512 alike (the casters' draws),
  the reduce 0.016 ms, the trace 0.055 ms. GBV (`SkyOcclusionMap,SkyOcclusionReduce,SkyVisFallback,Skybox` + the Skybox
  self-test, `--sweep=skyVis.fallbackResolution:512,256,1024,64` in one Debug process): clean, nothing alive at exit.

## RT Reflections -- Glossy by the GGX Lobe

The RT reflection is ONE sharp mirror ray a pixel whatever the roughness (stochastic GGX rays were tried and parked:
noisy without a production denoiser, RT reflections plan S11). Glossiness is a filter of that image in the
reflector's screen space, `shaders/reflection_glossy_cs.hlsl` (2026-10-26, owner: "the wood under the bathroom window
reflects too much"): through a planar reflector the reflected point `hitT` behind it is a virtual image `d + hitT`
from the eye, so the lobe's spread is `tan(angle) x hitT / (d + hitT)` radians of view, x `0.5 x height x P[1][1]`
pixels a radian. The angle: GGX's half maximum (`tan theta_h = alpha sqrt(sqrt2 - 1)`), twice that for the reflected
direction, as a Gaussian's sigma. A mirror stays sharp (sigma under half a texel copies the pixel), a contact stays
crisp, a far reflection spreads. Before, S16's separable blur added `roughness x Glossy blur` (1.0) pixels: the
panelling's roughness 0.40 got 0.4 px where its lobe is ~35 -- a mirror image of the window and the curtain.

- The trace stores the hit distance in the payload's alpha (`rt_reflect_common.hlsli RtPayloadAlpha`: 0 = miss,
  +t = shaded off screen, -t = finished from the lit HDR); the resolve decides by the sign.
- `Pass_Reflection.Blur` builds two pyramids on the reflection's grid (`reflection_pyramid_cs.hlsl`, the HZB's
  UAV-chain rule): the reflection (after the temporal resolve) and the GEOMETRY under it (world normal, device depth).
  The gather takes 16 taps on a golden-angle spiral out to 2 sigma, each read at the mip its spacing spans, weighted by
  the Gaussian, the centre's plane (2 % of the view distance) and normals^8 -- the bathtub in front of the panelling
  is another surface. Reading the taps' depth/normal from the full-resolution G-buffer missed the cache and cost
  0.11 ms of the gather's 0.13; from the geometry pyramid at the tap's mip the whole filter costs (alternated x3,
  editor closed) 0.077 ms in the bathroom and 0.062 on the grove, against the old blur's 0.024 / 0.022. The tap count
  barely mattered (8 vs 16: the same) -- the footprint's cache misses did.
- RT only (it needs the hit distance); SSR keeps the old blur and "Glossy blur" (`reflections.glossyBlur`) is now the
  SSR's. `--set=reflections.lobe:0` = the old blur on RT too, the A/B. Verified: the bathroom's frame-to-frame
  noise 1.72 % (1.8 before), mean brightness unchanged (energy kept: the DFG weight is compose's as before), the
  mirror sharper than the old blur left it, GBV (Reflection.Blur, RTTrace, RTResolve) clean.
- **An off-screen hit's SKY is occluded by the reflector's sky visibility** (2026-10-26, owner with SSR and RT shots of
  the bathroom, cam -25.27 0.64 88.47: "SSR is much more realistic" -- RT's reflections were blue). A hit shaded off
  screen used to add the sky's fill, the ground bounce and the environment specular at full strength, the open sky in
  a closed room. The trace now writes that sky part apart (`rtPayloadSky`, rgba16f at the reflection's size) and the
  resolve multiplies it by the REFLECTOR's plan-S visibility (`RtReflectorSkyVisibility`, lerped by `skyVis.diffuse`):
  the reflected point sees about the sky the reflector sees. A reuse hit (finished from the lit HDR) takes no
  environment term at all -- its lit colour already holds the occluded sky specular, which was the double count. The
  glass dispatch does the same with the visibility the opaque surface behind it lends. Bindless scene slots 46-49
  (`kScenePerFrame` 54): the trace's sky payload UAV, the resolve's SRV of it and of the sky visibility, the glass's.
- **The mirror ray stays above the reflector's GEOMETRIC plane** (`rt_reflect_common.hlsli RtSetupReflectionRay`,
  2026-10-28, owner at the bathroom window, cam -24.69 1.50 89.12 at 16:00: "the walls are thick, so what leaks here?").
  GB1 holds the SHADING normal; on a grazing view the normal map turned the mirror ray below the surface it left (the
  wainscot's grooves, the sill's grain), the clearance measured along that normal (TMin up to 20x the offset) skipped
  the panel itself, and the ray left the room through the wall behind it onto the sunlit beach: the boards under
  the window glowed with sand. Now the plane is rebuilt from the depth one reflection texel either side (lighting_cs
  SunGeometricNormal's rule: the nearer neighbour per axis, no plane across a 3 % depth step), a ray below it is folded
  back (its mirror image in the plane), the origin is offset along THAT normal and TMin is the offset alone; an edge
  pixel keeps the shading normal and P16.11's grazing TMin. Not the grazing TMin with the plane: the first version kept
  it, and a folded ray grazing up the boards jumped the sill above them and left by the window into the swaying palms
  -- a patch by the faucet that came and went with the wind, 11-50 of 255 (owner: "the spot appears and disappears");
  now 4-7 (the code before all this: 9.5-14; no reflection: 5.9). UE biases the origin along the normal too
  (RayTracingCommon.ush:393-407) with a small TMin (LumenReflectionHardwareRayTracing.usf:153-170). Exposure pinned:
  the boards 17.0 -> 8.7 (the sky-only reflection is 8.3), the sill 56 -> 18, the tent's cloth lost the sand glow on
  its folds (part of the leak earlier put on the GI). Other views (|d| > 8 vs the same arm shot twice): bathroom 1.0 %
  (0.14), mirror wall 0.45 % (0.04), grove 0.92 % (0.68), tunnel 0.32 % (0.37), means within 1.7 %; P16.11's bronze
  floor (ssr_bronze_palms, its camera and two grazing ones) 0.2-0.3 %, no band. Pass_RTTrace: the same within the noise
  of a contended GPU (0.16-0.24 ms both arms; the first version measured clean was 0.141 -> 0.126 at the window, 0.149
  -> 0.092 in the tent -- the escaped rays crossed the world). `--set reflections.geometricNormal:0` = before (the A/B).
  GBV (`RTTrace,GlassReflections,Skybox` + the Skybox self-test): clean.
- **The temporal resolve also follows the reflection HIT** (`ssr_temporal_cs.hlsl`, 2026-10-26, owner: "the reflections'
  temporal lags in motion -- the metal walls in the tunnel"). Its history followed the REFLECTOR's velocity; a mirror's
  image is a virtual scene `hitT` behind the surface along the view ray and slides across it with THAT depth's parallax,
  so the sunlit tunnel mouth in the metal walls trailed a flying camera. Now Lumen's pair
  (LumenReflectionDenoiserTemporal.usf:362-507): a second history reprojected by the virtual point at `d + hitT` (the
  payload's alpha; a miss = the far plane), for narrow lobes only (UE GetSpecularDominantDirFactor > 0.5, roughness up to
  ~0.6), both through the same YCoCg clamp, the one whose unclamped read lies nearer the neighbourhood's centre wins (the
  surface's at 0.8). RT only -- SSR and the water keep the reflector's (`hitReprojection` 0, t3-t5 placeholders).
  Owner's tunnel, moving frame vs its still retake, DLSS Balanced + bloom: flying 8 m/s 5.91-5.98 -> 4.98-5.20 (the
  resolve off: 4.49 -- ~70 % of what it added is gone); turning 90 degrees/s unchanged, 2.37 / 2.45 (a pure turn moves
  both points alike; the 0.7 the resolve still adds there is its bilinear resampling's softening, not lag). Still: the
  same (tunnel 3.1-3.25 %, house 2.2-2.6 %). +0.002 ms. `--set reflections.hitReprojection:0` = before (the A/B). GBV
  (the reflections' CL group, a flying camera) clean. MEASURING it: name `ssr.temporal`, `reflections.*` and
  `skyVis.specular/diffuse` in every arm's base -- they are project knobs and an arm that turned one off left it off
  for every later arm (a whole afternoon's wrong conclusions, 2026-10-26).

## GI Probe Volume (DDGI) -- the Bounce

`rendering/lighting/DdgiVolume` (resources, per-frame placement) + `SceneRenderer_Gi.cpp` (Main_GiTrace, Main_GiUpdate)
+ `shaders/ddgi_common.hlsli` / `ddgi_trace_cs.hlsl` / `ddgi_update_cs.hlsl`: RTXGI's layout and formulas over inline
RayQuery. 32x16x32 probes `gi.spacing` (0.5 m) apart, camera-centred and SCROLLING (a slot records the cell and the
generation it was traced for; a mismatch weighs 0); 1 / `gi.divisor` (4) of them trace 64 rays a frame (+ RTXGI's 32
FIXED rays, below), blended with `gi.hysteresis` (0.98) into octahedral irradiance (6x6, stored as E^(1/5)) and
depth-moment (14x14) atlases. Interior lighting plan D.
0.98, not RTXGI's 0.97: at 64 rays an update 0.9 left the bathroom breathing in metre-sized blotches (frame-to-
frame std 0.68 sRGB levels; 0.37 at 0.97, 0.29 at 0.98, 0.17 without GI -- scratchpad flicker_map: per-pixel std
over 8 clean shots 3 frames apart). The divisor does not change that noise, only how fast it moves.

- **The FAR cascade** (owner 2026-10-26, "сделай второй каскад проб"): `ddgi_[1]`, the same 32x16x32 probes
  `gi.farSpacing` (2 m) apart -- 64 x 32 x 64 m around the camera, where the near one is 16 x 8 x 16 (+-4 m up and
  down: a camera 5 m up had NO bounce on the ground). Same shaders, its own atlases/state (`DdgiFar.*`), bindless
  slots 42-45, its own infinite bounce; traced and updated in the same two passes (a second dispatch each, GPU
  scopes `Pass_GiTraceFar` / `Pass_GiUpdateFar`), 1 / `gi.farDivisor` (4) of its probes a frame. Lighting
  (t21-t23, `ddgiFarBase/Params`) takes the near cascade where it is whole and the far one for what it leaves --
  beyond it, the near one's outer 2 m (its edge fade widens to `kGiNearFadeMetres` while the far one is applied),
  a point no usable near probe surrounds; a pixel well inside the near cascade never reads it. `gi.far:0` = the near
  one alone. Developer Controls > GI "Far cascade" / "Far spacing", persisted (`gi.farCascade`, `gi.farSpacing`).
  Cost, wind_test's start camera, interleaved x3 in one binary: CPU.Frame 3.648 -> 3.889 ms (+0.24) at divisor 4,
  3.840 (+0.19) at 8; far trace 0.113 / 0.070 ms, far update 0.094 / 0.043, Lighting 0.179 -> 0.244 (that view
  is mostly outside the near cascade). VRAM ~51 MB (rays 25.2 -- every probe's row, for any divisor -- irradiance
  8.4, depth 16.8, state 1.0). In the frame the difference is subtle outdoors (the analytic ground bounce
  approximates an open-sky bounce well); `gi.debug:1` shows what it covers (red = no probes).
- The probes hold the BOUNCE only: a ray that misses brings 0. The sky reaches a surface through plan S (RT sky
  visibility), and lighting_cs ADDS the bounce to the indirect diffuse -- occluded by the AO, never by the sky
  visibility -- so nothing is counted twice and the volume's edge fades an addition, not a seam. Inside the volume it
  replaces the analytic `GroundBounceOverPi`, which approximates the same light.
- A hit is shaded diffusely: the directional light with an RT shadow ray, every spot/point light in reach with its
  shadow ray, the sky through ONE cosine visibility ray (plan S's reach), the volume's previous irradiance (the
  infinite bounce, `gi.infiniteBounce`). A back-face hit of a one-sided surface stores -0.2 t (RTXGI): more than a
  quarter of the 32 FIXED rays (`kDdgiFixedRays`: RTXGI's, the same directions every update, never blended, traced
  to their hit and not lit) switches the probe off (inside geometry). Masked geometry (leaf cards) is two-sided.
- **So is a two-sided material** (2026-10-27, owner: "what are these squares inside the tent", cam 71.17 2.11 41.16):
  the trace read every OPAQUE back face as the inside of a solid, and the tent's fabric (glTF doubleSided, `twoSided`
  in tent_2) seen from inside was walls all round -- the probes in the tent switched off or were relocated OUT
  through the fabric, and its floor took the sunlit outside's bounce in 0.5 m cells. The RT record carries the
  material's `doubleSided` as `kGeomFlagTwoSided` (`GeometryInfo.flags` bit1, BindlessTable / RtSceneAs), and a back
  face of such a surface is flipped like a leaf card's. `gi.twoSidedSheets:0` = before (the A/B). Tent: mean 109 ->
  47 of 255, no cells (a closed tent is dark: the fabric does not transmit). Sketchfab marks nearly every material
  doubleSided (108 of 175 here), so the bathroom was the check: day and night the same (mean +0.2 / -0.1, |d| > 8 on
  1.2 % / 0.05 % of the frame, flicker unchanged).
- **The "shadow flicker" in the corner cabinet (2026-10-25, owner: "вот тут тени мерцают")** was the bounce: in a
  shadow the probes are nearly all the light, and `gi.enabled:0` took the niche's frame-to-frame std (candles
  steady, exposure pinned, 16 clean shots) from 1.4 % to 0. Three causes, each RTXGI's own answer: (1) classified
  on the 64 RANDOMLY rotated rays, a probe beside the cabinet wall saw a quarter of them on back faces now and then
  and dropped out for exactly one update -- its patch +6 % for 4 frames; the fixed rays answer the same each time.
  (2) Blended linearly, a probe in the dark cabinet that sees the candle-lit room through its opening (a few rays
  of ~100x its mean) walked +-10 % an update; the atlas now holds E^(1/5) (`kDdgiIrradianceGamma`, RTXGI's
  probeIrradianceEncodingGamma; DdgiIrradiance decodes to E^(1/2) per probe and squares the blend), and the large-
  change test compares the mean of the NEW texels with the old ones in that encoding (the rays' arithmetic mean let
  one hot ray read as a 16x brighter probe). (3) The depth texel (cos^50 lobe, 0.63 rays an update on average, RTXGI
  tunes it for 256) took a lone ray 40 degrees off as a full estimate: it now enters by its weight, at a slower
  `1 - (1 - gi.hysteresis) x 64/256` = 0.995 (geometry rarely moves; a moved door settles its visibility in ~200
  updates). Owner's view (cam -24.90 1.35 89.34, rot -0.0431 -0.4556 -0.0221 0.8889, 00:00), HEAD vs now, candles
  steady: niche median 1.66 -> 1.10 %, p90 3.56 -> 2.56 %; the patch behind the cups 1.76 -> 0.11 % (largest step a
  frame 6.6 -> 0.2 %), the corner under the towel 10.4 -> 2.7 % (12.3 -> 2.5 %). With the level's candle flicker the
  niche stays at 2.7 % -- that is the flicker itself. Price: E^(1/5) averages a noisy probe low (Jensen): the room
  view -2.1 %, the niche view -3.7 %, its darkest corner -28 % (linear blending with the other fixes: -1.6 % on the
  niche, but hot patches 2.2 % instead of 1.4 %). Cost: trace 0.134 ms, update 0.068 ms (0.14 / 0.11 before); the ray
  target 25 MB (+8). A noon -> 23:00 jump still follows: 0.5 % of the day's bounce left at +40 frames.
- **Probe relocation** (RTXGI ProbeRelocationCS, owner: "добей остаток в углу шкафа"): ddgi_update_cs `DdgiRelocate`
  moves a probe by its 32 fixed rays -- inside geometry (a quarter on back faces) through the nearest back face and
  half the minimum beyond; nearer a front face than `kDdgiMinFrontfaceSpacing` (0.225 spacing = DdgiIrradiance's
  surface bias reach; RTXGI's probeMinFrontfaceDistance is 1 world unit) toward the farthest one by just what clears
  it, below 0.9 of the minimum only; at most 0.45 spacing off its grid point (clamped, where RTXGI rejects the move).
  RTXGI's move back toward the grid point is left out (a probe pushed out of a wall would walk back in and out), and
  a cell allows `kDdgiMaxMoves` (3, state bits 6-7): RTXGI runs relocation for a while and switches it off; without
  the budget probes between the one-sided curtain and its wall stepped clear of one face onto another none of the 32
  rays saw and started over every update (the curtain: p90 3.8 -> 7.1 %, 7 % of it over 10 % frame to frame). A
  door moved afterwards is not relocated around until the cell is re-entered or the generation changes. The
  offset is the second uint4 of the probe's slot in the state buffer (`DdgiStateIndex` / `DdgiOffsetIndex`); the
  trace starts there, DdgiIrradiance weighs visibility from there (trilinear weights stay the grid's), and a probe
  that moved starts over at its next update (`kDdgiStateMoved`: its history was seen from elsewhere). The fixed
  rays make it a fixed point: settled, nothing moves (gi.debug:2 after 3000 frames: no red in the niche or the room). `gi.debug:2` shows it -- per point, the 8 probes around it:
  red moved by their last update (red that stays = a probe that never settles), green off their grid point (most
  surfaces: the probes behind a wall were pushed out of it), blue inside geometry. The corner cabinet, candles steady,
  two launches: niche median 1.10 -> 0.94 / 0.93 %, p90 2.56 -> 2.41 / 2.40 %, the corner under the towel 2.7 ->
  1.0 / 1.1 % (largest step a frame 2.5 -> 1.0 %), the curtain beside it 2.16 -> 2.11 / 2.07 % (p90 3.8 -> 3.6 /
  3.5); HEAD had 1.66 % / 10.4 % / 2.86 % there. The patch behind the cups is ~15-20 % brighter (a probe that sat
  switched off in the cabinet's back now lights it). Cost alternated on one exe
  (fixes / + relocation, twice each): trace 0.153, 0.135 / 0.135, 0.142 ms, update 0.081, 0.071 / 0.069, 0.066 ms
  -- launch noise, nothing measurable. The noon -> 23:00 jump: nothing of the day left at +40 frames.
- Bindless scene slots 32-39 are the trace's (`BindlessTable::kScenePerFrame` 54). Lighting binds t18-t20 whenever
  the volume exists and reads them when `ddgiParams.x` (the spacing) is non-zero -- `decisions_.giApplied`.
- `Pass_BuildAS`'s recording task now fans out to six RT passes; `TaskSystemLockFree` dependents_ holds 8 (4 aborted
  the first frame with a bare 0xC0000409 and no log line -- `--log-sync` did not help; the minidump's stack scan did).
- Cost (wind_test, 1440p DLSS Balanced): trace 0.10 ms in the bathroom, 0.18 ms on the grove; update 0.06 ms.
  `gi.debug:1` shows the bounce alone (the lamps' own passes still add), RED where no usable probe surrounds a
  point; `gi.debug:2` the probes' relocation and classification (below). `gi.enabled:0` = the frame before it.
  Developer Controls > GI: on/off, spacing, hysteresis, intensity -- persisted in graphics_settings.json `gi`,
  which overrides the compiled defaults at boot (a `--set gi.*` still wins, it is applied after) -- and the debug
  view (not persisted). The trace's price is persisted too since 2026-11-09 ("Trace cost", below).
- A black patch WITH probes around it is albedo, not a hole: the_bathroom_free's dark wood (cabinet, wainscot, floor)
  measured albedo x AO ~0.07 under a bounce ~1/16 of the candle-lit wallpaper -- check `gi.debug` before blaming GI.
- A/B a look with the exposure pinned to the metered value (`exposure.autoExposure:0;exposure.manualEv100:<adapted
  EV100 from get_readouts>`), not `autoExposure:0` alone -- that jumps to the manual default.
- A LARGE change is followed fast (RTXGI's probeIrradianceThreshold, made scale-free -- the units run from the
  day's 1e5 to the boosted night): when the mean of a probe's new texels and of its previous ones (both as
  E^(1/5), the atlas's encoding, so 4^(1/5) there) differ by more than 4x in the same direction for 3 updates running
  (ProbeState bits 2-4), the irradiance blends at hysteresis - 0.75 until they are within 1.25x; the depth keeps its
  own slower hysteresis. A noon -> 23:00 jump
  (setTime, the slider): 0.98 alone still had +26 % of the day's bounce at a palm's foot 400 frames later, now +3 %
  at 40. The confirmation is what keeps 0.98's calm: taken at once, the 4x test pulsed a probe under the
  bathroom's countertop that sees the lit room through a few rays (+50..+89 % for 4-24 frames, 12 % frame to frame
  on the cabinet); confirmed, 0.05 % -- as before it.
- **What the trace pays for, and the probes nobody looks at** (2026-11-09, owner on the heaviest view, cam 37.93 4.63
  71.59 at 18:00, both cascades, the editor closed, wind frozen, alternated twice: "go through the frame's top consumers,
  GI first, optimise the code, and give knobs for the perf"). Before: trace 0.28 + 0.13 (far), update 0.07 + 0.065,
  lighting's sampling ~0.09 ms. Inside the trace the infinite bounce's lookup at each hit was 28 % (`gi.infiniteBounce:0`
  0.20 / 0.10), the sky ray at a hit 0.04 / 0.02, the bounce's Chebyshev visibility 0.03 / 0.02, the 200 m reach 0.02 /
  0.02 against 30 m, the lamps nothing at 18:00. Three changes with the same picture, one A/B each:
  (1) **a probe inside geometry traces only its fixed rays and blends nothing** (RTXGI ProbeTraceRGS; lighting and the
  bounce never read it): `gi.skipInactive:0` = before (transient). Coming back out (a door, a palm) it starts over (the
  moved flag): its atlases are as old as its last active update. ~-0.02 near, the far update -0.02.
  (2) **the 32 fixed rays every `gi.fixedRayPeriod`-th update** (4): their directions never change, so their answers hold
  in the probe's row while it stands where it stood -- the ray buffer's row is now the probe's own (`probeIndex`, not the
  dispatch group's), and a probe new to its cell, moved, or on its turn traces them. -0.03 / -0.02.
  (3) **probes no pixel reads sleep** (`gi.sleepPeriod`, 4): `ddgi_usage_cs.hlsl`, at the head of Main_GiTrace, stamps the
  8 probes round every 4th pixel each way (another of its 4x4 block each frame: all in 16 frames), both cascades, with the
  frame number (`Ddgi.Usage`, a uint a slot); a probe unstamped for 32 frames (behind the camera, under the ground, in the
  air) updates one turn in 4, staggered by a hash, and on that turn traces its fixed rays too. The trace and the update ask
  the same function (`DdgiProbeSleeps`) with the same stamp, so they skip the same probes. Trace 0.25 / 0.12 -> 0.10 /
  0.05, update 0.075 / 0.055 -> 0.039 / 0.027, the stamping 0.024: the GI passes 0.50 -> 0.24 ms (8: 0.20), GPU frame
  5.0 -> 4.69. The picture standing still is the same (the 18:00 view 1.17-1.23 % of pixels over 8 levels against the same
  arm twice 1.22 %; the bathroom by day within its own drift); turned 180 degrees 4 s after the clock moved 18:00 -> 17:30,
  2 s later 1.75 % of pixels off the settled frame against 1.04 % without sleep (right after the turn both ~11 %: DLSS).
  `gi.sleepPeriod:1` = every turn, and no stamping pass.
  The rest are knobs with a price in the picture (Developer Controls > GI "Trace cost", graphics_settings.json `gi.*`,
  `--set gi.*`): update divisor / far divisor (4 / 4; 8 halves the trace and the update, the light follows twice as
  slowly), rays per probe (`gi.rays` 16-64, 64; 32 = -0.13 ms trace, -0.04 update, and the bathroom by day 4 levels
  DARKER -- a noisier estimate averages low as E^(1/5)), ray reach (200 m), infinite bounce, its visibility
  (`gi.bounceVisibility`; off LEAKS: the bathroom by day +12 levels), the sky ray at hits (`gi.skyRay`; off: every hit sees
  the open sky -- the bathroom washes out, +109 levels), the lamps at hits (`gi.lights`). Outdoors the last three move
  the 18:00 view 0-3 levels. Tried and left: skipping the update's cos^50 weights under 0.75 (kept: free, but the
  update is not ALU-bound -- no measurable change); `pow(e, 2.5)` and the atlas sizes as constants in DdgiIrradiance (the
  same). GBV (`GiTrace,GiUpdate,Lighting,Skybox` + the Skybox self-test, `--sweep` over every knob above): clean.
  MEASURING IT: `gi.*` survives `reset_settings` -- a `gi.far:0` arm left the far cascade off for every later one (its
  rows then read the last EMA, `uses 0` in `get_profile`); name every gi knob in each arm's base.

## Local-Light Falloff -- Metres, Not UE's Centimetres

`LightDistanceAttenuation` (`shaders/utils.hlsli`, every point/spot path: the light passes, glass, the RT
hit shading, the GI trace) is UE's `saturate(1 - (d/r)^4)^2 / (d^2 + bias)` with the bias **1 cm² =
`kLightDistanceBiasSqr` 1e-4 m²**. UE's literal `+1` is in UE units (cm); copied as `+1 m²` it dimmed every
lamp near its source -- 2x at 1 m, 5x at 0.5 m, 17x at 0.25 m -- until 2026-10-24. The volumetric fog's
`max(cellRadius * scale, 1)` floor is 0.01 m for the same reason. Any UE constant that is a LENGTH (a bias,
a radius, a distance) must be divided by 100 when it is transcribed. A light's `radius` is UE's attenuation
radius -- a culling window, not physics: it cuts 12 % at r/2 and 65 % at 0.8 r, so a room-sized light wants a
radius about twice the room.

**Checked against UE 5.8 itself (2026-10-24)**: the source (`DeferredLightingCommon.ush` / `CapsuleLightIntegrate.ush`
`rcp(DistSqr + DistBiasSqr)`, `DistBiasSqr = 1` cm², `PointLightComponent.cpp:145` lumens x 1e4 / 4pi) and a render:
the_bathroom_free imported into the owner's UE (Interchange `import_scene`, an unsaved blank map), the five candles
4 lm / 12 m / (1, 0.7, 0.42) at the fitted positions (0.2 cm), the moon 0.32 lux, manual exposure matched (UE
manual = 2^bias with the physical camera off, so bias = log2(1.44) + log2(nightBrightness) - our EV100), a linear
PPV (ToneCurveAmount 0, no bloom / local exposure). Direct light only (UE Lumen off, our GI off): ours / UE 1.2-1.6
at all eight points of the reference view, near a candle and across the room alike -- the same falloff, a uniform
~0.45-stop calibration offset of the comparison. With GI: ours / UE's PATH TRACER 1.14-1.70 (the same offset: our
DDGI within ~20 % of the reference), while Lumen gives about half that bounce in this room. Spot lights DIFFER:
UE 5.8 puts a spot's lumens into its cone (`SpotLightComponent.cpp:97`, 1e4 / (2pi (1 - cos outer))), ours spreads
them over the sphere (`CandelaFromLumens`, UE4's old rule) -- with a 30-degree outer half-angle the same lumens
are ~15x brighter in UE. Not changed (every spot in the levels would brighten); decide before porting a UE spot.
