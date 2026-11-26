# Repository Instructions

Rules for every task, most important first. Each subsystem's full notes -- design, measurements, history, the reasons
behind these rules -- are in `docs/agents/*.md`, moved there verbatim on 2026-11-24. The index at the end maps every
old section title to its note, so «AGENTS.md "<title>"» still resolves. Read a subsystem's note before changing it:
its rules bind too.

## Edits

- Keep each file's line endings; C++/Windows project files are CRLF. `sed -i` and most write tools emit LF: check the
  bytes before finishing (no lone LF or CR) and normalize a touched source file to CRLF.

```powershell
$b=[IO.File]::ReadAllBytes((Resolve-Path $f).Path);$lf=0;$cr=0;for($i=0;$i -lt $b.Length;$i++){if($b[$i] -eq 13){if($i+1 -lt $b.Length -and $b[$i+1] -eq 10){$i++}else{$cr++}}elseif($b[$i] -eq 10){$lf++}};"$f loneLF=$lf loneCR=$cr"
```

- A new source file goes into `test_cube.vcxproj` AND `test_cube.vcxproj.filters`.
- Kill only your own PIDs (`Get-Process test_cube, MSBuild` first). A Debug run that may raise an assert window runs
  on a separate desktop, never the owner's screen.

## Build and gates

- `MSBuild test_cube.vcxproj /p:Configuration=<cfg> /p:Platform=x64 /m /nologo /v:minimal > <log>` (VS 18:
  `C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe`); read the log at once.
  Every change builds Debug (strictest: debug layer), Release_Editor and Release; one touching boot, settings or VFS
  also Shipping, Development and Cook. `/p:TcThreadChecks=true` compiles the thread-role checks into Release_Editor
  (`TC_THREAD_CHECKS=1`; `--thread-check-selftest` proves them on); switching it rebuilds the configuration whole.
- Before a commit: `python tools/check_logging.py`, `check_render_knobs.py`, `check_gpu_alloc.py`, `check_file_io.py`,
  `check_threads.py` (a `.detach()` says where its exceptions are caught: `// detached-ok: <where>`; a
  `CreateThread`/`_beginthreadex` start: `// thread-ok: <where>`), `check_layers.py` (an include goes only down the
  layers of `docs/architecture_review_plan.md` section 4; today's exceptions are `tools/arch_layers_allow.txt`, which
  only shrinks: `--update` drops an include that is gone), `check_switches.py` (every switch the code reads is a row of
  `sources/app/BootSwitches.cpp` -- name, value, help, kept by Shipping -- and every row is read; `test_cube.exe
  --help` prints the table; a row's parse function in `app/BootParse.cpp` / `BootParseDev.cpp` reads its own switch;
  a change of a parse is proven by `--boot-dump=<file>` -- every `BootOptions` field and the debug switches' globals,
  before and after), `check_shaders.py` (its own entry lists AND every permutation of the cook's record
  `tools/dist/shader_list.txt` at 6_6: a shader that no longer compiles is a FAIL naming it) (exit code = findings).
  A layout C++ and HLSL share lives in `shaders/shared/*.h` (`shader_types.h`: HLSL's type names for C++; HLSL
  includes `"shared/x.h"`, C++ `"shaders/shared/x.h"`; the cook packs them): a cbuffer is a field-list macro both
  spell, HLSL's names; a C++ default is `TC_INIT(one argument)` (FXC has no variadic macros). Tools:
  `arch_metrics.py` (the plan's numbers: map, include graph, header reach, the counters its steps move),
  `frame_diff.py a.png b.png` (the share of pixels more than 8 levels apart; `--mask`, `--rect`, `--max`).
- Editor gate: `tools/intent_regression.vcxproj` (Debug; links the Debug game's objects by its tlog, so build that
  first) -> `x64\Debug\intent_regression.exe`, last line `PASS`; `inspector_regression` likewise. After changing
  `sources/editor/intent/`, rebuild `tools/model_reaper.vcxproj` (close its tray icon: the running exe is locked).
- Engine gates (`test_cube.exe`; verdict line in the session log + exit code): `--physics-gate[=<name>]` (with `=`),
  `--vfs-selftest`, `--scalability-selftest`, `--scalability-check`, `--scene-stress=<n>` (150 for `mem tags`),
  `--rt-audit`, `--mem-readout=30`, `--crash-selftest=<arm>`, `--throw-selftest=<site>`;
  `python tools/dist/check_gate.py`.
- A gate or test counts only once shown to FAIL without the fix: every gate has a control arm or a self-test.

## Code rules

- Events go through `LOG_*` (`core/logging/Log.h`: `LOG_INFO(logging::LogCategory::Scene, "Loaded {}", path)`),
  never `OutputDebugString`/`printf`. A line evaluated every frame only behind a gate: `LOG_*_ONCE`,
  `LOG_*_EVERY_N(n, ...)`, `LOG_*_THROTTLED(d, ...)`, `LOG_*_ONCE_PER_MESSAGE`. `WriteRaw`/`WriteRawLines` take
  preformatted text. Levels: Trace (compiled out of Release), Debug (dev state; dropped in Release), Info (lifecycle,
  finished ops), Warning (recoverable fallback), Error (op failed, process goes on), Fatal (invariant broken, stopping).
  Category by owner (`render.rt`, `asset`, `vfx`, ...).
- Diagnostics are events, not files: dumps, validator lists, self-test verdicts go to `LOG_*` (Fatal before an assert
  is flushed synchronously). No new `logs/<name>.log`. A table a script parses is an artifact -- a closed set, a new
  one needs the owner's OK -- written via `diag::WriteArtifact`/`ArtifactFile` with a mode (`PerRunTruncate`, `Append`,
  `UniqueSession`, `AtomicReplace`), never `fopen(diag::LogPath(...))`.
