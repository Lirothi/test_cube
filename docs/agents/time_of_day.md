# Time of Day, the Night and Ultra Dynamic Sky

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## Time of Day Owns the Directional Light

A level's light is its `timeOfDay` section (`app/scene/TimeOfDay.h`, plan
`docs/time_of_day_night_sky_plan.md`): every frame it places the sun and the moon and hands the scene's
ONE `DirectionalLight` to whichever has any of its disc above the horizon (else it stays on its body,
lighting nothing, and the sun-shadow passes sleep). The sky -- its LUTs, disc, capture, aerial
perspective, the bloom's corona and the light shafts -- always takes the SUN (`SceneFrameData::skyLight`),
so the twilight outlives the hand-over -- until `timeOfDay.twilight` (Ultra Dynamic Sky's curves, the
defaults since 2026-10-15) takes it out of the atmosphere: 0 at -9.03 degrees, full from -6.49; the same
section tints the FRAME's Rayleigh and ozone by sun height (the level's `skyAtmosphere` scales stay as
written). Unit tints and `-90/-90` = physics. The hand-over itself is cross-faded (`twilight.lightHandOverDeg`,
3 degrees of sun elevation each side, 0 = the hard switch): one light at a time, the outgoing body faded to 0
before the switch, the incoming one from 0 after it. Anything that writes `Scene::DirectionalLightRef()` directly is
overwritten on the next Tick: knobs go through `Scene::SunLightRef()`, a direction through
`Scene::SetSunToward()` (which solves the time and the path).

