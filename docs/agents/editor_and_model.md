# The Local Model, the Editor's MCP, Photographing the Editor

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

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

**The watchdog HOLDS its server** (2026-11-24, the code review 10.15): one handle, opened when the watch starts
(`LlmReaper.cpp WatchedProcess`) -- "alive" is a wait on it, and the kill goes to it. It used to open "pid N" anew every
second and again before every kill: once the last editor has closed nothing holds that number, so a server that died by
itself could be replaced under it by any process (during a build, within the second), which then did not answer
`/metrics` and was terminated as "the server". **And the project links again**: `Log.cpp`'s writer thread tags its
memory (M3) and `tools/model_reaper.vcxproj` does not compile the tracker, so it had not linked since -- nobody rebuilt
it, the editor went on starting the exe an older build had left, and the first rebuild failed with two unresolved
externals and took that exe away. `model_reaper_main.cpp` gives `mem::PushTag` / `PopTag` empty bodies. Probe:
scratchpad-style `reaper_probe.py` -- stand-in servers, never a real one: one dies (the watch ends within seconds and
ends nothing), one stops answering (the HELD process is terminated), one gone before the watch began beside a bystander
(the bystander lives).

**The model's replies and paths are not trusted** (same day). A request's worker is a detached thread with nothing above
it to catch: its body runs under `RunRequestWorker` (an exception = a failed request with a line in the log, not
`std::terminate`), and llama-server's reply is read by `llmreply::Read` -- `content` must be a STRING (a `contains()`
was the whole check before `get<std::string>()`), the timings are read by type. The warmup's worker holds the prompt
counter by `shared_ptr`, not this object's address: its answer lands up to twenty seconds later, by which time the editor
may be gone. `TOOL: read` / `ls` paths are proven inside the repository by COMPONENTS (`EditorRepoSearch.cpp InsideRepo`,
`lexically_relative`): compared as text, `../test_cube_backups` and every worktree `test_cube_<name>` passed. Gates:
`intent_regression` `TestModelRepliesOfAnyShape`, `TestAToolPathCannotLeaveTheRepository`.

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
`render.*` -- are not in the document; since 2026-11-26 (architecture plan R2.n) `reset_settings` first puts back every
name `set_settings` changed to its value before, so they no longer stay as last set -- a name without a reader stays
and the answer names it); `get_settings {names: "a.b;prefix."}` reads any name; `get_readouts` returns the metered/adapted EV100, the pre-exposure,
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
**`set_settings` is transient; `reset_settings` undoes it**: a knob stays as set until `reset_settings` (or a
relaunch), so a script that toggles one and dies, or a later script that never names it, measures a different scene.
Start every series with `reset_settings` -- since R2.n (2026-11-26) it puts back project-wide knobs too. It cost a wrong conclusion on 2026-10-24: a leftover `sky.environmentLighting:0` lit
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
- Browser requests are refused, three ways (`editormcp::HttpRefusal`, in the gate): `Origin` / `Host` must be this
  machine (403), and **the body must be declared `Content-Type: application/json` (415, 2026-11-18)** -- `Origin: null`
  is let through and is also what a sandboxed iframe or a data: page sends, and a page of ANY local web server has a
  local Origin, so those two alone left a page a blind way in; a page cannot declare JSON without a CORS preflight, and
  OPTIONS gets 405. Every MCP client sends it; **a script must too** (`headers={"Content-Type": "application/json"}`:
  python's urllib does not by itself -- the 415's body says so). Checked live on a socket, seven arms.
  A second editor cannot bind the port and says so in its panel -- and a SCRIPT that waits for the port then talks to
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