- Every runtime read goes through `core/vfs/Vfs.h` (`vfs::ReadAll/ReadText/InStream/Open/GetStat/Exists/List`), never
  `std::ifstream`, `_wfopen`, `CreateFile`, WIC by file name or `std::filesystem::exists` on content; a line that must
  touch the disk says `// vfs-ok: <reason>`. Paths are virtual (`/`, from the content root = the cwd in dev).
  `GetStat().version` is a loose file's write time but a packaged file's content hash.
- GPU memory via `gpu::Create*` with a tag in `core/memory/MemTags.h`; a CPU owner wraps its work in `MEM_SCOPE`;
  memory the counters can't attribute gets a provider (`RegisterMemoryProvider`).
- Game code writes a render knob's `_game` twin (`core/threading/Knob.h`), never the render copy.
- JSON: nlohmann `value(key, fallback)` THROWS when the key exists with another type (`"zip": null`) or the json isn't an
  object -- `std::terminate` off a frame or thread. A file another program writes or a person edits, read in a frame or
  thread: `jsonfield::Get` (`core/JsonField.h`). Where a wrong type must be an error naming the file (level, cook
  profile, scalability table, material): `value()` inside `catch (nlohmann::json::exception&)`. Gates:
  `TestJsonFieldsAreReadWithoutThrowing`, `TestAMaterialOfTheWrongTypeIsAnErrorNotACrash`.
- A loader that refuses is half the fix: find who gets its `false` and what they bind instead -- in a live run, not by
  reading the loader (a missing sky file died in the driver after a correct refusal).
- `tc::inl_vector` aborts on overflow in every build: a caller that can exceed it checks `size() < capacity()` and
  logs what it dropped.
- Every object removal in `Scene` ends with `physics_.ForgetRemoved()`.
- A descriptor table that is the same for every view or draw is staged once a frame (the ring: 4096 a frame; an
  overflow throws).
