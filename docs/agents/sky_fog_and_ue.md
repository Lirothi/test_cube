# Atmosphere Terms, Sky Calibration, Matching UE 5.8, the Volumetric Fog

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## Atmosphere Terminology — Say Which One

Three different things used to share the word "atmosphere", and the overlap cost a full day of
chasing the wrong subsystem: an Inspector group holding the height fog's knobs was labelled "Aerial
Perspective", so "turn AP off and it goes away" and "AP measures innocent" were both true, about two
different switches. The names below are UE's and are now the only ones used in code, UI, settings
and levels.

| Thing | UE name | Ours |
|---|---|---|
| Analytic distance/height fog + its froxel volume | `ExponentialHeightFog` | `HeightFogSettings`, `--set=fog.*`, level `postProcess.heightFog`, `shaders/height_fog.hlsli` (`HeightFog*`), Inspector "Exponential Height Fog" |
| Hillaire sky: transmittance / multi-scatter / SkyView / distant-light LUTs | `SkyAtmosphere` | `SkyAtmosphereSettings`, `--set=sky.*`, `shaders/sky_atmosphere.hlsli` (`SkyAtmosphereCB`, `AtmosphereRadii`) |
| The sky's camera froxel volume applied to GEOMETRY only | aerial perspective | `sky.aerialPerspective`, `Main_SkyAerial`, `shaders/sky_lut_aerial_cs.hlsl`, compose's `aerialParams` |

Rules. **"Atmosphere" alone names the SKY, never the fog** — a bare `Atmosphere*` symbol belongs to
`sky_atmosphere.hlsli`. **"Aerial perspective" is only the sky's volume**, never the height fog, and
it is applied only where `z > kEps`; the sky already contains that integral (`SkyAtmosphere.usf:953-988`
returns before the AP branch). The height fog, by contrast, IS applied to the sky, which is UE's own
behaviour (`bOnlyOnRenderedOpaque` is false — `SceneRendering.cpp:909`).

Levels written before 2026-09-10 carry the fog under the old key `atmosphere`. Exactly two places
know that: `JsonLevel::HeightFogSection` and `EditorSceneDocument`'s section table. Do not spread the
alias any further; a level takes the new key the next time it is saved. An old `--set=atmosphere.*`
is not silently dropped — it logs `UNKNOWN SETTING (ignored)`.

## Sun / Sky Calibration -- the Sky's SHAPE, Not Only Its Energy

Calibrated 2026-10-31 (owner: "the sun / sky calibration"), against physics rather than photos (a photo's processing
moves shadows more than any lighting term). The probe: a matte grey 4 m `models/box` 45 m over the island (`place` +
`setSurface {metal 0, roughness 1, normalStrength 0, albedo}`, never saved), clouds / fog / GI / sky visibility /
reflections / aerial perspective / `light.groundAlbedo` off, each face probed with `light.skyFillIntensity` 1 and 0 --
sky and sun apart, albedo cancelled. The sun on the side faces follows its cosine to 0.01 (the lighting maths is
right), the diffuse cube equals the radiance capture integrated at mip 0 with black below (`fog.debugView:10` = the
diffuse probe as a raw equirect, read with the pixel probe), and the bare sky by elevation equals UE 5.8's within 2 %.
- **The energy was right**: the sky's share of horizontal illuminance (6 / 10.7 / 20 / 36 degrees: 0.66 / 0.48 / 0.34 /
  0.24) is Ineichen-Perez at Linke turbidity ~4 -- a humid tropical sky.
- **The shape was not**: vertical / horizontal sky irradiance was 0.91-0.99 facing the sun and 0.71-0.81 facing away;
  the CIE standard clear skies (ISO 15469 types 12 / 13) give 1.1-1.8 and 0.28-0.54. The sky had no aureole, and a
  vertical face turned from the sun took 1.5-2x the sky light it should -- our trunks against the photos. Cause: UE's
  Earth defaults carry almost no aerosol (Mie 0.004 / km over 1.2 km = optical depth 0.005; maritime air is 0.05-0.15),
  and Rayleigh is symmetric fore and aft; the level also had Mie anisotropy 0.6 (UE 0.8), Rayleigh x1.21 and the
  atmosphere's ground albedo 0.40 (land; the sea around the island is ~0.08).
- wind_test then: `skyAtmosphere` rayleighScale 1.0, mieScale 10 (optical depth ~0.05, clean maritime; the JSON and
  `--set` clamp it at 10), mieAnisotropy 0.8, groundAlbedo 0.08. Shares 0.58 / 0.40 / 0.28 at 10.7 / 20 / 36 degrees
  (Linke ~4 in lux), away from the sun 0.57 / 0.48 / 0.40 (CIE 12: 0.50 / 0.44 / 0.38), toward 1.6-1.9.
