# Repository Instructions

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

Levels written before 2026-10-13 carry the sun as `directionalLight`. Exactly two places know that:
`JsonLevel` (the runtime reader) and `EditorSceneDocument`'s section table, both through
`TimeOfDaySettingsJson::FromDirectionalLight`, which solves the simple path's time and yaw so the sun
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

## Line Endings

- Preserve the existing line ending style of every edited file.
- For C++/Windows project files in this repository, use Windows CRLF endings.
- Before finishing edits, verify touched text files do not contain mixed endings. In PowerShell, a useful check is:

```powershell
$files = @('path\to\file.cpp', 'path\to\file.h')
foreach ($f in $files) {
  [byte[]]$b = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $f).Path)
  $loneLf = 0
  $loneCr = 0
  for ($i = 0; $i -lt $b.Length; $i++) {
    if ($b[$i] -eq 13) {
      if (($i + 1) -lt $b.Length -and $b[$i + 1] -eq 10) { $i++ } else { $loneCr++ }
    } elseif ($b[$i] -eq 10) {
      $loneLf++
    }
  }
  Write-Output "$f loneLF=$loneLf loneCR=$loneCr"
}
```

- If a touched source file has mixed endings, normalize it to CRLF before the final response.

## Session Log

Every process (the app, every `--*` harness) writes one event log:
`logs/session_YYYYMMDD_HHMMSS_<pid>_<debug|release|release_editor>.log`; `logs/latest.txt` names
the newest one. Line shape:

```
01:14:22.381 WARN [render.rt] 1842 message (File.cpp:123)
```

Fields: time of day (the date is in the session header), level, `[category]`, frame (`-` before
the first frame), message; Warning+ carry the source suffix. The thread is not in the file (the
Session Log viewer shows it).

A missing `session end: clean shutdown` footer means the process did not shut down cleanly. In
the app, Window > Session Log in the editor or Developer Controls > Debug opens the Session Log
viewer (filters by level/category/text, pause, copy, source location); `--log-window` opens it at
boot so a `--shot` can capture it. Use
`LOG_INFO(logging::LogCategory::Scene, "Loaded {}", path)` and friends (`core/logging/Log.h`);
never `OutputDebugString` or `printf` for events. Gates for lines evaluated every frame:
`LOG_*_ONCE` (once per process), `LOG_*_EVERY_N(n, ...)`, `LOG_*_THROTTLED(duration, ...)`,
`LOG_*_ONCE_PER_MESSAGE` (once per distinct text among the callsite's last 16 — for a quantised
STATE line). `WriteRaw`/`WriteRawLines` take already-formatted text (SDK callbacks, compiler
output) without formatting or heap.