- Never write a runtime cap into a struct the editor saves (Ctrl+S writes it into the level): a cap is a runtime
  ceiling.
- A UE length is in centimetres: /100 (an area /1e4).
- Shaders: kill your running editor BEFORE changing a root signature, registers or a cbuffer layout -- it hot-reloads
  the saved `.hlsl` and binds it the old way (device removed). A body-only edit hot-reloads safely; a shader that
  failed at boot is not hot-reloaded. `line` and `in` are HLSL keywords.

## Threads and GPU lifetime

- `frame.renderThread` 1 (default): `Scene::Render` runs on thread `Render` while the game thread runs the next frame's
  game half (UI, editor, MCP, `Scene::Tick`). Game-half code that reaches what the frame in flight uses calls
  `threading::WaitRenderForGame("<site>")` first or hands over at `Sync`; a UI panel waits only where it reaches render
  objects, never as a whole. An edit of the scene's membership parks with `threading::ParkRenderForGame` (the flag
  `TC_CHECK_GAME_PARKED_OR_SYNC` asks, no GPU idle) and frees through `render::deferred`.
- `frame.rhiThread` 1: every queue op of a frame in flight goes through `Renderer::Rhi().Run`, never the queue; to
  need them done, `Flush` (`WaitForPreviousFrame` does).