- **mieScale is 1 again since 2026-11-02 -- the owner's pick, the look over the shape** (owner, at 14:00 on Nov 1, the sun
  24 degrees up: "where does this glare round the sun come from? I don't see the corona, and Sun Glare does nothing").
  Mie 10's aureole left the sky near the sun almost white -- display 244 / 239 / 219 / 166 of 255 at 2 / 4 / 10 / 20
  degrees -- and the corona and Sun Glare, small additions on top, had nothing left to brighten: x10 of either moved the
  2-degree ring +4 / +5, the bloom off -2; the sun probe read the disc whole (share 1), the fog and the shafts moved it
  0-3. Mie 5 / 3 / 1: 204 / 191 / 168 at 10 degrees, 147 / 135 / 123 at 20; at 1 the corona's needles read again on a
  blue sky; a larger g (0.9) only whitened the patch. The trade: a face turned from the sun takes the sky light it took
  before the calibration again (vertical / horizontal 0.71-0.81 away from the sun, CIE 0.28-0.54). The night is as it was:
  `twilight.mieNightScale` 0.1 -> 1 keeps the night's aerosol at 1 x UE's (Night Aerosol, below).
- **Light shaft bloom thresholds are in luminance AFTER exposure** (light_shafts_cs.hlsl, UE's units), so they are
  measured at the real auto exposure, never at a pinned one: at the level's 0.7 the shafts were 64 % of a backlit
  trunk's light (1609 vs 584 without), +27 % on the lit sand, and moved the metering 0.6 EV -- invisible at a pinned EV
  1.5 stops darker, where they did not trigger. With the aureole they took the trunks x4.4; the sun's
  `lightShaftBloomThreshold` was 3 (about the old strength) -- 0.7 again since 2026-11-03, UE's (below, "The light
  shafts are UE's again"). UE ships them OFF (`bEnableLightShaftBloom`,
  LightComponent.cpp:491). Result on the sunset view: trunk / sand 0.128 -> 0.103 (photos 0.07-0.10), p5 / share < 20 /
  p95 9 / 0.12 / 232 (photos ~7 / 0.11 / 230).