Levels: `Trace` = explicitly enabled high-detail (compiled out of Release), `Debug` = developer
state transitions (dropped by a Release session), `Info` = lifecycle and completed operations,
`Warning` = a recoverable fallback or degraded quality, `Error` = the requested operation failed
but the process continues, `Fatal` = an invariant is broken and the process is about to stop.
Pick the category by owner (`render.rt`, `render.shadow`, `asset`, `vfx`, ...), never mark
everything Info, and never log per frame without one of the gates above. Session logs are
rotated at boot: of every `logs/session_*.log` (auto-named or an explicit `--log-file` that keeps
the prefix) the newest 10 by write time (or 100 MiB) are kept — never more than 10 on disk;
fixed-name artifacts use other prefixes and are never deleted. `python tools/check_logging.py`
reports any new direct output (exit code = findings) — run it before committing. So does
`python tools/check_render_knobs.py`: a game file (the command line, `--set`/MCP, settings, Developer Controls...) that
touches a render knob's render copy instead of its `_game` twin (`core/threading/Knob.h`, docs/game_thread_plan.md G5).
`frame.renderThread` 1 (Developer Controls > Frame "Render thread", `--set`; the DEFAULT since 2026-11-03, the owner's
call after the G9 gates; 0 = one thread) runs `Scene::Render` on its own thread, named `Render` in the log (plan G8):
the game thread runs the next frame's game half meanwhile -- the UI, the editor, MCP, `Scene::Tick`. Game-half code that
reaches what the frame in flight uses calls `threading::WaitRenderForGame("<site>")` first (or hands over at Sync, as
ImGui's textures do); the line `render thread: the game half waited for its frame N times` names the sites every 5 s,
and a `--trace` shows each such wait as `Game: Park Render`. A UI panel waits only where it reaches the render half's
objects (Developer Controls: its Visibility, Sky, Debug, CSM, VSM, Tessy, Streaming and Memory tabs), never as a whole:
waiting for every tab took the overlap away whenever the window was open (Debug wind_test: the game half 12.6 -> 2.6 ms).
INPUT is latched per frame: `InputManager` accumulates the presses, releases, mouse motion and wheel its messages bring
whenever they are pumped (the game thread pumps while it waits -- `RenderThread::WaitIdle`, a park), and `NewFrame`,
called right AFTER the frame's own pump, hands them to the frame; it used to zero them, so a key or a mouse move that
arrived during the wait was lost (F9 in Debug almost never toggled). The function keys reach `InputManager` unless an
ImGui text field is being typed in (not merely while a window has keyboard focus). `--set` / MCP
`debug.profilerOverlay:1` turns the F9 overlay on without a key. WHAT A DEBUG FRAME PAYS FOR (2026-11-03, wind_test with
the editor and Developer Controls, two rounds): as built 13.0-13.2 ms; with `/Ob1` and no `/JMC` / `/RTC1` 6.3-9.1 ms
(the render thread 11.4 -> 5.1-7.7); without the D3D12 debug layer (`--no-d3d-debug`, Debug only, GBV keeps the layer)
11.4-11.8. A container of our own would only win STL's iterator checks back -- under `/Ob0` its `operator[]` is still a
call; a Debug build at `_ITERATOR_DEBUG_LEVEL=0` does not link (the prebuilt Debug `mimalloc.lib` was compiled at 2).
`frame.oneFrameThreadLag` (default 1, UE's `r.OneFrameThreadLag`; 0 = the game half waits for each frame right after
handing it over: no overlap, no frame of lag); the line `frame: input to present N frames` measures it (0 / 1 / 0 for
one thread / the default / lag 0). A gate that touches the frame's order runs in both modes (`--set=frame.renderThread:0`
for one thread). **`--rt-audit[=<s>]`** (600 by default; `--rt-audit-selftest` must FAIL naming `object #0 model
matrix`): random 0-2 ms pauses where the halves meet, and the render thread hashes everything the hand-over froze for it
(objects' matrices, positions, switches, the lights' render descs) at its frame's start and end -- `rt audit: PASS|FAIL
<field> xN` in the session log, the exit code the fields that moved (`app/diagnostics/RtAudit.h`). Run it after any
change to what the game half touches -- for coverage over time: `--scene-stress=150 --rt-audit` rides the level churn
(verdict at the harness's end), and an editor run with `--rt-audit=<s>` rides an MCP script; 600 s on one static view
adds rare timing windows, not code paths. `frame.rhiThread` (default 1 since 2026-11-03, UE's `r.RHIThread.Enable`; plan G10b) moves the
frame's submission -- ExecuteCommandLists, the waits between the queues, Present, the frame fences -- to a thread named
`RHI` (`rendering/core/RhiQueue.h`): every queue operation of a frame in flight goes through `Renderer::Rhi().Run`, never
to the queue directly, and anything that needs them done waits with `Flush` (`WaitForPreviousFrame` does). A level
switch's phases are in the boot profile's dump as `Level switch (runtime)` (its tree is Debug: `--log-level=debug`;
`--scene-stress=<n> --stress-level=<level.json>` boots into that level and reloads it first, plan G10d). A `--trace`
lists its rows as GPU Graphics, GPU Compute, MainThread, RenderThread, RHIThread, then the workers
(`thread_sort_index`). MCP `open_level {path, discardChanges}` switches the level (refused over unsaved edits unless told
to discard them).

**Diagnostics are events, not files.** Anything a subsystem wants to say -- a pre-assert dump, a
validator's mismatch list, a self-test's per-case lines and verdict, a cross-check that fired --
goes through `LOG_*` into the session log (Fatal before an assert: it is flushed synchronously, so
the record survives whatever the dialog's button does). Do NOT add a new `logs/<name>.log` for it:
the owner said so (2026-09-03) after `s14_assert.log` / `hzb_cull_selftest.log` appeared beside
the session log. A headless gate reads its verdict line from the session log (`Select-String` for
`cull validation PASS`, `hzb cull self-test: PASS`, ...) and the exit code.

The `mem:` line (Info, `core`, every 5 s; `rendering/core/MemoryReport.h`) puts process private
bytes, VRAM and mimalloc commit beside per-owner byte counts (`heaps` / `other` only under `--memtrack`: `HeapSummary` holds the driver's heap lock ~21 ms, on any thread a spike in the frame; `--memreport`, `get_memory` and the Memory tab read it on demand); a subsystem that owns memory the
process counters cannot attribute registers a provider (`RegisterMemoryProvider`). A number that
grows while the others stay flat names the owner -- read it before reaching for a profiler
(docs/bug_rt_retire_bin_leak.md). It ends with `desc ring <peak>/<capacity>`: the period's fullest frame of the
per-frame CBV/SRV/UAV staging ring (`Renderer::StageSrvUavTable`, 4096 a frame, `BindlessHeap::kFrameRingCapacity`).
An overflow THROWS -- a crash -- and a frame past three quarters logs `frame descriptor ring ... used` (render.rhi)
first. The rule it came from (2026-10-27, owner: wind_test's "Lamps" group switched on at runtime crashed): a table
that is the same for every view or draw is staged ONCE a frame and its GPU handle reused -- `ShadowGpuData::
RecordIndirectShadowDraws` staged its three SRVs per shadow view, and 128 shadowed points + 32 spots are 814 views:
~2.4k of the 4096. Staged once, the same switch-on peaks at 1257/4096.

Structured reports a SCRIPT parses as a table (`csm_readout.log`, `visibility_readout.log`,
`cull_benchmark.txt`, the stress verdicts) are ARTIFACTS, not events -- an existing, closed set;
a new one needs the owner's OK first. Write them
with `diag::ArtifactFile` / `diag::WriteArtifact(name, mode, text)` (`core/diagnostics/ArtifactWriter.h`),
declaring the mode — `PerRunTruncate` (first open per process truncates, later ones append),
`Append` (history across runs, session separator written once), `UniqueSession`
(`<stem>_<stamp>_<pid>`), `AtomicReplace` (temp + rename, one complete report). Never
`fopen(diag::LogPath(...))` with a hand-rolled "w"/"a" protocol; the API logs one
`artifact logs/<name> (<mode>)` event per name per process for you. Switches: `--log-level=<trace|debug|info|warning|error|fatal>`,
`--log-category=<name>:<level>` (repeatable; names are the `[..]` column, e.g. `render.rt`),
`--log-sync` (render every record on the calling thread — for a crash whose last lines never
reach the writer), `--log-no-file`, `--log-file=<path>`. `--log-stress` runs the logging harness
(verdict `logs/log_stress.log`, exit = failed checks). Design and status: `docs/logging_system_plan.md`.

## GPU-Based Validation (GBV) — How to Run It

Debug only (the debug layer exists only there). GBV rewrites each shader on the CPU the first time
its pipeline is bound, so the cost is minutes, not milliseconds: a full wind_test frame waits
**155-161 s** for its first frame, Lighting alone ~150 s of it. Pick the run by the question:

- **"Is the pass I changed clean?"** -- narrow it:

  ```bash
  test_cube.exe --level=data/levels/wind_test.json --gbv --gbv-passes=CloudTrace --no-streamline --shot=out.png --shot-delay=3
  ```

  Guarded validation on the listed passes' command lists, STATE_TRACKING_ONLY on the rest: **16 s**
  to the first frame. Names are `RenderPassToWString`'s (CloudTrace, Compose, Lighting, RTTrace, ...),
  case-insensitive; `GBuffer` also covers `GBuffer.*`; a CL group's members are patched together
  (logged). It says NOTHING about the unlisted passes -- their shaders are not validated.
- **"Is the frame clean?"** -- the gate is `--scene-stress-gbv` (guarded, every pass, level churn;
  it ignores `--gbv-passes`; a Debug build boots the editor's last opened level first, `--stress-level=<level.json>` names
  it and adds it to the churn), or a plain `--gbv` without `--gbv-passes` for one level with your
  `--set`s. Minutes; run it before trusting a pass/barrier/resource change.
- **Under GBV a resource's creation and a submission exclude each other** (2026-11-06; `gpu::SubmissionScope` round every RHI op, `CreationScope` in the `gpu::Create*` wrappers; `--set=frame.gbvCreationExcludesSubmission:0` = the A/B, which loses the device). GBV's layer runs an ExecuteCommandLists of its own from inside `CreateCommittedResource`, on the creating thread (`gpu::CreateCommittedTexture` -> D3D12SDKLayers -> D3D12Core's execute -> nvwgf2umx); with ours in the same driver function on the RHI thread at that moment, NVIDIA's driver (32.0.16.2101) reports an error and the runtime calls `D3DKMTMarkDeviceAsError` (0x80000006, the driver's). The reason reads `0x887A0005`, no breadcrumbs, no TDR in the system log: the GPU never saw anything wrong. Without the RHI thread the two never met. It takes both: a long Execute (a patched pipeline's first frames: any level with a spot in its first frames under `--gbv-passes=SpotLights,Skybox`) and a creation that makes the layer submit (the texture streaming's first new textures, frame 7, 3 ms into it; `streaming.enabled:0`, a 1 MB ring that only lets small ones through, or a spot added 30 s in, when the streaming has finished, all lived and each read as a cause for a while, as the rays did). local_shadow_test: without the lock 11 of 12 runs lost the device at frame 7, with it 0 of 3, id=935 caught, nothing alive at exit. wind_test with two spots and two points, and with the group "Lamps" (`VsmPageRequest,VsmPageRender,PointLights,SpotLights,Skybox`): alive and clean; `--scene-stress-gbv=20 --stress-level=data/levels/local_shadow_test.json --barrier-cmp --canonical-check`: CLEAN. The same frames still go to the queues in the same order, and the recording still overlaps them: what GBV sees of the RHI thread's lag stays.
- **`id=939` / `id=940` in the all-list gate with the RHI thread: twice on 11-06, never since, and the constants checked intact.** On the night of 2026-11-06 (843e4d23's code) `--scene-stress-gbv` on local_shadow_test reported them in 2 of 2 runs, once, in the first frame's messages: `rt_resolve_cs.hlsl:25` (list Compose, heap index 117, a UAV or a BUFFER SRV where a Texture2D is read) and `rt_reflect_common.hlsli:430` (GlassReflections, index 1, a BUFFER) -- both inside slot 0's frame descriptor ring (0..4095), where the constants held 12302 and 12311; the verdict said CLEAN. On 2026-11-07, the same code: 0 of 6 full gates (`=2` five times -- bare, with the ring check alone, with the GPU copies alone, twice with both --, `=20 --barrier-cmp --canonical-check` once with both). What differed from that night was not found: the DXIL and PSO caches were warm then too (the second run wrote no PSO), UE had closed at 22:17, the settings files had not moved. `--upload-ring-check` (next item) in five of those gates, 1 000-4 000 checks a plain run: no byte of any frame's upload ring moved between the end of its recording and the GPU finishing the slot, nothing was allocated or staged into a sealed slot, and the three RT reflection dispatches' constants as the GPU itself copied them right after each dispatch equal what the CPU wrote (288 of 288 in the gates). If it comes back: run that gate with `--upload-ring-check=gpu` (the copies alone, the lightest on the timing). A `rt constants: ... the GPU read other bytes` line beside it = the constants moved; none = the layer's report is off (its messages carry garbage in `Asm Operand Index`, the self-test's id=935 included). A run takes 11-12 minutes, 6.5 of them to the first frame. 2026-11-09, the same kind: the FIRST frame of a narrowed run (`Transparent,Transparent.Water,Skybox`, the editor, the yacht's spray emitters made in that frame's Sync) once reported id=1387 -- a particle draw (`Translucent.Simple`) whose t0, the emitter's own CPU heap, held `SkyAtmosphere.Transmittance`, the sky LUT table's first entry, rebuilt in that frame -- and id=1358 (`Ocean.ShoreDepth` in COPY_SOURCE in the ocean's VS); 1 of 3 runs on that build, nothing in the other two (one of them `ObjectCompute,Transparent,Transparent.Water,OceanWake,Skybox`). If it comes back, `--upload-ring-check` names a descriptor table staged after its slot was sealed.
- **`--upload-ring-check[=gpu|selftest]`** (2026-11-07; `FrameResource::SealUpload` / `VerifyUpload` / `NoteFilled`, `DescriptorAllocator::SealForCheck`, `SceneRenderer_Reflections.cpp ProbeRtConstantsOnGpu`): a frame's upload ring (`AllocDynamic`: every per-frame constant buffer) is sealed when its recording ends and compared before the queue takes it, after ExecuteCommandLists and once the GPU has finished the slot; a block filled through `UploadFrameCB` / `RecordComputeDispatch` / the RT reflections is compared from its fill on; an allocation or a descriptor table taken from a sealed slot is reported with its call stack (`tools/symbolize.py`); the RT reflection dispatches' constants are copied by the GPU right after each dispatch and compared when the slot comes round. Each report names the allocation the bytes lie in and the code that made it (and the slot's allocation there a frame before), with the old and new words. At exit: `upload ring check: N checks, M bytes moved, K allocations and T descriptor tables after a seal` and `rt constants as the GPU read them: N matched, M differed` -- Error when anything moved. `=gpu` = the copies alone (the ring check takes a call stack per allocation, Debug milliseconds of recording a frame, which moves a race's timing); `=selftest` breaks four things on frame 3 -- a filled block, a sealed canary, a descriptor taken after the seal, the resolve's unread pad behind its GPU copy -- and each must be reported (proven). Clean: local_shadow_test and wind_test in Debug, wind_test in Release (4 863 copies), the gates above.
- **A device removal says when and who** (`rendering/core/DeviceRemovalWatch`): a fence reaches UINT64_MAX at the removal, a thread that sleeps on it logs `device removal, the moment: reason ...` and every thread's stack by module, `python tools/symbolize.py x64/Debug/test_cube.exe <log or "module+0xoffset ...">` reads them (the engine by its PDB, a system DLL by its exports, no symbol server). Under GBV and `--dr-check` the stacks are taken within microseconds and `D3DKMTMarkDeviceAsError` is hooked in win32u: `device marked as error, reason 0x..., by: <stack>`, the thread and the call that ended the device; other runs get the moment and stacks tens of milliseconds late. Every other report is the first call that happened to notice (`[texcreate] ... FAILED`, `RT off: ... Map failed`), and what follows it (`id=907`: Present failed, so the back buffer did not advance; `std::terminate`) is the wreck, not the cause. DRED is always on in Debug, so `--dred` changes nothing there. Reason `0x887A0005` with no TDR event = look for that line first.
- **Never gate on `--gbv-mode=state`**: it is falsely clean on uninitialized root arguments (the bug
  class that stalled the queue). `--gbv-rest=none` is the fastest narrowed smoke; the runtime then
  calls its state tracking "possibly imprecise" (id=1044) from the first unpatched list on.
- **Prove a mode catches before trusting its silence**: `--gbv-selftest=1@<Pass>` injects an
  uninitialized-root-argument draw into that pass. Listed -> `ERROR id=935`. The line
  `gbv self-test: injected ... into command list '<Pass>'` must be in the log; without it nothing was
  injected and the silence proves nothing. Aim it at a list whose shader READS the tables it unbinds:
  `Skybox` is proven (id=935); `TerrainDepth` (gbuffer_csm) takes the injection and stays SILENT (2026-10-24),
  and a compute pass (GiUpdate, PointLights, ...) cannot take one at all -- list Skybox beside it as the run's
  catcher.

Reading it: errors land in the session log as `[ERROR][render.validation] ERROR id=...` (and in
`logs/gbv.log`); `[render.rhi] [gbv] requested patch mode = ... device default = ...` and the
`--gbv-passes:` line say what was actually patched. **A clean run has NO `render.validation` warnings**
(2026-10-20, owner: "I need a clean GBV"): under GBV the G-buffer draws its groups one ExecuteIndirect each
(`GraphicsDevice::GbvEnabled`, ShadowGpuData -- the merged bindless commands change VB/IB/root bindings per draw,
which GBV cannot track: that was the old `id=1044` on `GBuffer.Indirect`), and the exit report collapses to one Info
line `live objects at exit: none` when only the device, held by the report's own interfaces, is alive. So any
`id=1044` or `Live ...` WARNING is now a finding. A Debug run whose log just STOPS with an odd exit code is the debug layer's
break-on-error with no debugger: rerun the same line with `--gbv --gbv-mode=none` and the message
lands in the log.

Timing GBV: consecutive runs warm a patched-shader cache, so alternate A and B and compare second
runs. Look at the `--shot` before trusting a time -- a frame missing a mesh measures a different
frame (a worktree without `import_staging/sandy_gravel_1k/textures/*.dds` skips the island's draw).
Run it in a normal visible window: a minimized one has a degenerate backbuffer, so neither its time
nor its shot is the frame you think. Numbers and the trade-offs: `docs/gbv_startup_profile.md`.

**The PSO cache is one folder per device flavour and driver**: `shader_cache/pso` (Release, Release_Editor),
`pso_debug` (Debug: the debug layer), `pso_gbv` (Debug + GBV), each holding `<vendor>-<device>-<driver version>/`
(`Material.cpp PipelineCacheDirectory`, logged once as `PSO cache: <folder> (...)`). A driver refuses a blob stored
behind another layer or by another driver (`0x887E0002`, the debug layer's `id=886` "a cached PSO intended for a
different driver version"), so one shared folder ping-ponged: a Debug wind_test run after Release_Editor refused 69
of 158 blobs and overwrote them for the next Release run to refuse (2026-10-27). A driver update now starts an empty
folder and deletes the others; a refused blob is still compiled again and replaced (`PSO cache: the driver refused a
stored blob`, once a process). Measured after the split: Release / Release_Editor (Streamline on or off) / Debug /
Debug + GBV in any order -- all hits, 0 rejects. Neither `--dred` nor the GBV device's patch mode changes a blob.

## Memory Profiling — How to Run It

Plan and numbers: `docs/memory_profiling_plan.md` (UE's LLM + Memory Insights + `rhi.DumpResourceMemory`).
Pick the tool by the question:

- **"Who holds the memory?"** -- always on: the `mem:` line every 5 s (private, vram, the GPU census by
  pool, allocations per frame) and `--memreport[=<s>]` (one dump: the census as a tag tree, every GPU
  object by size). CPU owners need `--memtrack` (the pointer map: `mem:` gains `cpu X MB untagged Y`,
  ~+0.07 ms a frame). Live: MCP `get_memory` (snapshot / diff), Developer Controls > Memory
  (`--mem-window`). A new GPU owner gets a tag in `core/memory/MemTags.h` and creates through
  `gpu::Create*` (`python tools/check_gpu_alloc.py` = 0); a CPU owner wraps its work in `MEM_SCOPE`.
- **"Which allocation, from where?"** -- the trace:

  ```bash
  test_cube.exe --level=data/levels/wind_test.json --memtrace --profdump=p.txt --shot-delay=20
  ```

  `--memtrace[=all|cpu|gpu][,stacks=big|all|off|1/N][,start=<frame>]` writes
  `traces/memtrace_*.mtrace` (last 10 kept). Then `python tools/memtrace.py verify` (the replay must
  equal what the process counted alive -- PASS byte for byte, or the trace is not to be trusted),
  `summary`, `leaks A B C`, `growth A B`, `churn A B`, `tags A B`, `timeline` (Perfetto JSON);
  markers are frames (`600`) or seconds (`12.5s`). `stacks=big` costs nothing measurable,
  `stacks=all` ~0.5 ms a frame -- cap the frame rate (`--set=frame.maxFps:60`) for a long one.
  To look rather than grep: `python tools/memtrace.py view [file]` opens `tools/memtrace_viewer.html` on the
  trace (timeline by tag, markers A-D by click/drag, the same rules, stack trees, tag table, verify badge;
  served on 127.0.0.1 only) -- or drop a `.mtrace` on that page. Its core gives the CLI's numbers exactly.
  `memtrace.bat [app switches]` in the repo root does the three in one go: the run (wind_test unless a
  `--level=` is given, Release; `set MEMTRACE_CFG=Debug` for exact names, `MEMTRACE_OPTS` = the switch's value),
  `verify`, `view`.
  **Who freed it**: since trace format 2 every free carries the freeing stack (same `stacks=` policy) --
  `query AfB A B --by fstack|ftree|finverted`, the viewer's "free callstack" groupings and its Frees /
  Decline presets; a row's details show both "Allocated at" and "Freed at". Frees are negative sizes.
- **"Is there a leak?"** -- the gates:

  ```bash
  test_cube.exe --level=data/levels/occlusion_test.json --cam-pos=0,8,0 --cam-orbit=35,14 --mem-readout=30
  ```

  15 s warm-up, 30 s at 4 Hz, a least-squares slope per series (private, vram.local, every tag);
  `mem readout: PASS|FAIL <series> <MB/s>` in the session log, exit code = failing series. FAIL =
  above 1 MB/s AND climbing in every third of the window (a capacity step is not a leak).
  `--scene-stress=150` adds `mem tags: STABLE|DRIFT|NOT JUDGED` to `logs/scene_stress.log` (a tag
  that climbs from visit to visit of the same level; first visits are cold and resolution-sized
  tags are left out); fewer than 150 iterations is `NOT JUDGED`, never a pass.
- **Prove a gate catches before trusting its silence**: `--mem-selftest=leak@<tag>[,kb=<n>][,cpu|gpu|both]`
  loses 64 KB a frame under that tag. The line `mem self-test: injected ...` must be in the log;
  measured: the readout FAILs it at ~28 MB/s, the stress reports DRIFT on every level.

Reading numbers: `private` counts the VRAM twice on this machine (write-combined pages mirror it,
M1), so a GPU leak FAILs `private` and `vram.local` together. Release stack names of std templates
are folded by ICF (a vector of pointers shows up as nlohmann's) -- `memtrace.py` groups from the first
non-`std::` frame; for exact names use Debug.

**A texture its level left behind wants only its tail** (2026-11-04; `--scene-stress=150
--stress-level=data/levels/wind_test.json` said `mem tags: DRIFT -- tex.stream +1070 MB`, and it was not the budget: the
pool is 16.9 GB). `MaterialDataManager::cache_` outlives a level on purpose, so every texture stays registered on every
level, and a texture without a box read as an unknown ref: every mip. Leaving wind_test read all of it to full size in
the background -- 347 textures, 1373 MB, the plateau by iteration ~165, where what the four levels' cameras ask for comes
to 55 MB. The port had dropped UE's third term (`AsyncTextureStreaming.cpp:291-293`: unknown only if used since the last
reference was removed). Now a texture that lost its last box (`Persist::lastBoxed`) wants its tail and `TryKeepMips` holds
what it has, with no IO: `tex.stream` at the end 1436 -> 111 MB, read 1372 -> 53 MB, `STABLE` (+6.6 / +7.1 MB). One that
never had a box stays unknown (a LUT, the ocean, the emissive and transmission maps `BoundsBuilder` does not look at).
The readout: `[texstream] ... left behind N (M MB)`, `left-behind` in the `streaming.dumpRows` table (under `--set` it
fires again after every level load: a table per visit), Developer Controls > Streaming. What a level left behind keeps its
mips until the pool presses (`streaming.poolSizeMB:40`: used 28-35 MB, left behind 0-15 MB), so going back reads nothing:
the bathroom at arm's reach 472 MB, 12 s on demo no reads, back at the same 474 MB and the same picture (1.2 / 3.4 % of
pixels over 8 levels on the bathroom / the grove, the same arm twice 2.9 / 2.8 %). Proof of the holder and the price of
giving the memory back: releasing at a switch the materials only the cache holds (a probe, not kept) takes the table to
each level's own (338 / 82 / 18 / 9 textures) and `tex.stream` to 51 MB, and wind_test's switch 265-427 -> 574-659 ms
(texture loads 752 -> 6907). READING THE VERDICT: the rule wants growth in 2/3 of the steps, so a fill that ends inside
the run reads by the run's length -- that same climb said `DRIFT` at 150 iterations and `STABLE ... largest climb +1182 MB`
at 300. Read the largest climb.

**A level load shares its textures on its one upload list** (2026-11-04). `Texture2D::CreateFromFile` handed out no cache
hit in the frame a texture was uploaded -- the transition lives on the uploader's list, and another list may run first (the
editor's thumbnail jobs) -- and a level load is one list in one frame, so the cache shared nothing where it mattered:
`Wall_Trim_*.dds` 20 copies, 347 textures over 171 files, 875 of their 1373 MB at full size. The same list now shares at
once; another list in that frame still loads its own copy. wind_test in the editor: 339 -> 163 textures; the bathroom at
arm's reach `tex.stream` 472 -> 166 MB, read 459 -> 161 MB, GPU objects 273 -> 106. Stress 150 (alternated twice): the
`streaming.dumpRows` table 347 -> 171 rows, the 171 files; `[texcache]` 752 loads / 78 shared -> 576 / 254; the boot's texture
loads 343 -> 167, 172-191 -> 96-100 ms; the boot 4056-4315 -> 3876 ms; 50 switches 6242-6324 -> 6089-6154 ms. The same picture:
the bathroom 1.25-2.38 % of pixels over 8 levels against the same arm across processes 1.21 % (within a process up to 2.31 %,
its candles), the grove 0.10-0.40 % against 0.50 %.

## Reproducing a Camera View From a Screenshot

The on-screen HUD prints the camera POSITION and its ORIENTATION QUATERNION:

```
Cam: -4.11 0.87 0.83, rot: 0.0273 0.9078 -0.0599 0.4143, speed: 1.00, DLSS: 2, SSR: 1, Shadows: VSM
```

**Use those two values instead of guessing camera angles.** When a user reports a visual bug with a
screenshot, read `Cam:` and `rot:` straight off the image and reproduce the exact view headlessly:

```bash
test_cube.exe --level=data/levels/demo.json --shot=out.png --shot-delay=5 --cam-pos=-4.11,0.87,0.83 --cam-rot=0.0273,0.9078,-0.0599,0.4143
```

`--cam-pos` / `--cam-rot` are applied AFTER the level's own `freeCameraStart`, so any level works.
The quaternion is `x,y,z,w` and carries the FULL orientation including roll, so the pair reproduces
any pose the camera can hold. Verified: the HUD quaternion round-trips to 4 decimals, and the
rendered frame differs from the level-authored camera by 0.076 % of pixels.

A level's `freeCameraStart.rotationDeg` is `(pitch, yaw, roll)` in degrees.

Guessing a camera by hand does not work: a wrong angle shows an empty patch of scene and you conclude
the bug is not reproducible when it simply is not in frame.

**True 4K.** The default window is 2560x1440 (clamped to the work area); `--window=3840x2160` makes a
borderless window of exactly that client size and initialises the renderer at it. Measured
2026-09-24 on wind_test (DLSS Balanced): 4.3-5.0 ms GPU at 4K vs 3.3 at 1440p, GPU-bound. At 4K the
reflection trace and the bloom FFT grid were the costs that grew fastest; the bloom steps its percent
down automatically (BloomRenderer, logs `bloom: convPercent 25.0 -> 19.0`). The reflection
resolutions were then fractions of the DISPLAY; since 2026-10-10 they are fractions of the RENDER
resolution (DLSS's input), so a cheaper DLSS mode shrinks the traces with the rest of the frame.

### Mouse smoothing for a recorded turn

The free camera's look input is raw mouse counts summed per frame, and at a few hundred fps against the mouse's own
polling a steady hand arrives as 3, 5, 2, 6 counts in a row -- a turn that stutters on video (owner, 2026-10-27).
Developer Controls > Frame "Mouse smoothing (ms)" (graphics_settings.json `frame.mouseSmoothingMs`, `--set` / MCP
`camera.mouseSmoothing:<ms>`; 0 = off, the default) eases it in exponentially (`Camera::UpdateFromInput`: each frame
takes `1 - exp(-dt / tau)` of the input still owed, the same at any frame rate; the turn finishes after the button is
let go). 40-80 ms reads smooth for a recording. Any code that sets the orientation (`SetYawPitch*`, a cut) drops what was
still owed, so `set_camera` and `--cam-rot` land exactly.

### Make the frame deterministic before diffing

Add `--wind-freeze[=<seconds>]`. It pins the shared wind+ocean clock, so two runs are comparable
pixel-for-pixel (water, foliage sway and gusts all stop moving) without altering any authored
parameter. Without it, an animated scene differs ~2.3 % between runs and swamps a small regression;
with it the floor is ~0.05-0.4 % (the residual is DLSS jitter phase, which follows the frame index).
Use two different values (e.g. `=0` and `=1.5`) to prove something ANIMATES — that is the test a
frozen/over-cached shadow fails, and one that authoring `swayFrequency: 0` cannot perform.

## The Local Intent Model (llama-server) — Starting and Stopping It

The editor's Command Bar talks to a local `llama-server` (llama.cpp) holding a ~38 GB GGUF.
Neither is in the repository; `python tools/fetch_intent_model.py` puts both under
`D:/llm_models`, and `editor_state.json` -> `levelEditor.intentModel` points at them.

**The rule: nothing runs that nobody is minding.** A 38 GB inference server left over is a
third of this machine's RAM held by something the user did not ask to keep. There are two
arrangements, and `levelEditor.intentModel.keepServerAfterExit` picks between them:

- **`false` — the server dies with the editor, by construction.** It is launched inside a
  **Windows job object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`**, so it goes whether the
  editor exits cleanly, crashes, is killed from Task Manager, or is stopped in a debugger.
  No shutdown path is trusted, because a crash runs none of them.
- **`true` (the default) — the server outlives the editor, and `model_reaper.exe` minds
  it.** The job object is given up, so the guarantee moves to that watchdog: it polls the
  server's `/metrics` counters and terminates it after `keepAliveSeconds` (default 300) with
  no requests **from anybody**, which is strictly better than the editor's own timer — that
  one is blind to requests made from the server's own chat page. If the watchdog cannot be
  started the editor retires the server at once rather than leave it unwatched. The trade is
  real and worth knowing: lose power, or kill the watchdog from Task Manager, and the server
  is left running. What it buys is the next editor launch inside the window starting
  instantly instead of paying ~22 s to put the weights back on the card.

`model_reaper.exe` is its own binary (`tools/model_reaper.vcxproj`, built into its own folder
`x64/ModelReaper/`) and must be, twice over. As a mode of `test_cube.exe` a live watchdog
held that file open and the next `Release_Editor` link failed with LNK1104; launching a copy
of the exe from `%TEMP%` to dodge the lock produced a process without the DLLs the engine
links against, which died before its first log line and left the server unwatched — the
exact failure it exists to prevent. It has one configuration and one output; both the Debug
and the Release_Editor editor start it from `../ModelReaper/` relative to their own exe, and
`test_cube.sln` builds it under both of those configurations. **Build it after changing
anything under `sources/editor/intent/`**, or the editor will refuse to keep a server alive
and say so, naming the path it looked at. While a watchdog is running its exe is locked, so
close it from its tray icon before rebuilding it.

While the editor is running its own `idleTimeoutSeconds` (default 600) also applies, but
only in the `false` arrangement; with a watchdog the editor stands down so two things are
never retiring the same server.

**When starting one by hand** — a test script, a probe, a conversation with the model —
the same rule applies and nothing enforces it for you:

- Start it only for as long as the check needs, and **kill it in the same turn**:
  `Get-Process llama-server | Stop-Process -Force`, then VERIFY it is gone
  (`Get-Process llama-server` must come back empty). A `Stop-Process` can report success
  while the process is still tearing down 38 GB of mappings — check, do not assume.
- Prefer `-ngl 0` (CPU). The renderer owns the GPU, and a 35B-A3B answers in seconds on CPU.
- Always pass `--load-mode mmap` and never `--mlock`: mmap makes the weights file-backed
  page cache the OS can reclaim, mlock pins all 38 GB for real.
- Reuse a server that is already healthy on the endpoint instead of starting a second one.
  Two of these do not fit in RAM together.

Learned the hard way on 2026-09-15: a probe server was left running after the checks were
done, holding 35.4 GB of working set until it was noticed.

**Talking to the model directly.** `intent_regression --dump <level> <gbnf> <prompt>` writes
the exact grammar and system prompt the editor would send, so a conversation exercises the
contract that ships rather than a retyped approximation. Two things that cost an iteration
each and are not obvious:

- **Every GBNF rule must be on ONE line.** llama.cpp ends a rule at the newline, so a
  continuation line starting with `|` is a parse error, and the server rejects the whole
  grammar with one unhelpful sentence ("failed to parse grammar") naming neither rule nor line.
- **Qwen3.x thinks before answering**, and a grammar demanding JSON from the first token
  does not let it — the two fight over every token and the request fails or stalls. Pre-fill
  an empty `<think>\n\n</think>\n\n` after the assistant tag; it is part of the prompt, so the
  grammar never sees it.

### Claude Code drives the editor (MCP)

**ALWAYS CHECK THE MCP FIRST, AND EXTEND IT WHEN IT IS NOT ENOUGH** (owner, 2026-10-15). Anything
that looks at or tunes the RUNNING renderer -- an A/B of a look, a knob sweep, "does this control
move the picture", a readout -- goes through one open editor and these tools, not through dozens of
headless launches: a launch is ~15 s of boot and warm-up for ONE picture, and fifty of them is a
quarter of an hour the owner sits waiting for. If the MCP cannot do what the check needs, ADD it
(`EditorMcpServer.cpp` tool + its case in `tools/intent_regression.cpp`
`TestMcpDrivesTheSameRoad`, built in Debug for the gate and in Release_Editor to use it) rather than
working around it. Headless `--shot` / `--sweep` stay for gates and for builds without the editor.

The running Level Editor is an MCP server at `http://127.0.0.1:8128/mcp` (streamable HTTP,
localhost only; `sources/editor/intent/EditorMcpServer.*`), registered for Claude Code by
`.mcp.json` in the repo root. Tools: `editor_guide` (the local model's own system prompt, rebuilt
from the open level: every action, query and name — read it first), `run_action` /
`preview_action` (the same `{action, target, params}` JSON the local model emits, through the same
`ParseAnswer` → `BuildIntentPreview` → `ExecuteIntent`), `query`, `get_camera`, `set_camera`,
`screenshot` (the editor window as a PNG, taken at the `--shot` safe point), `undo`,
`thumbnails {assets: [...]}` (the Content Browser's thumbnails baked through its own cache even while the
browser is a hidden tab -- it bakes only what it draws; poll until each answers `ready`, `png` is the cached file).

**The Mesh Editor's preview keeps one picture per frame slot, and they must agree** (2026-11-11, owner: "the real sky
flickers in the Mesh Editor", after ~10 s with the window open). Under the level's sky three pictures took turns frame by
frame (a burst of the owner's editor: three distinct images, period 3): each slot had been drawn at another cloud capture,
and the preview compared the sky cube's pointer alone -- the content moves behind it as the clouds drift.
`SkyAtmosphere::EnvironmentRevision` counted only inside a commented-out log line (0 for ever); it bumps with every rebuild
now and reaches the preview through `Skybox::EnvironmentRevision` / `PhysicalLighting::environmentRevision`, taken
kFrameCount + 1 updates late (the render thread bumps it when it RECORDS the rebuild), and after any change every slot is
re-rendered once more (`MeshEditorPreviewScene::Update`). Four bursts of 240 frames: the preview's clouds follow every
rebuild. Then the owner: "it flickers dark, rare frames" -- and only at full frame rate (a burst's PNGs take it to ~10 fps
and it was gone): `SkyAtmosphere::BuildEnvironment` CLEARED the skybox's environment at its top every frame and set it
again a few lines on, and the preview reads the skybox on the game thread beside it -- a read in that window took the sky
for the file sky (no IBL, the file cube's calibration) and drew a dark slot, which the re-render after a change put on
screen for a frame. The environment is set once a frame now, to what it is. A race of the game half against the render
thread is invisible to a burst: reason it out from who writes what when.
**The Mesh Editor's preview has its own camera** (2026-11-05, owner: "make its own exposure, and so it can be set"): under Level lighting it is lit by the level's sun and sky in lux, and it took the VIEWPORT's metered EV, so from the bathroom (EV 7.44) the same door came out white and from the beach (13.36) right. Now `previewLight.autoExposure` (default on, `editor_state.json`, Mesh Editor > Preview Lighting): the EV a meter reads off a grey card under the preview's own light (the sun by the preview's direction + the sky's horizontal illuminance, `log2(0.18 E / pi) + 3`) less `exposureCompensationEv`; off = `manualEv100`. Only the colour pipeline stays the level's.

**When a thumbnail bakes** (`AssetThumbnailCache`, PNGs in `editor_cache/thumbnails`): when its signature changes --
`kThumbnailSchemaVersion`, the asset's path and mtime, its materials' and textures' files, and the environment (the
level skybox's path, mtime, intensity trim and cube size). The preview SHADER is not in it: a change to
`editor_preview.hlsl` reaches no cached thumbnail until the schema version is bumped. Thumbnails take the preview's
old uncalibrated path (key light 1, fill 0.3 of it, the level's HDRI cube for reflections); since 2026-10-28 (owner:
"the thumbnails' specular is fucked" -- wallpaper like wet lacquer, cornices like chrome) the cube is brought to the
fill's units by its log-average over 26 directions, and the cube's own sun is capped out of the reflection (16x the
sky's level; it put white pixels on a palm's glossy fronds).

The look-dev loop (2026-10-15): `set_settings {set: "bloom.intensity:0.03;tod.time:17.8"}` changes
the running scene through `--set`'s own name table (App.cpp `ApplySweepValue`) -- TRANSIENT: no undo
entry, nothing to save, an Inspector edit of the same object re-applies the document over it -- and
re-meters the exposure; `screenshot {clean: true, path: "...png", settleFrames: 60}` writes the
frame WITHOUT the editor's panels (scene + HUD, comparable with a headless `--shot`) at full size and
sends no picture back unless `image`; `reset_settings` puts every environment object back to the open
document and `fog.debugView` / `fog.skyAlongView` back to 0 (a debug view used to survive it: 2026-10-31 a whole
sweep measured `fog.debugView:4`) (project-wide switches -- `cloudTrace.*`, `cloudShadow.*`, `frame.*`, `skyVis.*`, `gi.*`,
`render.*` -- are not in the document and stay as last set: a bisect that turns `skyVis.enabled:0` off for one
arm runs every later arm without it unless each arm names it); `get_readouts` returns the metered/adapted EV100, the pre-exposure,
the sun probe (disc colour, visible share) and the frame rate; `set_camera {lookAt: "sun",
offsetYawDeg, offsetPitchDeg}` faces the sky's sun wherever the time of day has it (`position`
optional; `lookAt: "moon"` the moon; positive offsetPitchDeg tilts DOWN, so a view 4 degrees over the horizon
toward a sun at elevation e is `offsetPitchDeg: e - 4`). An exposure curve is shaped live with
`exposure.curvePoints` / `exposure.curveEv<i>` / `exposure.curveComp<i>` (`exposure.curve:0|1|2|3|4` = none/Eye/UDS/UDS
Steady Night/UE -- UE's wind_test has NO curve, one constant Exposure Compensation +0.5 on its PPV, the Inspector's "UE"
button; the curve's slope is the adaptation's strength -- the camera follows 1 - slope of the scene's change).
The Level Editor closed (F2) still serves every call -- MEASURE COSTS WITH IT CLOSED (its panels are a load of
their own) and on a real view, not wherever the camera happened to be: `get_profile {filter, reset}` returns the
profiler's rows (GPU/CPU per pass: EMA, max since `reset`, runs last frame), and `shadow.mode:0|1|2`
(Legacy/VSM/SDSM) switches the sun's shadows live. Kill the editor afterwards (`Stop-Process`), as for any live MCP check
-- and BEFORE editing a shader's root signature, registers or cbuffer layout: the running exe hot-reloads the saved
`.hlsl` at once and binds it the old way (2026-10-30: a 14th SRV under a 13-entry table -> device removed, the owner's
monitors blinked). A body-only shader edit hot-reloads safely.

MEASURE, DON'T READ A SCREENSHOT: the tone curve clips a glint to white, so "how bright is it" is
unanswerable from a PNG. `set_settings {set: "debug.probeU:0.537;debug.probeV:0.469;debug.probeRadius:40"}`
(UV of the frame, radius in px; `debug.probeSource:1` = the scene before DLSS, `2` = the bloom target
the tone curve adds) and `get_readouts` then
carries `pixelProbe`: the brightest PRE-EXPOSED pixel of the window (rgb), the same after the bloom
input's local exposure, a 4x4 box at it and the window's mean (bloom_conv_cs.hlsl stage 11, read back
with the exposure record). That is how the 65504-nit clip of the light target was found (2026-10-15).
`run_action setSurface {metal, roughness}` makes an object chrome/lacquer/matte live (undo restores).
**`set_settings` is transient but STICKY**: a knob stays as last set until `reset_settings` (or a relaunch), so a
script that toggles one and dies, or a later script that never names it, measures a different scene. Start every
series with `reset_settings`. It cost a wrong conclusion on 2026-10-24: a leftover `sky.environmentLighting:0` lit
the night with the daytime HDRI's 12,000 lux fill (x skyFillIntensity 2) and read as "the night sky is 1.8 lux".
`get_readouts` carries `skyFill` (`environment` true = the procedural sky's capture, false = the HDRI; `exposure`,
`hdriLux`, `irradianceScale`) to tell which fill is lighting the frame.
`set_settings {set: "wind.freeze:1"}` holds the wind + ocean pose (`--wind-freeze` for a running
editor; `0` releases it, `reset_settings` does not): without it two looks of the sea are two different
seas. It holds the moment it is set; `wind.freezeAt:<seconds>` (`--wind-freeze=<s>`) holds THAT clock, so arms set
up minutes apart see the same pose (the surf simulation still differs: it carries state). `set_settings {set: "camera.fly:-3.8"}` flies the camera along its own view at that many m/s (negative =
backwards, `0` stops it, `reset_settings` does not): a temporal history's artefact that only shows in MOTION is
looked at in the running editor, `screenshot` with `settleFrames` taking it mid-flight (`--set=camera.fly:` headless;
`--cam-fly` drifts along the WORLD's x/z instead). Stop it in a `finally` -- a script that dies leaves it flying.
`camera.spin:<deg/s>` turns it the same way (positive = right; `--cam-fly-yaw` headless). A motion artefact is MEASURED
against the same view still: every `screenshot` reply names its frame's camera ("Camera of this frame: {position,
yawDeg, pitchDeg}", full precision), so a frame taken on the move is retaken with `set_camera` at that pose once the
histories converge (400 frames) and the two are diffed; cap the frame rate (`frame.maxFps:120`) so the arms of an A/B
land on the same yaws. `dlss.mode:<sl::DLSSMode>` (0 off, 2 Balanced, 6 DLAA) switches the upscaler in the running
process -- DLSS's own motion reconstruction swamps a small temporal artefact otherwise. scratchpad-style
`motion_err.py` (2026-10-26): moving frame vs still retake, mean |d| and the high-pass grain, the arm
`skyVis.enabled:0` (or whatever feature) the control. The probe's window mean of a bloom target over water is dominated by any lens ghost lying on it
-- set `bloom.ghostIntensity:0` for that read.
**A HISTORY THAT ADAPTS AFTER A MOVE IS WATCHED FRAME BY FRAME, AND NOT AFTER A CUT** (2026-10-26, owner: "you
don't know how to take pictures -- jump the camera and take frequent shots while the cascades adapt"). `set_camera`
is a camera CUT by default (`Camera::ResetHistory`: DLSS, every temporal filter and the SDSM partition cache restart),
so a cut can never show what a person's quick dash shows -- one trial per frame offset found nothing. `set_camera
{..., cut: false}` is a very fast move the histories follow, and `screenshot {path, burst: N}` (1-240) writes N
CONSECUTIVE frames as `<stem>_000.png ...` in one call (settleFrames = the first one's delay; no picture sent back).
Settle at A, `cut: false` to B, burst 120, diff every frame against B settled: that is how the SDSM flash below was
caught at frames 65-75 -- long after any single-shot trial looked. scratchpad-style `sdsm_burst.py`.
A FLICKER IS READ FRAME BY FRAME: two `get_readouts` calls land ~10 frames apart and see a drift, never
a flicker. `get_readouts {history: N}` (N <= 512) adds the newest N frames, one row EACH (frame,
adapted/metered EV100, pre-exposure, the probe's window mean rgb and max luminance, the sun probe's luminance and visible
share) --
ExposureMetering's ring, filled as each readback slot retires. That is how the chrome star's 45 %
frame-to-frame flicker and the ghosts' DLSS ringing were found (2026-10-15). `set_settings` with
`resetExposure: false` changes a knob the way the Inspector does, without re-metering.

**A FILM IS RENDERED FRAME BY FRAME, NOT CAPTURED** (2026-11-10, owner: "shoot me a 30 s clip of the yacht tearing round the island, cinematic angles"). No screen recorder is needed and a real-time capture would drop and double frames: `set_settings {set: "frame.recordDt:0.016667"}` (App.cpp `g_recordDt`; 0 = real time) makes the world advance by exactly that on a frame an MCP `screenshot` captures and STAND on every other frame (`editormcp::ScreenshotCapturesThisFrame`: the request's settle count at 1 -- so ask with `settleFrames` 2 or more, or the frame taken is one that did not advance). Between two frames a script reads the world as it stands (`get_readouts` helm), steers (`player.testRudder`), puts the camera anywhere -- `player.testFreeCamera:1` makes the boat's own camera stand down at the helm, and `set_camera {position, lookAt: [x, y, z], cut: false}` is then the frame's camera (`cut: true` on a shot's first frame, with `settleFrames` ~45: the histories converge on the standing world before the shot starts) --, takes the frame and encodes it (PyAV is installed: libx264; there is no ffmpeg on the machine). `debug.hud:0` hides the HUD's text (`--no-hud` for a running process), `dlss.mode:6` (DLAA) costs nothing offline, `--window=1920x1080` sets the film's size. scratchpad-style `film.py`: a real-time pre-roll up to speed, an autopilot along a slalom round the island (pure pursuit on a polar road, rudder = 1.6 x the heading error), a cut list of shots as functions (state, u) -> (eye, look at) -- chase, from the sea with the island behind, a drone over the wake, ahead of the bow, a standing camera the boat storms past, a crane up to the island --, 0.25 s of work a frame at 1080p (30 s of film in 7.5 minutes). Frame-to-frame difference of a take: 9.0-10.2 over 60 frames, no doubled frame. Leave it OFF when done (`frame.recordDt:0;player.testFreeCamera:0;debug.hud:1`, in a `finally`): with it on and nobody taking screenshots the world stands still.

- **`save_level` writes the open level exactly as Ctrl+S does** -- only when the person asked
  (owner, 2026-10-23: «сам все сохраняй что надо»). Every other call leaves an unsaved edit that is the
  person's to keep or discard. The per-level editor camera is not saved with it.
- Each edit is one undo entry and shows in the command bar's transcript under **claude**.
- Only while the editor is open with a level; calls wait up to 60 s for a frame, then fail.
- Browser requests are refused (`Origin`/`Host` must be this machine), so a web page cannot
  drive it. A second editor cannot bind the port and says so in its panel -- and a SCRIPT that waits for the port then talks to
whatever holds it (2026-11-09: a check's reset_settings / set_settings / set_camera went to the owner's open editor;
withdrawn there, his frames were not served in 60 s). A script's own editor takes `--mcp-port=<n>` (App.h
g_bootMcpPort; scratchpad `mcp.py` reads MCP_PORT) and checks that port is free BEFORE launching.
- Toggle and status: command bar settings → "Claude Code (MCP)". `mcpEnabled` / `mcpPort` live in
  `editor_state.json` → `levelEditor.intentModel`.
- The gate covers the protocol without a socket (`TestMcpDrivesTheSameRoad`); for a live check,
  launch `--editor --level=...` and POST JSON-RPC to the endpoint. Kill the process afterwards
  rather than closing it, so a check never saves the level by accident.
- **Codex reaches the same door** through `.codex/config.toml` (project-scoped; read because this
  project is trusted in `~/.codex/config.toml`), with `default_tools_approval_mode = "approve"` —
  without it every call needs approval and `codex exec` (approval `never`) fails them all. Codex
  does not list MCP tools up front: they sit in its tool registry (`ALL_TOOLS` via
  `functions.exec`) as `mcp__test_cube_editor__*`. Verified 2026-09-23 with `codex exec` calling
  `get_camera` (the editor logs `MCP: tools/call get_camera`). Its edits also show under **claude**.

The local model (`intentModel.enabled`) is **off by default** since this exists.

### Photographing the editor

`--editor` opens the Level Editor at boot, for the same reason `--log-window` exists: so a
headless `--shot` can capture a panel that otherwise needs a keypress. Which panels appear is
whatever `editor_state.json` -> `levelEditor.panelState.*Visible` last held, so set those first.
Allow a long `--shot-delay` (40 frames is comfortable) -- the asset registry scan and the first
editor frame both happen before the panels look settled.

`--dev-tab=<label>` opens Developer Controls on that tab at boot (`gi`, `lod`, `memory`, ... -- the navigation's
labels, case-insensitive; an unknown one logs a warning naming them), as `--mem-window` does for Memory. Driving
ImGui with posted mouse messages does not work: while the window has focus the backend reads the real cursor
(`GetCursorPos`), so a test checks a persisted control by its load (a `graphics_settings.json` section shows in
the tab) and its save (delete the file: the boot writes it whole from Capture -- back it up and restore it).

`--show-import[=<name>]` (implies `--editor`) opens the Import Assets window with the staged item
`<name>` selected, so its details pane -- size, split, material preview, LOD, chunking -- can be
photographed; `--edit-mesh=<models/x/x.mesh.json>` does the same for the Mesh Editor, and
`--edit-mesh=<models/x/x.mesh.json>#buoyancy` opens it with the Buoyancy section expanded.
`--inspect=<environment type>[#<group>]` (implies `--editor`) selects the level's environment object of that type and
scrolls the Inspector to the group by its JSON key -- `timeOfDay#fog` (the fog curve), `timeOfDay#lamps`,
`postProcess#heightFog`; the log says `--inspect: selected '<name>', group <key>`. Not an environment type: the level
object of that NAME, quoted when it has a space (`--inspect="small_yacht 001#end"`; a script passes the command line as
one string -- a Python list quotes the switch's name with it), any group scrolling to the Inspector's end, where the
type's own rows are (Physics, Buoyancy); the log says `--inspect: selected object '<name>'`.

**Registering a panel is not the same as drawing one.** `EditorController::Draw` ends with a
hardcoded list of `drawPanel("<id>")` calls; a panel missing from it exists, toggles from the
Window menu, persists its visibility -- and never appears. Two panels shipped that way until a
screenshot showed the gap, because no headless gate can see ImGui wiring.

## Buoyancy — Meshes Floating on the Ocean

A level object `"buoyant": true` (Inspector: "Floats on the ocean", MCP: `setBuoyant`) heaves,
pitches and rolls on the ocean; position and yaw stay authored. Where and how it floats is the
ASSET's: mesh.json `"buoyancy": {draft, inertia, damping, pontoons:[[x,y,z,r]]}`, every key optional
-- no pontoons = the automatic layout (`ocean/BuoyancyLayout.cpp`: the hull's waterplane at the draft,
split into bins, one pontoon per bin with radius sqrt(area/pi)). Mesh Editor > Buoyancy edits it.

- The pose is a RENDER OFFSET (`RenderableObject::SetRenderOffset`), never the authored transform.
  Anything that builds a new authored transform from a live one must use `GetAuthoredModelMatrix()`
  (the gizmo and `bury` do) -- `GetModelMatrix()` would write this frame's wave into the level.
- Waves come from `ocean/OceanReadback`: the first 1-2 FFT cascades (the ocean preset's
  `readbackCascades`; **`None` = everything floats on a flat surface**, and both shipped presets say
  None) plus the shore depth map, copied to a readback ring and read the first frame the fence says
  it is done -- never waited on. Its `SampleHeight` is the vertex shader's surface term for term.
- **"Done" is asked by the copy's frame NUMBER** (`Renderer::IsFrameNumberComplete`, FrameScheduler keeps the number
  each slot's fence value was signalled for; 2026-11-09, owner: "the boat in the lagoon rocks like mad after the load
  and calms once I fly to it"). It asked by the frame SLOT, and under the RHI thread a frame's submission -- its
  `SignalFrame` -- runs after the hand-over that polls it, so the slot still held the fence of the frame three before,
  long done: a copy was adopted one frame after it was recorded, before the GPU had run it. The shore map is copied
  once a re-render, so the CPU kept that slot's old bytes (zeros at load) until the camera crossed a snap step: every
  floater in the lagoon rode the open sea's waves. Measured from the owner's 18:00 view (cam 37.93 4.63 71.59, the
  fishing boat 77 m off; `get_readouts` `floaters` / `oceanReadback`, scratchpad-style `floater_far.py`): heave sd
  0.17-0.33 -> 0.03-0.06 m, tilt up to 27-40 -> 5-10 degrees at load, the same after a fly-in and back, the shore read
  under each boat (the lagoon floor's 1.45 m); with `frame.renderThread:0` the same. The surf measurement above was
  taken before this. A shore re-render on a frame that copies nothing (no request) is also owed its copy now (re-armed
  before the request check). Anything else that reads a GPU copy without waiting: by frame number, never by slot --
  `Renderer::WaitForFrameSlot` (the occlusion's `vis.queryLatency:1`) now waits for the last frame's submission first.
- **The surf sim's wave too** (2026-11-09, owner: "the surf sim's wave does not affect the buoyancy"): its height field
  (`OceanSurfSim`, RG16F 512^2, the shore's breakers) is copied with the cascades on the frames its pass handed it over
  (`OceanSurfSim::GetWaveResource(frame)`, `OceanRenderable::GetSurfSimWaveResource`) and added where the vertex shader adds
  it: after the shore damping, at the warped coordinates, x `surfSimDisplacement`, its window's edge faded. A frame whose
  surf pass did not run (a shore map re-rendered) puts the copy off, at most 4 frames running -- adopted without the
  surf it dropped the breakers from under a hull for 1/30 s. +1 MB a ring slot (3 x 3.0 MB now), `Pass_OceanReadback`
  +0.025-0.03 ms a copy (0.12-0.15 with it, 0.105-0.12 without). Measured in Play (scratchpad-style `surf_buoyancy.py`,
  wind_test's yacht by the shore, 30 s an arm, alternated twice): `--set ocean.surfDisplacement:1` heave sd 0.144 / 0.058 m,
  pitch 1.98 / 0.81, roll 4.99 / 1.21 degrees; `:0` (the sim runs, the water does not rise by it -- the A/B, transient)
  0.038 / 0.017 m, 0.45 / 0.27, 1.18 / 0.83 (the boat drifting out to deeper water between the pairs).
- Cost, measured (Release, ~340 fps, 5 boats / 30 automatic pontoons): GPU `Pass_OceanReadback` 41 us
  per copy, but copies are capped at 30 a second -> ~3 us per frame averaged (every other frame at
  60 fps); CPU `Scene::TickBuoyancy` median 5 us (heights are re-sampled only when a copy lands). The
  floating objects come from a REGISTRY (`RenderableObject::SetBuoyant`), never a scene walk: a level
  with nothing floating costs 0 us CPU and no copy. The copy was tried on the async
  queue inside Main_ObjectCompute and cost the frame ~108 us: the ocean draw waits on that pass.

## Play Mode -- the World Runs Only There

UE's Play In Editor (2026-11-05, owner: "the physics should not simulate just in the editor"; `docs/play_mode_plan.md`, `sources/app/game`). In a build with the editor (Debug, Release_Editor) the world STANDS, panels open or closed, and nothing but a session changes that: `physics::Settings::simulate` is no knob any more, AppController says it every frame (`physics.simulate` answers `unknown setting`). A build without the editor (Release) always runs, and its camera is free until `--play` or F8: every shot and measurement of a Release build stands on that.

- **No copy of the world.** All a session changes is render offsets (below, "The pose is a render offset") and the camera, which `PlaySession` puts back, so Stop leaves the document and the level as they were. Checked 2026-11-09 (owner: "put the yacht back where it was before it sailed off"; scratchpad-style `yacht_stop.py`): wind_test's yacht after 72 m at the helm, and after drifting without it, is at its authored (52.657, 0, 115.447) after Stop, a frame of it the same as before Play (`physics::World::SetSimulating(false)`: every Dynamic and kinematic body to `entry.rest`, its offset cleared). What a person sees jump is the HEIGHT: a vessel stands in the editor where it is placed, and wind_test's yacht is placed at y 0 where it floats ~0.38 lower (its draft): it hangs over the water before Play and after Stop, and drops into it in Play. The owner's yacht that "did not come back" had been KEPT: the HUD said "K keep" beside the helm's keys and read as an anchor for the session; Keep wrote its pose into the level, so Stop put it there. The HUD says "K write the poses to the level" now, Keep's status "Wrote the pose of N objects into the level (unsaved; Ctrl+Z after Stop undoes it)", the Play button's tooltip "Not an anchor". `K` (UE's Keep Simulation Changes, `keepSimulation`) is the one thing that writes: the selection's bodies if any of them moved, else every body that did, one undo entry.
- **Keys** (`input/bindings.json`): Alt+P or Level Editor > Play (from the level's Player Start, from the camera without one), Play From Camera; in Play WASD and the mouse, Shift, C or Ctrl (held), Space; Esc or F2 stops (F2 and Developer Controls' checkbox open the editor, and opening it stops the session); F8 ejects and possesses (the body stands and lives, the camera flies, UE's Possess / Eject; `GenerateFont` moved to Ctrl+F8); Shift+F1 or losing the focus frees the mouse, a click takes it back. Release: F8 starts, Esc frees the mouse. The HUD's line under `Time:` says the mode and the keys.
- **Player Start**: level object `{"type": "playerStart", "position": [the FEET], "rotationDeg": [pitch, yaw, 0], ...}`, a document object with no renderable as `freeCameraStart` and the zones are. Create > Player Start or MCP `placePlayerStart {at, yawDeg}` (a second call moves the one there is); always on screen as a capsule of the player's size, an arrow from the eye and its icon (`art/editor/viewport_icons/player_start.svg`, `tools/build_editor_icons.py`). The numbers of `physics::PlayerSettings` are optional keys of this object, edited in the Inspector, in one table (`game::PlayerKeys`) with `--set player.<key>` and Developer Controls > Physics: walkSpeed 4, sprintSpeed 6.4, crouchSpeed 2, swimSpeed 2, acceleration 20.48, braking 20.48, friction 8, airControl 0.35, jumpSpeed 4.2, height 1.8, crouchHeight 1.1, radius 0.3, eyeHeight 1.65, crouchEyeHeight 0.95, stepHeight 0.4, maxSlopeDeg 50, swimDepth 1.3. A session starts from the values as they stand plus the level's keys, and gives them back at its end.
- **The controller**: `PlayerController` (the keys and the mouse) over `physics::PlayerBody` (`PhysicsCharacter.cpp`: Jolt's `CharacterVirtual` and UE's `CalcVelocity` / `ApplyVelocityBraking`, CharacterMovementComponent.cpp:3863, :4377, in metres, on the frame's clock). Run-up, stop and turn are UE's; a jump press counts 0.1 s before landing and 0.1 s after an edge, once; in the air a share of the acceleration and no braking; a crouch cannot stand up under a ceiling (`SetShape` refuses, it asks every frame); a step up eases in for the eye only; on a moving floor it rides and takes off at the floor's speed; deeper than swimDepth it floats with the eye over the swell (`OceanReadback`, the height under the feet), and there is no diving: under water has no render of its own. Jolt leaves the velocity it is given alone (UE: "make velocity reflect actual move"), so what a wall stopped is taken out by hand -- only what was really stopped: a walkable slope also moves less along the ground than asked, and taking that out would crawl, worse the higher the frame rate. Under `physics.killY` it comes back at the start. Not there: a body others hit (no `mInnerBodyShape`: it pushes, a falling coconut passes through), pause, arms in the frame, head bob, footsteps.
- **A panel that closes leaves its keyboard focus behind** (found by the live test): `WantCaptureKeyboard` stayed true after F2, `App::WndProc` gives every letter to whoever holds it, and the free camera never showed it because its right button clears the focus (ImGuiLayer). Play has no right button to hold: WASD never reached the player. AppController clears it whenever no window of ours is on screen. A key through a panel's focus reaches `ImGui::IsKeyPressed`, not InputManager: Alt+P and Esc read both.
- **The mouse is the desktop's**: `ClipCursor` confines it for everyone, so a window in the background never takes it (`InputManager::WindowIsForeground`) and keeps the keys. A script's Play (`--play`, MCP `play`) leaves it free until a click.
- **Scripts**: MCP `play {fromCamera}` / `stop`; while it plays `run_action` (but `keepSimulation`), `undo`, `save_level` and `import_asset` are refused, and `open_level` ends the session. `get_readouts` blocks `player` {exists, possessed, mode, onGround, crouched, speed, feet} and `ui` {wantsKeyboard, wantsMouse, typing}. `--set player.testMoveX / testMoveZ / testJump / testCrouch / testSprint` say what the keys would. `--play` boots into it and survives a level load (the player starts again; the editor's session ends). `--scene-stress` has its own loop without AppController: it churns the running world, with no player. Real keys: PostMessage WM_KEYDOWN / WM_SYSKEYDOWN to the `D3D12WindowClass` window reach InputManager (a process also owns `UAC_InputIndicatorOverlayWnd` and such: not the first visible one), and only keys the focus above does not hold; `ui.wantsKeyboard` tells. Alt+P with a panel's focus goes through ImGui's modifier, from `GetKeyState`, which a posted message cannot set: that one road was not run.
- **What it costs**: the idle world, the same view, alternated twice: `Scene::TickPhysics` 0.001-0.003 ms in the editor, 0.013-0.014 ms with the player, no frame rate to see. With a player every Tick joins a step still running (`Physics.Wait Step`): `stepWaits` +0 on that view; a world whose step outlasts the frame waits for it, where without a player the step would come a frame late.

## Boats -- a Hull on the Waves, a Helm in Play

A mesh.json `"vessel"` block makes the asset a powered boat (2026-11-05, owner: "import small_yacht, check the render, make a control model on our water", then "a really cool one that takes the waves into account"; `physics/PhysicsVessel.cpp`, `physics::VesselSettings`, one key table `VesselKeys()` in `CollisionJson.h` for the file, Mesh Editor > Vessel and `--set vessel.<key>`, transient, on every boat in the world). A copy is Dynamic and buoyant unless the object says otherwise -- the document writes "physics" / "buoyant" only where they differ from THAT default (`SetPhysicsCommand::VesselAsset`, `DocumentMotion`, `SetBuoyantCommand::DocumentBuoyant`). The Inspector shows the Body row on a boat too (2026-11-09, owner: "I don't see the body field"; it was hidden for vessels): Dynamic = a boat, still in the editor, floating by its hull in Play; Static + Floats = no helm, rocks on the waves in the editor too (OceanBuoyancy's kinematic floater: the selection's outline 1.6-1.8x its pixels moved in 1.2 s, as Dynamic 0.05, the DLSS floor); None = a mesh. A Dynamic floater's buoyancy line says the physics world floats it, not pontoons (it said "no pontoons could be placed"). In Play, within 3 m of its hull (the rectangle, `World::FindVessel`, no wait for a step), **E** takes the helm: W S throttle, A D rudder, the mouse orbits, **V** the helmsman's eyes, E leaves over the side. A boat with nobody at the helm floats with its engine off. A key's press is READ, not consumed (`WasActionPressed`): the frame whose E takes the helm must not read it again as leave -- until 2026-11-09 it did (`TickHelm` right after `TakeHelm`), so the real key took the helm and dropped the player over the side in one frame (owner: "E throws me into the water beside it"); the scripts' `player.testInteract` is cleared at the take and never showed it. Scratchpad-style `helm_keys.py` presses the real E (window messages, a desktop of its own): take, leave, take, three frames apart in the log.

- **The water under it** (`MeasureVesselWater`, game side, where the steps are counted): nine heights over the hull and one over the propeller. The least-squares plane is what Jolt's exact volume cuts the hull with, so a wave shorter than the hull lifts it no more than a real one does; three stations along it say what one plane cannot. The rates are followed (tau 0.08 s), not differenced: heights land from readbacks at 30 Hz. `ocean.windForce` moves it live: 0.6 (wind_test) is a 1 m sea, 2 is 5-6 m.
- **The step** (`PushVessel`, on the step's thread between Jolt's updates, the first of a batch where it is kicked): Buoyancy: `MakeVesselShape` moves the centre of mass over the centre of buoyancy at `draft` and down to `centerOfMassY`, so it lies level by construction, and the factor is total / under-draft volume; capped at `reserveLift` (2 x the weight), because the collision hull is a convex lump with the cabin in it (29.6 m3 against 3.25 under still water) and a bow driven into a crest took 9 x its weight and flew 2 m. Thrust at the propeller, by the water standing over the PROPELLER (`bite`): a crest's back lifts it clear, and the boat loses its push and its rudder together. The rudder: a force across the stern by the water past it (own speed + `wash`), at the centre of mass's height: a boat lying still answers no rudder, one on its wash does, astern it turns the other way. The hull: quadratic along, a hump (`humpSpeed`, `humpDrag`, `humpTrim`) and onto the plane (`planeSpeed`, `planingLift`, `planeTrim`): a fifth of the throttle stays under the hump at 3.4 m/s, the bow is 3 degrees up at it and 1.5 on the plane (the gate's box; the small_yacht 2.8 held under the hump at 0.35 throttle, 1.9 on the plane), the hull rises 9 cm. The trim (`humpTrim`, `planeTrim`) is the water's push on the fore part of the bottom, so it takes the bow station's immersion (0 = the bow's keel out of the water, 1 = at the draft): a bow in the air has nothing under it to lift it further. Sideways the force acts `swayLever` aft of the centre: the keel's, without it the sea slewed a full-ahead boat 69 degrees in 30 s (now 9). `rollStiffness` + `bank`, `maxBank`: the form stability a convex lump does not have, and a target lean into a turn; without them the real hull went to 80 degrees in a hard turn (the gate's box did not, it is stiffer: test on the asset). `waveForce`: Froude-Krylov, each station's share of the weight down its slope: a swell surfs it, a slope that differs at the bow and the stern yaws it. `slam`: a station falling into the water faster than it rises takes the water's mass at once (`state.impact`, up to `slamMax` of the weight).
- **The damping is against the WATER's own motion** (`water.speed`, `water.spin`), or a boat lags behind every swell and shoulders through it; on the velocities, so no setting can make it unstable; and BEFORE the step's impulses, or it takes a share of the buoyancy and the boat sits 2 cm under its draft. Pitch (`pitchDamping` 4/s) is apart from roll (3/s): under-damped, the pitch's 0.77 Hz resonated with a sea met at speed and the bow stood at 59 degrees (now 16-24).
- **Threads.** `helmInput` is the game's and is taken into `stepHelmInput` where a step starts; the water and `vesselState` are the step's while one runs; `helmShown` and `shownPosition` are written by `Display` (`ShowVessels`) and are the game's alone. The camera (`game::VesselCamera`) is set in `Scene::Sync` AFTER `SyncPoses` and BEFORE `renderCamera_.TakePose`, from the very pose the frame draws the hull at. The helm's roll is `atan2(starboard.y, up.y)`, exact at any pitch.
- **What the small_yacht does** (the numbers are the asset's: 10.2 x 2.3 m, 5500 kg, 22 kN, draft 0.38): 0 -> 10 m/s in 3.9 s, top 16.1 m/s = 31 kn, 33 at the peaks of a sea; hard over from full ahead 81 degrees in 4 s, leaning 13, down to 12 m/s; astern 8.9 m/s. wind_test's sea, full ahead: the bow -4 .. +19, the roll within 24, it leaves the water on crests; at `windForce` 2 it rides 6 m of heave, the bow -17 .. +25.
- **A hull at speed flies through the GI probes, and it must leave their rays** (`giProbeSpeed`, 2 m/s, with a margin back at 0.7 of it; `rt::kInstanceMaskGiProbe`, `RenderableObject::SetLeavesGiProbes`). Probes stand in the world: at 16 m/s each is inside the hull, out of it and behind it from one update to the next, and the cockpit's bounce jumped 25-37 % a FRAME (the probe's window mean at the dash: largest step 0.386 -> 0.024, mean second difference 0.0189 -> 0.0021, `gi.enabled:0` 0.0017). Below that speed the hull lights itself as anything else does.
- **The cockpit's shadows**: at the old 8 m base (3.9 mm a texel, a level's page grid sliding with the camera) every edge there showed stairs that crawled as the hull rocked (owner: "the shadow flickers inside"). At the helm under way, the second difference of the probe's mean, 8 m -> UE's levels (below): the panel 0.0049 -> 0.0002, the wide window 0.0017 -> 0.0008; the left of the dash wanders with the sea (0.0030 -> 0.0015).
- **MEASURING A FLICKER HERE**: `screenshot {burst}` stalls every frame on its PNG and the hull moves on between them, so a burst reads as a 16 % per-frame flicker that is not there. The pixel probe's history (`debug.probeU/V/Radius` + `get_readouts {history}`, one row a frame, no stall) is the measure, the camera at the helm, whose hull holds still in the frame: scratchpad-style `boat_flicker4.py`. `player.testThrottle / testRudder / testInteract / testView / testOrbit` (radians about the boat) `/ testOrbitPitch` (radians further down, the chase camera's steepest is 74 degrees) say what the keys would, `get_readouts` block `helm`. `--set vessel.cameraDistance:9;vessel.cameraHeight:0.8` for a look at the waterline.
- **The anchor** (2026-11-09, owner: "make an anchor"; he had taken K, Keep, for one). At the helm **X** drops it straight
  down from the bow (the stem of the hull's box at the draft, `Entry::bowLocal`), X again weighs it; it holds with nobody at
  the helm, and Stop weighs it with the session -- the world's, never the level's (`World::ToggleAnchor`, after a Join;
  `PullAnchor` in PhysicsVessel.cpp, after PushVessel in the step). The rode is slack within `anchorScope` (8 m) of the
  anchor, past it a spring on the bow along the water (`anchorStiffness` 8000 N/m, `anchorDamping` 6000 N s/m; only pulls):
  the boat swings bow to it, and a pull stretches it pull / stiffness further. mesh.json vessel keys like the rest (Mesh
  Editor > Vessel, `--set vessel.anchor*`); `player.testAnchor` = the key for a script; `get_readouts` helm / vessels
  `anchored`; the HUD says ANCHORED and "X anchor" / "X weigh anchor". Gate `vessel`'s arm: half astern for 60 s, the bow
  held at 8.54 m (scope + pull / stiffness = 8.55, farthest 9.82 at the first jerk), weighed it goes 343 m. Live (wind_test,
  scratchpad-style `anchor_live.py`): out past the surf, anchor, over the side -- 4.7 m in 40 s (farthest 7.0), without it
  53.6 m; after Stop the anchor is up and the yacht at its authored place.
- **A body put back by Stop did move** (`RenderableObject::SyncSceneState`, `putBack`): `RequestMotionHistoryReset` means no motion vector, and it used to clear `MovedThisFrame` too, so a boat back from 700 m read as "settled" where it had not been culled: `S14 cascade 0: LEAK`, a Fatal and an assert in Debug at Stop. Only a far trip shows it, and only Debug: run a change here through a Debug Play too (Jolt's asserts live there as well).
- **Gates**: `vessel` (through the functions the world calls, on a box of the yacht's size: still water, the helm, a swell, the beam, a sea at an angle, a head sea; its arms: no rudder keeps the course, a dry propeller pushes nothing, no water pushes nothing, the weight up top capsizes it, a boat that cannot see the waves does not ride them, no slam slams nothing), `world` (the helm's road: near, past, taken, tuned, a boat gone with its helm held).
- Not there: walking on the deck, an engine's sound. The bow rail's kink seen from the helm is the asset's own: the same at LOD0, `tessy.lod:0`, without DLSS.
- **The trim stood the yacht on its stern; it follows the bow's immersion since 2026-11-07.** Calm sea (`ocean.windForce:0.15`), `player.testThrottle:0.35` from rest: under the hump at 4.2 m/s the bow climbed at a steady ~3 degrees a second to 75 (y +3 m, the thrust then lifting it along its axis). `get_readouts` helm `buoyancyLead` (the centre of buoyancy ahead of the centre of mass along the hull, m) and `trimTorque` showed why: the water's righting moment grows only until the bow is out (the centre of buoyancy 1.5-2 m aft at 10-20 degrees: weight x 2 m ~ 108 kN m) and was no more than the hump's 93 kN m, while the trim, a torque on a hull wet anywhere, kept pushing. Full throttle crosses the hump in a second and never showed it. Now the trim takes the bow's immersion: 0.35 from rest holds 2.8 degrees at 4.47 m/s; full ahead 16.1 m/s as before, the head sea's worst bow 16.5 degrees (was 24). The `vessel` gate's arm: 2.5 x `humpTrim` at 0.35 throttle stays under 12 degrees and on its keel (as a torque: 76 degrees). Scratchpad-style `boat_hump.py` prints the climb every 0.5 s.

## Making a Staged Boat a Vessel -- fast_boat_13, police_boat

2026-11-10 (owner: "two more yachts in import_staging: import them, set up their helm, check how they look", then "make sure no particles and no water go through the boats"). `fast_boat_13` (a 6.7 m wooden runabout, 545 k triangles) and `police_boat` (9.8 m with a cabin, 209 k), both by agt14032013, CC-BY-4.0. The road, each step with what it caught:

- **Import** through the editor's MCP (`list_staging`, `import_asset {name, targetSizeM: 0}`, `import_status` until `running` is false: one import at a time). The bow is glTF -x, so the engine's +x (`vessel.forward` (1, 0, 0), as small_yacht); "POLICE" reads the right way round.
- **The origin must be on the keel**: `VesselSettings::draft` is the water's height over the mesh's ORIGIN and the hump's trim takes the bow's immersion as (water - keel) / draft, so an asset whose origin is mid-height (these: the keel at -0.90 / -1.78 m) cannot be given a draft. mesh.json `"pivot": "base"` and a re-bake (`--reimport --reimport-src=... --reimport-out=... --reimport-manifest=...`; the log's `[meshbake] pivot to base: origin moved by (...)`): y = the keel, x / z = the centre of the lowest 5 % of the hull -- for police_boat 1.2 m from the stern, so its bow is at x 8.6; every position in the block is measured in THAT frame.
- **Hydrostatics, not a guess** (scratchpad-style `hydro.py`: the glTF in the engine's frame after the pivot, the underside's height on a 4 cm grid from every triangle, area-sampled; the displaced volume by draft): fast_boat_13 at 1900 kg floats at 0.387 m (waterline 5.60 x 1.76 m), police_boat at 6500 kg at 0.707 m (8.68 x 2.40 m). In Play they came to rest at -0.393 and -0.719 m, level (pitch -0.1, roll -0.6 / -0.1 degrees): the same numbers. It also prints each material's box, which is how the wheel (the helm's eye half a metre aft of it), the dash, the stern drives and the transom (the propeller 0.3 m ahead of it, low) were placed.
- **The rest of the block is small_yacht's scaled** by mass (mu) and length (lambda): forces by mu (hump drag, slam, the anchor's rode), torques by mu x lambda (hump and plane trim, yaw damping), form stability by mu x the beams' ratio, the drags by the wetted area, the hump and planing speeds by sqrt(lambda), the keel's lever and the camera by lambda. The top speed is the one free choice: thrust = surgeDrag x 0.77 x v^2 (small_yacht: 22 kN, 110, 16.1 m/s) -- fast_boat_13 9 kN / 26 -> 21.1 m/s (41 kn) measured, 21.2 asked; police_boat 27 kN / 120 -> 17.1 m/s (33 kn), 17.1 asked. Hard over: 21 and 15 degrees a second.
- **A light fast hull needs less reserve lift** (`reserveLift`, the cap on the collision hull's buoyancy, 2 x the weight by default). On wind_test's 1 m sea (scratchpad-style `boat_sea.py`: full ahead, a slalom, astern, the hull sampled every 0.1 s) police_boat rode as small_yacht does (roll within 22 degrees). fast_boat_13 at 2.0 was thrown off crests and came down on its side: roll to 52 and 69 degrees in two runs, whatever the roll stiffness; at 1.6 with `rollStiffness` 36000, `rollDamping` 6, `pitchDamping` 5.5, `heaveDamping` 4, `maxBank` 18: within ~30 (four runs). The first seconds after Play read as a pitch of -11 .. -17 "at rest": the player landing on the bow, not the sea.
- **A `--set vessel.<key>` lands on EVERY boat of the world and stays** for the process: tune one boat with it, write the numbers into its mesh.json, RESTART the editor before testing another boat (a chase-camera distance set for one shot overwrote all three boats').
- **Glass came in opaque** -- the importer's gap, by hand here: a glTF material with `alphaMode: BLEND` and a constant alpha but no KHR_materials_transmission (an AC3D export) becomes a cutout at 0.5 (fast_boat_13's windshield, alpha 0.37: a solid blue band) or a plain opaque surface (police_boat's cabin windows: black). Both set to `thinTranslucent`, transmission = 1 - alpha, the tint's alpha = the alpha, `glassWall` 0 (panes). And the source says metal 0 / roughness 0.93 for everything: chrome, varnished wood and gelcoat were given their own by what each texture stands for (scratchpad-style `boat_materials.py`).
- **No water in a boat**: `"waterMask": true` on both. Checked from straight above the cockpit under way and in a slalom with `ocean.waterMask` 1 / 2 / 0 (scratchpad-style `boat_dry.py`): with the mask the cockpits are dry in every phase, 2 paints the water it keeps out, 0 floods them in a turn.
- **No particle is drawn in or over a boat** (same day, three cuts; owner after the second, watching the runs: "I saw particles in both boats"). (1) `InsideHull` tested a hull's solid alone: drops drifted through an open cockpit's air. (2) The cockpit's air up to its rim as well: the drops that came over the gunwale ABOVE the rim flew on, and from straight above (the only view checked) they hid in the wake's white. (3) What it is now, found by PAINTING THE SPRAY MAGENTA (`--set spray.colorR:1;spray.colorG:0;spray.colorB:1`; `spray.opacity:0` in the same held frame makes it go: it is particles, not foam, a glint or water) and shooting each boat from four sides close in a slalom (scratchpad-style `boat_parts.py`: the world held by `frame.recordDt`, a free camera above, astern, abeam and off the bow, four phases):
  - **the plan** (`ocean/WaterMask.cpp`, the columns' fourth component, `shaders/water_mask_columns.hlsli OverHullPlan`): the lowest surface over EVERY cell the mesh touches (`plan` in the `water mask:` log line), where the columns are only the cells that keep water out -- a cell under one sloping sheet or with a sample the mesh does not cover has no column, right for the water (it must stop inside the skin) and a hole for a particle. A particle whose centre is over the plan and not under that surface dies at ANY height (`particle_common.hlsli InsideHull`): the solid, a cockpit's air, the air over the deck.
  - **the sprite is clipped per pixel** (`particles.hlsl PSMain`, `GpuEmitterDrawParams::hullCount / hullRows` -- the CB is 480 bytes --, the columns at t6, the draw's table is 7; an emitter with `collideHulls`): a sprite is a quad up to 1.1 m across standing for a clump of drops, and one whose centre flies beside the hull laid a part of itself over the gunwale and into the cockpit at its centre's depth -- most of what was seen "inside" was this, not centres. Each pixel's own place on the quad is tested against the plan and discarded over it. `--set particles.clipHulls:0` = drawn (the A/B, transient). 32 frames a boat with the spray magenta: none in or over either hull. The frame the same with it on or off (police_boat at the helm, alternated twice: GPU 2.96-3.06 / 2.94-3.01 ms). Tried between the two and dropped: killing a particle whose sprite REACHES over the deck (taps a radius away): a drop beside a topside and below its gunwale still showed over it, and killing every drop within a radius of the hull kills the spray where it is born.
  - What still shows over a cockpit on screen is spray that is NOT over the boat: a slam's burst on the camera's side of the hull, a metre over the gunwale, seen from above and abeam.
  - GBV (`Translucent,Transparent,Transparent.Water,ObjectCompute,Skybox` + the Skybox self-test, the Debug editor on a desktop of its own, Play, the helm, 30 s at 16-17 m/s with a hard turn, 1570-1760 drops and ~115 puffs alive, 13 slam bursts; scratchpad-style `gbv_clip.py`): only id=935. Two runs before it validated NOTHING of the new path -- the helm was never taken: under GBV the first frame lasts a minute, a `set_camera` sent before it is overwritten by the level's saved editor camera, and Play dropped the player into the sea 30 m off. The script sets the camera until `get_camera` (TEXT, not JSON) says so, and prints the spray's counters: a GBV run of an effect counts only with the effect alive in its own printout.
- **A boat's own spray size**: mesh.json vessel `"sprayScale"` (Mesh Editor > Vessel "Spray size (1 = small_yacht's)", `--set vessel.sprayScale`; `VesselSettings::sprayScale`): the sheets' and the mist's sprites times it, their throw times its root (a model's heights go with its size) -- the presets were made for a 10 m hull. fast_boat_13: 0.6 (sprites 0.3-0.66 m where 0.5-1.1 hid the 6.7 m boat in a cloud); police_boat keeps 1.
- **In wind_test** since 2026-11-10 (owner: "put them in, commit, push"), group "Boats", beside the yacht by the shore, bows out to sea (yaw -65), each at y = minus its draft so it also sits right in the standing editor: fast_boat_13 at [59.97, -0.39, 109.83] (9 m to the yacht's starboard), police_boat at [44.26, -0.71, 119.92] (9.5 m to port), their sterns over ~1.0 / ~1.35 m of water (the sea floor from physics rays straight down, scratchpad-style `shore_depth.py`). In Play all three ride the surf there and drift a metre or two in 8 s, none aground; X at the helm anchors one.

## A Boat's Wake -- the Hull's Pressure on the Water

2026-11-07, owner: "the yacht's wave disturbance; the reference is Crest" (`docs/ref/crest`). `ocean/OceanWake.*`, `shaders/ocean_wake_cs.hlsl`, the pass `Main_OceanWake` (`--gbv-passes` OceanWake, GPU scope `Ocean.Wake`), t26 + `wakeParams`/`wakeParams2` of the surface (`ocean_surface_surf_sim.hlsli`; the run-up surface does not read it).

- **Crest's system, not Crest's wave speed.** The field is Crest's dynamic waves in what it does -- persistent, in a window round the camera (512 texels, `wakeTexel` 0.5 m = 256 m), re-anchored by whole texels, absorbed over its outer eighth (a rate, 6/s at the border, not Crest's per-step factor), NaN-guarded, its height in the surface's vertices and its slope in the normal and the reflection's normal. Not in its speed: Crest integrates the wave equation per cascade at its shortest wave's speed (`UpdateDynWaves.compute`: c = sqrt(g 2 texel / 2 pi), ~2 m/s at a boat's texels), so a hull at 16 m/s outruns every wave in the field and leaves a Mach cone where a real one leaves a Kelvin wake. Deep water's dispersion, omega = sqrt(g |k|), has no single speed to give a grid, and is exact per Fourier mode: Tessendorf's eWave. A step: the field (h + i v) and the hull pressure to the spectrum through the ocean's own FFT kernel (`ocean_fft.hlsl`, FFT_SIZE 512, a one-slice array view), every mode turned through omega dt about the equilibrium the pressure sets, back. Exact and unconditionally stable at any frame time: one step a frame (dt capped at 1/20 s) on the ocean's clock (`wind.freeze` holds it).
- **The coast** (same day, owner: "do what is not there yet"). The spectrum has one depth for the window, the water does not: `Step` turns every mode four times, omega^2 = g k tanh(k d) at d = 0.3, 1.5, 7.5 m and deep (two to a texture, the field's own and `Ocean.WakeSpectrum`), two inverse FFTs, and `Compose` gives each texel the two its own depth lies between, in log (37.5 m and over: deep; the shore depth map, the surf sim's `WaterDepthAt`); land holds the field at 0 (the last 10 cm ramp it), so the island stops a wave instead of letting it through. A frame that re-renders the shore maps holds the field (a step of no time), as the surf sim skips. **The surf zone** (`wakeBreaking`, 1/s, 2 = Crest's rate): Crest's Attenuation In Shallows damps where its deep-water waves stop being right, d < wavelength / 2 (`UpdateDynWaves.compute:130-139`) -- tried as such, it took a 13 m wake wave away over wind_test's 2-10 m shelf in seconds, where these finite-depth waves cross it as they should; here it starts at the shallow-water limit, d < wavelength / 20, per mode and per depth, Crest's rate as d goes to 0: a beach takes in most of what reaches it. Tried and dropped first: McCowan's breaker index (damping a free amplitude over 0.39 d) -- a 5-10 cm wake wave breaks only in the last 10-20 cm, 1-2 m of a 1:8 beach, and the picture was the reflecting one. Measured (scratchpad-style `wake_coast.py`: the yacht heads for the west beach at 0.35 throttle from (-215, 0, 0), the engine cut at x -140, from above at 46 / 56 s): `ocean.wakeShore:0` (deep water everywhere, before) -- the wake runs on through the sand ring into the lagoon; with the coast the lagoon is still and the waves bend along the beach; `wakeBreaking:0` -- the waterline is a wall, incident and reflected cross as a checkerboard to the sand; 2 -- the last metres of water go quiet, a weaker checkerboard further out (a partly reflecting beach). On deep water (over 7.5 m, wind_test's open sea) the coast changes nothing (a 13 m wave feels no bottom under 3 m).
- **Boats rock in another's wake.** `Compose` also writes the FREE height, h + head (0 under a hull at rest: no boat floats in its own hollow), R16F, which `OceanReadback` copies with the cascades (+512 KB a slot, `Main_OceanReadback` after `Main_OceanWake` in the graph) and `SampleHeight` adds where the vertex shader adds the wake (after the shore damping, at the warped coordinates, the window's edge faded). Everything that floats reads it -- `OceanBuoyancy`, Jolt's floaters, the player's swim, the boats' `MeasureVesselWater` -- a boat in full; one under way leaves it out (`SampleHeight(x, z, wakeShare)`, 1 at rest -> 0 from 1 to 3 m/s): its own bow wave and stern trough are what the hump, the planing and their trim stand for, and taken twice they would trim it twice -- the price is that a boat under way does not feel another's wake either. Measured (scratchpad-style `wake_shore.py`: yacht A at 0.35 throttle, 4.5 m/s, yacht B lying still 15 m off its course, 60 m ahead, calm sea; B every 0.1 s, `get_readouts` block `vessels`): 24-40 s, as A's wake passes B, its roll range 10.1 degrees (sd 2.1), pitch 3.6, heave 0.19 m; the control `ocean.wakeEnabled:0` 1.9 / 0.5 / 0.04. `get_readouts` `vessels`: every boat as the frame draws it {position, forward, pitchDeg, rollDeg, speed, atHelm}.
- **The foam** (same day, owner: "both" -- the white water and trail at the hull, foam on the steep crests as Crest's). A field of its own in the same window, RGBA16F (x the foam 0..2, 1 and over = fresh white water; yz the way the hull that laid it was heading, times it), two textures in turn (`Pack` reads last step's moved by the re-anchor: one texture would race), spread as nu = 0.3 m2/s and run down in two stages, as a breaking crest's foam does (stage A seconds, the residual patch tens of seconds): exp(-(`wakeFoamFade` 0.1 + `wakeFoamFadeDense` 1.5 x foam) dt) -- the denser, the faster. **The white water**: where the head under a hull FALLS faster than 1 m/s -- its transom leaving (a side, in a slide), where the water closes in; until 2026-11-10 its bow arriving laid as much, now `wakeWashBow` of it, 0 (below, "The bow's foam does not pop") -- `wakeWashFoam` (4) foam a metre of change: a texel the whole hull passes over takes twice the depth under it less the threshold over the time it took, so the trail is the hull's width, whitest on its keel, and a boat drifting or idling stays clean; a hull that jumped this step (placed, put back by Stop, teleported: over 40 m/s) washes nothing. **The crests**: Crest's wave foam (`UpdateFoam.compute:95-102`, 5 dt x `wakeCrestFoam` x saturate(`wakeCrestCoverage` - det J)) on the Jacobian of the FREE height's choppy displacement (Tessendorf's D = i k / |k| F, lambda 1 -- a Gerstner wave's particle paths -- a third inverse FFT, `Ocean.WakeJacobian`; the free height, so a hull at rest wears no ring round its hollow); Crest's coverage 0.55 is then a Gerstner wave at the Stokes limit, a k 0.44. Land holds none. **The surface** (t27, `WakeFoamCoverage`) reads the value as the SHARE of the water the foam covers, not as an opacity: a pixel is foam where it exceeds a lace -- the contact foam's two counter-drifting noise octaves folded into ridges (0 on the network of their mid-contours, 1.8 m cells) -- so 1 and over covers the water and 0.3 leaves the network's lines; the lace is drawn out 3.5 times along the way it was laid (the noise of the two nearest of 8 fixed directions, blended: one frame turned with the way would shear across the window wherever it bends), round where nothing says which way (the crests'); a finer, round octave (0.45 m) moves the lace's threshold +-0.13, so its edges tear into filaments where the 1.8 m cells alone cut smooth blobs. **The churn** (`wakeChurn`, 0-1, default 1; `WakeChurnHeight`, 2026-11-07): fresh white water (smoothstep 0.4-1.2 of the value) gets a relief -- two octaves (0.7 / 0.3 m) of the same noise folded into ridges, boiling on the ocean's clock, as a height field whose slope tilts the foam's normal (7.35 cm lumps: a low sun lights their faces and leaves their backs grey) and whose crevices darken it to 0.8285 -- 30 % under the first cut's 15 cm and 0.65 twice (owner: "take it down 30 %", 2026-11-09 "another 30 %") -- the lumps and grey seams of a prop wash (the photographs: iStock PaulRapson's stern wash) instead of a flat white sheet that the noon sun took to the tone curve's shoulder; a pixel wider than an octave reads its mips and keeps a mean grey. The ridges, not the noise's minima: those read as isolated grey spots, a leopard print. Its cost is not measurable (`Ocean.Surface` 0.60 / 0.60 and 0.48 / 0.50 with / without). It joins the whitecaps (their albedo); its **aeration**, saturate(2 foam) x 0.4 (`FoamData.aeration`; sqrt(foam) x 0.6 first lifted the field's spread-out trace into a milky halo wider than the trail), takes the underwater foam's colour without that term's fade with distance -- the lighter turquoise scar the bubbles leave under a trail, seen from a drone as well as from the boat. Compared with photographs (Bing: iStock wmaster890's aerial speedboat, Pexels' S-curve wake, stern views of prop wash): the white water just behind the transom, then lace streaked along the track on a turquoise scar wider than the hull, the trail curving with the path. The first cut (one decay 0.3/s, the value as opacity: solid while 1, then torn into round patches) laid a white carpet the hull's width (owner: "the trail is incredibly dense"). Measured (scratchpad-style `wake_foam_views.py` / `wake_circle_views.py` / `wake_crest_check.py`, calm sea and wind_test's): at 16 m/s white for some 10-15 m, lace over 100+ m; the crest foam at 0.55 two thin lines on the quarter waves' inner flanks (where the water closes into the hole behind the transom -- `wakeDebug` 1 and 2 overlaid), at 0.8 a band over the trail; foam laid at a crest stays where it was laid while the crest runs on, so it trails behind it and runs down. Not there: the white water's own self-shadowing and spray off its lumps (the churn shades them by its normal and darkens its crevices), the foam's drift with the water (orbital, Stokes; the field stays where it is laid), the active whitecap biased onto a crest's front face (det J is symmetric about a linear crest).
- **The hull is a moving pressure** (Havelock's ship): a head of water equal to how deep its underside stands under the asset's waterline -- `buoyancy::ResolveFootprint`, AutoLayout's underside raster along the vessel's `forward` at its `draft`, 64 x 24 cells, one binomial pass; small_yacht 10.16 x 2.34 m, 0.376 m deep. At rest the water under it sinks by exactly that and nowhere else (hidden by the hull); moving, the same field throws the bow wave, the stern trough and the wake from the hull's shape and path alone -- the speed is not an input. Every vessel of the physics world (`World::CollectHulls`, the poses the frame draws), handed to the ocean at `Scene::Sync`.
- **Measured** (small_yacht on a calm sea, from above: `vessel.cameraDistance:150;vessel.cameraHeight:0;player.testOrbitPitch:1.1`, scratchpad-style `wake_look.py`): at 16 m/s (Fr_L 1.6) the V's half-angle ~8 degrees -- the high-Froude narrowing of the angle the amplitude peaks at (Rabaud & Moisy 2013, ~0.2 / Fr = 7 degrees), which Kelvin's 19.47 only bounds; at 10 m/s wider. The trough behind the transom runs long at speed: the first trough of the transverse wave, 2 pi U^2 / g = 164 m long at 16 m/s. A boat placed or launched sends rings out (the pressure arriving at once). On wind_test's own sea (~1 m) the waves are subtle, as real wakes are without their white water; on a calm one the V reads from the helm and from above. `Ocean.Wake` 0.06 ms GPU (Release_Editor, a 4090), 0.085-0.093 with the four depths and the coast (`wakeShore` 1 or 0 the same), 0.108-0.122 with the foam (0.111-0.118 with its RGBA16F and two stages); `Ocean.Surface` the same with the foam on or off (0.27-0.31 either way, the camera circling, the streaks' two extra noise reads included); `Pass_OceanReadback` 41 -> 65 us a copy (30 a second); a level without a boat pays nothing (no resources, no pass, no copy), and the field is let go 30 s after its last boat. VRAM 20.5 MB (four 512^2 RGBA32F, the R16F free height and two RGBA16F foams).
- Knobs (the ocean preset, `--set`/MCP `ocean.wake*`, the Ocean window and the Inspector's "Boat wake"): `wakeEnabled`, `wakeStrength` (1 = physics), `wakeDisplacement`, `wakeNormal` (1 = the true slope: the FFT's kNormalScale is divided out), `wakeDamping` (1/s at the shortest wave, falling as k^2), `wakeBreaking` (the surf zone, above), `wakeTexel` (0.25-2 m; a change restarts the field), `wakeFoam` (the foam on the surface, 0 = none), `wakeFoamFade`, `wakeFoamFadeDense`, `wakeWashFoam`, `wakeCrestFoam`, `wakeCrestCoverage`, `wakeChurn` (above), and the arms' and the pattern's (below, "The bow wave's two arms"; every foam and arm knob is a row of `ocean/OceanWakeKnobs.h`, which names its label and hint). `ocean.wakeDebug:1` paints the height, opaque (red up, blue down, 0.25 m to full), `:2` the foam field (white, the tear not applied); `ocean.wakeShore:0` = deep water everywhere, the A/B (transient).
- **The bow wave's two arms** (2026-11-10, owner, with a sketch -- a trail from the stern and two lines from the bow's shoulders -- "now we only have the trail out of its arse; the extra exciters in front just thicken it; look at references"). Aerial photographs (Bing "speedboat wake aerial top down": dreamstime 432608826 / 432608783, vecteezy's ship's wake): a planing hull leaves the prop wash AND two arms of white water that start at the bow's shoulders and open at 7-12 degrees, their outer edge sharp and scalloped, lace and a turquoise scar between them and the trail. The field's own waves cannot break there (a linear pressure at 0.5 m texels has no chine; Crest's det J foams only the transom's quarter waves), and the spray's side lines (`spray.sideFoam`, DELETED: code, key, UI, `--set`; a level that still carries `spraySideFoam` ignores it) lay beside the hull and only widened the trail. So the breaking is carried by FRONTS (`OceanWake::StepFronts`, on the field's step): a point leaves each side of the waterline (the spray's root, `VesselSpray::BowWaveOf` -> `WakeHull::bowRoot / bowShare / speed / length / id`) every 0.125 s and runs outward ACROSS THE WAY THE HULL HEADED THEN, in the world -- thrown for a third of a second (the sheet off the chine: 0.09 s of the hull's speed, 1.4 m at 16 m/s), then at the diverging wave's own speed, min(0.354 U, 0.2 sqrt(g L)) (Kelvin's wedge at displacement speeds; past Froude ~0.57 Rabaud & Moisy's 0.2 / Fr, i.e. 2 m/s for a 10 m hull at any speed -- where the field's own outer crest runs: `ocean.wakeDebug:1` at 15 m/s, 11 m out 82 m behind). The line through a side's points (128 at most, 15.5 s) is its front. `ocean_wake_cs.hlsl` Pack draws the breaker ON it every step into the foam texture's w -- a band with a sharp toe outward (0.6 of its half width, 0.9 m + 0.25 m/s) and a tail behind (1.6), its foam `wakeBowFoam` x 1.5 x the bow wave's share (the spray's speed and immersion gate, whatever `spray.enabled` says) x exp(-age / `wakeBowLife`), NOT kept: it is where the front is now -- and lays `wakeBowLace` (0.15) of it into the kept foam under it: the lace between the arms and the trail. Laid whole into the kept foam (the first cut) every texel a front had passed stayed white for seconds: one solid wedge, no arms. The surface reads x + w. Kept in the world, so a turn leaves the arms on the path the hull took (the 11-09 flaw of lines computed from the heading now).
  - Knobs (the ocean preset, `--set ocean.wakeBow*`, "Bow wave arms" in the Ocean window and the Inspector's "Boat wake"; the table below): `wakeBowFoam` (1, "Arms amount"; 0 = no arms, and the GPU is handed no front), `wakeBowSpread` (1 = the wave's own speed, "Arms opening"), `wakeBowLife` (2.5 s e-fold, "Arms length (s)": white for ~4 hull lengths at 16 m/s), `wakeBowLace` (0.15, "Lace behind arms"; 0 = clear water between arm and trail, 1 = the solid wedge). `get_readouts` `wake` {fronts, frontPoints, frontsDrawn (8 at most: four boats)}.
  - **"The left edge of the foam jerks to and fro"** (owner, watching the first build): which side of a front's line is outward was read off its first piece, and that piece has no length on the step a point leaves the hull -- the cross product was 0, read as "left", right for starboard and wrong for port: the port arm's toe and tail swapped every eighth of a second. The side is the front's own now (port: the line's right). And a HITCH forgot both arms: a frame that took long moved the hull further than the step's capped time allows, which the wash reads as a jump (no foam that step, as before) -- the fronts are kept unless the hull is over 8 m from where the last step had it (placed, put back by Stop) or the field was cleared.
  - PHOTOGRAPHING A SERIES: `screenshot {burst}` is useless here -- every frame stalls on its PNG, the physics runs on (4 steps a frame at most) while the ocean's clock does not keep up, the wash reads every step as a jump and the trail falls behind the boat (frames 4-11 of a burst of 12 showed no foam under the hull). Single shots 0.12-0.15 s apart instead (scratchpad-style `wakeseries.py`: 8 frames from 60 m up, `KEEP=1` leaves the boat at the helm for `shot.py` / `perf.py`).
  - Cost: the fronts are walked by PIECES of 16 segments with a box each (`FrontBounds`, 8 a front): one box round a whole arm lying across the axes covered most of the window and `Ocean.Wake` read 0.17-0.25 ms; by pieces 0.107-0.114 with the arms, 0.110-0.117 with `wakeBowFoam:0` (alternated twice, the mid view, 16 m/s) -- nothing. WakeCB is 18 KB (three copies a step, 30 steps a second).
- **The wake's foam has its own texture and no repeat** (same day, owner: "even the tile shows on the current trail", then "pick a better foam texture if you can", then "play with the tile size"). The tile was three grids: the lace was the contact foam's noise at a 1.8 m tile (its streaks 6.3 m), the churn's relief the same texture at 0.7 and 0.3 m, the foam's albedo a 1 m tile -- and the lace read as paint splatter, hard-edged blobs. `textures/ocean/WakeFoam.png` (ours: `tools/gen_wake_foam.py`, numpy, a fixed seed; t29, the surface's table was 30, 31 with the water mask's crossings at t30; `textures/**` is gitignored: force-add it): R the LACE -- filaments at three scales (cells a fifth, an eleventh and a twenty-fourth of the tile) with rafts of bubbles between them, domain-warped, equalised to a flat histogram (a foam value v covers the share v, exactly) and stored GAUSSIANISED (0.5 + z / 6); G the churned white water's height (billows with rounded creases), BA its slope (0.5 + slope / 320, heights a tile). `ocean_surface_surf_sim.hlsli WakeFoamCoverage` reads it through a HEX TILING (Mikkelsen 2022): hexagons 0.475 of the tile across, each its own window of the tile (a random offset, drifting 6 cm/s its own way: neighbours slide and the foam boils), a pixel blends the three nearest with weights^4 -- the lace as Heitz & Neyret blend (2018: gaussian values, the variance kept), so a blend has the tile's own contrast and the share stays the value. A window is drawn out 3.5 times along the way the foam at ITS HEXAGON was laid, a pure stretch along that way (S = I + k way way^T), no turn: nothing shears where the way bends, nothing pops when a hexagon's first foam gives it a way. The cover is the normal CDF (a logistic) of (the value's rank - the lace) / spread, spread^2 = 0.14^2 + the variance the mips have lost (measured on the tile's chain: its deviation 1 / 0.93 / 0.75 / 0.45 / 0.18 at mips 0 / 2 / 4 / 6 / 8, fitted 1 - (lod / 8.5)^1.6): far away the cover goes to the value itself, the share. The relief's height and slope come from the same three taps (their mips flatten a far pixel's by themselves), and the wake's foam is plain white under them, not the whitecaps' 1 m albedo tile. 7 taps a wake pixel (4 of the 512^2 foam field, 3 of the detail) where the lace and the churn took 14: `Ocean.Surface` with `ocean.wakeFoam` 1 / 0, alternated twice: chase 0.299-0.305 / 0.287-0.288 ms, mid 0.339-0.355 / 0.329-0.330.
  - `wakeFoamTile` (m of water a tile covers, 0.2-16, default 3; its floor was 1 m for no reason -- owner: "why on earth is it clamped at 1 m"; "Pattern size (m)" in both windows, `--set ocean.wakeFoamTile`): the lace's scale and the lumps'. Looked at 2 / 3 / 4 / 6 / 8 / 12 m from the chase camera and from 45 m: 2-3 is the fine feathery lace of the aerial photographs, 4 reads best from the helm, 8-12 are slabs with metre-wide holes. The soft white blobs over the wake in a shot are the spray's MIST puffs, not the foam.
  - **Every number of the foam and the arms is a knob, in ONE table** (same day, owner: "bring out everything that can be tuned, with hints one can understand"): `ocean/OceanWakeKnobs.h` -- per knob the key (the ocean preset's JSON key and `--set ocean.<key>`), the `OceanRenderConfig` field, its range, the drag's speed, and the LABEL and HINT both windows draw (the Ocean window, the Inspector's "Boat wake"; three headings: "Wake foam: how much, how long", "Wake foam: the pattern", "Bow wave arms"; a tooltip ends with its `--set` line and range). `OceanRenderConfigJson`, `--set` (App.cpp, past "ocean.") and both windows walk the table: a knob added there is saved, loaded, settable and drawn, and nothing else lists these keys (the field's physics knobs -- strength, damping, breaking, texel -- stay as they were). A hint says what the eye sees and which way to turn, not how the term is computed. Brought out of the code with it: `wakeFoamStretch` (3.5, "Streak stretch"), `wakeFoamSoftness` (0.14, "Edge softness"), `wakeFoamBoil` (0.06 m/s, "Boil speed"), `wakeAeration` (0.4, "Bubble scar") -- the surface's `wakeParams4` --, `wakeFoamSpread` (0.3 m2/s, "Foam spreading": the field's diffusion, capped at 0.24 of the stability limit; at 0 the arms and the trail are three crisp stripes), `wakeBowThrow` (0.09 s, "Arms shoulder"), `wakeBowWidth` (0.9 m) / `wakeBowWiden` (0.25 m/s), `wakeBowScallop` (0.2, "Arms raggedness"), `wakeBowSpeedFull` (12 m/s: the arms come in from 3 m/s to it -- the wake's own gate now; `VesselSpray::BowWaveOf` hands over the bow's immersion alone, and a boat under 3 m/s keeps no front). NOT a knob: the hexagons' spacing (0.475 of the tile -- a window is then just under one tile across: larger shows the repeat inside a window, smaller is a mush of blends). Each proven to move the picture at its two ends (scratchpad-style `knobsweep.py`: the boat running, low / high / back to the default, from 45 m and from the chase camera); the boil's speed cannot be seen in a still.
  - **The white water behind a turning hull read as plaster** (same day, owner at 18:00, full throttle, the rudder hard over, the camera behind the stern: "fix this cringe patch with the bump"). A hull that slides sideways in a turn stamps its whole footprint: a slab of foam at the field's cap, one white, and the relief on it was a grain of centimetres under a low sun -- the height rode the lace's tile (a fortieth of 3 m), and the texture's height had only the two small scales. Now (1) the relief has ITS OWN scale, `wakeChurnSize` ("Relief size (m)", 4 m a tile, at least the lace's; `wakeParams3.w`): a second tap of the detail texture a hexagon, read only where the pixel is fresh white water (foam over 0.4 and `wakeChurn` on) -- `Ocean.Surface` from the chase camera at 16 m/s with `wakeChurn` 1 / 0, alternated twice: 0.371-0.376 / 0.372-0.375 ms; (2) the texture's height is mostly BOILS (a fifth to a twelfth of the tile: 0.3-0.8 m at 4 m) with the lumps and a little grain on them (`gen_wake_foam.py`; the lace's channel is the same bytes: its random draws come first); (3) `wakeFoamCap` ("Solid white limit", 0.93; `wakeParams5.x`; 1 = before): the densest foam covers that share at most, so the clearest few per cent of the lace stay holes with milky water in them -- froth, not paint. On the owner's frame (scratchpad-style `turnshot.py`: 18:00, the turn, arms of settings): soft boils under the stern where the slab was; `wakeChurnSize:1` with `wakeFoamTile:1` brings the grain back (the control).
  - **Each patch at its own size** (same day, owner: "can the tiling vary +-15 % along the tail, or is that a pain?"): no pain -- every hexagon of the tiling already draws its own window, so it also draws it at its own scale, the tile x (1 +- `wakeFoamTileVary`) from the hexagon's hash ("Pattern size variation", 0.15, 0-0.4: past it a window would hold its tile twice; `wakeParams5.y`; the relief takes the same factor). No seam: the blends are the tiling's own. From 45 m at 0 / 0.15 / 0.4: finer and coarser stretches at 0.4, subtle at 0.15; `Ocean.Surface` 0.345 ms either way. A body-only change of the surface shader on a field it already had: not put through GBV again.
  - **The bow's foam does not pop** (2026-11-10, owner, on police_boat at 33 kn seen from ahead: "the foam at the bow appears abruptly, it shows even at speed; can it be smoothed?"). The field steps 30 times a second and a hull at 17 m/s runs 0.56 m between two steps: whatever starts AT the hull and stands on the water meanwhile jumps forward with each step. Two things did. (1) The wash laid foam where the head under the hull RISES too -- a white collar round the stem (under a planing hull's stem, which is in the air): now only where it falls, twice as much (the trail the same), the arrival a knob (`wakeWashBow`, "Stem collar", 0; 0.5 with `wakeWashFoam` 4 = before). (2) The arms' band started at the roots at full strength, a round white cap beside the bow. A fade by the points' AGE (the first cut) only made the ramp softer: it still stood on the water and jumped. So the band is laid whole from the hull and the SURFACE lets it in over a gate that rides with the hull, every frame (`OceanWake::BowGates` -> b2's `wakeBowGate`, two float4 a boat: the roots' midpoint and the hull's forward, the gate's length = the hull's speed x `wakeBowFadeIn`, its lane; `ocean_surface_surf_sim.hlsli WakeBowGate`: w x smoothstep of the distance aft of the roots' line; beside the lane and 3-6 m ahead of it 1, another boat's arms). `wakeBowFadeIn` ("Arms start (s)", 0.3 = white from ~3 m aft of the roots at 17 m/s, lace before; 0 = whole from the hull). The residue (the lace the band leaves) comes in along the front's first points in the Pack (`FrontInfo.w`). Measured on 16 consecutive frames at 1/120 s, the camera riding with the bow, spray hidden (scratchpad-style `police_look.py` PHASES=bow + `bowdiff.py`): the frame-to-frame change's largest / median 1.09 with the hard cap (a spike every fourth frame), 1.02 with the gate at 0.3 and 0.5 and on wind_test's sea (no step stands out of the glints' own change); white pixels in the bow's window jumped 30 k a step before. `Ocean.Surface` with the gate / without 0.365-0.366 / 0.366-0.368 ms. A NEW KNOB IS STICKY UNTIL THE LEVEL IS SAVED: `reset_settings` reads the document over the running config, and a key the level's file does not hold yet keeps whatever a `--set` left -- a whole series of "new" arms ran with the "old" arm's values; name a new knob in every arm. `line` and `in` are HLSL keywords (a variable so named took the whole surface out: the sea not drawn, `error: modifiers must appear before type` in the log), and a shader that failed at boot is not hot-reloaded: restart. Not done: the trail's own start behind the transom steps the same way (under the spray there; not looked at).
  - Checked: straight at 16 m/s on wind_test's sea and on a calm one (`ocean.windForce:0.15`: two clean symmetric arms), 0.35 throttle (a wider V: the same 2 m/s outward at a lower speed), a hard turn (the arms stay on the path), a series of 8 frames (no edge moves). GBV (`OceanWake,OceanReadback,Transparent,Transparent.Water,Skybox` + the Skybox self-test, the Debug editor on a desktop of its own, Play, the helm, 30 s at 15-17 m/s with a hard turn, 2 fronts / 200 points alive, `wakeFoamTile` and `wakeBowFoam` switched in it; scratchpad-style `gbv_arms.py`): only id=935.
- **The field steps at most 30 times a second** (2026-11-10, owner: "at what rate do you simulate the disturbances on the water? limit it to 30 Hz"). It stepped every frame -- 200 a second at 200 fps, `Ocean.Wake` 0.088 ms each. `ocean.wakeHz` (30; the Ocean window "Wake steps a second", `--set`; 0 = every frame): `OceanWake::BuildPass` steps only once that much of the ocean's clock has passed, by all of it (eWave turns every mode exactly at any dt; the foam's diffusion stays stable to the 1/20 s cap); a frame between records no pass and keeps the field, its foam, its window and the time it stands at; a new or cleared field steps at once, and a frame that re-renders the shore maps skips and keeps its time for the next step (it used to step by no time and drop it). The surface carries the height on between steps by the field's own vertical velocity, h + v tau (`WakeHeight`, wakeParams3.y = seconds since the step: `Turn` keeps v = dh/dt), in the vertices and in the slope; `ocean.wakeExtrapolate:0` shows each step as it stands (the A/B). The buoyancy's copy reads the free height of the last step on any frame (`GetFreeHeightResource`; waiting for a step frame would have dropped the wake from the boats' water on most copies). Surf sim untouched: its explicit wave equation needs its fixed 1/120 s substeps, which already do not follow the frame rate. Measured at the helm, full throttle (16 m/s) on a calm sea, the chase camera's probe window on the white water behind the stern, alternated twice (scratchpad-style `wakeab.py`): GPU frame 2.82-2.87 -> 2.73-2.80 ms; the window mean's frame-to-frame change (mean |first / second difference|, 1e-3) every frame 0.79-0.82 / 1.11-1.23, 30 Hz carried on 0.73-0.89 / 1.06-1.41, 30 Hz as it stands (the control) 1.18-1.31 / 1.67-1.79. GBV (`OceanWake,OceanReadback,Transparent,Transparent.Water,Skybox` + the Skybox self-test, the Debug editor on a desktop of its own, Play, the helm, 40 s at 16 m/s at 30 Hz and every frame; scratchpad-style `gbv_wake.py`): only id=935.
- **Photographing it**: the field follows the CAMERA -- a Stop that sends the camera back 250 m leaves the wake behind; shoot from the chase camera in Play. At 0.5 m texels the depression at rest is half a texel wider than the hull (bilinear). wind_test carries a `small_yacht` by the Player Start (2026-11-07, owner: "put the yacht right at the shore"; [52.66, 0, 115.45], yaw -65, bow out to sea, the stern on the 0.9 m depth line): Play, walk to it, E. A 2560x1440 shot looked at whole is shrunk for viewing, and the shrink posterizes the sky into bands that are not in the PNG -- crop at native pixels before chasing banding (the 9:00 / 18:36 sky stepped one level at a time).
- **Measuring it**: a pass restarts the field with `ocean.wakeTexel:0.55` then `0.5` (a texel change clears it) and waits 8 s for the restart's rings to leave; name `ocean.wakeEnabled`, `wakeShore`, `wakeBreaking` in every arm (transient, sticky). The chase camera turns with the hull, so two arms' shots from above are not the same framing: compare what the water does (the lagoon, the band by the sand), not pixels.
- Not there: the shoaling's growth (each texel takes its depth's dispersion, so a wave slows and shortens in the shallows, but the local blend does not carry the energy flux that would make it grow); water under 0.3 m keeps 0.3 m's speed; the readback's wake with `readbackCascades` None (nothing is copied then); a boat outside the field's window round the camera feels no wake.

## A Boat's Spray -- Lit Drops Off the Chines

2026-11-08 (owner: "particles from under the bow, planes with a foam texture"; `docs/boat_spray_plan.md`). `vfx/VesselSpray`: every
boat of the physics world gets four runtime emitters -- a sheet of drops and a mist, port and starboard
(`data/particles/boat_spray_sheet.json`, `boat_spray_mist.json`) -- made at the hand-over the first frame the boat is seen
(`Scene::AddInitializedObject` on an UploadBatch: no editor identity, never saved) and driven at every `Scene::Sync` before the
objects' SyncSceneState; `Scene::Clear` forgets them with the level.

- **Where it leaves**: the forward part of the waterline, 5-35 % of its length aft of the stem, at its outer edge on each side
  (`FindRoot` on the hull's footprint, the wake's raster at the draft), laid across as the frame draws the hull and at the
  height of the water under it (`VesselHull::waterHeight`): the draft line rides 0.5 m over the sea off a crest. A segment
  emitter: a drop is born anywhere on it and a share of the frame ago (where the emitter was then, flown since), so a hull at
  16 m/s lays a sheet, not a clump a frame.
- **How it flies** (the earth's frame: the water a hull pushes aside, not the water it passes): outward `spray.out` (0.4) and up
  `spray.up` (0.2) of the hull's speed, +-15 degrees, 0.6-1.15 x, plus `spray.forward` (0.1) of its way; gravity, a little drag,
  a little wind; a falling drop under that water's height dies (`killBelowY`). None under 3 m/s, whole from 12 (`spray.rate`
  0.7 of what the presets keep alive, ~1400 clumps a second a side), and none off a hull in the air: x smoothstep of
  `VesselHull::immersion` 0.02 -> 0.25 (at full throttle on wind_test's sea p10 0-0.15, p50 0.6-0.8: a tenth of the time
  off a crest). A slam (`VesselState::impact` rising past 0.15 of the
  weight, `VesselHull::impact` from Display) throws a burst, higher and wider, from a hull at rest too (`spray.slam`).
  `get_readouts` `spray` {boats, rate, bursts, sheetAlive / mistAlive and their capacities}.
- **The knobs are the ocean preset's** (2026-11-09, owner: "can I tune the spray?"): `OceanRenderConfig::spray*`, saved with
  the level's ocean like the wake's, the Ocean window and the Inspector's "Boat spray" under "Boat wake", `--set` / MCP
  `spray.<key>` (transient there): enabled, rate (0.7), mist (1), out (0.4), up (0.2), forward (0.1), slam (1), colorR|G|B
  (the drops' albedo, 0.95 grey, TIMES the ocean's `foamTint`: the Inspector's Foam Tint moves the trail and the spray
  together), foamLight (1: lit as the water's foam, below), opacity (1: x the presets' alpha keys, each at most 1),
  forwardScatter (0.25). VesselSpray writes the albedo, the alpha, the forward share and `litFlat` into
  the emitters every Sync; the presets' own rgb, forwardScatter and litFlat are the defaults, unread.
- **What it looks like**: `textures/particles/spray_drops.png` (4x4 clumps of drops and torn filaments; ours,
  `tools/gen_spray_atlas.py` -- no CC0 drop sprite exists, so nothing was downloaded) on quads stretched along the velocity seen
  across the view (`velocityStretch`), and `spray_mist.png` puffs. LIT (`EmitterDesc::lit`, any emitter may): the colour keys
  are an albedo under the frame's directional light -- the water foam's wrapped Lambert (ocean `LitFoamColor`: 0.2 +
  0.8 N.L), and a Henyey-Greenstein forward lobe (`forwardScatter`, `phaseG`: spray against the sun glows) -- plus the
  air: the sky's fill at the normal x (1 + 0.3 (1 - N.y)), as the foam takes it, from the irradiance cube lighting_cs
  fills an opaque surface from (t5, `DrawParams.skyIrradianceScale`, `g_skyIrradianceSrv` / `g_skyIrradianceScale` set
  by the scene renderer a frame), else the height fog's colour, else the level's ambient share of the sun. The normal
  goes from the sphere the sprite stands for (0) to the world's up (1) by `EmitterDesc::litFlat` (JSON `litFlat`); at
  1 the forward lobe adds to the foam's whole diffuse instead of taking its share. Moonlit grey at night.
- **The white water beside the hull is the wake's** (2026-11-10): `spray.sideFoam` and its lines along the hull's sides
  (`VesselSpray::SideFoamOf`, 11-08 / 11-09) are gone -- they only widened the trail. The breaking bow wave's two arms
  (above, "A Boat's Wake": "The bow wave's two arms") start at the spray's root and take their share from the spray's
  own gate (speed 3 -> 12 m/s, the bow's immersion): `VesselSpray::BowWaveOf`.
- Compared with photographs (Bing "speedboat planing bow spray side view"): sheets off the chines, low, fanning outward and aft,
  translucent and streaked. The first cut (round procedural discs 0.18-0.45 m, 4096 a side, up 0.3) was a cotton cloud over
  the cockpit, its mist a white fog bank. Cost (Release_Editor, the chase view, alternated twice): `Pass_ObjectCompute`
  0.197-0.200 ms with, 0.198-0.206 without; `Pass_Transparent` 0.009-0.011 either way. GBV (`ObjectCompute,Transparent,
  OceanWake,Skybox` + the Skybox self-test, wind_test in `--play`; `ObjectCompute,Transparent,Skybox` on e3_particle_test with
  its emitters alive, the spawn shader every emitter shares): clean. And with the spray ALIVE: the Debug editor under GBV
  (`ObjectCompute,Transparent,OceanWake,Skybox` + the Skybox self-test) driven through MCP -- Play, the helm, full throttle,
  40 s at 15-17 m/s, 700-1900 drops and ~250 puffs alive, 23 slam bursts -- only the self-test's id=935 (scratchpad-style
  `gbv_spray.py`; a headless `--play` with `player.testInteract:1` does not take the helm).
- **Checked** (2026-11-08, owner: "are they lit right, not too many, the gravity right?"; scratchpad-style `spray_check.py`,
  `spray_kin.py`, `spray_flight.py`). Counts (`get_readouts` spray `sheetAlive` / `mistAlive` against the capacities, the
  emitters' dead-counter readback): at 14.5-16.8 m/s the sheets 1153-2213 of 4096 (54 % at most), the mists 276-374 of 512 --
  no pool full, no birth dropped. Kinematics: alive / spawn rate is the drops' mean life, and a CPU replica of the shaders
  (speeds, cone, g 9.81, drag, the kill plane) predicts it: `spray.up` 0.1 / 0.2 / 0.4 measured 0.30-0.31 / 0.52-0.58 /
  0.67-0.73 s against 0.27 / 0.50 / 0.69 (no gravity or no kill plane would be the preset's ~0.7 at all three). At 15.5 m/s
  the replica's drops peak at 0.32 m (p50) / 0.71 (p90) / 1.23 (max) and land 2.4 / 3.7 m out. Light, display values of the
  same frame (`box_stats.py`): side-lit spray p90 238.5 against the wake's white water 238.3 and the white hull 240 -- one
  white, as spray and foam should be; against the sun it lifts the water behind it 162 -> 196, over the hull's shaded front
  (165); at 23:00 it adds 9 levels to a 17 background (moonlit).
- **Whiter** (2026-11-09, owner: "the spray is bluish"; scratchpad-style `spray_white.py`, `spray_final.py`). Three things:
  the air was the fog's colour, which carries the atmosphere's HeightFogContribution (wind_test's UDS: 2 by day) and the
  fog's floor -- twice the sky's blue on a white drop; the forward lobe took 0.55 of the sun, so a drop seen from the sun's
  side had less than half of it; the albedo was (0.9, 0.92, 0.95). The core of the sheet (its brightest 30 %, the chase
  camera at 15:00, side-lit both ways): B/R 1.006-1.014 -> 0.972-0.976 (the sun's warm white), +6-8 levels; backlit the
  same. The atmosphere's distant sky light (the fog's own sky term) was tried as the air first: it has no night in it -- at
  23:00 the spray was black smudges on the moonlit water; the irradiance cube has the moon's sky, the night sky and UDS's
  layer. What still reads bluish is the turquoise water through translucent drops: `spray.opacity` (2 = a denser white
  sheet). `--set particles.airFromFog:1` = the fog's colour as before (the A/B, every lit emitter; transient).
- **One white with the trail** (2026-11-09, later; owner at 18:00 from behind the boat: "the trail is white white and
  the particles react to the light much more"; scratchpad-style `spray_match.py`). On his frame the sheets were B/R
  0.78-0.79 and 23 levels darker than the trail (0.87). Mostly the foam tint: wind_test's `foamTint` (0.89, 0.95, 1.0)
  bluishes the trail's white, the spray's albedo was neutral -- with the tint the dense core matches at any light
  model (18:00, his view: 0.84-0.86 against 0.86-0.87). Then the light itself: as spheres a low warm sun lights the
  drops' sides (golden edges as the view turns) and the sea under them takes half their sky, where the trail is a
  level sheet -- so the spray is lit as the foam (`spray.foamLight` 1, 0 = the spheres). What stays: ~7 levels darker
  than the trail at 18:00 (2-4 at 15:00) where the sheet is thin -- the water through translucent drops;
  `spray.opacity` 1.5-2 closes it (the top half of the sheet 160-193 -> 195-221 at 18:00). GBV
  (`ObjectCompute,Transparent,Transparent.Water,OceanWake,Skybox` + the Skybox self-test, the Debug editor on a desktop of
  its own, Play, the helm, 40 s at 14-17 m/s, the spray alive; scratchpad-style `gbv_spray2.py`, which presses the
  script's E until the helm is taken -- a press lands in a frame whether the body is there or not): only id=935.
- **Drops do not cross the hull in a turn** (2026-11-09, owner: "think what to do with the particles that fly through the boat
  in turns"). Born in the earth's frame with the hull's way alone, the drops a turn's inside threw were overrun by the bow
  swinging in and the hull slipping sideways, and the stern skidding out met the drops streaming aft: in a hard turn at
  12-13 m/s 45 % of the thrown drops entered the hull (straight: 4-9 %). Two things, measured by a counter of their own
  (`get_readouts` spray `hullEntries`, a running count of drops that entered a hull, each once; scratchpad-style
  `spray_straight.py`, a steady circle, alternated twice): (1) **the side's push** -- the root moves with the hull, v + w x r
  (`VesselHull::velocity` / `angularVelocity`, from the step's two poses in `ShowVessels`), and the side moving INTO the water
  gives its drops that speed outward (`emitterVelocity` = the root's true velocity too): 45 % -> 6-8.5 %; (2) **collision** --
  `EmitterDesc::collideHulls` (JSON `collideHulls`; VesselSpray sets it), a particle over a "waterMask" hull's PLAN and
  not under its lowest surface there (since 2026-11-10: in its solid, in a cockpit's air, over its deck; until then its SOLID
  columns alone) dies, and its sprite's pixels over the plan are not drawn ("Making a Staged Boat a Vessel") (`particle_update_cs.hlsl`,
  `shaders/water_mask_columns.hlsli`, the frame's hulls from `ocean::g_waterMaskFrame` in the emitter CB, the columns at t0
  of the particle CS's new SRV table), not in its first 0.08 s: the root lies on the skin (the waterline raster, up to a cell
  inside it), and tested at once a fifth of a straight run's drops died where they were born. `--set spray.sidePush:0` /
  `particles.collideHulls:0` = before (transient; the entries are counted either way). GBV (`ObjectCompute,Transparent,
  Transparent.Water,Skybox` + the Skybox self-test, the Debug editor on a desktop of its own, the helm, 40 s, the second half
  hard over, the drops entering): only id=935.
- **Drops fade into the water instead of being cut by it** (2026-11-10, owner: "make the spray soft particles so they do not cut the water"). Every particle already faded over its `softFade` (EmitterDesc, m of view depth) -- but against the OPAQUE depth copy taken before the water drew, so the sea cut a drop's sprite by the hardware depth test: a hard line along the waterline. `Main_Translucent`'s driver now copies the depth WITH the water (Main_Transparent drew it) into `depthWater` (a per-frame render-res depth copy, `DeferredSrvSlot::DepthWater`; its own point before the rebind, emitted every frame, the copy only while the water draws), and the particles read it at t2 (`g_particleDepthWaterSrv`; `depthCopy` stays the opaque depth glass refracts). `particles.softWater:0` = the opaque copy, the A/B (transient). The spray's fade is an ocean-preset knob beside the others: `spray.softFade` (0.4 m; the mist 2.5x it; `spraySoftFade` in the ocean preset, the Ocean window and the Inspector's "Spray Soft Fade"; the presets' own 0.15 / 0.6 are unread). The fade is in VIEW depth: from a low camera the water behind a drop is far, so the soft band is the line where the sprite meets the sea, as it should be. Looked at from low beside the bow at full throttle (scratchpad-style `sprayshot.py`): with the water's depth the sheet's lower edge dissolves into the sea where it ended in a line; 2 m (the control) softer still. Cost: the copy and the frame within noise (GPU frame 2.56-2.57 either way). GBV (`Translucent,Transparent,Transparent.Water,ObjectCompute,Skybox` + the Skybox self-test, the Debug editor on a desktop of its own, Play, the helm, 40 s at 16 m/s): only id=935.
- Engine pieces any emitter may use (ParticleTypes.h; the JSON keys alike): `lineOffset`, `killBelowY`, `collideHulls`, `lit` /
  `forwardScatter` / `phaseG`, `velocityStretch`; runtime only `emitterVelocity` / `baseVelocity` and
  `ParticleEmitterObject::AddBurst`. The old presets draw the same quad (the roll's two axes, the same arithmetic); every spawn
  is now spread over its frame.
- Not there: shadows on the spray (the hull's included), the whisker spray ahead of the root, a turn's outer chine throwing more
  than the inner, spray off a hull at rest in a steep sea other than its slams, drops on the camera.

## A Hull Keeps the Water Out -- mesh.json "waterMask"

2026-11-09 (owner, on a frame where the cockpit stood in the sea: "the water floods the inside of the boat; make a mask for
marked meshes so the water is not drawn inside them"). A mesh asset with `"waterMask": true` (Mesh Editor > Water "Keeps the
water out"; `RenderableObject::SetWaterMask`, read by the factory like every folded asset key) keeps the ocean's surface out of
its cockpits: whatever water poured on the boat would hold before it spilled. small_yacht, police_boat and fast_boat_13 carry it.

- **A LID, rasterized** (2026-11-11, owner: "the water cutting is still so-so, artefacts all the time -- go classic, mask
  rasterization"; on a mask mesh made by hand: "then you are needed at every import -- make a smart heuristic that generates
  it"). Until then the test was the water's own point against the COLUMNS (below) -- the mesh rasterized from above on a 4 cm
  grid, each cell keeping the water out from the skin up to a level --, and every wall the grid crossed was up to a cell wrong
  one way or the other: sea cut outside a single-skinned hull along its waterline (police_boat), or a line of water inside it
  (the 2026-11-10 skin fix moved the error, it could not remove it). Before them a convex hull (the first cut, 2026-11-09)
  bridged every concavity: a wedge from the swim platform up to the deck. The lid takes its walls from the HULL ITSELF and is
  generated from LOD 0 with nothing but the asset (`ocean/WaterMask.cpp BuildField`; baked beside the geometry and read by
  the frame, below "The bake"):
  1. **Basins**: the columns' flood (the rim's openings narrower than `kWaterMaskGap` 1 m shut, a priority flood from outside)
     gives every depression and its level -- where water poured on the boat stands before it spills. Depressions within
     0.15 m of level and 5 cells of each other are one basin at the lower level (a ladder's rungs had cut a cockpit into
     slivers with seams), taken lowest first; a higher one whose water the cap passes under is taken in.
  2. **The cap's height**: the basin's level less 5 mm, and no higher than `kWaterMaskReach` (1 m) over the hull's still
     waterline (the vessel's `draft`; a buoyant object that is no vessel: its buoyancy draft over its lowest point). A foredeck
     in its rails, a roof, a helm under a hardtop never meet the sea, and a cap up there only had more to go wrong (glass,
     frames, rails lying in the plane). A basin whose floor is over the cap gets none (police_boat's two on the roof: `over the
     sea's reach`).
  3. **The outline**, on a 1 cm raster (`kWaterMaskLidSub` 4 pixels a cell side): WALLS are the plane's exact cross-section
     through every triangle (walked through the pixels, Amanatides-Woo); SUPPORT is a pixel some triangle under the plane
     covers (a cap never overhangs the sea where nothing is under it -- the bow rail's did); a smooth field of the cells (+ in
     the basin, + in a basin not capped yet, + where the cell's own top is over the plane) says where the cap may go. A flood
     from the basin's cells under the plane fills it to the walls; it reaches through a door into an enclosed space under a
     deck or a roof (police_boat's wheelhouse), and a reached space that meets open, unwalled air is dropped -- it opens on the
     outside. The basin's own cells and their ring are never dropped.
  4. **The mesh**: marching squares on the pixel centres, a vertex on a wall moved onto the cross-section's segment exactly,
     then `meshopt_simplify` with the border locked (small_yacht's aft cap 41 918 -> 1 023 triangles).
  5. **Curtains**: where the outline crosses an opening instead of a wall (a walkthrough to the swim platform, a notch in the
     rim) a vertical quad hangs from it to the sill -- the floor inside, or the top beyond if that is under the cap and has
     something under it -- less 3 cm.
  6. **Glass**: a see-through slot (`RenderableObjectBase::SeeThroughSlots`: thinTranslucent without an alpha test --
     small_yacht 7, police_boat 13, fast_boat_13 2) is a wall for the flood and the cut, and where it parts the cap's side from
     the outside under the plane (a wall pixel between them) its faces, clipped to the plane, join the lid: a view ray through
     a windshield crosses it as it crosses a curtain. Without them the cockpit's water showed through the glass; with every
     glass face (the first try) the windshield's wings were counted twice and the sea showed there.
  The cap, the curtains and the glass with the hull's opaque walls enclose the basin. The log: one Info line a basin (`water
  mask: <path> -- basin N (cells, level, cap, taken in): segments of the cut, enclosed spaces reached beyond it, open ones
  dropped, pixels under the cap, cap triangles, curtain quads, glass faces`) and one a geometry; Debug lists every depression.
  On wind_test: small_yacht caps at 0.442 m (the aft cockpit) and 1.380 (the helm, at the reach), 3548 triangles; police_boat
  1.710 (the deck and the wheelhouse, at the reach, no curtain) and 1.526 (the bow), 2838; fast_boat_13 0.896 (no curtain) and
  0.912 twice, 4947.
- **The test is the parity of the lid's crossings**, the classic way: `Main_WaterMask` (`--gbv-passes` WaterMask, before
  Main_Transparent; GPU scope `Ocean.WaterMask`) clears `Ocean.WaterMaskCrossings` (R32_UINT, two slices, the render size:
  9.5 MB at DLSS Balanced's 1485 x 835, 28 MB at 2560 x 1440) and draws every hull's lid with no target, no depth test and no
  culling (`shaders/water_mask_lid.hlsl`): each pixel keeps the two NEAREST view depths of the lid by atomics. The surface's
  pixel shader (`ocean_surface_surf_sim.hlsli InsideWaterMask`, t30; its table is 31) counts the ones in front of its own
  point -- odd = inside, discarded. The camera's side starts the count: `watermask::InsideLid` on the CPU (under a cap, over
  its floor, inside its outline) flips it (`WaterMaskCB.count.w` bit 0; bit 1 = lids drawn this frame). The hull's walls are
  never approximated, so nothing is cut outside the hull however its skin leans and nothing is left inside it; the lid's own
  surfaces are see-through, so the parity holds from above, from beside, through the glass and from the helm. Two crossings
  a pixel suffice while a ray meets at most two lid surfaces before the water (a cap and a pane under it); a third, nearer,
  would flip the answer. The lids live in upload buffers (`Ocean.WaterMaskLidVertices` 2^17 float3, `...LidIndices` 2^19),
  each geometry written once, drawn with the matrix the frame draws its object at.
- **The columns stay for the particles** (`physics::ReadSolidTriangles`, a grid over the model's xz, `kWaterMaskCellsAlong`
  (256) cells along its long side -- small_yacht 4 cm, 284 x 88 with the padding): a cell is float4, `.w` the PLAN -- the
  lowest surface over the cell wherever the mesh touches it at all, +big where it does not; what a particle with
  `collideHulls` is tested against at any height and what its sprite's pixels are clipped by ("Making a Staged Boat a
  Vessel": no particle in or over a boat). `OceanRenderable::EnsureWaterMaskColumns`, every frame before the render graph,
  packs the frame's hulls (5 rows each) and the columns' SRV into `ocean::g_waterMaskFrame`; the particles' emitter CB reads
  it whatever `ocean.waterMask` says (Scene::Sync collects the masks always), the surface's b2 for mode 3. `Ocean.WaterMaskColumns`:
  `kWaterMaskMaxCells` (2^18) float4, 4 MB, one a cell; wind_test's three boats take 94 k. The registry: `ocean/WaterMask`,
  no scene walk (the destructor unregisters); `Scene::Sync`, after the physics poses, collects every visible marked object
  with the matrix the frame draws it at (its render offset: the waves, the physics) and its inverse, and hands them to the
  ocean (`OceanRenderable::SetWaterMasks`). The run-up surface reads none of it (as it reads no wake).
- **Knobs and tools**: `--set ocean.waterMask:0` = drawn inside, the A/B; `:2` = the kept-out water painted magenta instead of
  discarded (the lid's volume); `:3` = the mask, and the water still drawn over a hull's plan painted RED -- a wave over the
  deck, or a leak (transient). `test_cube.exe --water-mask-lid=<x.mesh.bin>[,<out.obj>[,<slot;slot>]]` builds one mask
  without a window (the vessel's draft from the mesh.json beside the geometry; the glass slots as given), checks the bake's
  round trip (below) and writes the lid as an OBJ, a group per basin's cap and its curtains (exit 0; 1 = no lid, 2 = the
  geometry unreadable, 3 = the bake does not round-trip; it writes no bake) -- what scratchpad-style `lid_plot.py` (the caps
  over the hull from above) and `glass_plot.py` (a pane's faces in the lid or not) drew to find each step above.
- **The bake** (2026-11-11, owner: "why does the mask take so long?", then "why can't this mesh be generated at import?").
  It was built the first time a boat was seen, ON THE GAME THREAD: Release_Editor 106 / 125 / 317 ms for small_yacht /
  police_boat / fast_boat_13, Debug 1.1-2.7 s, every launch. A field depends on what is set after the import -- the vessel's
  draft, the glass slots -- so it is baked where those are known: `models/<name>/<name>.watermask.bin` beside the
  `.mesh.bin` (gitignored with it; 370 / 457 / 666 KB), holding its KEY -- the `.mesh.bin`'s size and last write, the draft
  (or the buoyancy draft), the glass slots, `kWaterMaskBakeVersion` (WaterMask.h) -- and the field (the plan, the lid, the
  basins with their floors). `watermask::Collect` makes the key from the OBJECT (`SeeThroughSlots`, its vessel / buoyancy) the
  first time it meets one; the bake read when the key matches (0.6 ms Release_Editor, ~1 ms Debug; `water mask: ... read the
  bake`), else the field is built on a worker (`TaskSystem::SubmitDetach`; `the bake does not serve (no bake file | made for
  another draft | made for other glass slots | made from another bake of the geometry | made by version N)`): the hull keeps
  no water out until it lands (100-210 ms Release, 0.8 s Debug), and a build with the editor writes the file (temp + rename:
  two editors never leave half a file; `built on a worker in N ms, baked into ...`). A Release build only reads. So a Mesh
  Editor Save that changes the draft or a material's glass (its placed copies come back with the new key), a re-bake of the
  geometry and the first sight of a new hull rebake it by themselves; nothing is baked at the import (no draft, no glass, no
  `waterMask` yet then). BUMP `kWaterMaskBakeVersion` WITH ANY CHANGE TO WHAT BuildField MAKES, or the frames keep the old
  masks. The registry keeps one field per geometry AND key (two copies share it). The build's own cost went with it, by its
  phases in the log (`water mask: ... in N ms (read, grid, basins, support, lids)` and each basin's `ms: cut, support, flood,
  floors, mesh (simplify)`): the hull was read twice (all slots, then the glass) -- once now; every cap rasterized the whole
  hull for its support (108 ms of fast_boat_13's 317 for three caps) -- one raster for all of them now
  (`SupportLowest`: the lowest triangle over each pixel centre, a cap's support is what lies under its plane); every cap walked
  every triangle again for its floors (42 ms) -- the grid pass keeps every surface over its samples now and a floor is a
  look-up. 107 / 127 / 320 -> 98 / 109 / 201 ms; what is left is the grid (27-68 ms) and meshoptimizer on the caps' raster
  (36-42 ms on the two big caps). The lids against the old build: the same indices in every group, 8 of small_yacht's 3093
  vertices moved by 9 um (curtains' bottoms). Checked: `--water-mask-lid` on the three boats (Release and Debug): the field
  read back from its bytes is the field, a key with another draft is refused; my editor (port 8131) run 1 -- no bakes,
  three workers, three files; run 2 -- three reads; run 3 with police_boat's key poked (draft + 1 cm) and small_yacht's file
  deleted -- `made for another draft` and `no bake file`, both rebuilt and rewritten, fast_boat_13 read; the boats sunk (an
  unsaved edit) and shot from above with `ocean.waterMask:2`: the kept-out water (magenta by hue) of a rebuilt mask and a read
  one differs on 1.2-1.4 % of its pixels, the same mask read twice 1.1 % -- the waves' floor; mode 1 none. A Release run of
  wind_test read all three and wrote nothing; the Debug editor on a desktop of its own built the missing one on a worker in
  790 ms, no assert. No shader or GPU resource changed: no GBV run.
- **Measured** (2026-11-11, wind_test, the editor and Play; scratchpad-style `wm_look.py`, `police_look.py` +
  `magenta_sheet.py`): mode 2 on low views along the waterline of all three boats, at rest, full ahead and in turns -- no
  magenta outside a hull (police_boat's cut along its waterline was the complaint); the boats sunk 1.2 m in the editor (an
  unsaved edit) -- the cockpits dry to their walls, through the open transom and through the glass; the camera inside
  (police_boat at eye heights 1.2 / 1.6 / 2.2 m, fast_boat_13 0.55 / 0.75) -- the sea outside drawn, the inside kept.
  `Ocean.WaterMask` ~0.025 ms; `Ocean.Surface` at police_boat's helm under way, mask 1 / 0: 0.272 / 0.264 ms (the columns'
  five reads a pixel cost 0.379-0.393 / 0.364-0.375). GBV (`WaterMask,Transparent,Transparent.Water,Skybox` + the Skybox
  self-test, Debug, police_boat in the frame): only id=935, `live objects at exit: none`.
- **Not done, and by design**: small_yacht's aft cockpit floor lies at its spill level (0.447 m: its transom is open), so its
  cap keeps out only what is under 0.442 -- a wave over the transom stands in it, as it did with the columns. A wave higher
  than a cockpit's lowest edge stands in it, and water over a deck or a platform is drawn (a bow stuffed into a wave is green
  water over the foredeck). A new hull needs nothing: a basin the heuristic gets wrong shows in mode 2 and in
  `--water-mask-lid`'s OBJ.
- **A kept-out pixel writes no depth** (2026-11-11, owner at police_boat's helm in the editor, cam 44.89 1.57 120.47, 18:00:
  "the water is just in the boat"). The surface was `[earlydepthstencil]` (the shore wetness stamp UAV must not run for a
  hidden pixel), so a discarded pixel still wrote the water's depth, and `fog_apply` -- it fogs the pixels whose depth the
  transparent pass changed (`z != zPrev`) -- fogged the cockpit floor a second time wherever the live sea stood over it: a
  band a few levels lighter with the waves' own wavy edge, crawling with them, on the black floor at 2 m. Frozen water hid
  it (a frozen moment had the sea under the floor); found by sweeping the frozen clock (`wind.freezeAt` 0..60 s) for the most
  magenta (`ocean.waterMask:2`) on the floor, then knob by knob, each arm against the same knob with `ocean.visible:0` inside
  the water's band (scratchpad-style `owner_bisect.py`): 5.95 levels in the band with every knob but `fog.enabled:0` (0.15).
  Now `ocean_surface_surf_sim.hlsli PSMain` has no `[earlydepthstencil]` -- a discard writes no colour, G-buffer or depth --
  and its first line discards a fragment the opaque scene hides (`SceneDepthTexture`, the opaque depth before the water,
  reverse-Z), which is what the stamp needed. The band: 5.95 -> 0.28 levels (the fog-off control 0.14); the owner's view
  live 30 s after the launch, 16 frames: no band. The price is the hardware's early depth test on the hidden water:
  `Ocean.Surface` (alternated twice) cockpit 0.068-0.069 -> 0.081-0.082 ms, grove 0.10-0.12 -> 0.15, shore 0.22-0.26 ->
  0.27, from above the island the same (0.22-0.28 / 0.25-0.26). DLSS and the particles' soft fade read the floor there too
  now. A body-only shader change: no GBV run.
- **The columns' own history** (the surface's test until 2026-11-11): 2026-11-09 a cell kept the water out from its skin to
  its depression's level (the cockpit's air to its gunwale); 2026-11-10 (owner: "in the police boat I see a leak") the skin
  per sample and the top per quarter cell (`kWaterMaskJump`, gone) took police_boat's line of water along its cockpit wall,
  found with mode 3 on 30 held frames; `Ocean.Surface` then 0.379-0.393 ms at its helm. Both GBV runs then: only id=935
  (once the first frame also reported id=1358 / id=1387, see the GBV section's first-frame reports).
## A Race Course and Its Bot -- Buoys, Gates, a Helmsman

2026-11-10 (owner: "design a water course, mark it with buoys (ocean_buoy) and write a bot that runs it; part of it along
the island on wind_test"). `sources/app/game/RaceCourse.h` (what a level says), `RaceTrack.*` (the gates, the line),
`RaceBot.*` (the helmsman), `Race.*` (`game::Race`, the level's race, which the Scene owns: `Scene::RaceRef` -- the next
section), `VesselStats.*` (what a boat does, from its numbers: the section after), `physics::World::SetPilot`.

- **The course is its marks.** A level object with `"raceGate": N` is a mark (a buoy); two marks with one number are a
  gate, a boat passes between them; the gates are run in the order of their numbers and the last leads back to gate 1,
  the start and the finish. No track object: move a buoy and the course moves. A boat with `"raceBot": true` has a bot
  at its helm in Play. Both are read when Play starts -- the editor's from the DOCUMENT (`EditorController::
  FindRaceCourse`: an unsaved move counts), a build without it from the file (`JsonLevel` -> `Scene::LevelRaceCourse`),
  one function for both (`game::ReadRaceKeys`). Inspector > Race: "Race gate" (0 = none; every selected mesh, one undo
  entry -- select a gate's two buoys and number them together), "Bot at the helm" on a vessel. MCP `setRaceGate {gate}`
  (0 takes the meshes out), `setRaceBot {enabled}`, `place` item key `raceGate`. A number with one mark or three is left
  out and named (`race: no course -- gates left out: 4 (3 marks)`); under three gates there is no course.
- **The gates** (`RaceTrack::Build`): the way through one is square to its marks, on the side the next gate lies; its
  marks are then port and starboard of that way (the colours are the level's: wind_test's are red to port, green to
  starboard -- the island's side --, the start gate yellow).
- **The line** (`RaceLine`, one a boat: its beam is in it) is an elastic band through the gates. Where it crosses a gate
  is an offset along the marks, no nearer to one than the hull's half beam + `bot.margin` (6 m). A first guess: the
  offsets that bend the polygon through the crossings least (each has a closed form given its neighbours, swept until
  none moves) and the closed C2 cubic spline through them. That spline is cut into points every ~10 m and relaxed
  (Gauss-Seidel on p[i-2] - 4 p[i-1] + 6 p[i] - 4 p[i+1] + p[i+2] = 0: the least bending), one point a gate held ON its
  gate's line inside its window; the line is the spline through the band, sampled every 2 m of its own length, with its
  curvature. Its speed: `sqrt(a / curvature)`, a = `bot.grip` (0.8) of what THIS boat makes across its way in its own
  tightest turn (`game::StatsOf`: 3.1-4.8 m/s2 for the boats here), between half the boat's top speed and its top speed
  (thrust = 0.77 surgeDrag v^2), and no faster than braking at `bot.braking` (0.35) of what it loses with the engine off
  at its top speed still makes the next slow place (a pass backwards, twice round). The log says it at Play: `race: a line of 1440 m, the boat's top speed 17.1 m/s, ... the
  line's slowest place 12.4 m/s`.
- **The helm** (`RaceBot::Tick`, every frame of a session, on the game's thread: it reads `World::CollectHulls`, the
  frame's poses, and orders through `SetPilot` -- the game's own fields, taken for the steps where the helm's are). Pure
  pursuit: it aims at the line `bot.lookAhead` s of its speed ahead (at least `bot.lookAheadCircle` 0.43 of the radius
  of its boat's own circle -- the yacht's 24 m: at 12 m a slowed boat wove, rudder from side to side; and, where the
  line runs straight, as much further as it is off the line, 80 m at most, so it comes back at a shallow angle instead
  of across the line and out the other side), the bow pointed inside the aim by the angle its boat slides at in the
  bend (`bot.slip`, the section after next); the arc
  through the boat and the aim, tangent to the bow, is a curvature; the rudder is `bot.steerGain` x that over the
  tightest the boat turns at this speed, less `bot.yawDamping` x its turn rate. The throttle holds the line's speed
  (the share that holds it, (v / top)^2, + 0.5 a m/s of error; astern to brake), and is full ahead while the rudder is
  hard over at the line's speed. Held (under 0.5 m/s for 3 s with the throttle on): astern for 4 s, the stern swung so
  the bow comes round. THE MARKS ARE SEEN: one of the next two gates' that the bow would brush (ahead within 2.5 s of
  the speed, nearer its way than the half beam + `bot.margin`) is steered round -- on its gate's side when the boat can
  still get across in the time it has (at what its boat makes across its way), else on whichever side is nearer: a gate missed, not a
  buoy hit. "Always toward the middle" asked a boat coming onto the course from outside, already hard over, for a
  tighter turn than it has (the yacht met gate 4's port buoy head on); "always away from the mark" pushed one a little
  wide of a gate round the outside of its mark. The line keeps clear of the marks, the boat is not always on its line. THE LEAD LINE (`game::Race::Sound`, four times a
  second a bot: three physics rays straight down where the bow will be in ~2.2 s and 40 degrees to either side; each
  waits for a step in flight, `get_readouts` physics `stepWaits`): under 1.5 x the draft + 0.6 m of water ahead -- a
  shoal, a beach, another boat's deck -- the rudder goes hard over toward the deeper side with the engine on; none on
  either side: astern for 3 s (`race` readout `depthAhead`, `shoal`). The line never asks for it; a boat knocked off
  its line does: off a buoy the yacht sat on the east beach till Stop, backing and trying again. A gate counts when the boat crosses its
  line between the marks, a miss when it crosses outside them or runs 30 m past it by the line; nothing counts until it
  has been ON the line (within 12 m and heading along it for 1.5 s: out of its mooring it first crosses the line square
  on), and until then it runs at the speed it keeps in its own tightest turn (`VesselStats::turnSpeed`) -- from
  wind_test's mooring the bows point away from the course, so a boat swings 50-80 m wide of the line, to seaward, and
  comes back to it three or four gates on. The lap clock starts at gate 1.
- **Slower is not safer: these boats steer by their wash.** The rudder's push is (speed + wash x sqrt(throttle))^2, so
  at 18 kn on 0.4 throttle it has 44 % of what it has flat out. Three rules that slowed the boat were tried and every
  run that went wrong went wrong SLOWED: half speed while coming onto the course (the yacht took 50 s to come back and
  brushed a buoy); 4 m/s, astern to get there, while the aim lay 80 degrees off the bow (the swing out of the mooring
  only got wider); and a cap on the top speed while the hull spent over 12 % of the last seconds out of the water
  (`VesselHull::immersion` under 0.1 -- flat out round the course police_boat 2-3 % of the time, small_yacht 9-12 %,
  fast_boat_13 11-23 %; the `race` readout still tells it, `airborne`): capped to 18 kn the yacht drifted 10 m inside
  its line with the rudder over and nothing answering, hit gate 13's inside buoy and missed the turn. All three are
  gone; what is left of "slow" is the bend's own speed and the join's.
- **A boat's turn is its own, and the bot's numbers stand on a measurement** (scratchpad-style `turn_circle.py`: each
  boat's helm, full ahead, hard over, the last 6 s of 14): small_yacht 13.1 deg/s at 12.8 m/s, police_boat 14.7 at 14.0,
  fast_boat_13 14.8 at 14.1 -- a circle of 55 m for all three, 2.9-3.6 m/s2 across; at 0.45 throttle 53-69 m. The
  speed drops by itself in the turn, and EASING THE THROTTLE WIDENS THE CIRCLE: the rudder's push is (speed + wash)^2.
  `SteadyTurnRate` (VesselStats.cpp) is that turn from the vessel's own numbers: the rudder's moment against the yaw drag
  AND the keel -- the hull slides, and the water's push on its side (m v w + the rudder's force), acting `swayLever`
  aft, opposes the turn: yawDrag w^2 + (yawLinear + m v swayLever) w = F (lever - swayLever). Without the keel's term it
  said 26-32 m; with it 12.8 deg/s for the yacht. It scales the rudder's order; the bends' speed is a share of the boat's own
  (`bot.grip`). Tried and dropped, each a build: steering by the way the boat MOVES instead of by its bow
  (to take out the ~4 m it runs wide of the line round a long bend: the hull slides, bow inside its path) -- the path
  answers the rudder a second after the bow does, and the boat wove from gate to gate with the rudder banging from side
  to side, 12 gates missed; easing the throttle when the deck leans past 26 degrees (for the sea) -- a hull banks in
  every turn, and eased it ran 85 m wide; a bend's speed as a share of the steady turn RATE -- it braked inside the
  bends.
- **Play.** A boat with `raceBot` lies with its engine off until a race puts it on the grid (the next section; until
  2026-11-11 it set off three seconds after Play and ran lap after lap). Take its helm (E) and the bot stands down until
  you leave; at ANY helm **B** hands the boat to the same bot and takes it back (`player.testAutopilot` for a script),
  and the boat you hold has its own gates and lap clock whoever steers. The HUD's line under the play line: at a helm,
  outside a race, where the start zone is and the lap's times; in one, the place, the lap, the gate, the misses and what
  they cost. `get_readouts` block `race` (the next section), its `helm` and `bots [{vessel, state, lap, nextGate, passed,
  missed, station, length, speed, target, slowest, tightestRadius, airborne, depthAhead, shoal, crossTrack, lapSeconds,
  lastLap, bestLap, throttle, rudder, position}]`. `--set bot.<key>` (one table, `game::BotKeys`, also Developer
  Controls > Physics "Race bots" with a hint each): enabled, pace (every bot's, on top of its level's), easyPace,
  mediumPace, grip, braking, lookAhead,
  lookAheadCircle, lookAheadMin, steerGain, yawDamping, slip, avoidBoats, margin, debug (the gates -- port red,
  starboard green -- and the bot's line, green where it runs flat out, red where it slows, its aim a yellow mast).
  Game-side like `physics.*`: they survive `reset_settings`.
- **wind_test's course** (the group "Race Track", 42 buoys = 21 gates, 1.56 km through their middles; scratchpad-style
  `track_design.py` writes the gates from a list of centres, checks each post's depth against a polar map of the sea
  floor -- `depth_polar.py`, physics rays straight down -- and the radius through every three gates, and draws the map;
  `track_place.py` places them through MCP): the yellow start gate north of the island by the moored boats, eight gates
  clockwise round the east and south coast 25-30 m off the sand over 3.3-7 m of water (the "along the island" leg: a
  bend of 120-180 m radius), three slalom gates out to sea (13 m to either side every 82 m), a turn to starboard of
  110 m radius -- a gate where it begins, five round it --, three gates of chicane home north of the island. A gate
  where a turn BEGINS is what keeps the line off its inside: with the first gate already 37 degrees round the arc the
  line cut the corner to it at 53 m of radius, and police_boat went through hard over and 16 m wide. The turn was
  85 m at first: it lies open to the swell, the yacht was out of the water a quarter of the time there with the
  rudder hard over and nothing answering, and ran 20-40 m wide of it. THE GATES ARE
  24 m WIDE (the start 30, the far turn's 28): a boat holds its line within some 5 m (it slides ~4 m wide round a long
  bend, a sea moves it more), and through gates of 18 m with a 4.5 m margin the line ran 6 m from the inside marks --
  police_boat and the yacht each brushed gate 5's inside buoy as they came onto the course, were thrown off it and
  missed the next gates; the yacht ended on the south beach. The buoys are `ocean_buoy`'s four colours at scale 2.5 (2.2 m tall), `"buoyant": true` at y -0.3: Static
  floaters, so they rock in the editor too and a boat that hits one stops dead against it (a kinematic body; not
  violent). All four boats by the shore carry the bot.
- **Measured** (2026-11-11, wind_test's 1 m sea, each boat 250 s from its mooring; scratchpad-style `bot_run.py` -- the race's readout
  every 0.25 s, the laps, the misses, the worst cross-track, the path drawn on the depth map -- and `bot_three.py`, the
  three boats in turn): police_boat by its own bot 52 gates in a row and none missed, a lap of 93.1 s (1.53 km of line:
  32 kn mean), never more than 10 m off its line, 26 kn at its slowest; small_yacht on autopilot 45 gates, none missed,
  a lap of 101.2 s, within 11.3 m; fast_boat_13 on autopilot a lap of 91.4 s and 5 of 56 gates missed -- the slalom's
  last and one in the far turn each lap: at 41 kn on this sea it is out of the water 14 % of the time (40 % at the
  worst), rolls to 60 degrees, and has no rudder then; no pace of the bot's cured it (above). Since the gates are 24 m
  wide police_boat has run four times, 184 gates, none missed (laps 89.2-89.3 s on the 85 m turn, 93.1 s on the
  110 m one, which is 90 m longer); the yacht is clean on the 110 m turn in its one run, on the 85 m one it missed
  2-4 gates a run or left the course. A run differs from the next: the sea is not the same twice. What the race costs
  a frame: nothing to see -- the physics step 0.055 ms with the bot racing and with `bot.enabled:0`, 240 fps either
  way, `stepWaits` +0 a second (the soundings met no step in flight), `Scene::TickBuoyancy` 0.025 ms with the 42 buoys
  (alternated twice, scratchpad-style `race_cost.py`).
- **Checked** `intent_regression TestRaceCourseAndBot` (Debug): the editor's key undoes; a gate of three marks is no gate and the
  report names it; a stray mark is left out; the line crosses every gate inside its window; a toy boat (a first-order
  engine, a rudder that swings, a circle of 68 m) runs two laps of a round course from a mooring inside it with no gate
  missed -- and with its helm wired backwards runs none, the control arm; a shoal ahead turns the bot to the deeper
  side, none on either takes it astern. The three configurations build; `check_logging`, `check_render_knobs`,
  `check_gpu_alloc`: 0 findings; a Debug `--play` on a desktop of its own, 19 252 frames with the bot racing at
  16-17 m/s: no assert (Jolt's live in Debug), no Error; a Release `--play` (no editor: the course off the level's
  file) logs `race: 21 gates, 1 boat(s) with a bot` and `a bot at the helm of 'police_boat'`. No shader, pass or GPU
  resource changed: no GBV run.
- What these measurements were (2026-11-10 / 11): ONE boat at a time, on the bot as it stood then -- every boat's
  numbers and the bot's law have moved since (the section after next: the heavy police_boat, the slide's angle).
- Not there: the sea (flat out over every wave). A boat aground backs for 4 s and tries again; in a race the race's
  limits put it back on the course (the next section), outside one it stays there.

## The Race -- a Start Zone, a Grid, Three Laps, the Results

2026-11-11 (owner: "the start zone is lit somehow -- a translucent plane; drive into it and you and three bots are put on
the grid, a start timer runs and off we go; three laps; after the finish a window with the statistics"). `game::Race`
(`sources/app/game/Race.*`, the Scene's: `Scene::RaceRef`), ticked by `PlaySession` before the helm reads its keys.

- **Free -> Countdown -> Racing -> Finished.** Free: the boats lie where they are (a bot's engine is off until a race),
  and the zone before gate 1 is lit -- `game::StartOf(track)`: 48 m long, the gate's width less 4 m, its middle 32 m
  before the line; a translucent green sheet on the water with a fence round it and four white marks, the grid
  (`Race::DebugDraw`, DebugDraw's blended boxes; the editor's selection overlay draws its outline too). A boat with
  somebody at its helm that drives into it -- having been outside it since the last race -- starts one:
  `physics::World::PlaceVessel` (a Join, the pose, no speed) puts it on slot 0 (front row, port) and up to three boats
  that carry `raceBot` on the others, the fastest first (rows 16 m and 40 m before the line, 0.55 of the half width to
  either side); two boats of one asset are "small_yacht" and "small_yacht 2". Countdown: `race.countdown` (3 s), the
  player's helm answers nothing (`HoldsHelm`), the frame says 3, 2, 1, GO! (`Race::BigText`, AppController, 160 px).
  Racing: `race.laps` (3) -- gate 1 crossed once to start the first lap and that many times more. It is the player's
  race: it ends when they finish (the bots alone: when all have). Finished: the results' window (ImGui: place, boat, time,
  gap, each lap, best lap, missed gates and what they cost; Enter or Close) -- closing it frees everybody, and the zone
  waits again once the player's boat has left it. wind_test: four boats carry `raceBot` (the fourth a second
  `small_yacht` at [36.5, -0.38, 124.9]; owner: "no need to import a fourth one, boats may repeat").
- **A bot has one of three levels** (owner: "give the bots three levels of difficulty", "and when you spawn them write
  over each one's head which it has"). A level is a share of the pace the bot can run its boat at: `bot.easyPace` 0.8,
  `bot.mediumPace` 0.9, hard all of it (`RaceBot::SetDifficulty`; the player's own autopilot is hard). `race.difficulty`
  says what the next race's bots get: 1 / 2 / 3 = all easy / medium / hard, 0 (the default) = one of each, which boat has
  which turned by one every race. On the grid and for the race's first 8 s every boat wears who it is (`Race::DrawLabels`,
  ImGui's foreground list, from AppController beside the results): EASY green, MEDIUM yellow, HARD red, YOU white, the
  boat's name under it; the results name each bot's level, and `get_readouts` race `standing[].difficulty`. Measured
  (the four bots, two laps, the second lap): a yacht easy 117.1 s, medium 106.3, hard 99.7-100.9; fast_boat_13 medium
  102.6, hard 98.7 (its pace on the open leg is the sea's more than its level's); nobody missed a gate at any level.
- **A missed gate costs by how far from its middle** (owner: "the penalty for a missed gate must scale with the distance
  to the gate's centre"): `race.missPenalty` (3 s) is a gate missed just outside its mark; the distance from the gate's
  middle where the boat crossed its line, in half widths of the gate, multiplies it -- `RaceBot::Status::missedWidths`,
  1 at a mark, `kMissWidthsMost` (5) at the most: 24 m from the middle of a 24 m gate costs 6 s, a turn cut by a mile
  15 s a gate. "MISSED +3.9 s" on the frame for two seconds, the sum in the HUD's line and in the results.
- **Its own limits** (owner, on a race of the bots alone whose clock stood at 2018 s -- police_boat had left the course
  on lap 3 and sat on the island's beach, backing and trying again for ever: "check the bot was not carried off, why so
  long", "make internal thresholds"). (1) A boat a BOT steers (a bot's, or the player's on autopilot) that has been
  further than `race.resetOff` (60 m) from its line, or held -- under 1 m/s, or going astern off something -- for
  `race.resetSeconds` (8; 0 = never) is put back on its line 40 m before its next gate, at rest, and that costs
  `race.resetPenalty` (10 s): `Race::WatchRacers`, the results say "put back x1 (+10 s)". The player at their own helm
  is never moved. (2) Once the first boat has finished the race runs `race.finishWindow` (60 s) more: who is still out
  then did not finish, the player too. (3) A boat that has finished is ticked no more -- engine off, it went on counting
  gates as it drifted (-443 m off gate 5's middle, in the log). Checked by pinching the thresholds
  (`race.resetOff:15;race.resetSeconds:1.5`): a yacht 17 m off at gate 3 was put back, ran on and finished, +10 s.
- **Scripts**: `--set` / MCP `race.<key>` (one table, `game::RaceKeys`, also Developer Controls > Physics), `race.start:1`
  (a race now: with the player if they hold a helm, the bots alone if not), `race.close:1` (the results away).
  `get_readouts` `race` {phase, countdown, clock, results, armed, autopilot, atHelm, zone {center, heading, halfLength,
  halfWidth}, helm, bots [...], standing [{place, vessel, player, finished, finish, time, missed, penalty, resets,
  progress, laps}]}.
- **What it costs** (2026-11-11, real time, a race of four bots against the same race with `bot.enabled:0`, alternated
  twice; scratchpad-style `bots_cpu.py`; profiler rows `Race::Tick` and, inside it, `Race::Sound`): `Race::Tick` 28 us a
  frame with the bots steering, 26 us without (the gates, the places and the thresholds are most of it: some 6 us a
  boat); a sounding 23 us (three rays), 16 a second for four bots -- 1 us a frame; the physics step 0.066-0.076 ms
  against 0.059-0.061 (four boats under way); 310-319 fps either way; `stepWaits` +0 a second.
- **Checked**: `intent_regression TestRaceCourseAndBot` (the zone before gate 1 and its four slots inside it; a gate
  crossed between its marks is no miss, 1.3 half widths off costs x1.3, 2.6 x2.6); the three configurations build;
  `check_logging`, `check_render_knobs`, `check_gpu_alloc`: 0; a Debug `--play --set=race.laps:1;frame.timeScale:2;
  race.start:1` on a desktop of its own: the bots' race run to its results, no assert, no Error; the whole road as a
  player through MCP (scratchpad-style `race_run.py`: the helm, the zone, the grid, the countdown, three laps on
  autopilot, the results' window photographed, closed, the zone armed again). No shader, pass or GPU resource changed.
- Not there: a start signal's sound, positions shown during the race other than the HUD's line, a reset key for the
  player's own hands, more than four on the grid, a restart from the results.

## What a Boat Does -- Its Card, the Three Roles, a Bot That Knows Its Boat

2026-11-11 (owner: "which characteristics of a boat affect its handling? try to balance the boats -- one heavier but more
powerful, another turns better -- and when you walk up to a boat in Play a table of its characteristics is drawn over
it", then "teach the bot to take the characteristics into account so it drives any boat well"). `game::StatsOf`
(`sources/app/game/VesselStats.*`): one model, out of the vessel's own numbers, for the card and for the bot.

- **What moves the handling** (mesh.json "vessel"): `thrust` against `surgeDrag` is the top speed (thrust = 0.77
  surgeDrag v^2: a planing hull is 0.77 in the water); `mass`, `throttleRate` and the hump (`humpDrag`, `humpSpeed`,
  `planeSpeed`) how fast it gets there; `rudderForce` x (speed + `wash` sqrt(throttle))^2 x sin(`rudderDeg`) is the
  rudder's push -- with `yawDrag` / `yawLinear` and the keel (`swayDrag`, `swayLinear`, `swayLever`: the water's push on
  the sliding hull's side, m v w + the rudder's, opposes the turn) its circle; `rudderRate` how fast the helm answers;
  `mass` against `swayDrag` how far it slides in a bend; `rollStiffness`, `bank`, `maxBank` its lean; `reserveLift`,
  the dampings and `slam` how it takes a sea; the propeller's depth (`propeller` y) whether it keeps its bite -- and
  every force of the hull is times its share in the water, so a hull in the air has no keel and no rudder.
- **The model**: top speed; 0 to 20 kn by integrating the engine's spool and the hump; the tightest steady turn by
  three balances settled together -- the yaw's (`SteadyTurnRate`), the side's (the slide at which the hull's drag is
  m v w + the rudder's force) and the way's (the slide takes m s w off the thrust, so the boat slows by itself) --
  giving the circle, the speed kept in it and the m/s2 across; `SlipAngle` (the angle between bow and way in a bend of
  a curvature at a speed). `physics::VesselHull::rudderLever` (the propeller aft of the centre of mass) is the
  engine's. Against the boats themselves (scratchpad-style `boat_measure.py`, `turn_circle.py`): to 20 kn 4.3 s
  measured / 4.1 the card (small_yacht), 3.3 / 3.2 (fast_boat_13); the circle's radius 53.7 / 56.0 m, 30.4 / 30.1;
  the heavy boat's 67.0 / 73.2 with its first numbers. `vessels[].stats` in `get_readouts`; scratchpad-style
  `vessel_model.py` is the same model in Python, for trying numbers before they go into a mesh.json.
- **The three roles** (written into the two mesh.json; small_yacht is as it was):

  | | weight | thrust | top | 20 kn in | circle | draft | in a sea |
  |---|---|---|---|---|---|---|---|
  | small_yacht, the all-rounder | 5.5 t | 22 kN | 31 kn | 4.1 s | 112 m | 0.38 m | out of the water 17-21 % of the open leg |
  | police_boat, heavy and powerful | 7.5 t | 25.5 kN | 32 kn | 5.6 s | 128 m | 0.71 m | 1-8 %: it ploughs through |
  | fast_boat_13, light and agile | 1.9 t | 9 kN | 32 kn | 3.1 s | 61 m | 0.39 m | 17-21 %, and it is thrown about (below) |

  police_boat: mass 6500 -> 7500, thrust 27000 -> 25500, rudderForce 135 -> 125, throttleRate 0.45 (it had the
  default 1), humpDrag 7000 -> 10000, its trims, roll stiffness and slam scaled with the mass. fast_boat_13: surgeDrag
  26 -> 42 (it was a 41 kn boat that left the water), rudderForce 40 -> 60, throttleRate 1.6, and its propeller 0.2 m
  deeper (y 0.08 -> -0.12) -- it planed with half of it out
  (`bite` 0.59: half its thrust and rudder; 0.87-0.92 now). A lap of wind_test's course by their bots (two races of the
  four at their hardest, the second lap): police_boat 95.2 s, fast_boat_13 98.7, the yachts 99.7-100.9 -- within 6 %.
  THE LIGHT BOAT'S MINUS IS THE SEA, AND IT IS A REAL ONE (owner: "how is the lightest boat balanced if it is fast and
  agile?" -- on its card it beat the yacht in every row). Given 30 kn for that (surgeDrag 49: the slowest on a straight)
  he raced it and lost to every bot: "give it its speed back, it really gets thrown about by the waves". 32.4 kn again.
  Measured, a lap each alone (scratchpad-style `sea_roll.py` over `turn_trace.py`'s rows), the roll on the open leg:
  fast_boat_13 sd 14.9 degrees, 50 at the most, past 15 degrees 32 % of the time (near the island 12.3 / 31 / 24 %);
  small_yacht 8.9 / 34 / 6 %; police_boat 9.1 / 31 / 8 %. A hand at its helm pays for that; a bot does not, much.
- **The card** (`PlaySession::DrawBoatCard`, ImGui, takes no mouse and no key): on foot in Play, over every boat within
  16 m of its hull that is in the frame -- its name, length, weight and thrust, and four bars on one scale for every
  boat, longer = better (top speed, acceleration "20 kn in 5.6 s", handling "a 128 m circle", sea keeping "7.5 t, 0.71 m
  deep"), "E take the helm" on the one E would take. Sea keeping is half the DRAFT (`VesselStats::draft`, of 0.8 m) and
  half the WEIGHT (of 8 t): a hull is out of the water -- no keel, no rudder -- when a wave has dropped away by about its
  draft (the share of the open leg out of the water follows it: 0.71 m 1-11 %, 0.38 and 0.39 m 17-22 % each), and a wave
  throws a light hull further (the roll above). By the draft alone the light boat's bar was the yacht's, which its helm
  does not bear out; the weight's own bar is gone (heavier is not better; the weight stands in the card's first line). The first cut looked for the nearest boat alone -- the one behind the player's shoulder -- and drew
  nothing; white text on a yellow bar could not be read (the value stands beside its bar).
- **The bot counts on ITS boat** (`RaceBot::EnsureLine` takes `StatsOf` of the hull it holds): a bend's speed is
  `bot.grip` (0.8) of what the boat makes across its way in its own tightest turn, braking `bot.braking` (0.35) of what
  it loses with the engine off at its top speed, the rudder's order a share of the tightest it turns at this speed, the
  join's speed its own turn's, the reach at least `bot.lookAheadCircle` (0.43) of its circle's radius. It reads them
  every frame and rebuilds its line's speeds the frame any of them moves (the key: grip, braking, the top speed, the m/s2
  across, the braking, the circle -- the mass alone moves the braking and not the top speed).
- **A placed boat may race with numbers of its own** (2026-11-11, owner: "make the boats' racing characteristics
  tunable from the Inspector and check the race bot takes them into account"). Level object `"vessel": {"thrust":
  33000}`: `SceneObjectFactory::ResolveMeshAsset` lays it KEY BY KEY over the asset's mesh.json "vessel" (any other
  object key still replaces the asset's whole), so the copy says only what it does differently. Inspector > Boat (on a
  copy of a vessel asset): the card -- `StatsOf`, this boat against its asset, following a drag before it is let go --
  and the numbers by group (Engine, Hull and planing, Helm, Turning, In a sea; `physics::VesselGroupPerCopy`), each with
  its hint; its own in accent with a button that puts the asset's back, "All back to the asset's". How it floats
  (draft: the water mask is baked for it; centre of mass), the anchor, the spray and the cameras stay the asset's (Mesh
  Editor > Vessel, which heads its rows by the same groups and shows the hints: `physics::VesselKeys`, one table).
  `SetVesselTuneCommand`: every selected boat, one undo entry, a key written only while it differs from the asset's (a
  copy put back saves byte-identical), clamped to the key's range. MCP / command bar `run_action setVessel {key, value}`
  ({key, reset: true}, {reset: true} = every one); `get_readouts` vessels[] carry `geometry` now. The running copy takes
  the numbers at once: `RenderableObject::SetVesselSettings` moves `GetBodyRevision` (was the collision's alone) and the
  physics world rebuilds the body from them -- its entry kept a copy made at the build, so before this a change reached
  nothing until a respawn. The Physics row's "Mass (kg)" is gone from a boat: its body weighs `vessel.mass`
  (`SetVesselBody`), the row moved nothing. Gate `intent_regression TestRaceCourseAndBot`: the same bot on a 60 m circle
  -- twice the rudder force the next frame: circle r 52.5 -> 30.8 m and faster bends; half as much thrust again: the top
  speed x1.22; its numbers back: the line it had; half as heavy again: the same top speed, slower bends --; setVessel
  writes one key, the rest from the mesh.json, the asset's number takes the key away, draft is refused, a number past its
  range is clamped and says so, undo leaves the level as it was (the bot's check made to fail: it does). Live (wind_test,
  the four bots, one lap at `frame.timeScale` 4, through MCP; scratchpad-style `vessel_tune_live.py`): police_boat as its
  asset 102.7 s, its bot's line slowest 14.2 m/s, circle r 64 m; with thrust 33000 and rudderForce 200 of its own (the
  card 32.3 -> 36.7 kn, 20 kn in 5.6 -> 4.2 s, a circle of 128 -> 93 m) the bot's line 17.2 m/s slowest, r 46 m, the lap
  92.4 s and first; undone 102.0 s (the control); the mass 9500 alone: 20 kn in 6.8 s, r 70 m, braking 3.40 -> 2.68, the
  same top speed, the body rebuilt at 9500 kg (`physics: ... is a boat` in the log) and at 7500 after undo. No miss in any.
- **It knows how its boat slides** (`bot.slip` 1 = whole, 0 = before). The helm pointed the BOW at its aim, and a hull
  in a turn goes bow inside its way, so the boat ran outside its line in every bend by its reach times that angle:
  police_boat (11 degrees in the coast's 140 m bend at 22 kn) 7-9 m, at any speed -- onto the outer marks; a hit spun it
  and the next gates went too. Now the bow is pointed inside the aim by `SlipAngle` of the bend between here and the aim
  -- from the LINE's curvature, not from how the boat moves (that lags: steering by the velocity wove). police_boat
  alone, two laps (scratchpad-style `turn_trace.py`, the mean distance off its line by leg): the slalom 6-9 m -> 2.0,
  the far turn 10-78 m -> 1.6 (worst 3.6), the leg home 1.9.
- **It brakes on the straight, not in the bend**: with the rudder asked for much (over 0.25 .. 0.6 of it) the engine is
  not eased under what holds the line's speed, let alone put astern -- the rudder works by the propeller's wash, and
  astern at 23 kn took four fifths of its push: police_boat, hard over, went on out of the far turn. And the reach grows
  with the distance off the line only where the line runs straight (and while coming onto the course): in a bend a far
  aim asks for LESS rudder than the bend does -- 30-50 m outside the far turn it held 0.4-0.6 of its rudder.
- **Another boat is steered round only when the bow would reach its stern within two seconds** at the speed the two
  close at (`bot.avoidBoats` 0 = not at all). It was "ahead within 2.5 s of MY speed and closing at 0.5 m/s": four
  equal boats on one line threw their rudders over at sterns 40 m off all the way round.
- **What did NOT work, and the owner's word on it.** With the heavy boat missing gates in the far turn every lap the
  slalom was mirrored and the turn widened from 110 to 150 m: fewer misses, not none -- "why are you making it easy for
  the bots? can't they be taught to brake and do some cleverer analysis?" Both went back: the course is the committed
  one. Slowing does not cure a boat that slides off its line: at 0.8 of the pace the same gates were missed, and the
  hull is out of the water 20 % of the open leg at 24 kn as at 31 (11 % at 17 kn; police_boat 1-8 % at any) -- the
  water's forces go as the speed squared, so the path through a change of turn is the same at any speed, a length of
  the boat's own. What was wrong was the aim, not the speed.
- **Measured** (2026-11-11, wind_test's 1 m sea, the committed course, `frame.timeScale` 4; scratchpad-style
  `race_watch.py` -- every miss with its gate and what the boat was doing, each bot's worst distance off its line, its
  share of the time out of the water near the island and on the open leg, the paths on the map): a race of the player's
  police_boat on autopilot against three bots, three laps: no gate missed by any of the four, laps 103.1 / 96.5 / 95.1,
  the yacht 105.7 / 97.7, fast_boat_13 106.9 / 98.6. Races of the four bots alone: police_boat 95.3 a lap and none
  missed; fast_boat_13 100-103, none; the yachts 100-102. Before the slide's angle: 4-13 gates a race, police_boat
  30-78 m outside the far turn every lap. What is left: in three races of four ONE boat of the first lap's pack lost
  1-4 gates at the far turn's entry (a yacht each time: gate 13, 26 kn -> 13 in a second -- it met a boat or a buoy and
  was spun), and once police_boat left the course on lap 3 and beached (the limits put it back now).
- Not there: a lane of its own for each boat (they all want one line: the pack rubs on lap 1), the sea in the bot's
  speed (it cannot buy grip with it; it could pick a line that needs less), a leg of the course tight enough to pay
  for the light boat's circle, a bot that suffers from its boat's roll as a hand does.

## The World's Clock Runs Faster -- frame.timeScale

2026-11-11 (owner, on six-minute test races: "can you speed the whole simulation of the world and the bots up?").
`--set` / MCP `frame.timeScale:<x>` (0.1-16, 1 = real time; App.cpp `g_timeScale`): the frame's dt is multiplied, and
everything of the world goes by that dt -- the ocean's and the wind's clock, the physics' steps, the boats' water, the
bots, the race's clock, the time of day -- so a three-lap race of five minutes takes 80 s at 4. Capped at 0.1 s as a
frame's dt always is: a frame too slow for the scale slows the world, it does not jump it. The one reader of the wall
clock in the world was the cap on the copies of the water's heights (`OceanReadback`, 30 a second): it follows
(`OceanReadback::SetTimeScale`), or a boat at x8 would get 4 heights a second of ITS time.

- **What a scaled run is**: the game at (frame rate / scale) frames a second. At 240-300 fps and 4 the physics steps
  once a frame and the heights land 30 times a second of the world, as at 60-75 fps; past frame rate / 15 the physics
  drops steps (`physics.maxSteps` 4 a frame at 60 Hz). 4 is what the measurements here used.
- **The same answers**: the four bots' race at 1 and at 4 -- fast_boat_13's laps 109.5 / 102.0 / 103.6 against 108.7 /
  103.3 / 102.9, the yacht's 108.6 / 101.9 / 103.4 against 112.9 / 102.7 / 103.2, police_boat's 95.3 against 96.1, the
  misses at the same gates.
- **It is sticky and nothing resets it** (a `frame.*` switch: `reset_settings` leaves it): a script sets it back to 1
  in the call that ends its run. The HUD's `Time:` line says `WORLD x4 (frame.timeScale)` while it is not 1, and
  `get_readouts` carries `timeScale`. A script that times with its own clock (a sleep, `time.time()`) is wrong by the
  scale: read the race's clock, a lap's seconds, the readouts' own times. A `screenshot` stalls the frame as it does at
  1 -- the world goes on by at most 0.1 s a frame.

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

## Physics -- Jolt, Behind One Folder

`third_party/JoltPhysics` is Jolt 5.6.0 (`Jolt/` and `LICENSE` of the tag), built by `third_party/third_party_physics.vcxproj` from `Jolt.cmake`'s own list. `#include <Jolt/...>` only in
`sources/physics/*.cpp`: those files take the library's defines and `/arch:AVX2` per file and no PCH (`test_cube.vcxproj`, `third_party/jolt_defines.props`; the defines decide the ABI, and
`RegisterTypes` aborts on a mismatch). Everything else talks to `physics/PhysicsWorld.h`. Plan and numbers: `docs/physics_plan.md` section 9.

- **The pose is a render offset.** A body moves its object through `RenderableObject::SetRenderOffset`, at the hand-over (`World::SyncPoses`; in `Scene::Tick` on a frame with no step or under `physics.async:0`), so the authored transform, the document and a Save
  never see it; only Keep writes it. Numbers cross as they are (position and quaternion, never a matrix), no change of handedness: the gate `pose` holds it.
- **What does not move does not pay.** Objects arrive through a registry (`SetPhysicsMotion`, the transform setters and `SetVisible` queue an event; nothing walks the scene), and without an awake
  body or a moving boat `PhysicsSystem::Update` is not called: `Scene::TickPhysics` 0.001 ms.
- **The step runs in the frame's idle window** (`physics.async`, default 1; 2026-11-04, owner, on a trace where the game thread stood in `Scene::TickPhysics` and then idled in `Renderer::WaitForFrame`, and on the next, where the step lay over the render frame: "a huge idle of many cores, and we cram everything into one stretch"). `World::Tick` counts the steps owed and reads the water; `World::KickStep`, from the game loop the moment `Game: Wait Render Thread` ends, starts them on a thread of their own (row `Physics Step`, Jolt's pool as before), after what went since the Tick (`loadPendingLevel` runs in between, which is also why `ocean` is not touched past the Tick); `World::SyncPoses`, at the top of `Scene::Sync`, takes the poses. A step done by then gives the very bytes the step in line gives, every frame: no lag (gate `async`). One that is not leaves the object where it stood for a frame and the frame does not wait; its time waits, past `physics.maxSteps` it is dropped, as in line. While a step runs its thread owns the system, the entries' poses and `awake`, and never touches an object (one may be gone): anything else that reaches the world calls `Impl::Join` first (a ray, the player, the debug lines, an object added, moved or gone, Keep; a trace shows it as `Physics.Wait Step`, `get_readouts` physics `stepWaits`, and `handOversHeld` counts the frames shown late). It needs the render thread: on one thread there is no wait to run in, and `frame.renderThread:0` steps in line. 3000 coconuts landing (Release, the frame GPU-bound, alternated twice): the render frame on a step's frames 2.14 / 1.89 -> 1.82 / 1.45, the same as its other frames, `Whole Cycle` p95 2.91 / 2.36 -> 2.79 / 1.99, the step itself 1.39 / 1.32 -> 1.27 / 1.21, done 2.0-2.2 ms before the hand-over, late 0-1 of ~185; on the game thread `Scene::TickPhysics` p95 1.81 (in line) -> 0.15. `physics.stepDelayMs:8` (a heavy world without building one, a 9.6 ms step): in line 204 of 599 frames over 1.5x the median, p95 10.81 ms; here 0-2, p95 ~6.3, every step a frame late. The price: a frame with no wait to run in (CPU-bound) has the step miss its hand-over, a frame late. A profiler scope on a thread of its own that crosses a frame is dropped unless the thread calls `Profiler::MarkThreadCrossFrame`; such a scope is kept between two frames too. `--set physics.async:0` = in line, `physics.kickInTick:1` = started in Tick, beside the render frame: the A/Bs.
- **The world's limits are a heap's, not Jolt's sample's** (2026-11-04, perf review). `kMaxContactConstraints` 10240 ran out at 4000-5000 bodies in a heap, and what does not fit does not collide: 5271 of 10000 fell through the island, the step looking cheap all the while. 65536 now, and the step takes the whole table out of the temp allocator (~1 KB a contact, touched as used): `TempAllocatorImplWithMallocFallback`, 128 MB reserved, because the plain one aborts past its end. Jolt's own count 30.9 -> 163.8 MB; `mi commit` 359 -> 361-380 MB, private +20-50 MB. Heaps: 5000 a 3.7 ms step, 10000 11.5 ms and the frame still 5.5 ms, nothing lost. Past the limits again (body pairs from ~30000): `[ERROR]`, `get_readouts` physics `stepErrors`, red in Developer Controls. Past `kMaxBodies` the statics are built first, so the bodies go and not the ground.
- **The same level gives the same world.** `dirty` and `entries` walk by the objects' addresses, and Jolt's answer depends on the order bodies come and wake in: the same scene differed from run to run within 100 frames (the control in gate `async`). `BuildDirty` sorts by the bytes of the position, statics first; `WakeAllDynamic` and `FlushRemovals` go by body id.
- **A boat's `MoveKinematic` is a speed, and Jolt keeps it.** Told once and stepped N times it goes N times as far: under 20 fps the boats left the world for good (and Jolt asserted in Debug), and with the ocean hidden the collider flew off from a boat that stood still. Every step with nothing new now sends it where it already is (`kinematicMoving`, `kinematicTargets` for a batch), gate `world`.
- **A shape's key holds its file's time**, so a re-bake is another shape (a reload used to take the old one back out of the cache); a `custom` shape at a scale Jolt cannot apply exactly (`IsValidScale`: mirrored, stretched against a turned primitive) is built at that scale (`BakeScale`, one shape per scale), where Jolt's nearest scale laid a mirrored palm on its side.
- **Keys.** Level object `"physics": "none" | "static" | "dynamic"` (absent = static, UE's BlockAll) and `"mass"`. mesh.json / object `"collision"`: `shape` (auto | mesh | hull | box | sphere |
  capsule | custom | none), `shapes` (custom's list: `{type: box | sphere | capsule | cylinder, center, rotationDeg, halfExtents | radius, halfHeight}`, UE's simple collision), `rollingResistance`, `lod`, `excludeSlots` (absent = the slots with `windFoliage` > 0: a leaf does not collide), `density`,
  `mass`, `friction`, `restitution`, `angularDamping`, `float`, `convexRadius`, `twoSided`. `"buoyant"` without dynamic = a kinematic body following `OceanBuoyancy`; with dynamic = Jolt's impulse
  over the surface `OceanReadback` gives (`OceanBuoyancy` leaves the offset to the world and keeps the copies coming).
- **`shape: auto`, the rule** (`BuildAssetShape`, one road for the world, the preview and the gates): dynamic = a hull; static under 1 m = a hull; static with leaves = the triangles of LOD 2; the rest
  (the island, walls, boats) = LOD 0. The trunk stands on LOD 2 (gate `autoshape`: 0 % of rays through LOD 0's trunk miss, median <= 1.6 cm); the crown's small wood an asset's `lod2DropSlots` drop is not there
  for a falling body. `physics.autoShapes:0` / `physics.forceLod:<n>` = the A/Bs. **What a heap pays for is the falling body's own hull**: Jolt's 1 mm tolerance gives a round thing its cap of 256 points (508
  faces), so `physics.hullTolerance` (0.04 of the mesh's smallest size, 1 mm at least; 0 = Jolt's) -- 2000 coconuts landing 7.1 -> 3.4 ms a step, 9 mm off a coconut. The rule alone moved 3 %, memory 1 MB, the build
  nothing (the island is most of the triangles: 36 of ~45 ms). Tables: `docs/physics_plan.md` 9.5, 9.6.
- **An asset's `collision` block comes from `tools/collision_fit.py`** (`--write`; the cheapest shape that stays on its surface, both ways, p95): the coconut a sphere (the owner's pick; 2000 landing 0.87 ms a step on
  8 threads, a best-fit capsule +8-10 % and no closer, the 0.04 hull +57 %), palm trunks and sticks cylinders, plain walls, floors and pictures boxes, rocks and coral hulls; triangles stay on the island, the boats, the
  tent, the bath, the sinks and walls with openings. A block already there is left alone.
- **Rolling resistance is ours** (`ApplyRollingResistance`, `collision.rollingResistance`, `physics.rollingResistance` 0.2, the coconut 0.4; k = the sine of the slope it still rests on; gate `rolling`): Jolt has none, and a
  sphere did not sleep: 2930 of 3000 awake after 20 s, hundreds rolled off the beach into the lagoon; angular damping cannot cure it (40/s: 772 of 2000 awake after 30 s). "Touching" = the vertical speed moved by other than
  g dt: a `ContactListener` would make Jolt rebuild every cached contact's manifold.
- **Every dynamic body is `LinearCast`**: a triangle mesh has no thickness, and a coconut dropped 14 m passes a triangle within one step. `physics.killY` (-200, UE's KillZ) puts to sleep what still
  leaves the world, and Keep skips it.
- **It simulates only in Play** (above). Level Editor > Physics > Show Collision; Developer Controls > Physics. MCP: `play`, `stop`, `run_action setPhysics {motion, mass}`, `keepSimulation`, `place` with `physics` / `mass` / `buoyant` per item, `physics_raycast`, `get_readouts` block `physics`.
  Knobs `physics.*` (enabled, async, kickInTick, stepDelayMs, debug, debugRadius, stepHz, maxSteps, threads (8), gravity, autoShapes, forceLod, hullTolerance, rollingResistance, angularDamping, killY) and `player.*` are game-side
  and survive `reset_settings`: name them in every arm.
- **A box lies along the mesh** (`FitSolidBox`, 2026-11-05, owner: "the box should follow the door itself, not an AABB"): Shape: Box and the Collision tab's Fit box take the smallest rectangle round the mesh on the ground, about the vertical. A node split out of a scene keeps its layout's turn in its vertices (the_bathroom_free's door stands 42.7 degrees off its axes: bounds 0.68 x 0.62 m, the leaf 0.87 x 0.07), so the axes' bounds are a block round it. What lies along its axes keeps them exactly (a rectangle under 0.1 % smaller does not turn it). It takes in everything the mesh has, handles too: the door's own block in its manifest is the leaf alone, a custom box.
- **A surface marked as a leaf has no collision, and bakes as a leaf** (2026-11-05, owner at the bathroom's door): 33 assets of the_bathroom_free still said `windFoliage` > 0 from before the leaf-card measure (`defaultLit`, no `windStrength`, so nothing swayed): "a leaf does not collide" left doors, curtains, carpets, towels, hooks and the window walls' first slot out of the world, and the LOD3 prune had shredded and grown the doors' and curtains' cards (door 218 tris at a recorded error of 0, curtain 1118). The key is gone and they are re-baked from their manifests (the old files: `D:\Programming\test_cube_backups\mesh_bin_v5_bathroom_wind_2026-11-05`): LOD0 the same vertices and indices, LOD3 56 / 442-458 tris at an honest 0.05 / 0.30-0.35 m, the picture at the floor (the bathroom 0.001 % of pixels over 8 levels, floor 0.028 %; from above 0.129 %, floor 0.124 %). A flag that means "a leaf" anywhere must be true of the asset: every real one is `twoSidedFoliage` with a `windStrength`.
- **A kinematic body is given its mass** (`GiveKinematicMass`, gate `kinematic`; 2026-11-04). A `buoyant` object that is not dynamic gets the triangles (`Auto`), a MeshShape has no mass, and Jolt asks for one for
  every body that is not static: an assert in Debug (`inMassProperties.mMass > 0.0f`, wind_test did not boot) and 1/0 in Release, unseen because the solver reads that mass only for Dynamic. Jolt's asserts live in Debug only:
  a level checked in Release has not been through them.
- **Gates.** `test_cube.exe --physics-gate[=<name>]`, no window (smoke, pose, ray, foliage, determinism, scale, walk, player, water, vessel, kinematic, autoshape, custom, rolling, async, world; each has an arm that must come out wrong), `physics gate: <name> PASS|FAIL`
  in the session log, exit code = the failed ones; `intent_regression` runs them too (`TestPhysicsRoad`). The gate projects link the engine's objects from its tlog, so a new library goes into their
  `AdditionalDependencies` by name.
- **Mesh Editor > Collision** (`editor/ui/CollisionEditorTab`, `--edit-mesh=<mesh.json>#collision`): the shape's settings and result; its edges are drawn in the preview's own pass against the mesh's depth (`shaders/editor_preview_lines.hlsl`, screen-space quads; `EditorPreviewRenderer::PreviewLines`, re-rendered when `LinesRevision` moves): in full in front of the mesh, at 0.28 alpha behind it, because a shape lies mostly on or inside its mesh and hiding what is behind would hide most of it (owner, 2026-11-04); Custom = + Box / Sphere / Capsule / Cylinder, Fit (all solid
  slots or one), parts list, a gizmo Move / Rotate / Size. A gizmo over an ImGui button never activates (`ImGuizmo::CanActivate`), so while it wants the mouse the preview's button is a Dummy and the window takes
  NoMove. `inspector_regression TestCollisionTab` clicks and drags it with real ImGui events.
- **Jolt's step and jobs reach a `--trace`** (`JPH_EXTERNAL_PROFILE`, `JoltRuntime.cpp`; rows `Physics 0..7`): one atomic read a zone otherwise. Only those (`IsCoarseZone`; `kTraceEveryZone` brings back the thousands per contact,
  pair and island: a 300-frame heap 480 MB, its step inflated 1.5x; 600 frames are 21.5 MB now). A zone's key is a LITERAL one: a dynamic key returns to the profiler's free list with its capture. Unnamed broad-phase layers read
  "Undefined": `SetBroadPhaseLayerName`.
- **Why a body stays awake**: the Debug line `physics: N dynamic bodies awake, speed median .. max .. m/s, spin max ..` every 2 s. 180 m/s and no spin = it fell through the world.
- **A caster count past the level's used to lose whole VSM pages** (`VSM.PageVisibleList`, its capacity remembered in elements and compared with casters; fixed 2026-11-04). Lit rectangles in the
  shadows, with `clustersLost` / `runsLost` / `refused` all 0: UAV writes past a buffer's end are dropped silently, so a counter behind them sees nothing.

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

## Rest Pose — How a New Copy Lies

mesh.json `"restRotationDeg": [pitch, yaw, roll]` (the level's rotationDeg order) is how a NEW copy of
the asset lies; absent = as authored, upright. Every creation path applies it through one call,
`restpose::ApplyToNewObject` (`editor/assets/MeshRestPose.*`): the object factory (Content Browser,
viewport drop and its drag ghost), `spawn` (the pose turned by its random yaw) and `place` without a
`rotationDeg` (turned by `yawDeg`). The pivot is raised or lowered so the posed geometry's lowest point
sits at the ground height -- the lift is measured from the .mesh.bin at spawn, never stored, so a rebake
cannot leave it stale. Placed copies keep their own rotation. Mesh Editor > Rest pose > "Lay flat
(auto)" writes the automatic pose (thinnest surface axis up, a cupped shape rim-down);
`intent_regression --rest-pose <mesh.json>...` prints the same numbers headless. The seashells carry
one: imported, they stood on their hinges 37-56 degrees off flat.

mesh.json `"pivot": "base"` moves the model origin to the mesh's footing AT BAKE TIME (lowest point,
centred on the bottom 5 % of its height; `MeshLoadOptions::pivotToBase`, logged as `[meshbake] pivot to
base: origin moved by (...)`). A node split out of a multi-object glTF otherwise keeps that file's
layout offset -- fern_02 baked with the pivot 1.0-1.4 m beside the plant, so wind bent it about a
point beside it. New split imports get it by default; an existing asset keeps what its manifest says,
because switching it on moves the geometry under every placed copy: re-place them by
`T' = T + R (s * offset)` with the logged offset. Every node-split asset carries it since 2026-09-24
(fern_02, lowpoly_sticks, rocks_node, seashells, beach_grass), and wind_test's copies were re-placed
that way; for a rock or a stick the footing is its contact patch, up to 0.3 m off its box centre.

## Import — Where a Mesh Asset Lives, and Leaf Card or Surface

A mesh asset is `models/<name>/<name>.mesh.json`, beside its `.mesh.bin` and textures; a split import
writes `models/<name>/<name>_node_<part>.mesh.json`. Until 2026-10-23 they lay flat in `models/` (a
hundred-node pack spilled a hundred files between every other asset); all 148 were moved and every
level re-pointed then, and nothing resolves the old flat path -- a stale one reports `mesh asset not
found`. A mesh.json inside an import folder stands for the WHOLE import (registry status, Content
Browser Reimport), like a material preset: it is not a copied output, and the per-resource reimport
would have read it as an orphan and deleted it.

Each glTF material is read as a LEAF CARD (twoSidedFoliage shading, its slot marked wind foliage, an
auto windStrength) or a SURFACE (defaultLit, rigid). Leaf = a two-sided alpha cutout whose cutout
removes at least 20 % of its surface (`materialgen::ClassifyLeafCards`: the albedo alpha sampled at
area-weighted points over the material's triangles). The flags alone said leaf for anything Sketchfab
exported -- it marks every material doubleSided and anything with a hint of opacity BLEND -- and
the_bathroom_free's curtains, doors, towels and carpets swayed in the wind indoors. Measured: palm
fronds, fern_02, ground/young palm 35-68 %; that bathroom's Door_and_windows 2.2 %, Fabric 0.9 %. A
single-sided cutout stays a surface (curly_palm's frond boot is bark), which misses date_palm's
single-sided fronds -- that is what the choice is for. The Import window's Materials list shows each
material's verdict and measurement and takes a per-material choice (Auto / Leaf card / Surface); MCP
`import_asset {surfaces: {"<material>": "leaf"|"surface"}}` is the same choice and its answer lists
every verdict. Auto only decides NEW assets and NEW material files; a choice also switches an existing
asset's wind slot and its material's four shading keys (`materialgen::ApplyLeafShading`), every other
edit kept. `--seam-ratio=<gltf>` prints the verdict per slot headless; `intent_regression`'s
`TestLeafCards` holds the rule on the staged sources.

**Handedness.** glTF is right-handed (front +Z, up +Y, the asset's right -X), the engine left-handed
(`LookAtLH`): `MeshManager::ParseGltfFile` negates X after each node's world transform (UnityGLTF's
convention) and the det -1 turns the winding through the negative-scale rule. Until 2026-10-24 it read the
coordinates as they were and every glTF asset rendered as its own mirror image -- the_bathroom_free back to
front against its Sketchfab render; the CCW winding only looked right because the mirror flipped it too.
`intent_regression`'s `TestGltfHandedness` holds it (a vertex on glTF -X lands on +X; the front stays
clockwise). `kMeshBinVersion` 3 marks converted bakes; all 148 .mesh.bin were re-baked from their manifests
(`--reimport --reimport-src="<src>" --reimport-out=<bin> --reimport-manifest=<mesh.json>`; a quoted value may
hold spaces -- node names do). The levels were migrated so every copy stays its own mirror where it stood: the
`the_bathroom` group and its candles mirrored about the room's centre plane, 459 tilted copies (pitch/roll)
about the vertical plane nearest their local X (keeping their ground contact; keeping the rotation would
have mirrored them about a tilted plane, into the sand), the fishing boat about its hull centre (2.3 m off its
origin); seashells' `restRotationDeg` -> (p, -y, -r). OBJ is still read as it was (teapot, corgi).

## Mesh LOD -- the Error Is Measured on the Screen It Is Seen On

Selection is UE's (`rendering/meshes/LodSelect.h`: screen size against a per-mesh estimate from the baked deviation,
temporal dither), and so is the estimate's form -- `ViewDistance = deviation * pixelsPerUnit / PixelError + R`
(`StaticMesh.cpp:3064-3134`). UE fixes `pixelsPerUnit` at 960 (1920 wide, 90 degrees) and PixelError at 8: eight
1080p pixels, a visible pop, tuned to UE's own deviation metric (`sqrt(quadric)/8`, which reads small). With
meshopt's distance at 8, the_bathroom_free's props switched in arm's reach (LOD1 from a median 1.6 m, 36 of 100
inside 1 m, the candles at 0.37 m where their 0.9 mm was 3 pixels of a 1440p screen). Since 2026-10-24
`pixelsPerUnit` is THIS frame's -- 0.5 x output width x P[0][0] (1280 at 2560 wide, 90 degrees), the screen size
taken with this frame's multiple too -- and `lod.pixelError` (Developer Controls > LOD "Pixel error", graphics
settings `lod.screenPixelError`; the old key `pixelError` is ignored on purpose) is in those pixels, default 1:
a LOD comes in where its worst deviation spans one pixel of the output. The bathroom: LOD1 from a median 8.6 m,
none inside 1 m (the candles at ~2 m); every model: median 1.2 -> 6.4 m. Cost on wind_test (level start, the
grove, the bathroom; old = `lod.pixelError:6`, interleaved): GPU frame +0.009 / -0.007 / +0.015 ms, noise. A
mesh's numbers: scratchpad-style `lod_survey.py` (the .mesh.bin LOD table: triangles, deviation, the distance
each LOD comes in; `LOD_K` = pixels per unit / PixelError). **Superseded on 2026-11-11 by the paragraph below.**

**UE's heuristic again, on a MEASURED deviation** (2026-11-11, owner, on boats that never reached LOD3: "do it as UE: the
measured deviation and a normal LOD3", then "fully UE, and push the small meshes' threshold by hand so the bathroom stays
right", "the trees keep their aggressive LOD"). Four parts:
- **The deviation is measured** (`MeshManager.cpp MeasureLodDeviation`, every bake and the runtime parse; a chunked mesh keeps
  meshopt's): the two-sided distance between the level's surface and LOD0's -- every vertex plus points ~1/512 of the mesh's
  size apart, each against the other side's triangles exactly (an AABB tree). meshopt's `result_error` is its quadric's (and
  the attribute terms under `lodNormalWeight`) and read 2-3x the real distance at LOD3: police_boat 2.29 m against 1.10. The
  bake logs both: `[meshbake] '<bin>': LOD3 normal, deviation from LOD0 in m, measured (meshopt's): ...`.
- **UE's chain** for every asset with no foliage slot and no chunks (`BuildLodsCpu ueChain`): each level half the
  triangles (50 / 25 / 12.5 % of LOD0, UE's LOD groups' LODPercentTriangles=50) and no error budget (UE's MaxDeviation 0):
  the budgets stopped meshopt short -- steps of 85-99 %, LOD3 as heavy as LOD2. `lodErrorScale` caps the budget below 1 (the
  mesh's size); a cap of 0.1 is the cure for one collapse that ruins a level (police_boat's LOD3 1.10 -> 0.48 m,
  Sink_002's 0.44 -> 0.056). Foliage keeps its tuned chain (UE's foliage makes no LODs) and the terrain its budgets.
  `lod3Aggressive` absent = harsh only for an asset with a foliage slot (`Lod3AggressiveOf`); the import window says Auto /
  Aggressive / Normal.
- **UE's threshold**: `AutoLodScreenSizes` on UE's reference view (1920 x 1080, 90 degrees: 960 px per unit, multiple 0.889),
  PixelError `lod.pixelError` 8 (graphics settings `lod.uePixelError`; the old `screenPixelError`, 1 px of this screen, is
  ignored), or the asset's UE LOD group, mesh.json `"lodGroup"` (`render::kUeLodGroups`: SmallProp / LargeProp / Deco 10,
  LevelArchitecture 12, HighDetail 6; Mesh Editor > LOD "LOD group (UE)"), or its own `"lodPixelError"`. Assigned: the boats
  and the tent LargeProp, the beach props and the buoys SmallProp, the lagoon floor LevelArchitecture, and every
  the_bathroom_free node `"lodPixelError": 1` (seen at arm's length): its LOD1 median 8.6 -> 9.4 m, none nearer than 2 m
  (6 before), LOD3 77 -> 43 m. The boats' LOD3: police_boat 2.94 km -> 52 m, small_yacht 2.46 km -> 26 m, fishing_boat
  1.22 km -> 39 m; old_boat takes `lodPermissive` (its LOD2 and LOD3 were stuck at 3530 triangles on its seams: 4179 /
  2089 / 1043 now). The bins before: `D:\Programming\test_cube_backups\mesh_bin_v5_before_measured_lod_2026-11-11`, the
  manifests `...\mesh_json_before_ue_lod_2026-11-11`.
- **What is out of range, and what to do** (`rendering/meshes/LodSwitchReport.h`, one report): Mesh Editor > "LOD switching"
  (`--edit-mesh=<mesh.json>#lod`; the header counts the warnings) -- per LOD triangles, measured deviation, UE's screen size,
  where it comes in, the mesh's and the deviation's 1080p pixels there, "skipped" for a level the next one shadows --; the
  importer logs every warning with its fix (`import <asset>: LOD: ... Fix: ...`), shows them under the Import window's
  status and in MCP `import_status` (`lodWarnings`); `test_cube.exe --lod-report[=<path part>]` prints all of it for every
  asset (exit code = assets with warnings). Warnings: a level with as many triangles or a weak step (over 1000 triangles);
  a level that comes in under 4 px; over 2 triangles a pixel before the next level (over 1000); an authored screen size that
  pops; a gap of x25; no measured deviation. Each names the knob (`lodErrorScale`, `lodPermissive`, `lodDropSmallParts`,
  `lodRatioScale`, `lodGroup` / `lodPixelError`, the foliage prune).
- **The discrete chain folds double-sided duplicates as the DAG does** (same day, owner: "let date_palm take the DAG's
  method"; `tessy::FoldTwins`, public in TessyDag.h now): a range whose every triangle has a back twin simplifies its front
  triangles and emits each kept front's back. It needs `meshopt_SimplifySparse` (and the budget in absolute units): meshopt
  classifies a vertex by every vertex at its position in the whole buffer, so each front vertex, its twin unreferenced beside
  it, read as complex -- without Sparse the folded fronds came back whole, 8400 indices in, 8400 out. date_palm LOD1 / LOD2
  4438 / 4148 -> 1658 / 766 triangles at a measured 0.37 / 0.43 -> 0.085 / 0.27 m; curly_palm 2191 / 1221 -> 1181 / 275 at
  0.115 / 0.118 -> 0.062 / 0.105 m; their LOD3 (the prune) as it was. `--tessy-lod-fold=0` turns it off with the DAG's.
- **A skipped level is replaced and the chain filled** (same day, owner: "if LODs are skipped, as on some yachts, generate the
  missing levels: L3 becomes L1 and L2, L3 are made anew"). On UE's chain the levels are made one at a time, each half the
  last, and measured: one that deviates no more than the level before it (within 0.1 %, or 1e-4 of the mesh's size: a flat
  tile measures float noise) takes that level's place -- it would come in at the same distance and the finer one would never
  be drawn -- and the halving goes on until three levels differ, the simplifier stalls (under 5 % fewer triangles) or ten
  halvings are spent, so a mesh may keep fewer than three. The bake logs `chain UE, N skipped level(s) replaced by coarser
  ones`. fast_boat_13 (LOD1-3 all 0.0213 m before; with `lodPermissive` now): 25 / 12.5 / 6.3 % at 0.018 / 0.024 / 0.031 m;
  Floor_tile_001 (`lodErrorScale` 0.1): one level of 18 triangles, exact. After all of it `--lod-report`: 155 assets, no
  warning, no skipped level; every bin's LOD0 and vertices byte for byte as before.
- **A range keeps half its area** (same day, owner: "on small_yacht's last LOD the glass disappears"). With no budget a thin
  plate is the simplifier's cheapest prey -- its faces meet at the cost of its thickness: the windshield went 2.9 -> 0.027 m2
  at LOD3, and the measured deviation could not tell (the cabin lies behind the pane; LOD3's 0.215 m was mostly it). A range
  that keeps under half its area takes twice the triangles until it keeps it (`RangeArea`; not under SimplifyPrune, where a
  part's vanishing is the point); the bake logs `N range simplification(s) redone to keep half the area`. Hit: small_yacht
  (LOD3 0.215 -> 0.042 m, the glass whole), the tent, the bathroom's three curtains and two folded towels.
**A mesh takes a share of the pixel error**: mesh.json `"lodPixelErrorScale"` (Mesh Editor > LOD "Pixel error scale";
absent = 1; runtime, no rebake) multiplies the frame's pixel error for this asset -- the discrete LODs' (Mesh::
GetLodScreenSizes) and the cluster DAG's (ShadowGpuData divides the uploaded cluster and level errors by it, so the
camera's cut, the VSM pages' and the atlases' agree; a root's FLT_MAX stays). Set on the shared Mesh by every copy
(RenderableObject::SetLodPixelErrorScale). Under 1 an asset keeps its detail nearer the eye. small_yacht at 2 px (the
owner's tessy.lodPixelError) from 3.5 m: 0.25 took the camera's triangles 13.7 -> 23.4 k and the hatch's facets away.
But first look at how the asset was BAKED (below, the DAG's normal weight): the facets were its simplification's.

## Tessy -- Cluster Geometry (the VSM Pages, the Lamps' Atlases, the Camera; Plan N0-N7)

Tessy is our UE Nanite analog (`docs/tessy_plan.md`, journal in its section 9). A CLUSTER is a run of <= 128 triangles
/ 256 vertices of one LOD's index buffer inside one submesh (`Mesh::Cluster`); the classic paths draw the same index
buffer and never notice. `.mesh.bin` v4 carries them after each LOD; v3 still loads, without clusters, and such a
mesh stays on the classic path in the same frame (the log line `tessy: clusters for N of M caster groups (...);
classic path: <assets>` says who is where). `--tessy-upgrade=<a.mesh.bin>,...` rewrites a v3 file as v4 in place --
the same vertices byte for byte, the same triangles per (LOD x submesh), hashes kept; it writes into `models/`, so it
is the owner's call (plan Q4) -- back the files up first. Done for all 148 on 2026-10-30 (owner): the shared `models/`
is v4 (log `classic path: none`), the v3 originals are in `D:\Programming\test_cube_backups\mesh_bin_v3_2026-10-30`. A
checkout older than the Tessy merge (0bd5c72f) reads v3 ONLY and refuses every one of them.

**`.mesh.bin` v5 carries the cluster DAG** (N7, below) after the LOD blocks: a bake (`--bake`, `--reimport`, the import)
builds it with the asset's own simplification weights (`MeshManager.cpp BuildDagForMesh`, the same function a load uses
for a v4 file), `--tessy-upgrade=<bin>,...|all` adds it to a v3/v4 file in place (clusters kept, `all` = every
`models/**/*.mesh.bin`; a v5 file is left alone), and a load takes the stored DAG after `ValidateDag` -- no build at boot.
Done for all 148 on 2026-10-30 (owner: "bake the DAG at import and bake every mesh"): 4624 DAG clusters, 511 k triangles
over all levels; the v4 files are in `D:\Programming\test_cube_backups\mesh_bin_v4_2026-10-30`. A checkout before this
reads v4 at most and refuses them. `--tessy-lod-build` still rebuilds at load (the A/B of a builder change).

**What a bake stored is readable** (2026-11-11, owner: "is Tessy always baked into the mesh, with no settings of its own? at
least write its stats"). There is no per-asset Tessy switch: the clusters (<= 128 triangles / 256 vertices, every LOD) and
the DAG are in every v5 file; the DAG takes the LOD section's `lodNormalWeight` and `foliageUvWeight` (foliage slots) and the
process's `--tessy-lod-prune` / `--tessy-lod-fold`; a load cuts the clusters into meshlets (<= 64 vertices). A bake now logs
`[meshbake] '<bin>': Tessy -- DAG weights ...; LOD clusters (meshlets) ...; DAG ... by level (clusters, tris, max error m)`
beside its DAG summary, and Mesh Editor > "Tessy (clusters)" (`--edit-mesh=<mesh.json>#tessy` opens and scrolls to it)
shows the same from the bin (`MeshManager::DescribeMeshBinary` / `DescribeDag`): per LOD triangles, clusters, meshlets and
deviation; the DAG's levels with their clusters, triangles, share of LOD0 and error range; the roots (the fewest triangles a
far copy draws under `tessy.lod`); and the knobs in force. The Mesh Editor read the bin only for a chunked mesh until then,
so "Baked deviation from LOD0" never showed on any other. police_boat: 1750 / 904 / 474 / 205 clusters, the DAG 8 levels,
x2.00 of LOD0 over all of them, roots 8412 triangles (4 %) at 3.04 m.