- Input is latched per frame (`InputManager::NewFrame` right after the frame's pump).
- After any change to what the game half touches: `--rt-audit[=<s>]` (`--rt-audit-selftest` must FAIL naming
  `object #0 model matrix`); coverage via `--scene-stress=150 --rt-audit` or an editor run driven by an MCP script. A
  gate that touches the frame's order runs with `--set=frame.renderThread:0` too.
- A resource that frames in flight read is freed or recreated only at a GPU-idle point, after a wait, or through
  `render::deferred::Retire` (`rhi/DeferredRelease.h`, released once its frame NUMBER completed;
  `--retire-selftest`): a ring buffer released at once hung the GPU. For a lifetime alone, the service, not a
  `WaitForPreviousFrame` (each counted by caller: `gpu idle waits:` every 5 s, scope `Renderer: GPU idle wait`).
- A GPU copy is done by its frame NUMBER (`Renderer::IsFrameNumberComplete`), never by its slot.
- An ExecuteIndirect count buffer holds the exact count, never ~0 (NVIDIA then draws nothing).
- DLSS's feature at new sizes is released by us at the resize's GPU idle (`DlssHandler::ReleaseFeatureIfResized`):
  Streamline's own recreate defers the old one to a lambda that `slShutdown` runs after unloading `sl.dlss`
  (0xC0000005 at the exit of any run that ended within three frames of a resize).
- A bit-packed request or mark buffer does `InterlockedOr` only while the bit is clear.
- Mesh-shader PSOs stay out of the disk PSO cache (the runtime refuses them back).
- A task's exception means its `TaskSystem::FailurePolicy`, set by its creator and inherited by tasks created inside
  it: `Frame` (render-graph passes, the view culls) = Fatal naming the task, dump; `Background` (default) = Error, `Wait`
  returns false. A detached task its owner polls (a `done` flag, a claim) lands through `SubmitDetach(body, landing)`,
  which runs when the body throws too. A broken contract (a task's dependencies, a graph's capacity) is
  `InvariantFailure`, never `assert` or `throw`.

## Checking live: the editor's MCP

ALWAYS CHECK THE MCP FIRST, AND EXTEND IT when it lacks something: a tool in `sources/editor/intent/EditorMcpServer.cpp`
+ its case in `tools/intent_regression.cpp` `TestMcpDrivesTheSameRoad` (Debug for the gate, Release_Editor to use).
One open editor beats dozens of headless launches (~15 s each); headless `--shot`/`--sweep` are for gates and builds
without the editor.

- Endpoint `http://127.0.0.1:8128/mcp` (`.mcp.json`; Codex: `.codex/config.toml`, tools `mcp__test_cube_editor__*`).
  Read `editor_guide` first. Scripts send `Content-Type: application/json` (else 415).
- Your own editor: `--editor --level=<level> --mcp-port=<n>`, the port checked free first (8128 may be the owner's).
  It rewrites `imgui.ini` and `editor_state.json`: copy both before, put them back after. End it with `Stop-Process` on
  its PID, never by closing it.
- Tools: `run_action`/`preview_action` (the local model's `{action, target, params}`), `query`, `get_camera`,
  `set_camera`, `screenshot`, `undo`, `open_level`, `save_level` (= Ctrl+S: ONLY when the owner asked), `set_settings`,
  `get_settings`, `reset_settings`, `get_readouts`, `get_profile`, `get_memory`, `thumbnails`, `play`/`stop`,
  `build_dist`. In Play, `run_action`, `undo`, `save_level` and `import_asset` are refused; `open_level` refuses over
  unsaved edits unless `discardChanges` (the owner's call).
- `set_settings` (the `--set` names) is TRANSIENT; `reset_settings` undoes it -- every name it changed back to its value
  before (project-wide knobs too), then the document -- except a name without a reader (`vessel.*`, `player.test*`,
  actions), which its answer names. Start every series with `reset_settings`. `get_settings {names: "a.b;prefix."}`
  reads them (the settings registry, `app/settings/*SettingRows.cpp`). `wind.freeze:1` /
  `wind.freezeAt:<s>` hold wind + ocean; `camera.fly:<m/s>` / `camera.spin:<deg/s>` move the camera (stop them in a
  `finally`).
- MEASURE, DON'T READ A SCREENSHOT (the tone curve clips): pixel probe `debug.probeU/V/Radius` (`debug.probeSource`
  1 = before DLSS, 2 = bloom target) -> `get_readouts` `pixelProbe`. A flicker is read frame by frame:
  `get_readouts {history: N}` (<= 512 rows), never two snapshots. A look's A/B pins the exposure:
  `exposure.autoExposure:0;exposure.manualEv100:<metered>`.
- `set_camera` is a cut (temporal histories restart); `cut: false` is a fast move they follow. `screenshot {burst: N}`
  writes N consecutive frames, but each stalls on its PNG while physics runs on: use the probe history for motion.
  `screenshot {clean: true, path, settleFrames}` = scene + HUD, no panels.
- Costs: editor closed (F2), a real view, `get_profile {filter, reset}`, arms alternated twice. `dlss.mode:0|2|6`
  and `shadow.mode:0|1|2` switch live.

## Reproducing a view and measuring

- Measure, then conclude: the noise floor first (one binary twice), then the arms, alternated (A B A B) and compared on
  second runs, three runs before a verdict. Hold what moves (`--wind-freeze`, `--time-freeze`, `cloud.enabled:0`, the
  candles' flicker) and pin the exposure for any brightness A/B. Look at the picture before trusting a number: a frame
  missing a mesh measures another frame. A cost without its view and camera is not a number.
- The HUD prints `Cam: x y z, rot: qx qy qz qw`: reproduce with `--cam-pos=x,y,z --cam-rot=qx,qy,qz,qw` (applied
  after the level's `freeCameraStart`; the full pose, roll included). Never guess a camera.
- `--wind-freeze[=<s>]` pins the wind + ocean clock: two runs then differ ~0.05-0.4 % (DLSS jitter) instead of ~2.3 %;
  two values prove something animates. `--window=3840x2160` = true 4K.
- A frame diff = the share of pixels more than 8 levels apart; measure the floor (one binary twice) first. A git
  worktree lacks the untracked content (`models/**` binaries, `textures/`, `import_staging/`, `user_settings.json`):
  run from it and you render another scene.

## GPU-based validation (GBV, Debug only)

- Is my pass clean: `test_cube.exe --level=<level> --gbv --gbv-passes=<Pass>,...,Skybox --gbv-selftest=1@Skybox
  --no-streamline --shot=out.png --shot-delay=3` -- ~16 s to the first frame, validates ONLY the listed passes
  (`RenderPassToWString` names, case-insensitive; `GBuffer` covers `GBuffer.*`). A list with Lighting, Compose or
  GBuffer takes 5-8 min to the first frame: allow 20.
- Is the frame clean: `--scene-stress-gbv` (every pass, level churn, `--stress-level=<level.json>`) or `--gbv` without
  a list -- minutes to tens: ask the owner first.
- Prove it catches: the log holds `gbv self-test: injected ... into command list 'Skybox'` and `ERROR id=935`
  (`Skybox` is the proven target; a compute pass takes none). Never gate on `--gbv-mode=state` (falsely clean).
- The verdict is the log, not the exit code: `Select-String <log> '(ERROR|CORRUPTION|WARNING) id=|InfoQueue
  (ERROR|CORRUPTION)'` is empty (narrowed with the self-test: only `id=935`), and the log ends with
  `live objects at exit: none` and `session end: clean shutdown`. Any `id=1044` or `Live ...` warning is a finding. A
  Debug log that just stops: rerun with `--gbv --gbv-mode=none`.
- Timing: runs warm a patched-shader cache -- alternate the arms, compare second runs, a visible window, look at the
  shot. `0x887A0005` with no TDR: look for `device marked as error` first; `python tools/symbolize.py
  x64/Debug/test_cube.exe <log>` reads the stacks.

## Memory profiling

- Who holds it: the `mem:` line (every 5 s), `--memreport[=<s>]`, CPU owners with `--memtrack`, MCP `get_memory`,
  Developer Controls > Memory. `private` counts the VRAM twice on this machine.
- Which allocation: `--memtrace[=all|cpu|gpu][,stacks=big|all|off|1/N][,start=<frame>]` -> `traces/memtrace_*.mtrace`;
  `python tools/memtrace.py verify` must PASS before trusting it, then `summary|leaks|growth|churn|tags|query|view`.
  `memtrace.bat` = run + verify + view.
- A leak: `--mem-readout=30` (`mem readout: PASS|FAIL <series>`); `--scene-stress=150` adds `mem tags:` (fewer
  iterations = NOT JUDGED; read the largest climb). Prove the gate: `--mem-selftest=leak@<tag>`.

## The session log

`logs/session_<date>_<time>_<pid>_<cfg>.log`, the newest named in `logs/latest.txt`; a line is `time LEVEL [category]
frame message (File:line)`. No `session end: clean shutdown` footer = no clean exit. Dev builds write minidumps to
`logs/crashes` (not under a debugger). A Debug `assert` without a debugger: `[FATAL] CRT assert: Assertion failed: ...`,
then `crash: abort()`, exit 3, no window (stderr lands as `stderr:` lines). `--log-level=`, `--log-category=<name>:<level>`, `--log-sync` (for a crash that
loses its last lines), `--log-file=`.

## Local model server (llama-server, ~38 GB)

Nothing runs that nobody is minding. From the editor: `keepServerAfterExit` false -> a job object kills it with the
editor; true (default) -> `model_reaper.exe` (`x64/ModelReaper/`) kills it after `keepAliveSeconds` idle. By hand:
only for the check, `-ngl 0`, `--load-mode mmap`, never `--mlock`, reuse a healthy server, kill it in the same turn
(`Get-Process llama-server | Stop-Process -Force`) and VERIFY it is gone. GBNF: every rule on ONE line. Qwen3.x:
prefill an empty `<think>\n\n</think>\n\n`. `intent_regression --dump <level> <gbnf> <prompt>` writes what the editor
sends.

## Photographing the editor

`--editor` opens the Level Editor at boot (its panels per `editor_state.json` `levelEditor.panelState.*Visible`;
`--shot-delay` 40+ frames); also `--dev-tab=<label>`, `--mem-window`, `--log-window`, `--show-import[=<name>]`,
`--edit-mesh=<mesh.json>[#section]`, `--inspect=<type|object>[#group]`, `--settings-menu[=<tab>]`,
`--build-dist[=<profile>]`. ImGui reads the real cursor: posted mouse messages never reach it. A panel must be
registered AND called in `EditorController::Draw`'s `drawPanel` list, or it never shows.

## Settings files

- `user_settings.json` (the repo root in dev, gitignored) holds the developer's graphics; `graphics_settings.json`
  was read once into it, and every older mention of it means that section now. A test that writes the file backs it
  up and puts it back -- never deletes it.
- `--set`, MCP `set_settings` and `--sweep` pin a knob for the process over scalability (`sg.unpin:1` forgets).

## Naming: sky, fog, aerial perspective

- Height fog + its froxel volume = UE `ExponentialHeightFog` = `HeightFogSettings`, `--set fog.*`, level
  `postProcess.heightFog`, `shaders/height_fog.hlsli`, Inspector "Exponential Height Fog".
- Hillaire sky LUTs = UE `SkyAtmosphere` = `SkyAtmosphereSettings`, `--set sky.*`, `shaders/sky_atmosphere.hlsli`.
- The sky's camera froxel volume on geometry only = aerial perspective = `sky.aerialPerspective`, `Main_SkyAerial`.

"Atmosphere" alone names the SKY, never the fog. Aerial perspective is only the sky's volume, applied where z > eps;
the height fog IS applied to the sky (as UE does). The fog's old level key `atmosphere`: only
`JsonLevel::HeightFogSection` and `EditorSceneDocument`'s section table know it -- don't spread it.

## Time of day owns the directional light

A level's light is its `timeOfDay` section (`app/scene/TimeOfDay.h`): every frame it places the sun and the moon and
hands the scene's ONE `DirectionalLight` to whichever is up; the sky always takes the SUN (`SceneFrameData::skyLight`).
A write to `Scene::DirectionalLightRef()` is overwritten next Tick: knobs via `Scene::SunLightRef()`, a direction via
`Scene::SetSunToward()`. Old `directionalLight` sections: only `JsonLevel` and `EditorSceneDocument` read them
(`TimeOfDaySettingsJson::FromDirectionalLight`). Headless: `--time=HH:MM`, `--date=`, `--time-speed=`,
`--time-freeze`, `--set tod.<key>`; editor: `setTime`, `setDate`, `setEnvironment`. A level's numbers (sky, fog,
exposure) are its file's: read `data/levels/<level>.json`, not a note's description of it.

## Subsystem notes

Each note in `docs/agents/`, the rule from it broken most easily, then its old section titles, one per line, as they
were -- so a search for an old title lands here.

- `workflow.md`:
  Line Endings;
  A Field of Another Type, a File That Is Not There -- an Error, Not a Crash;
  Session Log;
  Reproducing a Camera View From a Screenshot;
  Mouse smoothing for a recorded turn;
  Make the frame deterministic before diffing.
- `gbv.md` -- creation against submission under GBV, `id=939/940`, `--upload-ring-check`, the device-removal watch,
  the PSO cache folders:
  GPU-Based Validation (GBV) — How to Run It.
- `memory_and_streaming.md` -- a texture its level left behind, a level load sharing its textures:
  Memory Profiling — How to Run It.
- `distribution.md` -- Shipping takes only its listed switches; generated headers always exist (never
  `__has_include`); `build_dist.py` content / full writes `dist/` only when the owner asked; a tool that deletes runs on
  a copy, read first for paths not from its `--out`:
  Distribution -- VFS, Packages, Shipping.
- `scalability.md`:
  Graphics Scalability -- Groups, Levels, the Player's Settings.
- `editor_and_model.md` -- a preview-shader change reaches no cached thumbnail until `kThumbnailSchemaVersion` is
  bumped; a film (`frame.recordDt`) ends with it at 0:
  The Local Intent Model (llama-server) — Starting and Stopping It;
  Claude Code drives the editor (MCP);
  Photographing the editor.
- `time_of_day.md` -- a night lamp switches by its brightness, never by `enabled`:
  Time of Day Owns the Directional Light;
  Ultra Dynamic Sky -- What It Does to the Fog and the Sky (2026-11-04).
- `sky_fog_and_ue.md` -- UE's (x, y, z) = ours (z, x, y) x 100, pitch flipped; photograph UE's linear viewport,
  never a SceneCapture2D:
  Atmosphere Terminology — Say Which One;
  Sun / Sky Calibration -- the Sky's SHAPE, Not Only Its Energy;
  Matching UE 5.8 -- the Same Settings Give the Same Picture;
  Volumetric Fog -- What the Air Sees.
- `boats_and_water.md` -- the pose is a render offset: an authored transform comes from `GetAuthoredModelMatrix()`;
  bump `kWaterMaskBakeVersion` with any change to what `BuildField` makes; `--set vessel.*` lands on every boat and
  stays:
  Buoyancy — Meshes Floating on the Ocean;
  Boats -- a Hull on the Waves, a Helm in Play;
  Making a Staged Boat a Vessel -- fast_boat_13, police_boat;
  A Boat's Wake -- the Hull's Pressure on the Water;
  A Boat's Spray -- Lit Drops Off the Chines;
  A Hull Keeps the Water Out -- mesh.json "waterMask".
- `play_and_physics.md` -- in a build with the editor the world stands outside Play (Release always runs); Jolt only
  in `sources/physics/*.cpp`:
  Play Mode -- the World Runs Only There;
  Physics -- Jolt, Behind One Folder.
- `race.md` -- two runs of one arm differ more than most changes: alternate the arms:
  A Race Course and Its Bot -- Buoys, Gates, a Helmsman;
  The Race -- a Start Zone, a Grid, Three Laps, the Results;
  Two Courses in wind_test -- "reef", One Colour a Course, Watching a Bot;
  Race Tracks in the Editor -- a Course and a Gate Are Their Marks;
  What a Boat Does -- Its Card, the Three Roles, a Bot That Knows Its Boat;
  The Side Grip -- a Hull Lets Go on the Plane, and the Bot Learns Its Own;
  The Bot Drives by Sight -- the Next Gates, Its Boat, the Others;
  The World's Clock Runs Faster -- frame.timeScale.
- `shadows.md`:
  VSM Is UE's High -- the Clipmap, the Pool, the Marks;
  The Sun's Shadow on the Water;
  Cast Shadow — Per Object;
  SDSM Partition Cache -- a Kept Tile Is Handed Only What It Was Rendered For;
  Local-Light Shadow Near -- 1 cm in Every Mode;
  Local-Light Shadow Cache (Atlas Modes) and VSM Page Requests -- What Made Them Jump.
- `geometry.md` -- a mesh asset lives in `models/<name>/`:
  Rest Pose — How a New Copy Lies;
  Import — Where a Mesh Asset Lives, and Leaf Card or Surface (with Handedness);
  Mesh LOD -- the Error Is Measured on the Screen It Is Seen On;
  Tessy -- Cluster Geometry (the VSM Pages, the Lamps' Atlases, the Camera; Plan N0-N7).
- `lighting.md`:
  The Water Reflection -- Its Resolve Runs Only Without DLSS;
  Glass -- the thinTranslucent Shading Model;
  Two-Sided Surfaces -- the Back Face and the Sun;
  RT Sky Visibility -- the Sky Stops Lighting What It Cannot See;
  RT Reflections -- Glossy by the GGX Lobe;
  GI Probe Volume (DDGI) -- the Bounce;
  Local-Light Falloff -- Metres, Not UE's Centimetres.