- **The exposure curve followed a camera, not a game**: wind_test's `cameraExposure.exposureCurve` took a dim room
  3.4 EV under its metering (EV 4.55 / 8.55 / 10.55 / 12.55 -> -4.92 / -2.52 / -1.32 / -0.85, UDS-style "dark stays
  dark"): the bathroom at 15:00 (cam -23.44 1.46 86.06) had 65 % of its pixels under 20/255 while its scene spans a
  plausible 10 stops (window 4591, walls ~23, dark wood 3-6). Now -1.5 / -1.0 / -0.8 / -0.6 (14.55 and 15.55 kept, so
  the daylight fit stays): the room 0.24 under 20/255, median 68; 18:00 outside unchanged, noon +0.14 EV. Nights are
  `nightCameraEv100`'s, not this curve's.

## Matching UE 5.8 -- the Same Settings Give the Same Picture

The owner keeps wind_test rebuilt in his UE (`/Game/ClaudeTest/Maps/WindTest`, MCP 127.0.0.1:8000) and compares views.
UE's (x, y, z) = ours (z, x, y) x 100 (fitted on 735 palms: 0.000 m), the same yaw, the pitch's sign flipped. Checked
2026-11-03 at his viewport (ours [-18.68, 31.35, 26.10], yaw -59.29, 6.57 down; sun 10.66 degrees, 120 klux) with UE's
scene numbers mirrored (`skyAtmosphere` rayleigh 1.21, mie 1, g 0.6, ground albedo 0.40 -- UE's FColor 170 is LINEAR 0.40
--, skyLuminanceFactor 2; fog 0.002 / falloff 0.06 / start 10 / lobe 0.5, exp 8, from 100 m / volume 60 m, g 0.4; unit
twilight tints; `daySkyBrightness` 1):
- **The sky and the fog in HDR**: the sky +1-3 % at +0.5 .. +17 degrees, its R/B within 0.01; the fog's saturated band at
  the horizon 3700 vs 3575 cd/m2 toward the sun (48.5 degrees off), 3337 vs 3256 away; fog on / off above 0.3 degrees
  within 1-3 %. `timeOfDay.daySkyBrightness` 2 (2026-10-19, for UDS's look) doubled the whole sky against UE's 1
  (it scales SkyAndAerialPerspectiveLuminanceFactor) -- wind_test has 1 since (owner).
- **The display**: `colorPipeline.ueGamut` (default on; Inspector Post Process > Film "UE colour pipeline (AP1)", `--set
  color.ueGamut`, `color.expandGamut` 1, `color.blueCorrection` 0.6 -- UE's defaults) runs UE's LUT as UE builds it
  (`shaders/ue_color_pipeline.hlsli`, from PostProcessCombineLUTs.usf / TonemapCommon.ush FilmToneMap / ACESCommon.ush):
  AP1 working space, ExpandGamut, the grade with AP1 luma, BlueCorrection, the ACES glow and red modifier around the
  five-knob curve -- and the grade AFTER the local exposure and the bloom, where UE's LUT sits (ours graded before them).
  Off = the linear-sRGB path, the A/B. On a blue sky the two differ by 1-3 levels. UE's natural vignette is ours too:
  `colorPipeline.vignetteIntensity` (UE VignetteIntensity, default 0.4 -- every level; Inspector "Vignette", `--set
  color.vignette`), the cosine-fourth law in UE's vignette space on the scene and the bloom before the grade
  (PostProcessCommon.ush ComputeVignetteMask, PostProcessTonemap.usf:513-526): 86 % at the top edge's centre, 57 % in a
  corner of a 16:9 frame.
- **wind_test carries UE's scene now** (owner, 2026-11-03: "make everything as in UE, overwrite the calibration"): the sky
  above (replacing the 10-31 calibration's rayleigh 1.0 / g 0.8 / ground albedo 0.08 / skyLuminanceFactor 1), the fog
  0.002 with its volume to 60 m and `timeOfDay.fog` all 1 (UE's density does not follow the sun), the sun WITHOUT its
  6500 K temperature (UE's light is plain white; our blackbody 6500 K is pinker than D65 and put the sky's R and B 7-8 %
  over UE's against G), the grade 1.15 / 1.3 / 1.1, local exposure 1 / 1, no meter mask, and the exposure curve's day
  points (12.55 / 14.55 / 15.55) at +0.5 = UE's histogram bias (the room and night points as they were). UE's sun
  (10.66 degrees) is our 18:00 on the level's date. Result at his viewport, the level as it is: the sky +3-4 % at every
  height (blue +2-4 % more), +0.5 degrees +3 %; the water -3 .. -18 degrees within 6 % with the clouds on in both; the
  displayed horizon at +2 degrees (218, 212, 170) vs UE (219, 213, 169). The exposures, solved from the displayed pixels
  through UE's pipeline (scratchpad `solve_exposure.py`, which recovers ours to 0.06 stop): UE 2^-13.9 (its own readout
  says 13.8), ours 2^-14.0 -- our clouds (UE's are its own, smaller) and our wider sun glitter meter brighter.
- **Then PHYSICS in both** (owner, same day: "set physics in both"): the UE scene's numbers above were OUR pre-calibration
  level copied into UE on 10-31 (only Mie x1, ground albedo 0.40 and fog density 0.02 were UE's defaults). wind_test and
  the UE scene now carry: Rayleigh x1.0, Mie x10 (AOD ~0.05, clean maritime; scattering and absorption both), g 0.8,
  ground albedo 0.08 (sea; UE FColor 80), skyLuminanceFactor 1; the height fog as a marine haze -- density 0.0002 / m
  with heightFalloff 0.0015 (UE's 0.002 / 0.015: UE's two numbers are per 10 m, ours per metre -- the Inspector's hint
  says so), maxOpacity 1, start 0, the grey sun lobe 0 with exponent 4 (UE's defaults), the volume g 0.75; the sun disc
  0.53 degrees, soft angle 0 (UE 0.5357 / 0). Clear-day visibility at the sea ~25 km (Mie 0.044 + Rayleigh 0.014 + fog
  0.1 per km). The UE scene is changed in the editor, NOT saved. Result at his viewport (linear, clouds on): the sky
  +0.5 .. +5 degrees within 2-6 %; the water -0.5 .. -18 degrees ours 1.2-1.9x UE's and warmer -- with the body at 0
  ours is already over UE's whole water in R and G, the sun glint off moves nothing, the fog off or AP off moves
  nothing: it is our water's sky reflection. The fit above (deepScatter 0.0013 / 0.055 / 0.12) was made while UE's
  skyLuminanceFactor 2 also doubled UE's sky LIGHT (UE puts it into the sky-light capture; ours does not by design), so
  it was 2x high for equal lighting. Against photographs of a low sun over the sea from height (Unsplash): the glitter
  is a column a few sun widths wide near the horizon, widening toward the camera, of distinct sparkles, the water beside
  it dark in a clear sky -- UE's is a narrower calm-water column, ours a broad smooth cone: `ocean.render.windSpeed` 12
  m/s (the Bruneton unresolved roughness) against a sea state of windForce 0.4 (~2.5 m/s in the preset) -- the two are
  not the same wind.
- **The sun glint's smear is a knob** (owner: "we reflect the sky honestly, we do not need the analytic smear of the
  specular by roughness, only the glitter path -- or a knob to fade it"). `OceanSunSpecular`'s GGX alpha is the Bruneton
  unresolved slope variance, and GGX's long tails times the sun's radiance spread it into a broad smooth patch a few tens
  of sun widths across, brighter than the sky around it -- Cox-Munk at 2.5 m/s puts the glitter's half width at ~2.7
  degrees at the horizon. `ocean.render.sunGlintSmear` (ocean window "Sun glint smear", `--set ocean.sunGlintSmear`;
  windParams1.w) scales that alpha: 0 = every resolved facet mirrors the disc (UE's sphere light at the disc's radius) --
  scattered sparkles over the resolved waves' whole slope spread, no continuous path; **1 = the default**, the path as it
  was (owner, at 0: "fix the water's specular back, bring the path back"). The sky reflection keeps the variance (its blur).
- **The land was 0.6 of UE's, and two causes were ours** (owner: "UE shows the leaves' SSS more", then "you overdid the SSS",
  "the same assets are imported, keep comparing"). Linear, both engines at the same pose (UE's viewport by
  `ue_linear_view.py`, ours by the pixel probe, scratchpad `crowns_compare.py`): the backlit crowns and sand from 35 m
  up 0.58-0.67 of UE in every third of the land band, ours bluer in the shade; UE's TwoSidedBxDF lobe moved it 2 %.
  A 4 m grey box (albedo 0.5, matte) 45 m over the island in both (`ue_box.py` / `ours_box.py`): the top and the sunlit
  faces 0.95-1.05 -- the sun and the sky agree --, the faces turned from the sun 0.69-0.71 and bluer (R/B 0.54-0.65 vs
  0.78-0.85). (1) **The GI probes' rays stopped at 30 m** (`gi.rayDistance`), so nothing more than 30 m over the ground
  took its bounce, and `light.groundAlbedo` (0.033 in wind_test) stands in only outside the volume: 200 m now (UE's
  r.Lumen.MaxTraceDistance) -- the box's shaded faces 0.87-0.95 and UE's colour, the crowns' land 0.72-0.84; the trace
  +0.15 ms seen from 35 m up, +0.02-0.04 on the grove. (2) **UE's diffuse is rough** (BaseWindowsEngine.ini:37
  r.Substrate.RoughDiffuse=1 -> Diffuse_GGX_Rough v3 = EON at roughness x 0.4, BRDF.ush:213-249): `utils.hlsli DiffuseEON`
  in EvalBRDF for every direct light (UE's weight 1 for the sun and small lights), `light.roughDiffuse:0` = Lambert for the
  sun (the A/B; Lambert exactly, EON at roughness 0). The sunlit west beach seen from the sea with the sun behind
  (cam -110 6 0, yaw 90, 14 down, clouds off in both): 0.45 -> 0.62 of UE, the dry sand uniform; the backlit crowns 0.74 ->
  0.71 (EON darkens a rough surface seen against the light). Still open there: a constant ~1.6 on grazing-lit sand that
  the box's faces do not show -- not the sand's normal map (normalStrength 0: 0.62), not the terminator gate or offset
  (identical), not the shore wetness (the dry rows 0.61-0.69). UE runs no GI ray limit, ours `gi.farSpacing` 2 leaves the
  land under a camera 35 m up outside the volume (`gi.debug:1` all red; 8 m covers it, +2-3 % there).
  MEASURING IT: `gi.*` survives `reset_settings` -- one arm with `gi.enabled:0` left every later arm without GI (a
  "GI does nothing" that was GI switched off); and clouds drift in both engines -- hide them in both or keep them in both.
- **The height fog is in the sky's capture, as UE's** (owner: "in UE turning the fog off changes the water, ours does
  not move -- their fog goes into the cubemap?"). It does: UE's real-time sky capture draws the analytic fog at an
  infinite distance from the probe over the sky and under the clouds (ReflectionEnvironmentRealTimeCapture.cpp:919-962,
  DistantHeightFog; ReflectionEnvironmentShaders.usf:1041-1073; the fog's bVisibleInRealTimeSkyCaptures, default true;
  volumetric fog not in it), so its probes -- the water's sky reflection, the sky light -- see the haze. Ours now too:
  `sky_ibl_capture_cs.hlsl` per tap `sky x T + HeightFogColor` (compose's sky branch, UE's colour, the probe at world
  height 0; `SkyAtmosphere::SetEnvironmentFog` from SceneRenderer_Graph, the cube rebuilt when the fog moves past
  kKeyLightShare); off under the HDRI and the sky-along-view A/B; `--set fog.inSkyCapture:0` = before (transient). At
  the owner's viewport (UE (-141, -3603, 5376), yaw -36.5, 3.8 up; clouds hidden in both; linear): the water -1 .. -8
  degrees was 1.5-2.4x UE's -- most of the "our water is brighter" above -- and is 1.04-1.27x now, toward the sun and
  away; the fog off: ours 0.85-1.31x of UE's foggless water, both brightening ~2x toward the sun. PHOTOGRAPHING IT: UE's
  sky capture is TIME-SLICED (r.SkyLight.RealTimeReflectionCapture.TimeSlice 1) and an editor in the background draws
  few frames -- a capture taken 6 s after hiding the fog still reflected the fog; `ue_linear_view.py` RECAPTURE=1 sets
  TimeSlice 0 and calls recapture_sky() (restored after).
- **The light shafts are UE's again** (owner, his viewport at 18:00 with clouds off: "look at these sun shafts, make ours
  the same"). The port is UE's line for line (LightShaftShader.usf:62-121); what differed was its input. Measured at ONE
  pre-exposure (ours pinned, UE's viewport in linear at bias = log2 of ours, the light's shaft bloom on / off;
  scratchpad `shaft_contrib.py`): with UE's numbers -- threshold 0.7, our own glare seed off, length 1 -- the shafts add
  0.05 / 0.05 / 0.12 / 0.07 / 0.01 (sky 6 degrees off the sun / sky over the crowns / crowns under the sun / crowns /
  sand) where UE's add 0.05 / 0.07 / 0.04 / 0.04 / 0.01-0.02; with wind_test's threshold 3 nothing. The sky round the sun
  is the same in both once UE's shafts are off (0.5 .. 8 degrees within 0-23 %; UE's +103 k cd/m2 at 0.5 degrees is its
  shafts, seeded by the disc at the cap). wind_test: the sun's `lightShaftBloomThreshold` 3 -> 0.7, and the shafts'
  glare seed (`lightShaftGlareSeed`, ours: the sun glare laid into the seed) is DELETED -- code, key, Inspector, `--set`
  (owner) -- a level that still carries the key ignores it; and `postProcess.bloom.threshold` 4 -> -1 (UE's BloomThreshold -1: the pyramid -- UE's Standard bloom --
  blooms everything; the FFT path has no threshold, so the level's FFT look does not move). Still different on that
  view: UE's auto exposure ~0.5 EV brighter (ours meters 13.2-13.7, the water's glint, the shafts and the bloom move it
  0.01), the larger halo of UE's Standard bloom round the disc, our lens ghosts.
- **A sun moving in the sky leaves no streak** (2026-11-04, owner: "if I move the sun quickly it trails across the sky").
  Not DLSS (bloom off: no streak with DLSS on) -- the bloom's temporal input (`bloom_conv_cs.hlsl` stage 12): under DLSS
  the FFT reads the scene BEFORE the upscaler (`bloom.convBeforeUpscale`, 2026-10-15: DLSS flattened a glint to ~10 % and
  the star never formed) and averages it over frames by the G-buffer velocity (`convTemporalWeight` 0.1, the jitter made
  glints flicker 40-50 %), with no neighbourhood clamp on purpose. The sky's motion vectors are the camera's, so a sun the
  clock or a drag turns stayed in the history at ~64000 pre-exposed and the FFT drew every old disc into a streak (the
  same on the pre-UDS level, with one thread or two). UE has no such stage: its FFT bloom reads TSR's output, and TSR
  clamps the history (its light shafts go through TAA with a clamp, LightShaftRendering.cpp:347-365); dragging UDS's sun in
  UE leaves none. Now while the sun's direction moves (> 0.01 degrees a frame) the history texels on its disc this frame
  and on its disc in the history's frame take this frame alone (`BloomRenderer::RecordTemporalInput`: 2x the disc's
  radius + 0.15 degrees + 2 texels); a still sun keeps the full average. `--set bloom.convTemporalSunReject:0` = before
  (transient). The time stepped 0.02 h per MCP call at the owner's position: the streak gone, the still frame identical.
  The moon is not covered (dimmer; not seen trailing). GBV (`Tonemap,Skybox` + the Skybox self-test, `--time-speed=600`):
  clean.
- **What the FFT bloom costs, and its order** (2026-11-09, the owner's GPU pass on his 18:00 view, cam 37.93 4.63 71.59;
  GPU sub-scopes `BloomConv.Flares / Setup / Forward / Multiply / Inverse / Resolve / Composite` inside Pass_BloomConv, which
  is inside Pass_Tonemap). It was 0.26-0.28 ms: the transform 0.15, the ghosts' scatter 0.065. (1) `bloom_fft_cs.hlsl`: a
  group is 1024 threads and holds one line of up to 2048 -- a 1024 row used half of it, a 512 column a quarter, at every one
  of its barriers; a group now transforms as many lines as fill it. (2) `bloom.convFftFused` (1; 0 = the separate passes,
  the A/B): the image fills the grid's top-left (640 x 360 of 1024 x 512 at 1440p), so the rows go forward and back over the
  image's rows only (the pad's are zero, read as zero by the column pass, `loadLimit`), and the columns go forward, through
  the multiply and back in ONE dispatch (mode 3), a column pair x, N - x a group -- the multiply needs the spectrum at -k,
  which is the partner column's, in the same LDS now; only the image's rows are written back (`writeLimit`). The fused
  columns keep the spectrum in fp32 where the separate passes rounded it to the grid's fp16: the bloom target's window mean
  moved 0.06 % (DLSS off, the temporal input at weight 1, the same arm twice 0.00-0.02 %). Needs a column of at most 1024
  (two in LDS), else the separate passes. (3) `bloom.ghostMeshShader` (1; 0 = the vertex shader): the bokeh scatter ran six
  vertices for every tile of the quarter-res grid -- 1.4 M a frame, each reading its tile's four taps again -- to collapse
  all but the few bright ones; `lens_flare.hlsl MSMain` takes 32 tiles a group, reads each once and emits only the bright
  ones' quads, in tile order (the VS's order: the same additive sums). Window mean with ghosts x250 (`bloom.ghostIntensity
  0.5`): 0.05 % from the VS's. Without a mesh shader tier the VS. Measured (alternated twice): Flares 0.065 -> 0.017, the
  transform 0.12 -> 0.075, Pass_BloomConv 0.26-0.28 -> 0.145 ms, the GPU frame -0.15. Left: the setup 0.02, the composite
  0.016, the resolve 0.011. GBV (`Tonemap,Skybox` + the Skybox self-test, `bloom.ghostThreshold:0.5` so the mesh shader
  emits): clean.
- **The clouds in the sky's cube are re-captured by how far they drifted** (2026-11-09, the same pass). A capture
  (`VolumetricCloud::MakeEnvironmentOverlay` -> `SkyAtmosphere::BuildEnvironment`) rebuilds the whole environment -- the
  sky picture, the clouds marched into all 8 mips (`SkyEnvironment.CloudCapture` 0.25-0.32 ms), the 9 probe mips --
  0.35-0.4 ms, and it ran every 4 frames while the wind's clock moved: 50 times a second at 200 fps. Clouds drift: fastest
  straight overhead at the layer's bottom, windKmH / 3.6 / height rad/s against the 128-texel cube's 0.70 degrees -- on
  wind_test (15 km/h, 0.6 km) a texel in 1.8 s. `cloud.captureTexels` (0.5; graphics_settings.json `cloudTrace`
  captureTexels, Developer Controls > Sky "Cloud capture step"; 0 = every 4 frames, the A/B): re-captured once they could
  have drifted that far (~0.9 s here); the probe moving 10 m, a setting, the noise or the distant light still re-capture at
  once, the clock going back too. GPU frame (alternated twice) 4.47-4.51 -> 4.37-4.41 ms. Checked by stepping the wind's
  clock by hand (scratchpad-style `cloudlag.py`, the water's reflection masked, against the 4-frame rule at the same
  steps): 0.5 at the floor at 50 ms and 2 s steps; the control 16 texels (no capture in 20 s) 1.01 % of the water's pixels
  over 8 levels against 0.24 %. What is left of the clouds: the view trace 0.18-0.21 ms (it traces behind near geometry on
  purpose, the rim it fixed), the shadow map 0.06 (`cloudShadow.updateFrames`).
- **The water** (UE's ocean, Lumen reflections) in UE's LINEAR viewport, not a scene capture (below): our body was half as
  bright and bluer. wind_test's `ocean.render.deepScatterColor` (0, 0.0127, 0.040) -> (0.0013, 0.055, 0.12), fitted by
  least squares over -3 .. -18 degrees (the body is linear in it): those rows within 1-6 %, G/B and R/B matched. Still
  ours: -0.5 .. -2 degrees +16-30 % and warmer (the reflection of the low sky), and a far wider, brighter sun glitter.
  The sea state `ocean.windForce` 0.6 -> 0.4 (owner: flatter; `--set ocean.windForce`, the colours `ocean.deepScatterR|G|B`,
  `ocean.diffuseR|G|B`, `ocean.sssR|G|B`, `ocean.absorption<0-3><R|G|B>` -- all live).
- **Photographing UE**: a SceneCapture2D switches Lumen GI and reflections OFF unless told (SceneCaptureRendering.cpp:
  799-801) -- its sky and fog are right, its water is not (half the viewport's, even with Lumen overridden and a
  persistent view state). For the viewport in linear: set the unbound PPV to manual exposure 2^bias (physical camera off),
  tone curve 0, neutral grade, no bloom / vignette / lens flares / grain / expand gamut, CaptureViewport, decode sRGB /
  2^bias, then restore every touched property WITH its override flag (scratchpad `ue_linear_view.py`); its sky equals the
  capture's HDR within 1-3 %. HighResShot writes nothing while UE is in the background. At the owner's exposure (13.8)
  UE's default `r.EyeAdaptation.CachedLightingPreExposure` clipped Lumen's cache and the sky capture (the red warning; the
  safe range is that value -12 .. +8): the water's reflection was cut -- 8 is set for his session.

## Volumetric Fog -- What the Air Sees

The froxel volume (`fog_scatter_cs.hlsl` / `fog_integrate_cs.hlsl`, UE's VolumetricFog.usf) lit the air of a closed
space with the open sky, and one fix uncovered the next (2026-10-26, owner in a box tunnel he built on wind_test at
15:00: "what is this light frame at the screen's edges", then "look at the centre -- a whole rectangle"):

- **Nothing nearer than the volume's start** (`fogVolumeZParams.w` = `FogVolumeNear`, the fog's start distance):
  `fog_common.hlsli FogVolumeInFront`, in compose and every `FogVolumeSampleAt` consumer, is UE's step at
  `HeightFogCommon.ush:568-570`. Clamped to slice 0, a wall 3 m away read the light of the first slice -- air BEHIND it,
  10 m out. The conservative depth emptied that slice in most columns and hid it, but `FogTileFarthest`'s 3x3 reaches
  past the HZB on the grid's border columns (Load returns 0 = the far plane, never culled): the frame, 38 px left and
  ~65 px right at 1440p, one cell plus the bilinear blend (`fog.gridPixels:8` halved it).
- **The sky term sees the sky** (`FogSkyVisibility`): UE's `ComputeSkyVisibility` (VolumetricFog.usf:713-735, one
  45-degree cone straight up against the global distance field) as `skyVis.fogRays` (2, GI tab, persisted
  `graphics_settings.json skyVis.fogRays`) RT rays spread over that cone against the TLAS, solid geometry
  (`RtVisibilityBlocked`), out to `skyVis.maxDistance`; averaged by the fog's own history. Compiled in on RT hardware
  only (`FOG_SKY_OCCLUSION`, t15 = the TLAS), rides plan S (`skyVis.enabled:0` = the open sky in the air too). It
  was the rectangle: the tunnel's air from the 10 m start on glowed with the whole sky in front of dark walls. Cost
  (Scatter, editor closed): +0.008-0.017 ms at 2 rays on the tunnel / sea / grove, up to +0.05 at 4 -- with solid
  cards; through the crowns' RT opacity (RT Sky Visibility, "A tree's leaf card") 0.25-0.28 ms of Scatter against 0.23. Frame-to-frame
  noise (10 shots, wind frozen): grove median 0.49 % at 0, 2 and 4 rays, the tunnel 1.92 / 1.95 / 2.07 % -- nothing
  measurable. Open views barely move (mean <= 0.15 %).
- **SDSM's air traces the sun** (2026-10-27, owner's closed tent at 15:00, cam 71.43 1.96 40.75: "another volumetric
  fog problem" -- sunlit haze inside). SDSM fits each partition's box to the SURFACES the camera sees, so the air in
  front of them need not lie in any box, and `CsmSampleChain` called such a point lit; the surfaces never notice, the
  volume is mostly that air. Legacy and VSM cover the frustum and were dark there. `fog_scatter_cs.hlsl FogSunShadow`:
  under SDSM, with the TLAS the fog's sky term binds, a point outside every partition traces one ray to the sun
  (shadow casters, a tree's leaf card partly open as above). The tent's sun-only fog (`fog.debugView:4`, `fog.skyVolScatter:0`): mean
  0.4 of 255, dark as in the other modes. Scatter +0.00-0.01 ms on four views. `fog.sunRtFallback:0` = before.
- **VSM's air traces it too, where no level has a page** (2026-11-06; `VsmClipmapShadowResident`). The page request marks pages under RECEIVERS, so air whose page no surface asked for has a page at NO level: that read as lit, a veil over a closed room's frame (the 2 m base's bathroom at 15:00: the fog's sun alone 4.31, SDSM 0.43, Legacy 0.44; 12 % of the frame over 8 levels). Since UE's coarse pages (below in "VSM Is UE's High") most air near the camera has one and reads it; these rays are for what is left (`vsm.markCoarsePages:0`, `fog.sunRtFallback:1`: 0.42). `Pass_VolumetricFog` the same within noise on 8 views. Without RT hardware the air stays lit. GBV (`VolumetricFog,Compose,Skybox` + the Skybox self-test, the bathroom at 2 m): clean.
- **Samples are kept in front of the depth** (`FogApplyDepthConstraint`, UE `ApplyDepthConstraintsToOffset`
  VolumetricFog.usf:87-114, on the medium and every lighting sample): a cell sample behind the surface its own pixel
  sees (this frame's furthest HZB, mip 0) moves `fog.depthOffset` (0.5, UE's) slices in front of it. With the sky
  occluded the cells straddling the tunnel's walls showed: their samples behind a thin wall saw the open sky and the
  sun past the roof, one ring per slice down both walls. UE moves only samples within ONE slice
  (`fog.depthThreshold` 1); a wall at a grazing angle still leaked at 1 (the next column's cell at this pixel's depth
  sits several slices behind ITS wall and the trilinear read takes it), so ours moves every sample behind (128). On
  three open views the constraint at 1 or 128 moves the mean <= 0.15 %, the changed pixels at the noise floor of the
  same shot taken twice. `--set=fog.depthOffset:-1` = off, the A/B. Both `fog.depthOffset/depthThreshold` are
  project knobs under fog.* (like `fog.gridPixels`): `reset_settings` leaves them as set.
- GBV (`--gbv-passes=VolumetricFog,Compose,Skybox --gbv-selftest=1@Skybox`, the injection caught): clean.

The UE review that followed (owner: "fix the volumetric fog"), each against `VolumetricFog.usf` / `HeightFogCommon.ush`:

- **The volume has its own start**, `heightFog.volumetricStartDistance` (UE VolumetricFogStartDistance, 0; Inspector
  "Volume Start", `--set=fog.volumetricStart`) and `volumetricNearFadeInDistance` (UE's, 0; `fog.volumetricFadeIn`).
  `FogVolumeNear` used the ANALYTIC `startDistance` -- wind_test's 10 m emptied a 4 m room of air and spread the 64
  slices over 10-300 m. The analytic start still keeps the analytic model off the near field, which the volume
  excludes up to its far plane anyway (UE `ExcludeDistance = max(VolumetricFogMaxDistance / cos, StartDistance)`).
  The history now resets when the slices move (start or far changed: `fogHistoryZParams_`).
- **The far plane stays 300 m, not UE's 60** (`SceneFrameData.h volumetricDistance` says why): beyond it the analytic
  model fogs the SKY with the sky itself (an identity), so the far plane is how much air hides the sky -- at 60 m a
  dense fog (0.02) left the sky clear behind 60 m of haze. At the level's density 60 and 300 are the same picture.
- **The sky term is UE's SH at g·V** (`GetSkySHDiffuseSimple`, their note: "+PhaseG", not the -PhaseG they keep for
  compatibility): the diffuse cube's two opposite taps, `((1+g) E(V) + (1-g) E(-V)) / 2` = DC + g x the linear band.
  It read E(-V) alone -- five times UE's tilt, away from the view. HDRI sky only: the procedural sky always feeds the
  volume B5's isotropic distant light (`SkyAtmosphere::DistantActive`; the level key `distantSkyLight` is read by nothing).
- **The history is rejected in depth too** (UE :900): a cell whose last-frame position lay beyond the far plane or
  nearer than the start read the clamped last/first slice. **`FogTileFarthest` clamps its 3x3 to the pyramid** (UE
  clamps its conservative depth read): the border columns never culled.
- **The analytic sun lobe is added beside the fog colour** (`height_fog.hlsli HeightFogColor` = UE's FogColor,
  `Inscattering * (1 - T) + DirectionalInscattering`, :407): it was returned inside the colour and multiplied by
  (1 - T) with it -- (1 - T)^2. On wind_test (strength 0.1, exponent 32) the difference is under 1.3 of 255; levels
  at the default 0.35 / 8 see it more. The line integral switches to its Taylor pair at |f| 0.01 (UE FLT_EPSILON2), not
  1e-6 (float cancellation). The lobe's strength is UE's since 2026-11-03 (below, "The sun lobe is UE's too").
- Every local light stays shadowed in the volume (UE's points and spots default `bCastVolumetricShadow` off).
- Cost (Scatter + Integrate, editor open, alternated x3): the start at 0 instead of 10 m +0.01 ms on the grove, 0 in
  the tunnel and over the sea; 60 m instead of 300 would add +0.02. GBV (`VolumetricFog,Compose,Skybox,
  TransparentFog,Translucent` + Skybox self-test, the bathroom at 00:00): clean.

**The analytic fog's COLOUR is UE's under the procedural sky** (2026-10-31, owner after a side-by-side with UE 5.8: wind_test
at 18:00 rebuilt in his UE -- island, 661 palms, UE's ocean, our sky and fog numbers, one exposure -- "do it as in UE").
Extinction and the volume were already UE's (palm-line contrast kept at density 0.002 / 0.01 / 0.04: ours 89/61/22 %, UE
82/49/28 %); the colour was not. UE 5.8: `FogInscatteringLuminance` (black) + `SkyAtmosphereAmbientContributionColorScale`
(white) x the distant sky light LUT (`HeightFogCommon.ush:154-198`), ONE colour for every direction, on geometry and on
the sky; its directional lobe is black by default. Ours took the sky along the view and was an identity over the sky, so
the air never veiled the horizon: horizon sky -1 / -3 / -8 % where UE's HDR frame has -9 / -27 / -35 %. Now
`PackHeightFog` hands compose, fog_apply (the water) and glass `inscatter` = `heightFog.inscatteringLuminance` +
`heightFog.skyAmbientContribution` x `SkyAtmosphere::DistantSkyLight` (read back to the CPU after each rebuild, 16 bytes;
x the level's `skyLuminanceFactor`, which UE bakes into that LUT) -- w = 0 (the HDRI sky, or the first frames) keeps the
sky along the view, UE's InscatteringColorCubemap option. Our sun lobe (`sunScatterStrength`) now reaches the sky too, as
UE's. After: -5 / -20 / -24 % (UE -9 / -27 / -35), analytic only at 0.04 -47 % (UE -52 %), the haze grey-blue like UE's.
What it costs: at twilight a horizontal ray always hits `maxOpacity` (0.9), so the sunset band behind the palms is 90 %
fog colour, as in UE (19:30, horizon band 90.8 vs 102.6 of 255 without fog; 24:00 49.9 vs 53.5). Knobs: Inspector "Fog
colour (procedural sky)", `--set fog.skyAmbient` / `fog.inscatterLuminance` (grays), `fog.skyAlongView:1` = the old colour,
the A/B (reproduces the old frame: 0.7 of 255 mean, clouds). No volumetric shafts at that view in UE either, at any density
(g 0.2). GBV (`SkyDistant,Compose,TransparentFog,Translucent,Skybox` + the Skybox self-test): clean.

**The sun lobe is UE's too** (2026-11-03): `HeightFogColor` adds `(HeightFogContribution (1) x the sun's illuminance on the
ground after the atmosphere + sunScatterStrength x the sun's OUTER-SPACE luminance (grey)) / 4pi x pow(V.L, exponent) x
(1 - 2^-tau past sunScatterStartDistance)` -- HeightFogCommon.ush:328-340 with FogRendering.cpp:431-438
(DirectionalInscatteringLuminance x SunLight GetColor().GetLuminance(), UE's 0.3 / 0.59 / 0.11 weights): `sunScatterStrength`
IS UE's DirectionalInscatteringLuminance. `PackHeightFog(a, hasSun, sunOuterLuminance)`, `DirectionalLight::
GetOuterSpaceLuminance`. The froxel volume keeps extinction 0.4805 x density (exact parity with the analytic integral)
where UE writes 0.5 (VolumetricFog.usf MatchHeightFogFactor): 4 % over its first 60 m. Measured against UE's HDR: above.
**The ocean reaches the horizon**: its skirt is 50 ring widths (~200 km, `OceanRenderable::BuildMesh`), and wind_test's far
plane 100 km. At 10 the water stopped at ~41 km and the level's 10 km far plane nearer still: a strip of the lower sky,
fully fogged, showed between the waterline and the true horizon (0.18 degrees from 34 m up) -- the dark line UE's far
water mesh does not have.
**`timeOfDay.fog` checked** (the curve's maths): continuous at `duskDeg` (both branches give dusk), monotonic, smoothstep,
dawn = dusk, one function for the frame and the Inspector graph. Its numbers on wind_test (density 0.005, falloff 0.06;
visibility 3.912 / (0.4805 x density x 2^-0.06h)): at sea level 1.6 km at dusk (mist), 11 km by day and by night; from 34 m
along the horizon 6.7 / 45 km. A clear tropical evening at sea is 10-20 km: the dusk key is an artistic haze, the
sunset's GLOW is the lobe -- UE's scene runs 0.002 at every hour (= dusk 0.4 x 0.005).
