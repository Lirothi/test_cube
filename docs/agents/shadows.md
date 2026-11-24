# Shadows

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## VSM Is UE's High -- the Clipmap, the Pool, the Marks

2026-11-06, owner: "let's match UE's VSM, as they have it on High: as many pages, the same physical layout, the selection heuristic". UE 5.8.2 at `sg.ShadowQuality` 2, read off the owner's running editor and the source; the table with UE's files and lines: `docs/vsm_ue_high_plan.md`.

- **The pool**: 2048 pages, 128 x 16 (`vsm::kPoolPagesX / Y`), D32 16384 x 2048, 134 MB. No second slice for statics: ours caches in one.
- **The levels**: `render::kShadowViewClipmapLevels` 17, absolute numbers `vsm.clipmapFirstLevel` (6) .. 22. Level L must cover a radius of 2^(L+1) cm, is 4 radii across, 16384 texels, 128 x 128 pages, no mips: level 6 is 5.12 m (0.31 mm a texel), level 22 is 168 km. The page table is [prefix local views | 17 x 16384 | the other local views], `vsm::ViewBase` / `VsmViewBase` the one place that knows, 3.07 M entries (12 MB) at the initial capacity, 9.37 M with wind_test's "Lamps" on, 17.8 M (71 MB) at 128 + 32 shadowed lights. `kPageIdShift` 21: 2M casters, 2048 pages.
- **What a pixel asks for is its distance**, nothing else (`vsm_page_request_cs.hlsl MarkClipmap`, UE GetBiasedClipmapLevel): floor(log2(d cm) + bias) - first, one page. bias = max(0, `vsm.resolutionLodBiasDirectional` (High 0, Epic -1.5) + log2(0.5 / P00 x 16384 / the RENDER width)) (`vsm::ClipmapLodBias`, `g_clipmapFrameBias`) + the dynamic resolution's. DLSS Balanced at 2560 is 2.46, native 1.68. A texel under DLSS Balanced: 2.5 mm at 1 m, 1 cm at 4 m, 2 cm at 10 m, 16 cm at 100 m; native half that. Every second pixel (`vsm.requestDownscale` 2), the page across an edge within `vsm.pageDilation` 0.05 by one diagonal dithered over the group; a pixel turned from the light marks nothing (`vsm.cullBackfacingPixels`; foliage does, it passes light).
- **What a sample reads** (`VsmClipmapShadowResident`, UE GetMappedClipmap): from floor(log2(d cm)) without the bias, the first level with a page there, through the propagate's pointers. No blend between levels (UE has none; `vsm.clipmapBlend*` and `vsm.smrtLevelMargin` are gone). The march stays in the receiver's level and a sample whose page is not mapped there reads the coarser level that holds it, its depth brought back, (z_L - b_L) 2^(L - level) + b_level: the samples next to the receiver read the receiver's own level however far up the ray went. SMRT 8 rays, 4 steps, slope clamp 0.05, UE's dither.
- **The snap and the pan** (`Scene::UpdateClipmap`, `PlanClipmapPan`): a centre snaps to whole radii, 32 pages, so a level's texel grid keeps its world place, and its pages MOVE with it (`vsm_page_alloc_remap_cs.hlsl`, bits.z; UE's VSM_NEXT_FLAG_KEEP_PAGE), the matrix's _41 / _42 being no change to the dirty test. The matrix is written out, not LookAtLH: a dot with the centre rounds differently at every snap and reads as a change in depth. Along the sun the centre stays while the camera is inside 0.9 of the +-1000 radii. `vsm.clipmapPan:0` = the level redraws, the A/B. Truth: rigid casters, the camera walked across many snaps, the frame out of moved pages against the same pose redrawn from nothing 0.044-0.050 % of pixels over 8 levels, the same pose twice 0.020-0.023 %.
- **Coarse pages** (`MarkCoarsePages`, thread (0, 0); `vsm.markCoarsePages` bit 0 / 1): four pages round the camera on levels 15..18 and the last, each local view's last mip, so the air and the far end of a ray have something to read. In a bitfield of their own behind the pixels' (`Request`: [pixels | the water's mask | coarse]), because a page only they asked for is a kind of its own, UE's MarkCoarsePages 2 (`vsm.coarseCacheDynamic`): no dirt from the wind or a moving caster, no instance under `vsm.coarsePagePixelsStatic` 1 / `...Dynamic` 16 texels, and when what asks for a page changes it redraws (`PageDetail`, `vsm_page_bounds_cs.hlsl`). Without this a floating boat redrew pages of 20 m to 2.6 km every frame: the tent 0.53 -> 0.19 ms. The size threshold is worth 0.06-0.10 ms with the sun moving and nothing still.
- **What hung on a level's number**: the wind is a distance now, `vsm.windRange` (64 m, `graphics_settings.json` windRange; `vsm::ClipmapWindLevels`: the levels the pixels within it ask for); the classic caster LOD's `ClipmapTierShift()` follows level 0's outer distance, -4 at 2.46 (a DLSS mode that crosses a whole step rebuilds the casters); the single tap's constant bias is in today's texels (`vsm::ClipmapDepthBiasNdc`: as the old 2048-texel level's number it was 20 of them, 3 m on the coarse level the air reads).
- **The wind redraws only what sways, and a far level only when its pose has moved** (2026-11-09, the owner's GPU pass on his 18:00 view, cam 37.93 4.63 71.59: while the wind blew, `Pass_VsmPageRender` was 0.67 ms of which 0.44 the clusters of pages redrawn for the wind -- `vsm.windRange:0` 0.20 -- and the frame paid 0.55 ms for it, more than the pass: `Pass_RTTrace` on the async queue stretched 0.16 -> 0.33 beside it). Two things, each its own A/B (`--set`, Developer Controls > VSM, transient). (1) `vsm.windPageMarks` (1): the wind dirtied EVERY page of its levels; now only a page a swaying caster lies on (the caster meta's bit 1, `ShadowGpuData::kCasterMeta*` / `VSM_CASTER_META_*`, the slot count moved to bits 2..) and sways in -- the shadow VS's sway fades to 0 at `vsm.windRange` from the camera in xz, so a box wholly past it this frame and last draws one rigid pose (the scatter's `SwaysNearCamera`, `PageScatterDyn` bit 1, UE's per-instance WPO invalidation) -- and none at all while the wind holds still (the same clock, direction, amplitude, frequency and gust, the camera where it was). A page with nothing swaying draws the same depth: against `vsm.pageCaching:0` (every page every frame) at the floor, still and after a camera cut. (2) `vsm.windSliceTexels` (2): a clipmap level takes the wind's pose -- its clock, gust and the camera the sway fades round, held per level in the setup CB (`gWindLevelPose`, `gWindTickMask`) and written into every page of that level -- only once the fastest swaying vertex could have moved that many of the level's texels (`ShadowGpuData::WindSpeedBound`: wind.hlsli WindOffset's terms at their maxima over the swaying casters, x1.5; `WindOffsetBound` for the fade's camera), and its swaying pages redraw only then. A level whose texel the sway crosses every frame takes every frame; at 200 fps on wind_test (bound 4.3 m/s) the 4 / 8 cm levels take every 2nd-4th. The pose a page shows lags its tree by at most that many of its own texels. Checked by stepping the wind's clock by hand (`wind.freezeAt` + 5 ms a shot, a 200 fps sequence; scratchpad-style `windlag.py`) against the same steps unsliced on sand masks: 1 and 2 texels at the floor (0.03-0.10 % of pixels over 8 levels, the same arm twice 0.04-0.10 %), 4 just over it (0.24-0.33 %), the control 64 5.5 %. A probe window's per-frame mean did NOT see even 64 (the palms in the frame sway): not a test of this. A change of the wind's levels, its fade or the slice redraws every page once (a page drawn swaying kept that pose; the wind stopping left it frozen). Measured (alternated twice): wind running GPU frame 4.67-4.72 -> 4.54-4.56 ms, `Pass_VsmPageRender` 0.67 -> 0.53; the wind held 4.57 -> 4.16, 0.61 -> 0.25. GBV (`VsmPageRequest,VsmPageRender,Skybox` + the Skybox self-test, the view, the wind running): clean.
- **Passes made for the size**: touch, allocate and the next frame's clear go by a SUMMARY at the buffer's end, a bit per word of the bitfields (`MarkBit`; `Request`: [pixels | the water's mask | coarse | summary]): a table of millions of entries holds a few hundred marks, and one thread a word was a million threads a frame with the lamps on. `vsm_page_bounds_cs.hlsl` keeps per (view, mip) the rect it has pages in (`VsmBoundsIndex`; a caster across a lamp's plane overlaps all 21845 entries of a view, the island all 16384 of a level) for the scatter and the propagate. The scatter also drops a view with no page and a caster past the light's reach, which shadows nothing anybody reads (0.09-0.13 -> 0.04-0.06 ms; with the lamps on 0.14-0.39 -> 0.06-0.11). The HZB's "last frame's page" is `PhysOwnerPrev`, no copy of the table; the free list sorts 2048 in a 1024-thread group, two pages a thread; what is longer than 65535 groups of 64 reach (the table's first clear, the buffer's) runs in rows (`vsm::kClearRowGroups`).
- **A lamp is UE's too** (same day, owner: "take on the lamps"): a local view is 16384 texels and 8 mips, 128 x 128 pages at mip 0, 21845 entries. What a pixel asks for was already UE's (`VsmLocalMipFromPixel`, one mip + the last, MarkCoarsePages), and it holds: one point light on one floor in both engines at the same size (scratchpad `floor_common.py`, `ours_floor.py`, `ue_floor.py`; lamp on minus lamp off, far / mid / near / on the floor), native 2196 wide +90 / +151 / +215 / +209 against UE's +90 / +152 / +213 / +208; at DLSS Balanced (1485) against UE's 67 % (1471) mid +82 / +83, near +100 / +101, floor +117 / +114. A camera-distance read (`VsmSelectLevel`: the fog, glass) STARTS at that mip and walks to coarser ones, so starting finer costs table reads only; as a mark (`VsmSelectMarkLevel`, `vsm.localMipFromPixel:0`) the old law is 3 mips further on.
- **A lamp's shadow is rays, UE's TraceLocalLight** (`shaders/vsm_smrt_local.hlsli`; `vsm.localSmrtRayCount` 4, `vsm.localSmrtSamplesPerRay` 4 on High, Developer Controls > VSM, `graphics_settings.json`; `vsm.localSmrtTexelDitherScale` 2, `...ExtrapolateMaxSlope` 0.05, `...MaxSlopeBias` 50, `...MaxRayAngle` 0.03; rays 0 = the 3 x 3 PCF of one mip, the A/B). The march is one file (`vsm_smrt_march.hlsli`, UE's template), included for the sun's ray and the lamp's: its flips exist once. Direct Z adds two the sun did not need: a ray's depth step toward the light is negative, so the extrapolation's clamp takes abs, and a two-face ray's reset takes -slope. A light of no size (UE's `SourceRadius` 0, the default) takes no steps, one sample at the receiver moved by the dither, with its own short path. A light's `"sourceRadius"` (m; Inspector "Source Radius", MCP `setLight {sourceRadius}`) is how fast its penumbra widens: VSM's rays alone read it, the shading has no area light. Measured, the candles at 23:00 (alternated): `Pass_PointLights` bathroom 0.14 -> 0.17 ms, hall 0.17 -> 0.21; with 1 / 3 / 10 cm of radius 0.35 / 0.39 / 0.48. Frame to frame through DLSS the same grain as the PCF (p95 1.23-1.53 % against 1.09-1.51 %; a 4 cm radius 1.5-3.4 %). Shadows where SDSM has them, on the wall by the sconce sharper than the PCF; a hand from a candle no leak and no acne. GBV (`VsmPageRequest,VsmPageRender,PointLights,SpotLights,Skybox` + the Skybox self-test; two spots and two points with a radius, and the group "Lamps" at 1280x720): clean. The capacity's growth (433 views) went through Release and plain Debug, not through GBV.
- **Not UE's, on purpose** (the plan says why): a lamp's read walks from the receiver's mip to coarser ones where UE's flattens the table per frame (`PropagateMappedMips`: 16384 threads a view, 2 M at our 128 views, for what the walk does in a few reads); the lamps' plane is the geometry's normal where UE only has the shading one; no receiver mask; one pool layer, no static / dynamic split; the fog and glass read lamps by the 3 x 3 PCF as before; the water's marks are masked by the casters' boxes where UE's are not.

Measured 2026-11-06, the live editor, panels closed, the wind's pose and the clouds held, DLSS Balanced. Pages at 15:00, was (2 m, 12 levels, 1024 pages) -> now, UE with one sun on its own WindTest (its viewport 2196 wide at `r.ScreenPercentage` 67, so some 1471 and a bias of 2.48): beach 562 -> 281 / 236, grove 421 -> 244 / 262, aerial -> 203 / 180, open sea 84 -> 129 / 184 (its water is unmasked), bathroom 161 -> 188, second room 324, tent 57, 700 m out 119. Nothing refused on any view; `resolutionLodBiasDirectional` -1.5 (Epic) 238 -> 918 on the grove. `Pass_VsmPageRender` ms at 15:00: beach 0.41, grove 0.41 (0.56 at 2 m), tunnel 0.27, aerial 0.21, bathroom 0.20 (0.25), second room 0.26 (0.27), tent 0.19 (0.18); 18:07 beach 0.74, grove 0.69; 23:00 beach 0.33, grove 0.37, bathroom 0.20. `Pass_VsmPageRequest` 0.09-0.19 (0.06-0.07: the stride). "Lamps" on at 23:00, the lamps at 2048 -> 16384 + rays: pages grove 743 -> 766, bathroom 418 -> 514, hall 608 -> 682, aerial 549 -> 549 of 2048, nothing refused; `Pass_VsmPageRender` 0.98 -> 0.91, 0.60 -> 0.41, 0.50 -> 0.46, 0.60 -> 0.34; `Pass_PointLights` 0.08 -> 0.14, 0.12 -> 0.21, 0.15 -> 0.24, 0.04 -> 0.06; `Pass_VsmPageRequest` 0.16 -> 0.21, 0.40 -> 0.49, 0.21 -> 0.19, 0.24 -> 0.35 (each pixel walks all 210 lights: UE has a light grid). Fog's sun alone (`fog.debugView:4`, `fog.skyVolScatter:0`) VSM / SDSM / Legacy: bathroom 0.42 / 0.43 / 0.44, second room 0.42 / 0.42 / 0.43, tent 0.66 / 0.42 / 0.43, tunnel 5.65 / 5.64 / 5.66, grove 9.38 / 9.42 / 9.39, beach 17.43 / 17.44 / 17.45. The water's mask: hull behind the camera 0 vs 1 0.098 % (floor 0.081), 0 vs 2 1.351 %, open sea 176 -> 127 pages. At the helm under way, the probe's second difference: panel 0.0002, wide 0.0008, left of the dash 0.0015. GBV (`VsmPageRequest,VsmPageRender,ShadowCull,Lighting,Transparent,Transparent.Water,VolumetricFog,Compose,Skybox`, and at night `...PointLights,SpotLights,VolumetricFog,Skybox`, + the Skybox self-test): clean.

READING UE'S PAGES: `r.ShaderPrint 1`, `r.Shadow.Virtual.Stats.Visible 1`, the block's "Requested" in a `CaptureViewport` at the viewport's own pose (scratchpad `ue_pages.py`). `captureTransform` is `{location, rotation {pitch, yaw, roll}, scale}`: an unknown key is ignored silently and it captures the world's origin. That is what the first version of this section's UE column was, four "views" of one camera; look at a capture before reading a number off it. WindTest as loaded has TWO suns, `CLAUDE_TEST_env_sun` and UDS's, so twice the pages: `set_is_temporarily_hidden_in_editor(True)` on one, no package gets dirty, a reload undoes it. UE's render width follows `r.ScreenPercentage` 67 (scalability), not the viewport's: `r.Editor.Viewport.ScreenPercentageMode.RealTime 0` + `r.Editor.Viewport.ScreenPercentage 100` takes the floor's pages 212 -> 385, and to native the lamp's deltas match ours (mode 1 is the setting to put back). The counters lag and drift by some 15 %, and a light spawned just now counts as moving for `r.Shadow.Scene.LightActiveFrameCount` frames, one mip coarser, in an editor in the background that draws few: go round the poses twice and capture each several times. Compare a lamp as lamp on minus lamp off, on a floor of your own: the shore's water marks no lamp. A spawned actor dirties the map: load the owner's level back, unsaved.

A KNOB THAT SEEMS TO SAVE TIME: compare what it costs the picture on what reads it. The size threshold's first cut read `params.w` from the render slot's view record, which was zero, so every caster left the coarse pages: the "saving" of 0.2 ms was empty pages, no surface showed it, and the closed room's air came out lit (17.8 of 255 against 0.42). Only `fog.debugView:4` said so.

## The Sun's Shadow on the Water

The ocean takes the sun's shadow from GEOMETRY (2026-11-02, owner: "make the ocean receive shadows"; until then only the
cloud shadow, plan C3, reached it -- a boat cast no shadow on the sea and the water kept its glints inside a dune's shadow).
`ocean_surface_common.hlsli OceanSunShadow` reads the frame's own clipmap (VSM) / cascades (Legacy) / partitions (SDSM)
through `shaders/forward_sun_shadow.hlsli` -- glass's sun shadow moved there, ONE function for both forward surfaces,
lighting_cs's arithmetic and numbers -- from the transparent pass's GlassView CB (b1, which RenderableObject already
handed every draw): `GLASS_VIEW_CB_STRUCT 1` declares it as `ConstantBuffer<GlassViewData> gGlassView`, because OceanCB
already names view, proj, sunDirAmbient and the fog block (`GLASS_VIEW(x)` reaches a field either way). The ocean RS grew
CBV(b1), t22-t25 (the sun atlas or its EVSM moments, the VSM page table and pool, the SDSM partitions -- the transports
Pass_Transparent publishes, a buffer slot falling back to the dummy BUFFER) and s4 (ComparisonLinearClamp).

- The glint and the foam take it whole at the surface point; the body's scatter takes `ocean.sunShadowBody` of it (0.5,
  transient; 1 = whole) at the same point, sharp; the cloud shadow stays dosed for the body (`cloud.oceanBodyShadow`). The
  sea floor seen through the water keeps its own shadow from lighting_cs, displaced by the depth, as it should be.
- **Why a share** (2026-11-08): taken whole, a hull's shadow on the open sea is a navy hole -- the shaded water is still
  lit by the lit water round it, which the single-scattering body does not carry. Tried first and DROPPED (2026-11-07,
  d131f653): averaging the body's shadow down the column a view ray scatters in (8 taps along the refracted sun, traced
  back to the surface). It is right in geometry -- the hull's shadow volume is a slanted prism and the view sees it
  displaced, a tongue 20 m long toward a camera that faces a low sun -- but 8 deterministic taps draw 8 offset copies of
  the hull's silhouette (each edge a 15-22 % step), and jittered taps a fixed grid (the water is barely accumulated).
  Owner, on the yacht under way at a low sun: "several silhouettes instead of a sharp shadow, looks bad -- give the
  sharp shadow back, just weaken the darkening". The cost it had (+0.02-0.08 ms of Ocean.Surface) went with it.
- The biases take the FLAT surface (0, 1, 0), not the ripples' shading normal: the water is no caster, so a bias only keeps
  the sea floor at the waterline from shadowing it.
- **VSM pages for the water**: the page request reads the OPAQUE depth (the water draws later), so `vsm_page_request_cs`
  also marks the clipmap page where each pixel's view ray meets the ocean's plane before the opaque surface or the sky
  (`PageRequestConstants::waterPlane` = `SceneRenderer::VsmWaterPlane`: 1 and the water level on a frame the water draws
  and takes the shadow; it is in the still-camera request key too). The plane, not the displaced wave: a crest moves the
  sample within a page or to a coarser resident level.
- **Only where a caster can shadow them** (`shaders/vsm_water_mask_cs.hlsl`, `vsm.waterCasterMask`, default 1; 2026-11-06, owner: "take the sea's marks away"). The plane asked for a page under every water pixel out to the horizon, ~30 pages a level, and most of them draw empty. A caster's box in the light's frame IS the column it can shadow, so before the marks a small dispatch sets one bit per (clipmap level, 16th of its side: 8 pages) some box touches (`kWaterMaskMargin` 2 m round each, the wave's: the marks take the plane, the water samples the displaced surface), in the request buffer's tail (`RequestBufferWords`), and a water pixel whose page has no bit marks nothing and reads lit, which it is. Pages at 15:00, every water pixel (0) -> the mask (1), no marks at all (2) in brackets: beach 645 -> 562 (483), grove 492 -> 421 (336), open sea 352 -> 84 (62), bathroom 162 -> 161 (161). What it buys is pool room, not time: `Pass_VsmPageRender` the same (an empty page draws nothing). `VsmPageRequest.WaterMask` 0.026-0.030 ms on land, 0.013 on the open sea, `Pass_VsmPageRequest` +0.02-0.025 / +0.008; most of it is the dispatch and its barrier, 480 casters cost what 3827 do. On the open sea `Ocean.Surface` -0.023 and `Pass_VolumetricFog` +0.017 (its air has no page now and traces the sun, below). `vsm.waterCasterMask:0` = before; 2 = the control. THE CONTROL IS A HULL BEHIND THE CAMERA: arm 2 lost nothing on any view looked at first, because a lagoon's floor keeps those pages and a hull in frame keeps its own (its column is its shadow's). small_yacht at [-400, 0, 0], 16:00, the camera at [-396.61, 3, 3.1] with its back to the hull, over open water: pages 270 / 222 / 0, pixels over 8 levels 0 vs 1 0.069 % (the same arm twice 0.128 %), 0 vs 2 2.815 %, the shadow gone. It caught the first cut: `RecordComputeDispatch` divides by 8, so a 64-thread group takes `groups * kComputeDispatchGroupSize`, and at `groups` alone the mask saw 480 of the 3827 casters while every other view looked right. GBV (`VsmPageRequest,VsmPageRender,Transparent,Transparent.Water,VolumetricFog,Skybox` + the Skybox self-test, the beach): clean.
- **Rays** (`ocean.sunShadowRays`, default 1, at most the frame's SMRT count; 0 = the clipmap's single tap): the single
  tap's constant depth bias swallowed the low parts of a hull close to the water -- part of a boat's shadow was missing
  where Legacy, SDSM and SMRT all had it. Boat view (cam -33 4 24, rot 0.0635 0.9056 -0.1464 0.3930, 16:00, the water ~70 %
  of the frame), the shadow's mean luminance (off 90.6): tap 76.6, 1 ray 70.7, 2 rays 70.4, 8 rays 70.1; frame-to-frame
  std inside it 0.35-0.36 in every arm (DLSS resolves it). `Ocean.Surface` (alternated twice): off 0.220 ms, tap 0.237,
  2 rays 0.316, 8 rays 0.624; on the lagoon view (cam -11.6 4.17 73.6) 0.20 / 0.21 / 0.24 / 0.38. 1 ray, alternated
  three times: VSM boat 0.212-0.221 -> 0.260-0.266 ms, lagoon 0.196 -> 0.223-0.228; SDSM (the cascades, no rays) boat
  0.213 -> 0.254, lagoon 0.199 -> 0.215. `Pass_VsmPageRequest` the same within noise (0.057-0.070).
- `ocean.sunShadow:0` = before (the A/B; transient, like `ocean.sunShadowRays`). Legacy / SDSM need no residency:
  SDSM's partitions are fitted to the opaque samples, and on wind_test's views the water above the sea floor lay inside
  them (boats and palm shadows on the water as in VSM and Legacy).
- GBV (`Transparent,Transparent.Water,Translucent,VsmPageRequest,Skybox` + the Skybox self-test, the boat view): VSM
  and SDSM clean.

## Cast Shadow — Per Object

A level mesh `"castShadow": false` (Inspector: Lighting > "Cast Shadow", MCP: `setCastShadow`; staticMesh and
instancedModels) is UE's `CastShadow`: no shadow from it in any map -- the sun's (CSM/SDSM/VSM clipmap), the lamps'
(atlases, VSM), the RT shadow rays (its TLAS instance lacks `rt::kInstanceMaskShadow`) -- while it stays visible and lit.
Absent = casts; the key is written only while off. Contact shadows still see it (they trace the depth buffer; UE keeps
those on a separate `bCastContactShadow`).

- The mesh STAYS in the GPU-driven caster table (`ShadowGpuData::IsCaster`): that table is also the camera's indirect
  G-buffer, and a mesh left out of it drops to the CPU G-buffer draw. The verdict rides its bounds instead
  (`CasterBounds.halfExtents.w = 1`), and every shadow consumer tests it: `shadow_cull_cs` (shadow views; the camera row
  ignores it), `vsm_page_scatter_cs`, the setup's brute-force fallback, the cull validator. A type that never casts
  (the ocean, particles) stays out of the table -- `RenderableObject::ShadowSwitchedOff()` is the switch, not the type;
  letting the ocean in made the merged caster VB/IB heterogeneous and turned VSM's single draw off (45 ms CPU).
- A switched-off mesh still builds its shadow PSOs at Init, so switching it back on works on the CPU path too.
- A switch rebuilds the caster set (`NotifyEditorShadowCasterVisibilityChanged`, like a visibility change), and every
  `ShadowGpuData::Rebuild` flushes the VSM page cache once (`casterLodsChanged_`) -- the switch no longer resizes the table,
  which is what used to flush it.
- Only casters count for `HasWindCasters()`: grass with its shadow off does not keep VSM redrawing for the wind.
- **A caster-set change mid-session waits for the GPU before it rebuilds** (`ShadowGpuData::UpdateForFrame`, as
  `Scene::RebuildShadowCasters` always did): `Rebuild` re-creates every ring it outgrows and `EnsureRing` /
  `EnsureUavRing` release the old buffer AT ONCE, while the frames in flight on both queues still read it. Switching
  wind_test's 104 "Lamps" bulbs on early in a session hung the GPU about one launch in three (DRED, `--dred`: a page
  fault on the freed `ShadowGpuData.Bounds`; frames of 400-800 ms, then DEVICE_HUNG; never with
  `--no-async-compute`). After: 15 of 15 launches clean. The per-frame rings that grow with it (`RefreshVirtualGroups`,
  `UploadCasterLods`, `UpdateViewFrustums`) grow only in the frame of such a rebuild, after the wait. Diagnosing a
  hang like it: every RT build failure logs `RT off: ...` with its HRESULT, and the RT-off path and a failed capture
  call `ReportDeviceRemovalOnce` (DRED into `device_removed.log`), so a removal nobody noticed still leaves its
  record; an intermittent hang needs 12+ launches an arm before an arm counts as clean (6 clean of one arm was luck).
  The same `Rebuild` drops the consolidated mega VB/IB, which only an upload batch on the idle GPU can make: Scene
  rebuilds it right after (`ShadowGpuData::TakeMegaRebuildRequest` -> `Scene::RebuildShadowMegaBuffer`). Until
  2026-10-30 nothing did, and after the "Lamps" switch-on the local views lost their compaction and the Tessy atlas
  path (VSM its single-draw page render) until some full rebuild came along -- 13 s in one Release session.

## SDSM Partition Cache -- a Kept Tile Is Handed Only What It Was Rendered For

SDSM (`--shadow-mode=sdsm`, `shaders/sdsm_analyze_cs.hlsl`) re-renders partition 0 every frame and partition p >= 1
every min(`sdsm.cacheFrames`, 2^p) frames (S20); a kept partition carries its matrix, box and tile. The interior
splits are smoothed over frames (`sdsm.stabilityFrames`, S16.7), so after a quick step out of a deep view (the sea
through two doorways) they slide nearer for ~100 frames -- and a boundary sliding nearer handed the next, KEPT
partition receivers its box never held: they fell through `CsmSampleChain` to "lit". The owner's lit triangles on
the far bathroom wall (2026-10-26; cam -22.52,1.73,87.46 -> -22.82,1.73,87.51 at 15:00), frames 65-75 after the step
in a rhythm of four; `sdsm.cacheFrames:1` or `sdsm.stabilityFrames:0` alone made them go. Now each partition records
the interval its tile was rendered for (`SdsmPartition.renderedInterval`, the struct is 176 bytes), and LogPartitions
moves a boundary next to a kept partition only toward handing receivers to the FRESH side (fitted to whatever it
gets); a kept partition's interval stays inside its rendered one, empty if the splits passed it. No gap can open
between two kept neighbours (whichever rendered last was fitted up to the other's rendered end). The cross-fade band
blends into the next partition only when that partition's box holds the point (`csm_sample.hlsli`): a kept partner
holds the band it was rendered with. Measured with the burst (3 runs): flashes after frame 33: none (were 0.03-0.28 %
of the frame at f65-75); what remains before it is every temporal filter settling, as with the cache off. The reverse
step (B -> A) never flashed. Geometry that comes into view inside a kept interval (disocclusion) is still the camera
guard's (`Scene::UpdateSdsmFrameParams`: a move or turn past half the cache dilation re-renders out of turn).
GBV (`SdsmAnalyze,SdsmCull,SdsmShadow,Lighting,Skybox` + self-test): clean.

## Local-Light Shadow Near -- 1 cm in Every Mode

A spot's and a point's shadow projection start `render::kVsmLocalShadowNear` / `kPointShadowUeNear` = 1 cm from the light
(`ShadowSettings.h PointShadowNear` / `SpotShadowNear`; UE's MinZ 1 cm, ShadowSetup.cpp:1169) under VSM as on the atlases.
VSM kept the old near -- max(0.2 m, 2 % of the radius) for a point, 0.1 m for a spot -- and every caster nearer than that
was cut out of the light's pages (owner, 2026-10-28: "VSM shadows leak through the walls"): wind_test's bathroom sconces
(radius 12 m: a 24 cm near) sit 10 cm in front of the mirror wall and lit the hall behind it through the wall, and the
torn wallpaper beside them cast nothing; Legacy and SDSM were dark there. The owner's view (cam -22.19 1.53 86.70, rot
0.0647 -0.1394 0.0091 0.9881, 23:00), the hall wall's mean: VSM 33 -> 26.7, SDSM 26.7; zero local biases did not move
it (30.4) -- not a bias. The VSM pool is D32 float with direct perspective Z, so 1 cm costs ~2^-24 x w^2 / n of depth
step at the far end (0.9 mm at 12 m, 5 mm at 30 m), under the texel bias: local_shadow_test changed 0.7 % of its pixels,
a dune's self-shadow by a low lamp that the old near had clipped and the atlases draw. `--set shadow.pointNearUe:0` =
the old nears in both modes (the A/B; it switches live).

## Local-Light Shadow Cache (Atlas Modes) and VSM Page Requests -- What Made Them Jump

`get_readouts` `localShadows.cache.totals` counts the static-caster cache (plan S7, `render::LocalShadowCache`) since
the process started -- misses by reason (`missNew` / `missRes` / `missMoved` / `missStale`), `uncached` / `uncachedNoRoom`,
`evicted`, `flushes` (the pool re-created with entries in it), `staticBumps`, `fillTexels`, `blockNoRoom` -- because one
frame's stats, sampled ~every 10 frames, never see a miss (~0.02 a frame in flight). wind_test with "Lamps" at night: a
still camera has no misses and the cache pass does not run; ONE full refill (every view stale) is 0.23-0.47 ms -- the
"0.25-0.5 ms that jumps" was refills, all of them warm-up (2026-10-30, details in docs/local_light_shadows_plan.md S7):

- **The pool grows by doubling and keeps its entries** (`LocalShadowCache::GrowPool`: the old pages copied whole into the
  new array on an `UploadBatch` at the GPU-idle reconcile). It grew a page at a time, each `EnsurePool` throwing every
  cached light away (eight stalls, 61 views refilled at once).
- **A capacity-only rebuild keeps the cache** (`RebuildShadowCasters(renderer, sameGeometry = true)` from
  `ReconcileShadowCapacity`): the view slots doubling does not change what the casters draw.
- **The working set is never evicted**: a request evicts only entries unused this frame AND the last one -- a failing
  request used to throw out the lights asked later in the same frame, which refilled next frame (0.23 evictions, 0.22
  refills, 0.31 uncached a frame in flight). A point light's 3f x 2f block with no free 4f square (512^2 faces need a whole
  2048^2 page) is cached as six face squares instead; the buddy allocator takes a free block on the FULLEST page, so empty
  pages stay whole. Event `local shadow cache: no room for ...` (throttled) names the free space and the largest square.

`vsm_page_request_cs.hlsl` marks a page with `MarkPage`: read the word, `InterlockedOr` only while the bit is clear. Every
pixel of every local light marks its page's whole coarser mip chain, and the coarse levels are a few words a view -- one
atomic from nearly every thread on the same address. `Pass_VsmPageRequest` 0.42 -> 0.10 ms in the bathroom at night,
0.28-0.33 -> 0.10-0.12 on the grove (day and night), the frame -0.2 to -0.4 ms; VSM now matches SDSM in the bathroom at
night (the grove's remaining +0.5 ms is the lamps' page RENDER). Any new bit-packed request / mark buffer: the same rule.
