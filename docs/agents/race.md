# The Race and Its Bot

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## A Race Course and Its Bot -- Buoys, Gates, a Helmsman

2026-11-10 (owner: "design a water course, mark it with buoys (ocean_buoy) and write a bot that runs it; part of it along
the island on wind_test"). `sources/app/game/RaceCourse.h` (what a level says), `RaceTrack.*` (the gates, the line),
`RaceBot.*` (the helmsman), `Race.*` (`game::Race`, the level's race, which the Scene owns: `Scene::RaceRef` -- the next
section), `VesselStats.*` (what a boat does, from its numbers: the section after), `physics::World::SetPilot`.

- **The course is its marks.** A level object with `"raceGate": N` is a mark (a buoy); two marks with one number are a
  gate, a boat passes between them; the gates are run in the order of their numbers and the last leads back to gate 1,
  the start and the finish. No track object: move a buoy and the course moves. A boat with `"raceBot": true` names a kind of
  boat the bots race in (since 2026-11-13 a race makes its own copies: "The Race", below). Both are read when Play starts -- the editor's from the DOCUMENT (`EditorController::
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
- **Play.** A race's bots helm boats made for it (the next section; until 2026-11-13 the boats with `raceBot` themselves,
  and until 2026-11-11 they set off three seconds after Play and ran lap after lap). Take its helm (E) and the bot stands down until
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
  missed the next gates; the yacht ended on the south beach. The buoys are `ocean_buoy` at scale 2.5 (2.2 m tall) -- all
  YELLOW (`Object_5`) since 2026-11-11, one colour a course (owner; the second course is red; `Object_3` blue and
  `Object_4` green are lost on the turquoise water) --, `"buoyant": true` at y -0.3: Static
  floaters, so they rock in the editor too and a boat that hits one stops dead against it (a kinematic body; not
  violent). The boats by the shore carry `raceBot`: their kinds are the race's fleet.
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
  of the race's own (below) on the others, the fastest first (rows 16 m and 40 m before the line, 0.55 of the half width to
  either side); two boats of one asset are "small_yacht" and "small_yacht 2". Countdown: `race.countdown` (3 s), the
  player's helm answers nothing (`HoldsHelm`), the frame says 3, 2, 1, GO! (`Race::BigText`, AppController, 160 px).
  Racing: `race.laps` (3) -- gate 1 crossed once to start the first lap and that many times more. It is the player's
  race: it ends when they finish (the bots alone: when all have). Finished: the results' window (ImGui: place, boat, time,
  gap, each lap, best lap, missed gates and what they cost; Enter or Close) -- closing it frees everybody, and the zone
  waits again once the player's boat has left it. (Since 2026-11-13 the bots' boats are the race's own: below.)
- **The race's boats are its own; the shore's are the player's** (2026-11-13, owner: "at the shore stand as many boats as
  there are to drive; when we board one the others stand on a hard anchor; the race's boats just spawn, not the ones at the
  shore"). `raceBot` on a level boat now names its KIND as one the bots race in (`RaceCourse::fleet`: the assets, each
  once, in the level's order; Inspector > Race "Races as a bot"); the boat itself stays where it is. A race begins in
  `Phase::Starting`: it asks the scene for its boats (`Race::TakeSpawns` -> `Scene::SyncRaceBoats` at the hand-over: a
  staticMesh of the asset through the object registry, `AddInitializedObject`, no editor identity, never saved) -- three
  beside the player, four for the bots alone, one for `race.solo`, the fleet's kinds in turn, or the editor's grid --,
  lays the grid once their hulls are in the world (`FinishStart`, five seconds at the most), and takes them out when it is
  over: the results closed, called off, Stop (`Race::DespawnBots` -> `TakeDespawns` -> `Scene::RemoveRuntimeObjects`: the GPU
  idle first, then `physics::World::ForgetRemoved`, so nothing hands out their pointers). `VesselSpray` gives a new boat the
  emitters of one gone, or every race would add four. `get_readouts` race `fleet`, `spawned`, phase `starting`.
  wind_test's shore: one boat of each kind (small_yacht, fast_boat_13, police_boat; the second small_yacht is gone).
- **A boat nobody steers stands on a hard mooring** (`physics::HoldMooring`, `World::Impl::PushVessels`): no helm, no bot,
  its own anchor up -> once it has slowed to `physics.mooringSettle` (0.6 m/s) its place along the water and its heading are
  held by critically damped springs on the velocities (`physics.mooring` 2.5 rad/s, `physics.mooringTurn` 2.0; 0 = drift,
  before); the waves still heave, pitch and roll it. Taken (E) or piloted it lets go; left, it coasts and moors where it
  stops; `PlaceVessel` and Stop moor it afresh. Developer Controls > Physics "Boats nobody steers". Measured (wind_test,
  Play, 20 s): the three shore boats moved 3-5 cm (they drifted metres before). Checked live (scratchpad-style
  `fleet_check.py`): race_bots on reef -- vessels 3 -> 7, the shore's three where they were; results closed -> 3; race.start
  then Stop -> 7 -> 3; the session log clean.