Levels written before 2026-10-13 carry the sun as `directionalLight`. Exactly one place knows that since
R9.5 (2026-11-27): `levelmigrate::Migrate` (`app/levels/LevelMigration.cpp`), run first by `JsonLevel` (the
runtime reader) and `EditorSceneDocument`, through `TimeOfDaySettingsJson::FromDirectionalLight`, which solves the simple path's time and yaw so the sun
stands exactly where it was authored -- the frame does not change (measured, within noise). A level takes
the new section the next time it is saved. `--tod-legacy-light` reads the old section as before (runtime
only). Headless: `--time=HH:MM`, `--date=YYYY-MM-DD`, `--time-speed=<x>`, `--time-freeze`,
`--set/--sweep=tod.<key>`; the HUD prints `Time: HH:MM` under `Cam:`. The STEADY night camera
(`timeOfDay.nightCameraEv100`, 0 = off; `nightCameraFollow`, 0.2): at night (the -6 -> -12 degree share) the camera
is held near that EV100 and follows only `follow` of where the auto exposure goes -- `exposure_solve_cs.hlsl`
`nightSteady`, after the curve, the eye untouched -- so dim lamps look dim (wind_test: 13.13 was the moonlit beach's
auto camera at 24:00; candles 4 -> 0.5 lm move the camera 0.21 stop instead of the auto exposure's 0.96) -- under the OPEN
sky only (below, "The night camera is the OPEN night's"), following the night's own light through the exposure
compensation curve's night section, with the trims in full (below, "The night camera follows the night's own light").
In the editor (command bar, MCP):
`setTime` ("18:30", "sunset", "+2h") and `setDate` ("2026-06-21", switches to real positions); every
`timeOfDay.*` key is in the document, so `setEnvironment` reaches the rest.

The height fog's density by the sun's height (`timeOfDay.fog`, 2026-10-31, owner: "with the sun high the fog should be far,
at sunset I want the sunset haze"): `dayDensity` / `duskDensity` / `nightDensity` multiply the FRAME's `heightFog.density`
(the level keeps its own) with the sun at `dayFullDeg` (25) and over / at `duskDeg` (5) / at `nightFullDeg` (-8) and under,
smoothstep between, dawn = dusk (`TimeOfDay::FogDensityScale`, Inspector Time of Day > "Fog By Sun Height", `--set
tod.fogDayDensity` ...); all 1 (the default) = the level's density at every hour. Ultra Dynamic Sky's Fog Density Daytime /
Nighttime Multiplier with a dusk. Author the level's density for the dusk and take the day down: a clear afternoon at the
sea sees 20-40 km (density ~0.0005), wind_test's 0.004 is 2.4 km of mist at 15:00; 0.15 by day keeps the far palms crisp at
13:00-15:00 and leaves 18:00 and the twilight as they were. The sunset GLOW is the fog's sun lobe (`sunScatterStrength`
0.5, `sunScatterExponent` 8) and the volume's `phaseG` (0.7), not the density. The Inspector draws the curve over the
level's day (x = the clock on its date and place, `SunElevationAtHours`; y = density, log, visibility in km beside it;
the day / twilight / night bands and the sun's arc): a handle dragged up / down sets its multiplier, left / right the sun
elevation it sits at (drawn where the afternoon passes it); a drag elsewhere moves the clock. One function
(`FogDensityScaleAt`) for the frame and the graph. Photograph it with `--inspect=timeOfDay#fog`.

**Night Brightness is ONE gain that comes in with the night** (`TimeOfDay::NightGain` = `nightBrightness^NightShare`, the
sun -6 -> -12, 2026-10-31): the moon (the ground's light, the atmosphere's second light, the disc), the night sky (stars,
airglow, Milky Way), the night lamps AND the sun's twilight sky all take it, and the steady night camera's EV moves with
it (`nightCameraEv100 + log2(NightGain / nightBrightness)`, Scene). The moon and the night sky used to take the whole x11364
at any hour, so a full moon rising at dusk (the level's 2026-09-27) was a ~3600-lux second sun over a physical twilight:
clouds and sand lit white, a giant aureole, the night's stars over a blue-hour sky (owner: "very pretty, not realistic"),
and by day a moon in the sky lit the atmosphere at 3 % of the sun. One gain keeps every ratio physical at dusk and makes
the multiplier an exposure shift by night. Measured on the moonrise view (cam -10.94 6.19 72.56): 21:00 and 24:00 unchanged
(0.18 % / 0.03 % of pixels over 8 levels, the noise), the dusk -> night now monotonic (median 55 / 12 / 8 / 7 at 19:54 /
20:06 / 20:15 / 20:24; the camera held at 13.13 alone had put 20:15 1.4 stops under both). What dusk shows now is the
twilight sky itself, and UDS's tints make it saturated blue (`twilight.ozoneNightTint` R x6.15: sky R/B 0.05 at 19:54);
unit tints give Hillaire's pink-violet (0.32, the sand pink) -- the truth lies between, not calibrated yet.

**The night camera is the OPEN night's** (2026-10-31, owner at the bathroom, 20:21: "can the outside be not too bright and
the inside normal?"). Held at the moonlit beach's EV, the candle-lit bathroom (cam -23.30 1.85 86.04) was 3.4 stops under
its own eye -- median 2 of 255, 86 % of the frame under 20. Now `nightExposureEv` and the steady camera come in by the
frame's OPEN-SKY share: `exposure_histogram_cs.hlsl` reads the RT sky visibility on its metering grid (the sky itself 1, a
surface the length of its bent normal; untraced = 1, every frame open) into two sums behind the bins, `exposure_solve_cs.hlsl`
smooths it (0.5 s) and takes `timeOfDay.nightCameraOpenFrom` 0.15 -> `nightCameraOpenFull` 0.35 of the OPEN night's camera
(`NightOpenCamera`: the trim, then the steady hold); a CLOSED frame takes `NightClosedCamera` -- the auto camera, opened at
most `nightCameraIndoorLift` (5) stops past `nightCameraEv100`, so a lit room keeps its own brightness and an unlit one stays
dark (followed freely, the empty tent at midnight came out as a grey day, camera EV 1.4). The share the camera takes moves at
the level's adaptation speeds (a full switch moves the camera `span` stops: `speedUp` / `speedDown` x dt / span a frame;
smoothed alone the 4 stops beach -> bathroom passed in 0.75 s). The trim moved from compensationEv to the camera after the
curve: at the full share the frame is the same, the eye readout (`eyeEv100`) is now the scene's. Shares at night: the
beach 0.88-0.91, the grove 0.39-0.76, the bathroom 0.03-0.10, the second room 0.07-0.11, the tent 0.001-0.007
(`get_readouts` openSky / openNight). Measured: bathroom 20:21 camera 12.47 -> 8.64 (eye 7.56), median 2 -> 74, under-20
share 0.87 -> 0.25; the beach at 21:00 / 24:00 and the grove at 24:00 unchanged to the digit; the tent and the second room
held at the floor (8.13). Dim candles still look dim: the bathroom's candles x0.125 (4 -> 0.5 lm) move its eye 2.8 stops
and the camera 0.6 (8.64 -> 8.05, the floor at 20:21), median 74 -> 13. Full <= From = always whole, the A/B
(`--set tod.nightCameraOpenFull:0`). Inspector Time of Day >
"Night Camera Open Sky From / Full", "Night Camera Indoor Lift". GBV (`ExposureMetering,Skybox` + the Skybox self-test,
the bathroom at 20:21): clean.