- **Developer Controls > Tessy** (2026-10-30, owner: "its own tab, a master switch to turn it all off, and a cluster
  debug view"): the master switch `tessy.enabled` (default 1; `graphics_settings.json` vsm.tessyEnabled, `--set`, MCP)
  turns EVERY Tessy path off -- the VSM pages and the lamps' atlases draw whole meshes, the cluster view is off -- while
  `tessy.vsm` / `tessy.raster` / `tessy.atlas` keep their values (greyed out); the cluster tables stay loaded, so it
  switches both ways at once. The verdicts say `off (tessy.enabled 0)` (`tessy.vsm: classic path -- ...`).
- **The cluster view** `tessy.debug:1|2|3` (the tab's "Cluster view" Clusters / Meshlets / Levels; not persisted) draws
  every caster the camera drew this frame over the lit scene colour in `Main_DebugDraw`, one hashed colour per cluster or
  meshlet -- or, Levels (N7e), a DAG cluster by its DAG level (green = the leaves, yellow, red, magenta = the coarsest; the
  kernel's `kFlagLevelId` puts the level in the record's .w high half) and a discrete LOD's cluster by its tier (light to
  dark blue), so `tessy.lod` is SEEN cutting the DAG as the camera moves,
  cutouts clipped: the atlas kernel runs a third row set (2, the camera's row: `camArgs_` / `camVisibleList_`) with keep
  rows / no cull / cluster id in the record's `.w`, and `TESSY_ATLAS_MS` + `TESSY_VIEW` draws it with the G-buffer's
  jittered view (`BuildGBufferViewCB`), depth `GREATER_EQUAL` + a slope bias against the frame's own depth. So it shows
  the camera's geometry and LODs, not the shadows' -- the shadow paths cull their own. While it is on, the camera's
  two-pass HZB cull is off (pass B's rows would be missing from the row it reads); it needs the indirect G-buffer and a
  mesh shader tier, and says why not (`tessy.view: off -- ...`, the tab's `cluster view:` line, `get_readouts`
  tessy.view). The auto exposure meters the colours too: the sky darkens behind them. Cost: `Pass_DebugDraw`
  0.03-0.04 ms on the grove (9.2 k meshlets).
- **N5, the camera's G-buffer by clusters** (`tessy.gbuffer`, default 1 since 2026-10-30 -- owner: "switch Tessy on by
  default in the G-buffer" -- at the cost measured below; Developer Controls > Tessy "camera G-buffer",
  `graphics_settings.json` vsm.tessyGBuffer, `--set`, MCP; verdict `tessy.gbuffer: on` / `classic -- <why>`): the camera
  row goes through `tessy_atlas_cull_cs.hlsl` too, BY PSO RUN (`kFlagByRun`: one 1024-thread group per run of the merged
  G-buffer commands, `VgRun` maps a virtual group to its run) -- every (caster, cluster) box, wind padded, against the
  camera frustum, the survivors' meshlets to the run's row, the rows zeroed -- and each run's MESH SHADER TWIN draws
  them in pass A (`gbuffer_indirect.hlsl TESSY_GBUFFER_MS`: the run's own desc and defines; one DispatchMesh spans the
  run's groups, so the record's `.w` carries the virtual group -> the LOD side of the crossfade and the group's
  SurfaceParams from a per-frame buffer (`MaterialData::FillGBufferSurfaceParams`), the textures by
  `NonUniformResourceIndex`). A run the kernel took whole writes 0 into its classic merged commands' COUNT buffer
  (`kTessyCamClassicCountOffset` in the args region): 824 empty commands cost the grove 0.045 ms. Table PSOs, pass B,
  GBV's per-group path keep their classic draws; never on the validation frame (the readback sums the camera row) nor
  in wireframe. Measured 2026-10-30 (DLSS Balanced, panels closed, alternated x2): the G-buffer 0.155 vs 0.171-0.174 ms
  on the grove, 0.169 vs 0.187-0.189 in the tunnel, 0.181 vs 0.173-0.174 in the bathroom, but the kernel adds
  0.013-0.035 -- the frame +0.01-0.02 ms. Picture: on/off at the noise floor (grove 0.23 vs 0.21-0.22 %, the cabinet
  0.03-0.16 vs 0.44-0.64 %): on wind_test's low-poly assets clusters do not pay in the main pass (off by default until
  the owner switched it on).
  The mesh shader's 25 output values a vertex are not its limit (a 19-value version measured the same).
- **`tessy.gbufferHzb`** (default 0, not persisted): N5's two-pass HZB per CLUSTER (UE's main/post): a cluster last
  frame's pyramid hid goes to `TessyCamDeferred` (claimed with the run's records -- both fit or the run stays classic;
  unwritten slots dead), `tessy_camera_post_cs.hlsl` in Main_CamCullPost retests it against this frame's pyramid into the
  run's pass-B row (after pass A's), Main_GBufferB draws it with the twins. wind_test defers 1.5-5 % of the clusters
  (grove 66-76 of 4325, tunnel 330-338), and the test + pass B cost more than they cull (+0.03-0.04 ms) -- for dense
  assets. Needs the camera's two-pass HZB that frame (the cluster view switches it off).