- **A bot has one of three levels** (owner: "give the bots three levels of difficulty", "and when you spawn them write
  over each one's head which it has"). A level CAPS the bot's speed on the straights at a share of its boat's top speed:
  `bot.easyPace` 0.8, `bot.mediumPace` 0.9, hard flat out (`RaceBot::SetDifficulty`; the player's own autopilot is hard;
  `bot.pace` the same cap for every bot). Its bends are a hard bot's: until 2026-11-12 the level was a share of every
  speed of the line, and slowed in a bend these boats lose their rudder (its push is (speed + wash)^2, the wash the
  throttle's) -- the owner's race on reef (he on fast_boat_13, flat out: "I just rolled the bots, no chance") had the easy
  police_boat and the medium yacht miss 4 and 6 gates, nearly all in the shore's slalom and the hairpin. Capped instead,
  one lap of reef: all easy 119.7-127.3 s, all medium 111.2-116.4, no miss; hard 105.6-111.9. A hard bot on fast_boat_13
  laps reef as he did holding the throttle (106.8 / 106.5 s): the bots are at their boats' limit; `bot.grip` 1.2 for
  bolder bends was a second a lap faster in single laps and no faster over three, with more misses (reef 4 against 1),
  so it stays 0.8. **A held throttle is as fast as the bot** (owner: "did you run the new course with the throttle held
  down?" -- I had not): `bot.holdThrottle:1` (the bots steer, the engine flat out bends and all, for measuring a course),
  one lap of reef at x4: police_boat 104.8, fast_boat_13 105.9, the yachts 110.4 / 116.1 s, one gate missed -- the hard
  bots in the same session 102.6-113.3. Nothing on either course asks a boat to lift: the rudder pushes by the wash and
  every force of the hull goes as speed squared, so a boat's circle hardly changes with its speed and slowing turns
  nothing tighter. Braking would matter only with a side grip that falls off on the plane (not there). `race.difficulty`
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
  (a race now: with the player if they hold a helm, the bots alone if not), `race.close:1` (the results away), `race.solo:N` (the next race's bots: only the N-th of the grid; the time trial, in "The Bot
  Drives by Sight").
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

## Two Courses in wind_test -- "reef", One Colour a Course, Watching a Bot

2026-11-11 (owner: "make one more course, harder than this one, and race the bots on it"; on the first cut, two straights
and two hairpins: "you just made a sausage -- study race courses and do it properly"; "approve the trajectory with me";
"hang the camera on one of the bots when you run them"; "each course its own one colour of buoys"; "one more buoy with a
white albedo to tint later").

- **A level holds several courses.** A mark's `"raceTrack": "<name>"` says whose it is (absent = the level's first, the
  unnamed one, shown as "classic"); each numbers its gates from 1 (`RaceCourse::TrackNames`: the unnamed first, then by
  name; `RaceTrack::Build(course, track, report)`). In Play every course's start zone is lit and the one a helm drives
  into is raced (`Race::SelectCourse`: the lines are rebuilt); a script's `race.start:1` runs `race.track` (0, 1, ... in
  that order). The HUD, the results and `get_readouts` race {track, tracks [{name, gates, zone}]} name it. Inspector >
  Race "Race track" (a mark with a gate number; every selected mark, one undo entry), MCP `setRaceGate {gate, track}`,
  `place` items' `raceTrack`; the editor's overlay draws the selected mark's course (a bot's boat: all of them).
- **One colour a course**: classic all yellow (`ocean_buoy_node_Object_5`, 42 marks, its meshes swapped by MCP
  `replace` -- every other key kept), reef all red (`Object_2`, 40). The start is the lit zone, not a colour.
  **`ocean_buoy_white`** (`models/ocean_buoy/ocean_buoy_white.mesh.json`, Object_5's geometry, material
  `ocean_buoy_white`, albedo `material_white_baseColor` -- the yellow paint made white with its wear kept, stripes black,
  metal neutral grey: scratchpad-style `buoy_white.py`, BC7 by `test_cube.exe --import <a staging folder>`): tinted per
  object by its `baseColor` times the texture -- Inspector "Tint" (new: the object's baseColor, "Colour" when Use Albedo is
  off), MCP `setSurface {tint: [r, g, b]}` (keeps the texture on; `albedo` replaces it, the two at once refused).
- **"reef"**, anticlockwise (classic is clockwise), north-east of the island in deep water, 40+ m from classic's buoys,
  laid as a circuit is (F1H2O: 1.8-2.2 km on 5-6 pins, one long straight braking into turn 1 where the passes are, mostly
  one way round with one or two the other; circuit design: straight braking into a slow corner, sequences that keep the
  boats close, asymmetry): the start / finish on a ~400 m straight north -> T1 a tight left of 87 degrees (r 62) -> a
  fast dogleg right-left along the north -> T3 a left of 116 (r 65) down onto the shore -> the shore: a long right round
  the island's east side with a slalom of 4 gates (7 m either side every 78 m) -> a hairpin of 152 degrees in two apexes
  back onto the straight. 20 gates, 20-22 m wide (the start 26), the line 1551-1562 m. Scratchpad-style
  `track2_design.py` (corner vertices filleted with their radii, gates on the arcs and straights, depth / classic-buoy /
  turning checks; `depth_map.py`: the sea floor by MCP `physics_raycast` on an 8 m grid), `present_plot.py` (the plan
  the owner approved), `track2_race.py` (place / RELAY: delete the group and place again, races, the lines' tightest
  places, SAVE only past MIN_R).
- **A slalom gate on a corner's apex adds to the corner.** The first reef put its slalom gates at the shore's corners'
  apexes: the bots' line turned at r 40 there (drawn at 150), police_boat (a 64 m circle) slowed to 9.9 m/s and still
  missed, every race lost 4-5 gates. Laid by distance along the centreline instead, the tightest place of every line is
  r 52 at T3 (a tight corner at the end of the fast section, the boats take it clean at 11.2-11.6 m/s). `RaceBot` logs
  where its line turns tightest (`RaceLine::TightestRadius(where, station)`, "before gate N") and names the boat on a
  missed gate; `get_readouts` race bots[] `lineRadius`.
- **Harder than classic, measured** (the four bots, all hard): the line's slowest place classic 86-100 % of the boat's top
  speed (fast_boat_13 flat out all round), reef 60-85 %; tightest line r 82-87 against 52; police_boat 16.0 against 14.4
  m/s average. Reef, one lap at x2: police_boat 107.7 s, fast_boat_13 106.9, the yachts 111-112, no miss; two races of 3
  laps: police_boat 311.2 / 310.9 s (first both), fast_boat_13 312.7 / 316.7, the yachts 321-333, one gate missed a
  race (the yacht at the shore's first gate, fast_boat_13 at T1's entry, in the pack).
- **Watching a bot: `race.watch`** (N = the N-th boat on the grid, 1-4; outside a race the N-th boat with a bot; 0 off;
  Developer Controls > Physics "Watch a bot"): the helm's chase camera (`game::VesselCamera`) on that hull, set in
  `Scene::Sync` from the pose the frame draws it at, a cut on a change of boat; not while the player holds a helm. The
  bots' HUD line says "CAMERA: <boat>, P<n>". Scripts run with it on (and x2 rather than x4) for the owner to watch.

## Race Tracks in the Editor -- a Course and a Gate Are Their Marks

2026-11-13 (owner: "make the tracks entities in the editor: in the outliner, a section in the Inspector, the gates edited
in the viewport"). Nothing new is saved: a course is still its marks ("raceGate", "raceTrack"), and the editor shows views
over them (`editor/scene/EditorRaceTracks.*`: `Collect`, `SelectedGate`, `SelectedTrack`, `FrameOf` / `Place`, the edits).

- **Outliner**: "Race Tracks" above the groups -- each course ("classic" for the unnamed one, its gates and length), each
  gate in the order it is run (gate 1 "start", a number with other than two marks orange, "left out"), each gate's marks.
  A mark is listed there whatever group it carries. Clicking a course selects every mark of it, a gate its two (Ctrl adds).
- **Viewport**: a click on a mark selects its GATE (Alt+click: the mark alone). A selected gate takes one gizmo of its own
  at its middle, X along the line between the marks: Translate moves it, Rotate turns it about the vertical (each mark's yaw
  with it), Scale (X) widens it about its middle. Live while dragging, one undo entry. While a mark is selected every gate
  of its course shows its number over its middle; the selected gate is drawn bold with an arrow the way a boat passes.
- **Inspector**: a selected gate -- its width, its line's turn and its middle as drags (live, one undo entry a drag), the
  distances to the gates either side, previous / next / whole course, "Insert a gate after" (copies of its two marks
  halfway to the next gate, the later gates renumbered, the new gate selected) and "Delete gate" (the later ones renumbered
  back). A selected course -- its name (a name another course has is refused), length, gaps and odd gates, "Reverse
  direction" (gate 1 stays the start, 2..n become n..2) and its gates as a table to pick from. One mark -- buttons that select
  its gate or its course.
- Gate: `intent_regression TestEditorRaceTracks` (the courses and their gates collected, a gate and a course recognised in the
  selection, width and turn about the middle with the yaws, rename, reverse, each one undo entry that puts it back). Insert
  and delete make and remove runtime objects (DuplicateObjectCommand's road) and are not in the gate.
- **A bots' race from the course** (same day, owner: "set in the editor for a track how many bots and of which level race,
  with the camera hung on any of them"). The course section's "Bot race": the laps, four places, each with a tick, a kind
  of boat (the fleet's: the kinds that carry `raceBot`), its level (Easy / Medium / Hard) and a camera radio ("No camera: yours");
  "Race N bots (Play)" starts Play and the race on THIS course with a boat of each ticked kind made for it, at its level
  (`editorrace::RequestBotRace` -> `game::RaceGridRef()`, taken by `Race::Tick`: `BeginStart(&grid)`, the fastest in front
  as always). The setup is the editor session's, per course, not saved. MCP `race_bots {track, laps, boats: [{kind, level}],
  level, camera}` (a kind by its asset's name, `police_boat`, or path; camera a place 1-4 or a kind) is the same road
  (refused while playing, without a course of three whole gates, for a kind not in the fleet, a camera off the grid). Gate: `TestEditorRaceTracks` (the grid as asked, every refusal), `TestMcpDrivesTheSameRoad`
  (race_bots on a level without a course starts nothing).
- **Watching a boat in Play** (owner: "a small UI when I hang on a bot: pick another bot, the debug drawing tick; camera modes
  -- third person, from the cockpit, cinema -- and the same when I drive"). On foot, **Tab** hangs the chase camera on the next
  racer (the next bot boat outside a race; past the last, yours again: `Race::WatchNext`, input binding `WatchNext`); while it
  hangs on one the mouse is free and a panel stands top right (`PlaySession::DrawSpectatorPanel`): the racers in their places
  (radio rows, no popup), "My camera", the view, "Bots' vision" (`bot.debug`, also in Developer Controls > Physics > Race bots
  "Bots' vision (debug)" -- it was labelled "Draw the course", which stopped describing it). **V** cycles the view -- on a
  watched boat and at your own helm alike (`game::VesselCamera::View`: Chase "behind", Helm "cockpit", Cinema; the HUD's helm
  line names it; `player.testView` cycles too). The cockpit on any boat: the asset's `helmEye` through the hull's frame from
  its forward and up (`game::VesselPose`, `PoseOf(HelmState | VesselHull)`). Cinema is a director: a shot every 5-8 s of the
  world's clock, never the same kind twice running, a side picked each time -- a camera standing ahead the boat storms past
  (it cuts once the boat is 35 m by), tracking abeam at 16 m, a drone 30 m up turning slowly, low over the water behind, ahead
  of the bow looking back; every cut resets the temporal history, the aim follows the boat at 0.12 s so the heave does not shake
  it. `--set` / MCP `camera.boatView:0|1|2` sets both boat cameras (a script's V). Checked live (scratchpad-style
  `spectator_check.py`: race_bots on reef with three boats at three levels, the camera on police_boat, each view shot with the
  panel, then race.watch:2).

## What a Boat Does -- Its Card, the Three Roles, a Bot That Knows Its Boat

2026-11-11 (owner: "which characteristics of a boat affect its handling? try to balance the boats -- one heavier but more
powerful, another turns better -- and when you walk up to a boat in Play a table of its characteristics is drawn over
it", then "teach the bot to take the characteristics into account so it drives any boat well"). `game::StatsOf`
(`sources/app/game/VesselStats.*`): one model, out of the vessel's own numbers, for the card and for the bot.

- **What moves the handling** (mesh.json "vessel"): `thrust` against `surgeDrag` is the top speed (thrust = 0.77
  surgeDrag v^2: a planing hull is 0.77 in the water); `mass`, `throttleRate` and the hump (`humpDrag`, `humpSpeed`,
  `planeSpeed`) how fast it gets there; `rudderForce` x (speed + `wash` sqrt(throttle))^2 x sin(`rudderDeg`) is the
  rudder's push -- with `yawDrag` / `yawLinear` and the keel (`swayDrag`, `swayLinear`, `swayLever`: the water's push on
  the sliding hull's side, m v w + the rudder's, opposes the turn) its circle -- under gripSpeed; past it `sideGrip` x
  `sideGripPlane` of g sets it (the next section but one); `rudderRate` how fast the helm answers;
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
- **The three roles** (written into the two mesh.json; small_yacht is as it was). The circle is flat out, so since the
  side grip (the next section, 2026-11-12) it is the grip's; the circle under the rudder alone was 112 / 128 / 61 m:

  | | weight | thrust | top | 20 kn in | circle flat out | grip on the plane | draft | in a sea |
  |---|---|---|---|---|---|---|---|---|
  | small_yacht, the all-rounder | 5.5 t | 21.2 kN | 34.5 kn | 4.1 s | 162 m | 0.27 g | 0.38 m | out of the water 17-21 % of the open leg |
  | police_boat, heavy and powerful | 7.5 t | 25.5 kN | 32 kn | 5.6 s | 172 m | 0.24 g | 0.71 m | 1-8 %: it ploughs through |
  | fast_boat_13, light and agile | 1.9 t | 9 kN | 32 kn | 3.1 s | 117 m | 0.33 g | 0.39 m | 17-21 %, and it is thrown about (below) |

  small_yacht (2026-11-12, owner: "raise small_yacht's top speed by 10 %"): its card's top speed 31.3 -> 34.5 kn by
  `game::SolveCard` (MCP setBoatCard on a copy, the numbers read back and written into its mesh.json), the other bars held:
  thrust 22000 -> 21200 N, surgeDrag 110 -> 87.56, rudderForce 120 -> 123.7.
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
  boat, longer = better (top speed, acceleration "20 kn in 5.6 s", handling "a 172 m circle flat out", sea keeping "7.5 t, 0.71 m
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
- **The card is dragged as it reads** (same day, owner: "where is the top speed? I see the card over the boat in the
  game -- I want to drag these; if one moves a set of parameters, that must happen by itself; a separate zone for casual
  editing"). Inspector > Boat opens on the card's four bars -- Top speed (kn), Acceleration (s to 20 kn), Handling (m
  across its circle), Sea keeping (%) -- each a drag against its asset's value, the numbers behind it in collapsed
  groups below (showing what a bar drag moves, live). `game::SolveCard` (VesselStats.cpp): each bar has ONE number,
  solved in order so the others hold -- sea keeping -> the mass (the draft is the hull's), acceleration -> the thrust,
  with the drag ahead tied to it by the top speed (surgeDrag = thrust / (0.77 v^2)), the circle -> the rudder force --
  by bisection on the log (48 rounds; `ToTwentyKnots` now finds its crossing inside its 0.02 s step, so the model is
  smooth), each number kept in its key's range and to four figures. Every selected boat is solved from its own numbers,
  one undo entry. `game::SeaKeeping` and the bars' scales (`TopSpeedBar` ...) are the card's in Play and the Inspector's
  alike. MCP `run_action setBoatCard {topSpeedKn, secondsTo20Kn, circleM, seaKeeping}` (any of them; the status says
  what the boat reads after). Gate: from the yacht's numbers each bar moved alone (+10 % top speed, a second sooner, a
  circle 30 % tighter, +10 points of sea keeping) reaches its value with the other three within 0.5-1 %; heavier gets
  more thrust and rudder; a bar out of reach stops at its range with the rest held. Live (police_boat): 36 kn -> thrust
  25500 -> 24590 N, drag 120 -> 99, 5.6 s and 128 m held; sea keeping 91 -> 100 % -> 8900 kg, 28130 N, the rudder up,
  the three held; a 100 m circle -> the rudder alone; the bots' race on it 96.5 s, first, no miss; three undos back to
  the asset's.
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

## The Side Grip -- a Hull Lets Go on the Plane, and the Bot Learns Its Own

2026-11-12 (owner, after winning every race holding the throttle down on the light boat: "make the boats' side grip fall
with the speed, if physics allows; put it in the physics model and the characteristics' influence", then "teach the bot
now"). Until then the rudder's push ((speed + wash)^2) and every force of the hull (speed^2) scaled alike, a boat's
circle hardly changed with its speed, and braking for a bend bought nothing.

- **The physics** (`PushVessel`, `VesselSettings::sideGrip` 0.6 g, `sideGripPlane` 0.45 of it on the plane; mesh.json
  vessel keys, Inspector > Boat "Turning", `--set vessel.sideGrip*`): the net force across the hull -- the water on its
  side and the rudder pushing its stern out -- is held to sideGrip x the weight, falling to sideGripPlane of it by the
  planing share (humpSpeed -> planeSpeed), as a tyre's mu N: a planing hull bites with less of itself. N is the WEIGHT:
  on the plane the water still carries all of it (the speed's lift the share the volume does not), so not times the
  immersion (that measured 0.5-0.66 in a turn at speed and took a third off a second time); a hull leaving the water
  (immersion under `kGripContact` 0.3) lets go with it. What a turn asks past it is taken back at the centre of mass:
  the boat runs wide (understeer), the keel's moment kept, so it points along its wider path instead of spinning. 0 = no
  limit (before). Physical: a displacement hull's side force grows with its speed squared and is never short; a planing
  hull's is the lift that carries it, tilted -- bounded by its weight.
- **What it does**: under `VesselStats::gripSpeed` (where the rudder's turn meets the grip, ~10-12 m/s here) the circle
  is the rudder's and hardly changes with the speed; past it the grip's, v^2 / (g x sideGrip x sideGripPlane), growing as
  the speed squared. So a boat brakes for a bend and powers on through it. The rudders were doubled with it (small_yacht
  123.7 -> 247.4, police_boat 125 -> 250, fast_boat_13 60 -> 90): the grip now sets the circle flat out, and a boat slowed
  for a hairpin turns as tight as a real one (the yacht at 0.35 throttle hard over: r 30-34 m at 4 m/s; flat out r 78 m in
  a calm, 125 m in wind_test's sea). The model (`game::GripAccel`, `TurnRateAt`, VesselStats.cpp) equals the sim in a
  calm to the metre (the yacht's circle flat out 90.5 / 90.5 m, fast_boat_13 83.0 / 82.9, before the rudder change;
  scratchpad-style `grip_live.py`); a 1 m sea takes ~25-30 % of the grip (the hull out of the water), which the bot
  measures (below). The card's Handling is the flat-out circle (`HandlingBar`: 75 m full, 250 m empty), and its drag
  (`SolveCard`) moves the rudder force and the grip together by one factor.
- **The bot** (`RaceBot`): (1) its line's speed in a bend is the fastest at which `bot.grip` of what the boat turns AT THAT
  SPEED (`CanTurn`: the rudder's, or past gripSpeed the grip's) makes the bend (`TurnSpeed`); a bend tighter than it makes
  at any speed is taken at the speed it turns tightest at. (2) The same for the turn it needs NOW, to its aim: a boat off
  its line asks a tighter turn than the line does there, and flat out with the rudder over it only ran wider (the first
  run of reef v2: 75 m outside the esses at 14 m/s, the line asking 17.5). (3) Over the line's speed it coasts even with
  the rudder over -- the reach sees a bend 50 m off, so the rudder is over before the bend is, and "rudder hard over ->
  full ahead" kept every boat at 15-17 m/s into T1 --, never astern (that takes the rudder's wash); full ahead in a hard
  turn only under gripSpeed. (4) It MEASURES its grip (`Status::gripSeen`, `get_readouts` race bots[] `gripSeen`): with
  the rudder hard over past gripSpeed, at a throttle whose rudder could turn it harder than the grip ever would, for 1.5 s,
  the way's turn (velocity heading, not the bow: the hull slides 20-33 degrees) x the speed against the model's
  GripAccel, followed over 4 s; the line's speeds follow it by steps of 0.04. Kept through a race's Reset. Reach and the
  "straight" share use the circle at the CURRENT speed. `bot.holdThrottle:1` (the engine flat out, the bot steering) is the
  control.
- **Measured** (wind_test's 1 m sea, the four bots at their hardest, one lap at x4; scratchpad-style `track2_race.py`,
  `race_trace.py` + `trace_show.py`): reef v2 laps 153-175 s, 4-7 gates missed a boat, none lost; classic 108-125 s,
  0-4 missed; reef held flat out 138-164 s a lap but 9-11 missed and 1-2 put back each, 40-60 s worse with the penalties.
  The first run of reef v2 with the grip and the old bot: 10-12 missed and every boat lost at T1 / the north carousel.
  Still open: the bot plans on the model's rudder at full wash, and at ~10 m/s on part throttle the sim turns wider
  (r ~65 against ~38); the misses left are in the esses and the shore's slalom.
- **The bot's memory** (`game::BotMemory`, same day, owner: "a bot with saved knowledge", "make the table"): the TURN MAP, one
  file per boat asset and level, `data/bot_memory/<level>/<boat>.json` (written when a race ends and when Play stops;
  `RaceCourse::level` names the level; every bot at the helm of such a boat shares it): hard over for a second with the
  power on (throttle 0.6 and over), the path's curvature at that speed (1 m/s bins, trusted after 3 s of samples), and
  `RaceBot::CanTurn` takes it in for the model as far as it trusts it. It is the BOAT's (and the sea's), not a course's.
  POWER ON ONLY: learned while coasting it said the boat turned wider than it does (no wash past the rudder), the bot
  planned slower, coasted more and learned wider still -- its target under 7.5 m/s for 26-47 % of a reef lap. A COURSE
  MEMORY (a factor on the line's speed every 10 m, down 6 % before a place it went 5 m wide or missed a gate, up 1.5 % for a
  clean bin) was built the same day and taken out: five races of two laps on reef cut the misses (10-12 -> 2-9 a boat) and
  slowed every lap (150-187 -> 165-190 s), lowering stretches where speed was not the cause; the bot plans by sight instead
  (the next section). `bot.learn` (1; 0 = the model alone), `bot.forget:1` (the level's files emptied); `get_readouts`
  race bots[] `memoryTrusted`. Gate: `intent_regression TestRaceCourseAndBot` (the map between its bins, its version, the
  file round trip, a bot planning by a map of r 200). Trap: a range-for over `json.value(...).items()` of a temporary
  dangles (an access violation in Load, found by the minidump's stack scan).
- **reef v2** (same day, owner: "narrow the gates by 10 %, bend it harder, longer, harder corners", then the shore
  section 50 m east, "the track is fine"): 20 gates, 18-23 m wide, the line 1797-1802 m, carousels north-west and south,
  its tightest line r 32-33 m before gate 18. Saved in wind_test 2026-11-12 (the group "Race Track Reef", red).
- **Gates**: physics `vessel` (flat out the hull holds its grip across its way, no more, and runs wider than at 0.45: r 72
  / 41 m; the arm: without the grip 40 / 41 m), `intent_regression TestRaceCourseAndBot` (the grip sets the flat-out
  circle, the rudder alone 0.76 of it; past gripSpeed the circle grows with the speed squared; SolveCard's circle moves
  both).

## The Bot Drives by Sight -- the Next Gates, Its Boat, the Others

2026-11-12 (owner: "a bot that sees the next N gates, judges the situation and passes them as well as it can, so it does
not learn each course", then "make the bot on sight of the gates, knowledge of its boat and sight of its rivals").
`game::RacePlanner` (`sources/app/game/RacePlanner.*`, pure, its own checks), driven by `RaceBot::MakePlan`.

- **The plan** (every `bot.planEvery` 0.1 s, at once when the gate ahead changes or the boat is past the plan's middle): a
  path from where the boat IS -- its place, the way it moves (velocity, not bow), its speed -- through the next
  `bot.planGates` (4) gates, a cubic Hermite through where it crosses each, the directions Catmull-Rom's scaled by each
  piece's own chord (scaled by the neighbour's, the short piece from the boat to a near gate took an S-bend). Each crossing
  is an offset in its gate's window: `halfWidth - (halfBeam + bot.planMargin)` (4.5 m), the gate it is about to cross
  (within 60 m) `- (halfBeam + 1.5 m)` -- held to the margin there, a boat already safely inside the gate was dragged to its
  edge in the last 30 m and braked to 5 m/s for the S. A gate its way carries it through inside the window within a second
  (15 m) is passed as it goes and left out. Search: coordinate descent over the offsets from the last plan's (shifted by a
  gate passed) and from the middles, every step halved, four sweeps (~35 paths a plan).
- **Its cost** is time to the last gate plus penalties: speeds from the curvature (the boat's `SpeedFor`: a 64-entry log
  table of `TurnSpeed` -- the model and its turn map --, rebuilt with the line), braked back from every slow place (its hull's
  drag `0.77 surgeDrag v^2` and its engine 0.7 astern, x `bot.planBrake` 0.6: speed-dependent, a constant 1.9 m/s2 let a slow
  bend come too late), accelerated forward from its speed now (`0.85 thrust/mass (1 - (v/top)^2)`); a bend past what it
  makes at any speed (300 s per m per 1/m), a place it is too fast for (0.5 s per m per m/s), the first 6 m excepted (the
  helm's: the way it moves now); RIVALS -- every other boat within 200 m run on at its velocity for 6 s, a place it reaches
  when one will be there costs 1 s per m at full overlap, inside `0.5 (halfLengths) + halfBeams + bot.rivalClearance` (3 m);
  and 0.2 s x (offset / window)^4 for a crossing at a window's very edge (on a circle the shortest path hugs every inner mark).
  THE BUOYS are rivals that stand still and count all along the path (4 s per m at full overlap): the marks of the gates it
  plans through and of the one before, at `halfBeam + 0.35 halfLength + 1.5 m` (a hull sliding 20 degrees sweeps wider
  than its beam). Before them 10 of 13 knocks on a two-lap reef race were buoys 1-5 m from the hull's middle (the plan
  only kept its crossings clear; between gates it cut over the next inner mark); with them 6 of 10, the time coming round
  2-16 % -> 3-8 %. The gate it is about to cross takes `halfBeam + 3 m` (1.5 m hit marks).
- **The helm by it**: pure pursuit on the plan (its reach without the off-the-line extension: the plan starts at the boat),
  the bend for the slide from the plan's curvature, the target the plan's LIMIT half a second on (the most it may have
  there: NOT the speed it expects to have, which is only a little over its speed now -- steered by that it never gave full
  throttle). `TurnSpeed` (the line's and the plan's) goes down to the speed the boat turns tightest at (`recoverSpeed_`,
  ~3.5-5 m/s by the turn map), not half its top speed: under its grip's reach a slower boat turns tighter, and bends it ran
  wide of at 10-12 m/s with the rudder hard over were most of reef's misses. "The turn it needs now" (the pursuit's arc) is
  the line's only (its old floor, half the top speed): with a plan it fired 29-49 % of a lap on the arc's wobble.
- **Coming round** (owner: "a bot that hit a buoy tears off somewhere flat out instead of coming back carefully at low
  speed"): pointing the wrong way against the COURSE where it is (its line's tangent there, over 75 degrees -- not its aim:
  in a hairpin that lies 60-90 degrees off by design) or knocked (its speed 4 m/s under its half-second average) -> it aims at
  the gate ahead itself (where its plan crosses it; 25 m before it if it is past it), at `recoverSpeed_`, power 0.5 and over
  with the rudder hard over (the wash turns a crawling boat), out under 57 degrees to the course and 34 to that point.
- **Readouts**: `get_readouts` race bots[] `planned`, `recovering`, `slowedBy` (c cap, p plan, l line, a the turn it needs
  now, r coming round, j joining), `planSeconds`, `planPenalty`, `planOffset`; `bot.debug:1` draws every bot's plan (cyan,
  lighter where fast). Knobs (`--set bot.*`, Developer Controls > Physics "Race bots"): plan (1; 0 = its line, the A/B),
  planGates, planMargin, planEvery, planTurns (0), planBrake, rivalClearance, planLead (0.5), steerByRudder (0), turnPower
  (0.6), turnPowerOver (3), gripLearn (1), planBrakeBend (0), stepSteer (1), markRule (1), turnWash (0), planSwitch (0).
- **Measured** (wind_test's 1 m sea, the four bots, one lap at x4, the camera on a bot; scratchpad-style `race_trace.py`
  (MAXMISS stops a run that has gone wrong), `why_stats.py`, `misses.py`, `knocks.py`): reef v2 -- 0 / 1 / 0 missed (fast /
  police / yacht), laps 168 / 181 / 172 s, two knocks, nothing in recovery long (the first plan of the day: 9 misses on four
  boats; the line bot before the day's fixes 1-5 a boat; held flat out 9-11); classic -- 0 / 1 / 1, 115 / 131 / 151 s (the
  yacht cautious, 10 m/s its plan's mean, three knocks in the pack). x1 against x4 the same within a race's own spread (classic
  122-139 s against 118-155).
- **Tuning for speed** (same night, owner: "tune for speed"). The margins are not the lever: two laps of reef with `bot.grip`
  0.9 / 1.0 and `planBrake` 0.9 missed MORE (three of four arms stopped at 10 misses) and were no faster (161-186 s against
  168-189). Choosing each gate's crossing DIRECTION too (`bot.planTurns`, +-20 degrees of the Catmull-Rom way, a second
  parameter a gate for the descent) lost an A/B of six one-lap reef races, alternated: on 12+, 12+, 7 misses (two races
  stopped), off 11, 3, 8 -- off by default, kept as the A/B. Where the misses are (53 over that series): gate 4 (T1's apex,
  after the 400 m straight, lap 1's pack) 15, gates 3-7 (T1 and the esses) 31 in all, gate 18 (the line's r 32) 7. THE SPREAD
  BETWEEN TWO RUNS OF ONE ARM (3 to 12+ misses, laps 173-232 s) IS LARGER THAN ANY CHANGE'S EFFECT: four boats in a pack on a
  sea; judge a change by an alternated series, never by one race (a 0 / 1 / 0 lap earlier that night was luck). Laps on
  reef 165-210 s, 1-3 misses a boat a lap; held flat out 138-164 s with 9-11.
- **A time trial** (`race.solo:N`, 2026-11-12, owner: "build time trial"): `race.start:1` runs the N-th boat of the grid (1-4,
  the fastest first) alone, the other bots' boats left where they lie -- a change of the bot measured without the pack, whose
  spread swamped every change. Scratchpad-style `time_trial.py` (ARMS alternated, LAPS, BOATS, REPEATS; MAXMISS ends a trial
  gone wrong; FORGET 1 = every trial from an empty turn map, or the arms teach each other through it; a trace a trial, read by
  `incidents.py`: misses, knocks, seconds coming round and crawling). IT NAMES EVERY BOT KNOB AT ITS DEFAULT IN EVERY TRIAL:
  `bot.*` survive `reset_settings`, and three series that night ran later arms with an earlier arm's `bot.gripLearn:0`,
  `bot.planLead:1` or a since-dropped knob left set -- those arms were void. Solo is still noisy (laps 2+ of one
  arm: sd 4-17 s; an incident -- a gate circled for, a knock -- costs 20-110 s), so two trials of three laps an arm and boat
  is the least that reads.
- **Power on in a hard turn** (`bot.turnPower` 0.6, `turnPowerOver` 3 m/s; 2026-11-12). Under gripSpeed these boats turn by the
  propeller's wash past the rudder, (speed + wash sqrt(throttle))^2, and the helm coasted to the plan's speed with the rudder
  hard over: the yacht, coasting at 5-9 m/s, made circles of 55-180 m (power on: 27-44 m, its turn map); about a target of
  4 m/s its throttle went 0, 1, 0, 1 and it circled 25-37 m off its line for 10 s a gate -- three gates in a row after one
  knock. Now hard over under gripSpeed the throttle never falls under 0.6 while the speed is under the target + 3 m/s (past it,
  it still coasts: a boat that came in too fast slows first; coming round keeps its own rule). Measured (reef, 3 laps, two
  trials an arm and boat, alternated, every knob named; laps 2+ mean s, misses a lap; seconds crawling under 6 m/s hard over a
  trial): small_yacht 192.9 -> 175.2, 1.17 -> 0.83; fast_boat_13 171.7 -> 165.8, 1.17 -> 0.67; police_boat 186.8 -> 175.7,
  1.17 -> 0.67; lap 1 205 / 226 / 186 -> 174 / 171 / 175; crawling ~105 -> ~34 s. `bot.turnPower:0` = before.
- **Its grip, measured** (`gripSeen`, the night of 11-12): hard over with the power on (any throttle), over recoverSpeed + 2 m/s,
  for a second -- the way's turn against the LESS of what the model gives there (the rudder at that throttle, the still-water
  grip); the share comes into the grip only (CanTurn). Before, it asked for the speed past gripSpeed and the rudder 1.25 over the
  grip for 1.5 s, and a planned bot was never there that long: the share stayed 1, and police_boat planned reef's T1 at 12 m/s
  on a sea that gives three quarters of that grip, ran 10 m wide and hit the apex buoy. Reef's 1 m sea reads 0.77-0.89.
  `bot.gripLearn:0` = the model's grip (the A/B; a clean series of it is still owed).
- **Its engine's spool and its rudder's order** (single trials an arm, so read loosely): `bot.planLead` 1 (the least of the
  plan's limits over the stretch it covers in half its throttle's swing from full ahead to astern) against 0 missed half as
  many gates (0.67 / 0.33 / 0.67 against 1.33 / 0.67 / 1.67 a lap) and lapped 8-15 s slower; 0.5 is the default, between them,
  not measured on its own. `bot.steerByRudder` (the rudder's order a share of the rudder's own turn at this speed, 1, or of
  the grip's circle, 0; between in log): 0 / 0.5 / 1 -- the yacht 190 / 193 / 231 s, police_boat 211 / 220 / 181, fast_boat_13
  the same -- no winner, 0 kept.
- **Tried and dropped: giving a gate up** (`bot.giveUp`, the night of 11-12): when the plan through the gate ahead asked more
  than the boat turns, the plan through the gates after it with the race's penalty for the miss, if cheaper. Weighed whenever
  the plan's penalty passed 20 s it gave up 10-12 gates a lap -- T1's and the hairpin's, gates it then made -- and missed more;
  on the geometric share alone, with the gate behind it, it fired 0-3 times a trial and lapped no better (police_boat
  191 s / 1.33 misses against 176 / 0.67). A boat past a gate it did not cross is the race's already: 30 m on, the miss counts.
- **What is left** (the clean series, turnPower on): gate 4, T1's apex at the end of the 400 m straight, missed in most
  trials -- the boat brakes from 16 m/s to 12-13 by gate 3, the rudder goes hard over before it is slow enough (the reach sees
  the bend), it coasts, and runs 15-20 m outside the apex at 6-8 m/s with the rudder hard over. Its plan brakes as if the
  engine went astern; with the rudder over the helm only coasts. `bot.planBrakeBend` 1 makes the plan count no engine braking
  where the helm's rudder will be over (the bend within its reach asking over 0.17 of what the boat turns there, none past 0.4;
  RacePlan's PlanBoat::turnAt, a table by speed from the speed table): A/B (the same series' shape) 0 / 1 / planBrake 0.4 --
  misses a lap 1.05 / 0.78 / 1.06, gate 4 missed 5 / 2 / 2 times in six trials, laps 2+ 173 / 182 / 183 s (the yacht faster
  with it, 181 -> 173, fast_boat_13 and police_boat 15-19 s slower). Off by default; the miss there is a cost of its speed.
- **The helm steers in every physics step** (`bot.stepSteer` 1; 2026-11-13, owner: "the bot's algorithm must not depend on the
  frame rate"). It decided once a frame: at x4 under vsync (60 fps) it steered 15 times a second of the world, at 500 fps 125 --
  two different helmsmen. Now it hands its pure pursuit (the aim, the reach, the slide's angle, the gain, the yaw damping, the
  avoidance's offset) to the step in `physics::VesselInput` (`steer`), and `PushVessel` orders the rudder from the hull's pose
  in each step; the throttle and the plan stay the frame's. 0 = before. MEASURING FAST: `time_trial.py` FAST =
  `frame.vsync:0;dlss.mode:1;cloud.enabled:0;gi.enabled:0;skyVis.enabled:0` at `--window=1280x720`, 480-560 fps, so x12 is
  1.3-1.5 physics steps a frame. The HUD's fps had counted the SCALED dt (465 shown as 39 at x12): `Renderer::Tick` takes the
  real one.
- **What the bot sees, on screen** (`bot.debug:1`; owner: "draw the bot's vision and decisions when you run for me"): its plan
  coloured by its limit (red slow, green fast) with a fence every third point 0.25 m tall a m/s, the speed it expects (white),
  the rivals it plans round (marks orange rings, boats pink with their next 6 s), its aim (a mast and a line), its bow and its
  velocity; a card over each racing bot (`Race::DrawBotCards`; the watched one docked bottom-left): speed and target, throttle
  and WHY (`throttleBy`), rudder and why (`rudderBy`). Readout bots[] `throttleBy`, `rudderBy`, `plan` [[x, z, limit, speed]].
- **The corner drill** (`race.drill:N`; owner: "why so many runs?" -- a whole race's spread drowned every change): the race's
  boats are put at rest `race.drillRun` (220) m before gate N, run `race.drillGates` (3) gates and are put back, again and
  again (`race.drillTimeout` 60 s a passage). Readout `race.drill` {gate, boats [{passages, clean, contacts, timeouts, resets,
  meanSeconds, sdSeconds, meanEntry (m/s across gate N), missed[]}]}. Scratchpad-style `drill_ab.py`: arms alternated in blocks
  of 20 passages, 40 an arm and boat -- one corner in ~15 minutes at x12.
- **Tried on it**: `bot.turnWash` (the least throttle whose wash brings the rudder's turn to the grip, held in a hard turn) --
  whole laps at x12 2.0 misses a lap for the yacht and police_boat against 0.83 / 0.33; `bot.planSwitch` (a warm plan kept
  unless a new one is cheaper by that many seconds) -- no gain; both 0. `bot.planBrakeBend` on the drill: T1 (gate 3) misses a
  passage 0.25 -> 0.05 (the yacht), 0.225 -> 0.05 (police_boat, 1.9 s faster, 7.5 m/s across the gate against 9.6); the
  hairpin (gate 16) worse, more timeouts -- still 0: it wants choosing by the corner.
- **The helm's own buoy rule only for a contact about to happen** (`bot.markRule` 1, 2026-11-13; 0 = before). With a plan
  the marks are already rivals it keeps clear of, and it crosses a narrow gate 4.5 m from a mark on purpose; the helm's rule
  kept its line's 6 m (`bot.margin`) from every buoy 2.5 s ahead and fought the plan: through reef's shore slalom police_boat
  went hard over one way and the other, 15-20 m each side at 5-8 m/s, "round a buoy" at every one of 25 contacts logged. Now,
  with a plan, it steers round a mark only within its half beam + 1.5 m, 1.2 s ahead. Drill, 40 passages an arm and boat
  (yacht / fast_boat_13 / police_boat): the hairpin (gate 16, 4 gates) clean 47.5 / 77.5 / 45 -> 65 / 80 / 65 %, contacts a
  passage 0.58 / 0.25 / 0.30 -> 0.30 / 0.23 / 0.18, 2.3 / 1.4 / 2.8 s faster; T1 (gate 3) clean 57.5 / 70 / 55 -> 67.5 / 55 /
  67.5 %, misses a passage 0.23 / 0.13 / 0.28 -> 0.15 / 0.33 / 0.10. The light boat lost at T1 (contacts 0.20 -> 0.48): it
  slides wider than 1.5 m past its half beam there -- the rule's room should follow the boat's slide.
- **Gates**: `intent_regression TestRaceCourseAndBot` -- the planner: a straight row straight through; a right angle faster
  through the gates' width than their middles; from 16 m/s it brakes for it; into a hairpin it cannot brake for at 16 m/s a
  penalty, at 8 less; a boat lying in the next gate's middle passed on one side, one far off costs nothing; the toy boat's two
  laps through every gate (its slip 0: a toy hull does not slide).

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
- **It stays until it is set back** (a `frame.*` switch; since 2026-11-26 `reset_settings` puts it back too): a script
  sets it back to 1 in the call that ends its run. The HUD's `Time:` line says `WORLD x4 (frame.timeScale)` while it is not 1, and
  `get_readouts` carries `timeScale`. A script that times with its own clock (a sleep, `time.time()`) is wrong by the
  scale: read the race's clock, a lap's seconds, the readouts' own times. A `screenshot` stalls the frame as it does at
  1 -- the world goes on by at most 0.1 s a frame.