**The night camera follows the night's own light, and its trims work in full** (2026-11-01, owner: "at night with a partial
moon it is terribly dark and no knob does anything"). Held at the full moon's EV, a quarter moon 22 degrees up (14x less
light on the ground) came out nearly black and a moonless night wholly black -- 01:00 on the moonrise view, camera 12.86 /
12.53 / 11.64 for full / quarter / none, median 8 / 1 / 0 of 255, p95 33 / 26 / 0 -- and Night Exposure and the
compensation sat INSIDE the auto camera the steady hold follows by 0.2 (Night Exposure -1.5 -> 0 moved the quarter-moon
camera 0.3 stop). Now (a) the night's light: `TimeOfDay::NightEv100` = the EV100 a meter reads off middle grey under it
(log2(0.18 E / pi) + 3), physical lux before Night Brightness -- the moon (its phase law, distance, sin of its height), a
clear sky's twilight (log10 lux 0.53 at -6 degrees, -0.43 a degree: 3.4 / 0.08 / 0.009 at -6 / -10 / -12) and the night sky
(2 pi x airglow zenith + stars + Milky Way, ~0.001 lux): -3.27 for the full moon at 45 degrees (`NightRefEv100`), -6.9 a
quarter moon 22 degrees up, -11 no moon (`get_readouts` nightEv100, nightLightEv = the difference from the full moon). The
steady EV (Scene) = `nightCameraEv100` + that difference - [curve(NightEv100) - curve(NightRefEv100)]: the camera follows
the night's light less the exposure compensation CURVE's change, (1 - slope) of it, as the auto exposure follows a scene --
computed from the sky's bodies, not the meter, so lamps and a turn of the view still do not move it. Read at the PHYSICAL
EV on purpose (owner, 2026-11-01: "we have a special correction curve for the night, edit it"): keyed with Night Brightness
in it, a quarter-moon beach (eye 7.56), wind_test's bathroom by day (6.74) and at night (7.6) sat on one point of the curve,
so the night could not be darkened without the day's room; physical, the night's section lies under ~0 EV, away from every
day scene. A curve flat there = the night's light followed in full. The curve holds 12 points now (6, then 8; 12 since
2026-11-04 so UDS's eight day points keep the night's three: the shader's three float4 pairs). (b) `exposure_solve_cs.hlsl`: the open night's anchor is `nightCameraEv100 - nightExposureEv -
compensationEv` and the closed frame's floor `- compensationEv`, so a stop of either is a stop of the night.
`nightCameraEv100` is therefore the camera for the full moon 45 degrees up with the trims at 0: wind_test's 13.13 had its
-1.5 inside and would put that full moon 0.9 stop darker; 11.93 keeps it (camera 12.76 vs 12.86, median 9 vs 8).
THE PRESETS CARRY THE NIGHT (`SetUdsSteadyNightExposureCurve`, `SetUdsExposureCurve` -- the latter UDS's curve one to one
by day and, for one afternoon of 2026-11-04, flat -1.51 under our EV 5.3 as UDS's is, which made every moon phase as bright
as the full moon: below "Ultra Dynamic Sky", "The night under UDS"): from -3.5 at their first point's
value, 1.05 stops down at -7, 0.6 more at -11 -- the camera follows 70 % of the darkening from the full moon to a quarter,
85 % on to no moon (a first cut, 35 % / 79 % below, held the moon's aureole down before the aerosol was cut at night and
left every partial moon black); each dropped its 8.55 point for room (UDS Steady Night's lay on the line, UDS's 0.06 EV off it). A
curve FLAT under ~0 follows the night's light in full: the owner's UDS Steady Night as it was (2026-11-01, 00:00, a
last-quarter moon 10.8 degrees up, cam -9.00 7.65 89.30) exposed the night as a dim day, the moon in a great glow --
camera 9.45, median 57, p95 186; with the night section 11.34 / 5 / 72. wind_test is that preset (the owner's pick), except
its EV 4.55 point: -2 instead of -4.92 since 2026-11-03 (owner at the bathroom, 16:00, cam -22.89 1.49 87.92: "isn't it too
dark?"). The room was not under-lit -- a grey probe at desk height read 316 lux against 46.7 klux in the open (a daylight
factor of ~2 % of the sky alone, normal for one small window) -- the curve took it 3.43 EV under its meter (eye 7.03, camera
10.46, median 8). Now -1.72 (camera 8.76, median 45). Everything metering EV -3.5 .. 10.55 moves: the bathroom at night
+1.4 stops (median 12 -> 43), the blue hour outdoors at 19:30 +1.9-2.1 (median 2 -> 13-16), the open night 21:00-24:00
0.2-0.45 stop brighter (the steady camera's follow above the full moon's EV), the day outdoors unchanged. With
the first night section (35 % / 79 %) on the moonrise view at 01:00 (2026-09-27 / 10-03 / 10-07): full 12.76 / median 9 /
p95 35, quarter 11.38 / 4 / 60 (a 50 % follow gave 8 / 78, a flat curve 41 / 160), no moon 7.95 / 1 / 5 with the stars and
the island's silhouette; the moonrise 19:54 / 20:06 / 20:15 / 20:24 median 52 / 21 / 20 / 21 (was 55 / 12 / 8 / 7). A
closed frame keys the curve on the meter as before, so the curve's day section sets the rooms: under UDS Steady Night the
bathroom at night is median 17, under the gentle 10-31 curve 73.
THE GLOW ROUND A LOW MOON is the sky's aerosol (mieScale 10, g 0.8 -- the day's calibrated aureole, the same air). On the
owner's view, clouds off, the sky's median display value at 2 / 4 / 7 / 10 / 15 / 20 / 30 / 40 degrees from the moon: 105 /
95 / 78 / 54 / 26 / 11 / 4.4 / 2.3. The height fog off: 111 / 100 / 80 / 52 / 22; its sun lobe off or the volume's sun off
moved it 2-6; `sky.mieScale:1` 41 / 39 / 33 / 23.5 / 12; both 22 / 20 / 15 / 7.7 / 3.4. Not it: the moon's glow, its
shafts, the bloom (p95 0-2 of 255), the night tints (unit: 54 -> 48 at 10 degrees), the local exposure (lifts the
background, keeps the glow). A larger Mie g made it BRIGHTER here (g 0.9: 86 at 10 degrees, 5.5 at 40), not tighter.
The owner, looking at the real moon from a window: "nowhere near any glow"; photographs of a moon over the sea (Wikimedia
Commons, long exposures, e.g. "Moon At Francavilla Al Mare Coast") show a halo a few degrees wide on an even dark-blue sky --
the great glows are the misty nights'. So (owner's pick: less aerosol at night) **`timeOfDay.twilight.mieNightScale`**
(Inspector Time of Day > Twilight "Night Aerosol", `--set tod.mieNightScale`; 1 = physics): the frame atmosphere's Mie
scattering and absorption times it by the night's share (the sun -6 -> -12, in the tints' 1/64 steps so the LUTs rebuild
at most 64 times a dusk; the anisotropy kept), not by the tints' day blend -- the sunset's haze round the sun stays.
wind_test: 0.1 (the sky's aerosol at UE's default by night; 1 since the day's mieScale went back to 1 on 2026-11-02, the
same night), and its `fog.nightDensity` 1 -> 0.15 (the day's: at 1 the
night was 2.4 km of mist, and the fog's moon scattering was most of what remained). The rings at 2 / 4 / 7 / 10 / 15 / 20
/ 30 / 40 degrees: 25 / 23 / 18 / 10 / 4.4 / 2.4 / 1.5 / 1.3 (0.1 alone: 42 / 39 / 33 / 23.5 / 12 / 5.5; 0.3: 61 / 56 / 47 /
32 / 16); the frame median 5 -> 3, p95 72 -> 35, the moon's path on the water as it was. The day and the sunset unchanged
(16:30-17:40 toward the sun: 0.3-0.8 % of the frame moved > 8 levels); the fog's dusk -> night ramp thins the twilight's
haze a little (18:00 3 %, 18:20 9 %, 18:40 14 % of the frame, a crisper horizon, the blue hour kept).
THE MOON'S CYCLE CHECKED (2026-11-01, owner: "check the lighting is sane at every phase -- the island and the trees too"):
Oct 26 - Nov 24 2026 at 20 / 23 / 02 / 05, then the sea view (above), the palm grove ([34, 3.5, 68] yaw 80 pitch 8) and
the island from above ([-75, 28, 40] toward [10, 0, 90]). The light is consistent: nightLightEv full 0 / gibbous -0.9 /
quarters -2.5 .. -3.8 / a crescent 9 degrees up -6.5 / no moon -7.75 (a quarter's ground light 15 % of the full one's, the
new-moon floor 1.5 % in it), the palms' shadows wherever the moon is up and none under the horizon, nothing blown out; a
moon rising (10-29 20:00, its centre -0.3 degrees, the level's 2-degree disc half up) gives the frame of a moonless night
with a faint glow and a path on the water at the horizon. But at the 35 % follow and Night Exposure -1.5 every night read
too dark: the full moon's grove median 0 (the palms black: their albedo a third of the sand's, all in the tone curve's
toe), the quarters' median 1-3 on every view. So the night section is 70 % / 85 % and wind_test's `nightExposureEv` -1.5
-> -0.5 (the owner's knob): median full / last quarter / first quarter / no moon -- sea 22 / 9 / 11 / 16, island 25 / 10 /
11 / 5, grove 1 / 0 / 2 / 1 with p95 85 / 46 / 34 / 41 (the palms and the sand read, the shadows of the fronds); the low
last-quarter moon of the owner's view: camera 9.28, median 32, the glow 61 at 10 degrees on a sky of 18 at 40 -- a compact
halo, the stars and the path on the water. The bathroom (closed: the curve's day section, the owner's UDS Steady Night)
camera 10.11 at 15:00, 10.44 at night.

Night lamps (plan L, `rendering/lighting/NightLamps.h`): a point/spot light or a glowing mesh with `"onAt": "night"`
(or `"day"`) switches when the sun crosses `timeOfDay.lamps.sunElevationDeg` (0 = UDS's Sunset), UDS-style delay and
warm-up, instant when the clock is set by hand; a night lamp takes `nightBrightness` as the night comes in. Brightness,
never `enabled` (that rebuilds the lights behind a GPU wait). MCP: `setOnAt` -- lights through the selection;
`setLight {lumens | scale, radius, color, shadows, flicker, flickerHz}` retunes lights that exist, by name or group
("Bathroom Candles"), one undo entry -- `scale` multiplies each light's own flux, a spot's radius is its range;
`flicker` is a point light's +/- share of its flux (PointLightFlickerDesc, three incommensurate sines; a candle 0.15
at 3 Hz), each lamp seeded on its own so a row of candles does not breathe in step. A point/spot light's own
`"dayBrightness"` (Inspector "Day Brightness", `setLight {dayBrightness}`; absent = 1, 0 = dark by day) multiplies its
flux by day and goes in stops by the night's share (`Scene::TickNightLamps`), on top of the lamps' global Day
Brightness for an always-on lamp; without a time of day it is always day. Checked live: the bathroom candles x1000 at
15:00 blew the frame (mean 77 -> 241), at 24:00 no pixel moved > 8 levels. The Inspector's logarithmic drags cross
their range in ~600 px (`dragF`: ImGui moves a log drag's 0..1 parameter by speed x px / (max - min), and a "%.0f"
row rounded the flux back to 0 or 1 every frame).

## Ultra Dynamic Sky -- What It Does to the Fog and the Sky (2026-11-04)

The owner's UE WindTest carries UDS (actor `CLAUDE_TEST_UDS`, editor layer `CLAUDE_UDS`; our env sun / sky / skylight / fog
/ clouds are layer `CLAUDE_Env`; WindTest is no World Partition map, so no data layers). Owner: UDS is the default there
(`CLAUDE_UDS` shown, `CLAUDE_Env` hidden; not saved by us). Read off it with UE Python (scratchpad `ue_uds_sweep.py`:
setting its `Time of Day` re-runs its construction and REPLACES its components -- fetch them again after every set;
`ue_fngraph.py` / `ue_matdump.py` walk its material graphs; Git Bash rewrites `/Game/...` arguments -- `MSYS_NO_PATHCONV=1`).
- **Why UE's plain height fog draws a dark band at a high sun**: its colour is the distant sky light (the sky's AVERAGE,
  isotropic) plus a narrow `pow(V.L, e)` lobe -- no side or back phase of the sun, no multiple scattering -- so a dense
  horizontal fog is darker and bluer than the horizon sky it covers. UE's legacy artist model; the physical haze is meant to
  be the SkyAtmosphere's aerial perspective.