- **N7, the cluster DAG (continuous LOD)** (plan section N7 "Этапы"): `rendering/meshes/TessyDag.*` builds
  UE's DAG over LOD0's clusters per submesh (baked into v5 since N7e; at load for a v4 file) -- groups of ~16 (`meshopt_partitionClusters`), each simplified to
  half (the discrete LODs' normal / foliage-UV weights, `ErrorAbsolute | Sparse`) with the vertices it shares with other
  groups or other submeshes locked BY POSITION, recut into clusters, to the root; every cluster carries the error and
  sphere of the simplification that made it (lod*) and of the one that consumes it (parent*). Every v5 asset has one; a
  v4 file builds one at load only when its mesh.json says `"tessyLod": true` or `--tessy-lod-build=all|<path part>,...`
  names it. `Mesh::Dag` + its own IB, appended to the mega IB after the mesh's LODs; the DAG clusters join
  the cluster table with a parallel `ClusterLod` table, per mesh group a DAG range and per level its error interval
  (`TessyClusterLodSrv` / `TessyDagGroupSrv` / `TessyDagLevelSrv`). `tessy.lod` (default 0, Tessy tab) lets the VSM
  CLIPMAP pages draw the DAG's cut (`tessy_vsm_cull_cs.hlsl`: an orthographic page has a distance-free threshold,
  `tessy.lodShadowTexels` (2) x its texel size; the run's levels a contiguous band by its casters' scale spread); a
  switch flushes the page cache. Gate: `intent_regression TestTessyDag` (invariants, the cut's triangles falling, the
  fine cuts' area within 20 % of LOD0's). Measured on the grove (2026-10-30): the palms' DAG at 2 / 4 / 8 texels draws
  3.19 / 3.00 / 2.84 M triangles into the pages against the shadow tier's 2.77 M, `VsmPageRender` the same within
  noise -- the date and curly palms' fronds stop halving at ~45 % where the discrete LOD3 drops whole leaves -- and the
  palms' self-shadowing changes (6-8 % of pixels): the camera still drew discrete LODs, and a shadow finer than its
  receiver breaks the caster/receiver LOD contract.