- **UDS's fog** (UE units / 10 = ours per metre): density 0.0055, falloff 0.06, at z -1.5 m, start 73.5 m, max opacity 1, no
  volumetric fog, aerial perspective OFF (`AerialPespectiveViewDistanceScale` 0: the haze is the height fog alone), its
  SkyAtmosphere's `HeightFogContribution` 2 by day (it scales BOTH the fog's sky ambient and the ground half of the sun lobe,
  HeightFogCommon.ush:190 / :334), a warm `DirectionalInscatteringLuminance` (0.79, 0.64, 0.56) at exponent 5 from 100 m,
  both easing to the horizon (UDS's curves: `TimeOfDay.cpp kUdsFog*`), a blue `FogInscatteringLuminance` floor from the
  sunset on. Its sky: Rayleigh x1.21 (its colour 0.1686 / 0.4078 / 1), Mie x3.5 with a BLUISH colour (0.802, 0.880, 1) and
  Earth's absorption, g 0.75, ozone (0.8972, 1, 0.0953) x 0.002, ground albedo 0.40, sun disc 1 degree, 6500 K.
- **Ours now has every one of them**: `skyAtmosphere.heightFogContribution` (`--set sky.heightFogContribution`), the
  medium's per-channel `rayleighTint` / `mieTint` / `ozoneTint` and `mieAbsorptionScale` over the x Earth scales (Inspector
  Sky Atmosphere > Medium; `SetSkyMedium`), the fog's `sunScatterColor` (UE's DirectionalInscatteringLuminance is a colour;
  `--set fog.sunScatterR|G|B`; the packed fog grew `sunDirectional`, params2.y is reserved), and `timeOfDay.fog.udsColor`
  (Inspector Time of Day > Fog By Sun Height "Ultra Dynamic Sky fog colour", `--set tod.fogUdsColor`): UDS's three curves
  by the sun's elevation, multiplying the level's HeightFogContribution and sun scatter and adding its floor (per lux of
  its 5-lux sun x ours). Off and at 1 / white = the frame before, bit for bit. A copy of wind_test with UDS's numbers:
  scratchpad `make_uds_level.py` (the level's tints relative to UDS's day: night ozone (2.23, 1.70, 0.64), sunrise (1.67,
  1.46, 1.80), night Rayleigh (1.43, 0.85, 0.61)). GBV (`SkyEnvironment,Compose,TransparentFog,Transparent,Translucent,
  Skybox` + the Skybox self-test, that copy at 17:36): clean.
- **Compared** (scratchpad `ours_uds_shots.py` / `ue_uds_shots.py` / `uds_sheet.py`, the owner's viewport, toward and away
  from the sun, ours with the sun put exactly where UDS's is -- `sun.elevation` / `sun.azimuth` = UE light yaw + 180 --,
  both exposures pinned, UDS's 14.55 stops up for its 5 lux): the horizon haze, the sunset's colours and the glitter column
  match; no dark band. What remains is NOT the fog:
- **UDS's sky is an art layer over the SkyAtmosphere** (`Ultra_Dynamic_Sky_Mat`, read node by node): sky = `Overall
  Intensity` x contrast(`Contrast Midpoint`, `Extra_Contrast` 0.1) x [ SkyAtmosphereViewLuminance x S x (1 - A) + A^2 x S ]
  + `Night Sky Glow Color`, with A = `Ambient Color` ((0.15, 0.27, 0.50) at noon -- half the blue replaced by a constant),
  S = `Lit_Intensity` 0.8 x (2.3 + 1.8 (u + v)), u = (1 + dot(view stretched vertically, sun)) / 2 (`Sun_Centered_Gradient`,
  a Customized UV), v the moon's at night. `Overall Intensity` 0.375 by day, 0.69 at sunset, 1.23 at -3 degrees: by day the
  sky is x0.69 away from the sun and x1.23 toward it; ours measured 1.3-2x UDS's in cd/m2 by day. Its twilight is brighter
  still for its MOON: 0.15 lux = 1/33 of its sun, full, opposite the sun, from the sunset on, and its Night Sky Glow.
- **Ported, all of it (owner, same day: "fit everything of ours to them, create new entities if needed")**:
  `timeOfDay.twilight.udsSkyMaterial` (`--set tod.skyUdsMaterial`, Inspector Time of Day > Twilight) = `shaders/uds_sky.hlsli
  UdsSkyLayer` on the drawn sky (skybox.hlsl) AND its capture (sky_ibl_capture_cs.hlsl: UDS's sky light captures its
  sphere; its glow x2 there), UDS's Overall Intensity / Contrast Midpoint / Night Sky Glow by the sun's elevation
  (`TimeOfDay.cpp kUdsSky*`, read off its material instance every 3-30 minutes; `UdsSkyFrameData`). Its Ambient Color lerp is
  left out: measured, it does nothing. CHECKED: ours with / without the layer against UDS / UE's bare atmosphere at 6
  times x 4 views x 4 points (scratchpad `ours_layer_check.py`): median 0.99-1.005, p10-p90 0.90-1.18.
  `timeOfDay.moon.udsMoon` (`--set tod.udsMoon`, Inspector Moon "Ultra Dynamic Sky moon"): the moon and the night sky take
  Night Brightness in full from the sunset (0 at +1.15 degrees, linear, as UDS's moon light), the sun's twilight sky none
  (`TimeOfDay::MoonGain`; the lamps and the night camera keep NightGain). UDS's sun path is our SIMPLE path yaw 180 / tilt 30
  / 6:00-18:00 (within 0.05 degrees), its moon full, tilt 33.73 (within 2.6: UDS's sits ~2.3 higher).
- **wind_test carries UDS since 2026-11-04** (scratchpad `make_uds_level.py uds_edit`, written by `apply_uds_to_level.py`,
  which touches only the changed lines; the file before is `wind_test_before_uds.json` in the scratchpad): the sky above
  (tints, Mie 3.5 bluish, AP off, ground 0.40), the fog (0.000551 / 0.006 / -1.5 m / start 73.5 m, lobe 1 x5 from 100 m),
  `timeOfDay.fog` all 1 + udsColor, the twilight tints relative to UDS's day, udsSkyMaterial, udsMoon, the simple path's
  numbers (UDS's), the sun disc 1 degree / soft 0.5 / 6500 K, and the exposure = UDS's curve ONE TO ONE with compensation 0
  (owner, the same evening: `SetUdsExposureCurve` is that now -- its .uasset's five cubic keys +14.55 EV, eight points within
  0.015 EV, checked against UE's own get_float_value to 0.00007 EV; the Inspector's "UDS" button sets the compensation 0 with
  it). The owner KEPT his volumetric fog, real sun positions and the sun's light shafts (UDS has none of the three; he
  switched them back on, and `uds_edit` no longer touches them). Before the one-to-one curve (the old preset + 0.85 by day,
  each engine on its own auto exposure, the owner's viewport toward / away from the sun 12:00-18:15): the sky within ~0.3
  stop on most views, the horizon's haze, the sunset and the blue hour with the moon rising as UDS's; the old preset was
  shifted -0.919 to keep the old day, and the one-to-one curve is ~+0.9 over it by day -- the same gap. Still different:
  the water (UE's water body is another system) and our wider sun glitter (`ocean.sunGlintSmear` 1).
- **The night under UDS** (owner, the same day, at 21:54 on Sep 11 -- a new moon, under the horizon -- cam -18.30 10.42
  88.47: "insanely bright with the moon invisible"; median 206 of 255, a pale day). Two of ours: (1) the steady night camera
  follows the night's light as TimeOfDay models it (moon + twilight + night sky), and UDS's Night Sky Glow is a light of its
  own -- a moonless night under it is 3.6 stops under the full moon, the model said 7.9, and the camera opened the
  difference: every waning phase BRIGHTER than the full moon (simple path, 21:54, full / gibbous / quarter / crescent /
  new: median 45 / 46 / 55 / 88 / 108). `TimeOfDay` now counts the glow (2.618 x its luminance on a level surface, x2 in
  the capture, x Overall Intensity), and under `udsMoon` each light with the gain it is drawn with (the moon and the night
  sky Night Brightness from the sunset, the twilight none): the model reads -4.15 for the new moon against the frame's
  -3.8. (2) the one-to-one curve was flat under 5.3, i.e. the night's light followed in full; `SetUdsExposureCurve` carries
  the night section again (12 points) and so does wind_test's curve (three points written under its eight). After: full /
  gibbous / quarter / crescent / new 43 / 35 / 29 / 29 / 30, the owner's view 34 (36 % of the frame under 20, stars, the
  island's silhouette); real dates at 21:54 -- Sep 15 / 18 (the quarter low) 33, 22 36, 26 (the full moon rising, its path
  on the water) 59, 27 at 01:00 41, Oct 4 / 7 at 01:00 / 03:00 32 / 31; the dusk at his view 18:00-21:00 122 / 107 / 69 /
  68 / 36 (monotonic); the bathroom (closed) 64 at 16:00, 58-60 at night with any moon. Root fix alone (the flat curve)
  left the new moon at 54 over the full moon's 43. UDS ITSELF in UE (the same view, 21:54, its own auto exposure, coverage
  0, Moon Phase 0 / 3.75 / 7.5 / 11.25 / 15 days = full .. new): a dim DAY at every phase, median 172-184, the moon's
  shadows as hard as the sun's -- its moon light follows the phase (0.15 lux x lerp(0.015, 1, lit), lit 0.158 at a quarter)
  but its sky material's Overall Intensity RISES as the moon wanes (0.45 full -> 1.35 new) and its auto exposure lifts the
  rest. Not ported (it brightens what the owner calls too bright). Also seen: the rising full moon wears a tall Mie aureole
  (`twilight.mieNightScale` 1 from uds_edit, UDS's Mie 3.5; 0.1 -- the owner's 2026-11-01 pick -- takes most of it).
  GBV (`ExposureMetering,Skybox` + the Skybox self-test, that view): clean.
- **MEASURING UE UNDER UDS** (cost an afternoon): (1) UDS rebuilds its components on every property edit, so pinning ITS
  post process does nothing -- the frame at bias b and b+1 decoded 2x apart; pin with a post process volume of our own on
  top (scratchpad `ue_linpp.py`: unbound, priority 1000, label CLAUDE_TEST_linear_ppv, destroyed after). (2)
  `CaptureViewport` from a transform other than the viewport's renders a view of its own whose exposure does not follow:
  move the viewport (`UnrealEditorSubsystem.set_level_viewport_camera_info`) and capture there; the first frame after the
  volume appears is stale. (3) Absolute cd/m2 do NOT carry between UDS's 5-lux world and ours: under a 5-lux sun at
  exposure +14.55 UE lit the probe box's SHADED face 600x what it got at 120 klux (Lumen's cache and the sky light are kept
  at a fixed CachedLightingPreExposure), and its bare atmosphere read 2x ours everywhere. Compare RATIOS within a frame (UDS
  / the bare atmosphere, sky / ground) and the display as each engine exposes it, not absolute radiance across worlds.