- **N7d: the camera draws the DAG's cut too** (`tessy.lod` turns the cluster G-buffer on for itself; the gbuffer verdict
  then says `on (tessy.lod: ...)`, the event `tessy.lod: the camera draws the cluster DAG's cut`). On the camera's runs
  `tessy_atlas_cull_cs.hlsl` takes a mesh group with a DAG from `DagGroups` instead of its tier's clusters and tests
  every (caster, DAG cluster): drawn when its own error x the caster's scale x (`g_lodPixelsPerUnit` / lod.pixelError /
  lod.distanceScale) is at most the distance to the NEAREST point of its lod sphere and its parent's is more -- where a
  discrete LOD of that deviation switches (ViewDistance + R), so DAG and discrete meshes share one rule. ONE function
  (`DagTooCoarse`, `precise`) answers both tests: a parent's must answer exactly as its children's own, or the cut
  tears. A crossfading caster sits in two tiers' entries and is drawn once, from its own (`CasterLods`); the record's
  `kTessyCamDagBit` (.w bit 15, `kTessyCamVgMask` is 0x7FFF now) gives the mesh shader no crossfade. Pass B (the casters
  the instance HZB hid) stays classic, on the discrete LOD, for the frame it draws them. Readouts: `tessy.camDag`,
  `camTris` / `camDagClusters` (pass A, counters [24] / [28]). Measured on wind_test (2026-10-30, the palms' DAG, panels
  closed, VSM, alternated twice): the grove draws 259 k camera triangles either way, cull 0.029 -> 0.041 ms (the flat
  test doubles: 4.3 k -> 9.0 k), G-buffer 0.163 / 0.162, VSM 0.42 -> 0.44 (its 2-texel cut is finer than the shadow
  tier); the far view [-20,30,20] 428 k -> 657 k triangles, G-buffer 0.165 -> 0.18-0.19, cull 0.04 -> 0.07-0.09 -- the
  palms' foliage LODs are not error-driven (they drop whole leaves by distance) and the DAG's roots stop at ~45 % of
  LOD0. No crack looking up into the crowns (DLSS off) nor in the cluster view; the picture moves where the shadows do
  (8.5 % of pixels, floor 2.2 %). GBV (`ShadowCull,GBuffer,CamCullPost,GBufferB,VsmPageRender,Skybox` + the Skybox
  self-test): clean.
- **N7e: the DAG's cut in every shadow and a level band for the camera** (2026-10-30). `tessy.lod` now also cuts the
  DAG in the lamps' ATLASES (SDSM / Legacy; the atlas kernel's row sets 0 and 1 take each view's light and its texels per
  unit at distance 1 / `tessy.lodShadowTexels` from `ShadowGpuData::viewDag_`, filled from `LocalViewCull::dag`; a switch
  bumps the lamps' static cache version) and in EVERY VSM page (`tessy_vsm_cull_cs.hlsl PageDagTooCoarse`: an error spans
  e s K / w texels, K = |column x| x 64 / texels, w = the clip w of the sphere's nearest point -- 1 on a clipmap page, the
  light's depth on a local one; the run's levels a band from its casters' threshold spread over `DagSpheres`; a change of
  the cut or its texels flushes the page cache). The crossfade dedupe is the camera row's only (a shadow row holds a
  caster once). The camera's DAG groups test each caster's BAND of levels (`DagBand`: the levels whose error interval can
  meet its threshold anywhere on the DAG's sphere -- `Tessy.DagSpheres`, around every lod and parent sphere): tests on the
  grove 8.3 k -> 4.5 k, the far view 21 k -> 6.0 k, the same clusters; `tessy.lodBand:0` = the flat test (the A/B). Leaf
  cards simplify with `meshopt_SimplifyPrune` (whole cards leave once their radius fits the error, which the error then
  carries; `--tessy-lod-prune=0` = edge collapses alone): date_palm's roots 4275 -> 829 triangles at 0.37 -> 1.17 m, the
  camera's distances unchanged (a leaf is a pixel only at hundreds of metres -- the honest metric does not drop visible
  leaves; the palms' discrete foliage LODs do, by hand, `lodDropSlots` / `foliagePruneKeep`). Measured, wind_test with
  "Lamps" at 23:00 on the grove, alternated twice: SDSM `Pass_PointShadows` 0.280-0.282 -> 0.260-0.261 ms (atlas meshlet
  records 34.3 k -> 27.5 k), VSM `Pass_VsmPageRender` 1.082-1.089 -> 1.050-1.060 (9.5 -> 8.9 M triangles); the picture moves
  where the lamps' shadows fall on far palms (SDSM 1.7 %, VSM 2.3 % of pixels over 8 levels, floor 0.2-0.3 %), "0" repeats
  "0" and "1" repeats "1" at the floor. A flight burst (60 fps, DLSS off, the temporal second difference per frame) tells
  the arms apart nowhere -- the motion swamps the metric, so it shows no pop stands out, not that none exists. GBV (SDSM:
  `ShadowCull,PointShadows,SpotShadows,LocalShadowCache,GBuffer,Skybox`; VSM: `VsmPageRender,ShadowCull,GBuffer,Skybox`; the
  bathroom at 23:00 with the Skybox self-test): clean. The DAG is baked into .mesh.bin v5 since (above). The SDSM / CSM
  sun partitions draw discrete LODs, by clusters under `tessy.sdsm` (below) -- an orthographic DAG cut there is not done.
  With every asset's DAG (2026-10-30, SDSM, panels closed, alternated twice) `tessy.lod` 1 in the G-buffer: grove
  0.157 vs 0.160 ms, cull +0.009-0.012 (262 k vs 263 k triangles); bathroom 0.167-0.168 vs 0.170, cull +0.001-0.004;
  tunnel 0.174 vs 0.170, cull +0.014 (482 k vs 419 k triangles); the far view [-20,30,20] 550 k vs 382 k -- the palms'
  foliage LODs drop whole leaves by hand, the DAG's honest metric does not. The picture moves where LODs differ (grove
  2.6 %, tunnel 1.8-2.0 %, floor 0.8-1.7 %). Hence `tessy.lod` stays 0 by default. A FACETED DAG ON A GLOSSY HULL IS ITS BAKE (2026-11-09, owner at small_yacht's bow hatch:
"tessy switches clusters very early, the DAG mangles the UVs"): the DAG takes the asset's `lodNormalWeight` (and leaf
UV weight), and small_yacht's is 0 -- a position-only simplification on ONE shared vertex buffer: a collapse a tenth of
a millimetre off the surface leaves LOD0's normals interpolated across a triangle twice the size, and at 2 px a
smooth white hull showed it as wedges of light 3 m from the eye. Rebaked into the scratchpad (`--reimport
--reimport-out=<scratch> --reimport-manifest=... --reimport-normal-weight=<w>`, models/ untouched) and viewed through
a scratch level copy: w 0.5 and 1 both smooth where w 0 was faceted, the camera's triangles 13.7 -> 16.3 / 18.9 k on
that view (the normal error joins the cluster's error: the roots 1.5 m -> 10.6 / 40 m). No UV weight on a surface slot
exists yet (`foliageUvWeight` is the leaves'); the yacht's artefact was the normals'. small_yacht carries `lodNormalWeight` 0.5
since (rebaked in place, the bytes the scratch bake was judged on; the old file in
`D:\Programming\test_cube_backups\mesh_bin_v5_small_yacht_before_normal_weight_2026-11-09`): both bow views smooth, 18.3 /
16.3 k triangles. Its LOD0, collision and water mask hull are LOD0's and did not move. The Import window's LOD "Normal
weight" starts at 0.5 since (owner: "make it 0.5 by default"; MCP `import_asset` takes the window's value too): a NEW
import writes `"lodNormalWeight": 0.5`; an absent key still means 0, the bake the manifest describes, and a bulk
re-import keeps each asset's own (Mesh Editor > LOD shows the same key).
- **The camera's cluster records are laid out in the classic draw's order** (`tessy_atlas_cull_cs.hlsl` `kFlagOrdered`,
  2026-10-30, owner at cam -62.12 46.92 136.30: "with the DAG the meshes flicker, the bathroom roof most"). The mesh
  shader draws a run in RECORD order, and the records went where the atomics landed -- new every frame. Two coplanar
  surfaces of two meshes (the_bathroom_free's ceiling: `Selling_top` and `Selling_top_001`, one LOD each, the same
  plane) then swapped the depth-test winner every frame: the roof flickered between the perforated sheet and the rusty
  one, 19.5 % of the roof's pixels over 8 levels across 48 still frames with DLSS off (10.7 % with DLSS). The classic
  draw had kept them in its command order, and before the DAG both drew classic (no clusters at their far tiers). Now
  the camera rows (the G-buffer's runs, the cluster view) gather a run's virtual groups in the merged draw's command
  order (`RunCmds`, t13: per run {first, count}, then `cmdOrder_`) and write the records in test order (entry, caster,
  cluster) by a group prefix sum (`OrderedPrefix`); the shadow rows write depth alone and keep the atomics. After: the
  roof 0.06 % (no DAG 0.06 %), and the same layer wins as classic (3 % of the roof differs, the DAG's geometry). Cost:
  `ShadowCull.TessyCamera` +0.004-0.006 ms (grove, tunnel), +0.012-0.018 on that aerial view, +0.02-0.04 with the DAG
  there; the G-buffer reads ordered records a little faster (bathroom 0.179 -> 0.172). `tessy.ordered:0` = the atomics,
  the A/B (it brings the flicker back). Which of a run's entries overflow `kMaxEntries` (1024; thousands of palms from
  the air) and stay classic is now the command order's too. GBV (`ShadowCull,GBuffer,SdsmCull,SdsmShadow,Skybox` + the
  Skybox self-test, that view with `tessy.lod:1`, 833 frames): clean.
- **A camera run is culled by 16 groups** (`tessy.camSplit`, transient, 1-32; 2026-10-31, owner on the aerial view
  cam -70.98 34.67 144.04: "why are the discrete LODs faster even at 2 px?"). One 1024-thread group a PSO run left as
  many SMs working as there were runs (six) -- ~8-10 ns a (caster, cluster) test, the rate the SDSM partitions had
  before their split -- and the DAG's ~30 k tests cost 0.26 ms of `ShadowCull.TessyCamera` against the discrete LODs'
  8 k at 0.08, more than the DAG saved the G-buffer. Now group g takes run g / split and the (g % split)-th CONTIGUOUS
  part of its commands (the classic draw's order), writes its own row, and `RecordTessyGBufferDraws_` issues a run's
  rows in slice order, so the records keep the order the roof flicker fix needs (the bathroom roof: 0.05 % at 1, 8 and
  16 slices). The run's classic count is written by its LAST slice to finish: each adds 1 (+ 1 << 16 when it left records
  classic) to the run's counter at `kTessyCamRunDoneOffset` + 4 r (the counter region is 336 bytes; at most 64 runs).
  Pass B's rows start after pass A's slices. Measured (DLSS Balanced, panels closed, alternated twice), the camera cull
  at 1 / 4 / 8 / 16 / 32: DAG 0.258 / 0.172 / 0.113 / 0.057 / 0.042 ms, discrete 0.082 / 0.066 / 0.060 / 0.058 / 0.056;
  the G-buffer +0.005 at 16 (a DispatchMesh a slice). On that view the GPU frame with the DAG at 1 px is now the
  discrete LODs' (3.34 vs 3.32 ms), at 2 px below it (3.29).
- **A count buffer never holds 0xFFFFFFFF** (2026-10-30, owner at cam -44.92 11.56 97.73, 16:00: "with the DAG this
  small rock dithers"). A camera run the kernel did not take whole wrote ~0 into its classic count, meaning "all the
  run's commands" -- ExecuteIndirect is specified to take min(count, MaxCommandCount), but this driver (NVIDIA
  32.0.16.2101) then drew NOTHING: every group a run left classic vanished. Proven by forcing such a run classic whole:
  the island, the bathroom's walls and the trunks went with it; with the exact count back. The rock's group fell out of
  the kernel's entries in some frames (DAG band blocks handed out first come, first served: 156 blocks left 18 DAG
  groups no room on the grove's run) and then out of the frame. Now the count is the run's exact command count
  (`RunCmds[2r + 1]`), and a band block is handed out only if every DAG group after it keeps room for a plain entry (no
  DAG group falls to classic for want of entries; the band then still tests 25-40 k clusters on the rock and aerial
  views against the flat test's 62-82 k: `ShadowCull.TessyCamera` 0.20 vs 0.24 ms, 0.24 vs 0.33). The rock's box over
  48 still frames: 0.00 % of pixels move over 8 levels (DLSS off), DAG and discrete alike. Any count buffer: write the
  number, never a sentinel.
- **Double-sided duplicates are folded in the DAG** (2026-10-31, owner: "so the current LODs are better for the palms? is
  it the foliage, do the trunks simplify fine?"). The trunks did (one cluster, 70-120 triangles). date_palm's fronds,
  curly_palm's fronds and curly_palm's frond boots are double-sided sheets built of DUPLICATES -- every triangle has a
  back twin at the same positions with the opposite winding (`twin_scan`: exactly those three submeshes over all 148
  assets, 100 % paired) -- so every edge is shared by four triangles, meshoptimizer calls the vertices complex and never
  collapses them, and only `meshopt_SimplifyPrune` acted: date_palm's 35 fronds (~1.1 m each) went 5600 -> 560 at once at
  1.05 m of error, i.e. the camera drew all 5600 out to ~1.3 km. `TessyDag.cpp FoldTwins`: a submesh whose every
  triangle is paired grows its DAG over one side (half-size clusters) and every emitted cluster carries its triangles'
  twins (each front vertex's back vertex, the winding reversed). date fronds now 5600 -> 2796 -> 1398 -> 698 -> 136 -> 20
  (1.9 / 6.9 / 26 / 113 cm), curly fronds 1568 -> ... -> 72, boots 2400 -> ... -> 74; roots date 829 -> 283, curly 1515
  -> 307. Live, `tessy.lod:1` with the two palms rebuilt at load (`--tessy-lod-build=date_palm,curly_palm`), DLSS
  Balanced: grove 243 k triangles (discrete 263 k, old DAG 262 k), far view 320 k (378 k, 544 k), aerial 916 k (972 k,
  2.21 M); G-buffer 0.156 / 0.153 / 0.245 ms against the discrete 0.166 / 0.178 / 0.26 -- the DAG now beats the
  discrete LODs on all three, but its camera cull costs the aerial view 0.25 ms against ~0.1. date_palm and curly_palm
  were rebaked with it the same day (owner: "rebake"; `--reimport` from their manifests -- every byte before the DAG
  section identical to the old file, the loaded numbers equal to the at-load build); the unfolded files are in
  `D:\Programming\test_cube_backups\mesh_bin_v5_unfolded_dag_2026-10-31`. `--tessy-lod-fold=0` = no folding (the A/B); `intent_regression TestTessyDag` holds it on date_palm (folded, the 10 cm cut under half,
  every folded cluster's second half its first half's twins).
- **`tessy.lod` is persisted** (`graphics_settings.json` shadows.vsm.tessyLod, since 2026-10-31 -- owner: "turning the
  DAG on is not saved"): every v5 asset carries its baked DAG, so nothing is built at load any more.
- **`tessy.lodPixelError`** (default 1 px; Developer Controls > Tessy "DAG pixel error", `graphics_settings.json`
  vsm.tessyLodPixelError, `--set`, MCP): the camera's DAG cut threshold on its own -- the pixels a cluster's error may
  span before its parent takes over (the discrete LODs keep `lod.pixelError`; the SDSM sun's camera cut follows it).
  Measured with the folded palms: 0.5 / 1 / 2 / 4 / 8 px = grove 298 / 244 / 159 / 108 / 78 k triangles, far 481 / 320 /
  205 / 176 / 126 k, aerial 1.42 M / 916 / 749 / 564 / 505 k (G-buffer 0.295 -> 0.175 ms); the discrete LODs unmoved.
- **`tessy.sunCut`** (default 1, transient): under `tessy.lod` the SDSM partitions cut the DAG with the camera's eye and
  threshold (`kFlagCameraCut`) -- a far palm's shadow then has the fronds the camera draws, not LOD3's thinned ones (the
  aerial view: 2.4 % of the frame over 8 levels against a 0-0.7 % floor, the sand under the far crowns); +0.015-0.06 ms
  of SDSM cull + draw. 0 = their discrete tier.
- **`tessy.vsmCameraCut`** (default 1, transient; 2026-10-31, owner at cam 45.20 4.51 42.03, 17:00, VSM: "shadow acne"):
  under `tessy.lod` the VSM CLIPMAP pages cut the DAG with the camera's eye and threshold too (`tessy_vsm_cull_cs.hlsl
  CamDagTooCoarse`, the camera row's `DagTooCoarse` term for term; `ShadowGpuData::TessyDagCamThisFrame`, kept every frame).
  Cut by their own texels, a coarser level drew a coarser surface than the receiver's -- up to `tessy.lodShadowTexels` of
  ITS texels above it -- and SMRT's rays, which walk out of the receiver's level into the coarser ones, met it: the sand
  dark and hatched in a patch edged where the level changes. Neither half alone: `tessy.lod:0` or `vsm.smrtRayCount:0`
  was clean, and caching off was not (it is geometry, not stale pages). Measured at that view: the patch's mean 149.5 ->
  158.0 of 255, 9.0 % of the frame over 8 levels against a 0.4-0.6 % floor; `Pass_VsmPageRender` with every page redrawn
  0.32 -> 0.345 ms there, 0.64 -> 0.70 on the grove (page triangles 1.20 -> 1.41 M, 6.37 -> 7.06 M: a coarse level draws
  the receiver's clusters); cached the same. A local light's (perspective) pages keep their texels' cut. A cached page
  holds the cut of the eye it was drawn from until its level re-renders (a level snaps every texel it moves). 0 = the
  page's own cut, the A/B; a switch flushes the page cache. GBV (`VsmPageRender,Skybox` + the Skybox self-test, that
  view): clean.
- **A cluster or a triangle that holds no sample is not drawn** (2026-10-30, owner: "do we drop a cluster that is tiny on
  the screen? the shadow passes need it too"). Per TRIANGLE: every mesh shader -- the camera's twins, the VSM pages and,
  since then, `TESSY_ATLAS_MS` (the lamps' atlases, the SDSM partitions: root constants {row, flags, viewport} -- 4 now)
  -- culls a triangle whose box holds no pixel / texel centre of its viewport (`tessy.msCull`, exact, default 1; the
  atlases' cost and picture unchanged within noise). Per CLUSTER: `tessy.clusterCull` (default 0, transient; Tessy
  setting `g_clusterCull`) has every cull drop a cluster whose padded box projects between two sample centres
  (`tessy_cull_common.hlsli TessyBoxHoldsSample`: its 8 corners through the view's matrix; a corner at w <= 0 keeps it)
  -- the camera's runs by `CameraHzbCB`'s jittered matrix (SceneRenderer fills it every frame now, HZB or not), the lamps'
  views by `ViewRects` (t15, per view slot: the atlas matrix and the face's edge, from `LocalViewCull`), the SDSM rows by
  their partition's `lightViewProj` (t14) on `Renderer::ShadowTileContentTexels`, the VSM pages by `PageProj` on 128.
  Exact as well, but on wind_test (DLSS Balanced, SDSM cache off) it drops 0-19 clusters of 5-35 k records a view, VSM
  0-17, the lamps' grove atlases 0, 103 of 57 k from 140 m up -- LOD selection at one pixel and the DAG keep a far
  object's clusters pixels wide -- and costs each cull 0.005-0.009 ms; hence off. Counted: `get_readouts` tessy
  `rowRectCulled` (camera + lamps), `sdsmRectCulled`, `rectCulled` (VSM).
- **A RAW view starts on 16 bytes**: a frame's DispatchMesh args are `kTessyAtlasRows` x 12 bytes, and with the third
  row set that stopped being a multiple of 16 -- the second frame's RAW UAV removed the device at its creation (debug
  layer `id=345`, Release: `0x887A0005` at the next create, then abort). The region is `kTessyAtlasArgsRegionBytes`,
  rounded up; anything that sizes a per-frame RAW region by a row count rounds it the same way.

- The VSM page setup hands each (page x caster bucket) of a group with clusters to `tessy_vsm_cull_cs.hlsl`, which
  tests every (caster, cluster) box against the page and appends survivors; `VSM_TESSY` of `shadow_indirect_csm.hlsl`
  draws them as ONE `DrawInstanced(384, N)` (UE's VS raster path, vertices fetched by hand from the mega VB/IB).
- N6, the default: `tessy.raster` 1 draws them through a MESH SHADER instead (`VSM_TESSY_MS` `MSMain`, one
  `DispatchMesh` whose args `tessy_ms_args_cs.hlsl` writes): one group per MESHLET -- a cluster cut at load into runs of
  <= 64 unique vertices (`Mesh::BuildClusterLocal`; a leaf card's cluster has ~2 vertices a triangle) -- each vertex
  transformed once, triangles outside the page or covering no pixel centre culled (`tessy.msCull`, exact). 64 because
  the DECLARED output sets how many groups an SM holds: a 256-vertex group was slower than the VS path. Without a mesh
  shader tier (log `mesh shader tier: N`) or its PSO it falls back to the VS path, logged `tessy.raster: vertex shader
  -- <why>`. Near `SetMeshOutputCounts` no branch and no early return: DXC's `ms_6_6` tail-duplicates the call and the
  validator refuses the shader.
- `tessy.vsm` (default 1; Developer Controls > Tessy, `graphics_settings.json` vsm.tessy, `--set`, MCP) switches live;
  the event `tessy.vsm: on` / `classic path -- <why>` is logged on every change of the verdict. VSM only: the level
  default may be SDSM -- pass `--shadow-mode=vsm` (or `shadow.mode:1`) or the cluster path never runs.
- The wind pad: a cluster's box grows by an exact-per-frame, per-axis interval bound of `WindOffset`'s bend (a
  rotation about the pivot) -- `tessy.windPad` 1; 2 = the isotropic 2D + F A/B; 0 = none (only to see the holes).
  `tessy.validate:1` sways every vertex of every tested cluster with the shader's own function and counts those
  outside their padded box -- `get_readouts` tessy.padMisses must stay 0 (proven to catch: 98 % with no pad).
- N4, the lamps' ATLASES (SDSM / Legacy): `tessy_atlas_cull_cs.hlsl` runs in `Pass_ShadowCull` between the cull and
  the S6a compaction -- one 256-thread group per local view (a point face, a spot) tests every (caster, cluster) of the
  view's records against the view's six planes (the wind pad as above), claims the view's run of meshlets with one
  atomic, writes it and zeroes the taken records' `InstanceCount` (the compaction skips them); `TESSY_ATLAS_MS` draws
  the run with one `DispatchMesh` per view right after the view's classic ExecuteIndirect. S7's static rows go through it
  only for the views that draw them this frame (`SetLocalStaticDrawn`: a cache miss or an uncached view; a hit copies
  them). `tessy.atlas` (default 1; Developer Controls > Tessy, `graphics_settings.json` vsm.tessyAtlas, `--set`, MCP);
  event `tessy.atlas: on` / `classic -- <why>`; every 5 s `tessy.atlas: <main> + <static> meshlet records (cap ...), <n>
  views did not fit, <m> clusters tested` (counters of frame N - 3). A view that does not fit stays classic. Measured
  (SDSM 23:00, "Lamps" on): the grove's point atlases 0.415 -> 0.287 ms for +0.034 of cull, frame -0.10 ms; the bathroom
  (five candles) +0.025 -- nothing there to save.
- **The SDSM sun partitions by clusters** (`tessy.sdsm`, default 1 -- owner, 2026-10-30: "switch Tessy on by default in
  SDSM"; Developer Controls > Tessy, `graphics_settings.json` vsm.tessySdsm, `--set`, MCP; verdict `tessy.sdsm: on` /
  `classic -- <why>`, `get_readouts` tessy.sdsm*): Main_SdsmCull, after its re-cull of rows 0..3, runs the atlas kernel's
  ROW SET 3 against the analyze's 16 planes a partition (`gPlanes`: its cull volume and its box; a local view tests 6),
  `kTessySunSplit` (16) 256-thread groups a partition, each a slice of the virtual groups (vg % 16) and its own run (one
  1024-thread group a partition cost 0.05 ms of latency for 6 k tests); Main_SdsmShadow draws each partition's 16 runs
  after its classic draw with the lamps' atlas mesh shader (`RecordTessySunDraw`: the cascade atlas has the atlas PSO
  state, the partition's GPU-written view CB). Pass B of the light-space HZB stays classic; the cut is the discrete
  tiers'. The SDSM counters are [32..48) of the counter region (`gCounterAt`), cleared by Main_SdsmCull only when the
  main cull cleared none; a re-cull that did not run writes every run empty (no stale args). Measured (2026-10-30, the
  live editor, panels closed, 15:00, wind held, alternated twice): `Pass_SdsmShadow` grove 0.203-0.211 -> 0.170-0.173 ms,
  bathroom 0.106-0.112 -> 0.088-0.092, tunnel 0.121-0.131 -> 0.097-0.098, but the cull `SdsmCull.TessySun` costs
  0.034-0.041 / 0.020-0.023 / 0.032-0.040 and the `Pass_ShadowCull` row (the main and the SDSM cull share it) grows by
  0.02-0.05 -- the frame +0.01-0.02 ms by the passes, within the frame total's +-0.1 noise. SDSM does not follow its
  triangle count (N4): 0.18-0.56 M triangles into the partitions. Picture at the floor (grove 1.3-1.6 vs 1.4-1.6 %,
  bathroom 0.19-0.27 vs 0.18-0.26, tunnel 0.75-0.83 vs 0.75-0.83). GBV (`ShadowCull,SdsmCull,SdsmShadow,Skybox` + the
  Skybox self-test, wind_test SDSM): clean.
- **A wave's prefix sum runs in LANE order, not in `SV_GroupIndex` order** (2026-10-29): in an 8x8 group NVIDIA does not
  lay the threads into lanes by `SV_GroupIndex`, and `gsPrefix[SV_GroupIndex] = WavePrefixSum(x)` handed the prefixes to
  other threads' entries -- the atlas kernel tested the wrong (caster, cluster), different lamps lost their shadows every
  frame (the entries' order is the atomics'). Give each thread its place in wave order (the wave's index from an LDS
  atomic by its first lane, plus `WaveGetLaneIndex()`) and distribute by that; `WaveActiveSum` alone is safe.
- A classic shadow range must come from the EXPLICIT LOD's tables: `groupLodMega_` read `SubmeshesForLod` (the runtime
  `lod.forced` / `lod.enabled` override) until 2026-10-29, and a Rebuild under the knob drew another part of every mesh
  in the local views until the next Rebuild. A/B a shadow path with those knobs at their defaults and relaunch after.
- Mesh-shader PSOs stay out of the disk PSO cache (`kMeshPsoDiskCache`, Material.cpp): the runtime takes no cached
  MESH pipeline back. All six stored blobs were refused on every launch (debug layer `id=887`), and a probe in one
  process (2026-10-30, system D3D12 runtime, NVIDIA 32.0.16.2101) had a mesh pipeline refuse its OWN fresh blob
  whatever the stream held (zeroed padding, no root signature / blend / rasterizer / sample mask / pixel shader,
  DEPTH_STENCIL1) and `ID3D12PipelineLibrary::LoadPipeline` refuse it too (`id=972`), while a vertex-shader pipeline
  through the same stream took its blob back. So they compile once a boot (~18 ms of Release); probe again with
  another driver or an Agility SDK runtime before turning the flag on.
- Counters: `get_readouts` block `tessy` (pairs drawn whole / handed to clusters, clusters tested / drawn and their
  triangles, `records` = the draw's instances or mesh shader groups, lost runs / clusters, `meshShader`; `enabled`,
  `debugView`, `view` = the cluster view's verdict, `gbuffer` = N5's, `gbufferTwins`, and the cluster rows' meshlet
  records `atlasRecords` / `staticRecords` / `viewRecords` (the camera's pass A), `viewsNotFit`, `rowClustersTested`,
  `camDeferred` -- frame N - 3 of the last frame a row ran); profiler rows `GBuffer_Indirect.Tessy`,
  `ShadowCull.TessyCamera`, `CamCullPost.TessyCamera`;
  `tools/tessy_baseline.py --port <p>` measures the plan's three views.
- Measured 2026-10-29 (all 148 assets v4 in a copy): the grove's `Pass_VsmPageRender` 1.34-1.39 -> 0.66-0.69 ms on
  the VS path (20.7 M triangles into the pages -> ~3 M in 25-29 k clusters) -> 0.51 with the mesh shader (its draw
  0.41 -> 0.23) -> 0.40 with the scatter's pages shared by the group (below); the bathroom 0.91 -> 0.59 -> 0.48 ->
  0.46, the tunnel view 0.67 -> 0.33 with the `tunnel` group on (N3), then 0.27 -> 0.30 at the same camera with the
  group off as the level saves it -- the boxes are not in those two. Time an A/B of the raster with the
  wind FROZEN (`wind.freeze:1`: the pages still redraw, the cluster counts stay put) -- with the wind running the two
  arms draw different counts and the EMA mixes them (a first reading put the 256-vertex mesh shader ahead; frozen, it
  was behind).
- The page scatter (`vsm_page_scatter_cs.hlsl`, every VSM page render pays it): a thread finds its (object, target)'s
  page rects and counts them, the GROUP queues those pages (a prefix sum in groupshared) and all 64 threads take them in
  turn. Walked by its own thread, each pair waited on its atomic's return before its write and a straddling local caster
  on 341 page-table reads: grove 0.21 -> 0.08 ms, bathroom 0.22 -> 0.14, the same pairs count for count. Comparing two
  runs of the scatter needs `wind.freezeAt:<s>` (and `tod.animate:0`): `wind.freeze:1` holds whatever moment it is, and
  the pages the receivers request move with the pose.
  A picture A/B of it needs the bathroom candles' flicker off and the clouds off (`cloud.enabled:0`): both move with
  time and read as a difference between two shots of the same arm.

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
