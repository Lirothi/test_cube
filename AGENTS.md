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
`nightSteady`, after the curve, the eye untouched -- so dim lamps look dim (wind_test: 13.13 = the moonlit beach's
auto camera at 24:00; candles 4 -> 0.5 lm move the camera 0.21 stop instead of the auto exposure's 0.96). In the editor (command bar, MCP):
`setTime` ("18:30", "sunset", "+2h") and `setDate` ("2026-06-21", switches to real positions); every
`timeOfDay.*` key is in the document, so `setEnvironment` reaches the rest.

Night lamps (plan L, `rendering/lighting/NightLamps.h`): a point/spot light or a glowing mesh with `"onAt": "night"`
(or `"day"`) switches when the sun crosses `timeOfDay.lamps.sunElevationDeg` (0 = UDS's Sunset), UDS-style delay and
warm-up, instant when the clock is set by hand; a night lamp takes `nightBrightness` as the night comes in. Brightness,
never `enabled` (that rebuilds the lights behind a GPU wait). MCP: `setOnAt` -- lights through the selection;
`setLight {lumens | scale, radius, color, shadows, flicker, flickerHz}` retunes lights that exist, by name or group
("Bathroom Candles"), one undo entry -- `scale` multiplies each light's own flux, a spot's radius is its range;
`flicker` is a point light's +/- share of its flux (PointLightFlickerDesc, three incommensurate sines; a candle 0.15
at 3 Hz), each lamp seeded on its own so a row of candles does not breathe in step.

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
reports any new direct output (exit code = findings) — run it before committing.

**Diagnostics are events, not files.** Anything a subsystem wants to say -- a pre-assert dump, a
validator's mismatch list, a self-test's per-case lines and verdict, a cross-check that fired --
goes through `LOG_*` into the session log (Fatal before an assert: it is flushed synchronously, so
the record survives whatever the dialog's button does). Do NOT add a new `logs/<name>.log` for it:
the owner said so (2026-09-03) after `s14_assert.log` / `hzb_cull_selftest.log` appeared beside
the session log. A headless gate reads its verdict line from the session log (`Select-String` for
`cull validation PASS`, `hzb cull self-test: PASS`, ...) and the exit code.

The `mem:` line (Info, `core`, every 5 s; `rendering/core/MemoryReport.h`) puts process private
bytes, VRAM and mimalloc commit beside per-owner byte counts; a subsystem that owns memory the
process counters cannot attribute registers a provider (`RegisterMemoryProvider`). A number that
grows while the others stay flat names the owner -- read it before reaching for a profiler
(docs/bug_rt_retire_bin_leak.md).

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
  it ignores `--gbv-passes`), or a plain `--gbv` without `--gbv-passes` for one level with your
  `--set`s. Minutes; run it before trusting a pass/barrier/resource change.
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

## Reproducing a Camera View From a Screenshot

The on-screen HUD prints the camera POSITION and its ORIENTATION QUATERNION:

```
Cam: -4.11 0.87 0.83, rot: 0.0273 0.9078 -0.0599 0.4143, speed: 1.00, DLSS: 2, SSR: 1, FXAA: 0
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
`screenshot` (the editor window as a PNG, taken at the `--shot` safe point), `undo`.

The look-dev loop (2026-10-15): `set_settings {set: "bloom.intensity:0.03;tod.time:17.8"}` changes
the running scene through `--set`'s own name table (App.cpp `ApplySweepValue`) -- TRANSIENT: no undo
entry, nothing to save, an Inspector edit of the same object re-applies the document over it -- and
re-meters the exposure; `screenshot {clean: true, path: "...png", settleFrames: 60}` writes the
frame WITHOUT the editor's panels (scene + HUD, comparable with a headless `--shot`) at full size and
sends no picture back unless `image`; `reset_settings` puts every environment object back to the open
document (project-wide switches -- `cloudTrace.*`, `cloudShadow.*`, `frame.*`, `skyVis.*`, `gi.*`,
`render.*` -- are not in the document and stay as last set: a bisect that turns `skyVis.enabled:0` off for one
arm runs every later arm without it unless each arm names it); `get_readouts` returns the metered/adapted EV100, the pre-exposure,
the sun probe (disc colour, visible share) and the frame rate; `set_camera {lookAt: "sun",
offsetYawDeg, offsetPitchDeg}` faces the sky's sun wherever the time of day has it (`position`
optional; `lookAt: "moon"` the moon; positive offsetPitchDeg tilts DOWN, so a view 4 degrees over the horizon
toward a sun at elevation e is `offsetPitchDeg: e - 4`). An exposure curve is shaped live with
`exposure.curvePoints` / `exposure.curveEv<i>` / `exposure.curveComp<i>` (`exposure.curve:0|1|2|3` = none/Eye/UDS/UDS
Steady Night; the curve's slope is the adaptation's strength -- the camera follows 1 - slope of the scene's change).
The Level Editor closed (F2) still serves every call -- MEASURE COSTS WITH IT CLOSED (its panels are a load of
their own) and on a real view, not wherever the camera happened to be: `get_profile {filter, reset}` returns the
profiler's rows (GPU/CPU per pass: EMA, max since `reset`, runs last frame), and `shadow.mode:0|1|2`
(Legacy/VSM/SDSM) switches the sun's shadows live. Kill the editor afterwards (`Stop-Process`), as for any live MCP check.

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
seas. `set_settings {set: "camera.fly:-3.8"}` flies the camera along its own view at that many m/s (negative =
backwards, `0` stops it, `reset_settings` does not): a temporal history's artefact that only shows in MOTION is
looked at in the running editor, `screenshot` with `settleFrames` taking it mid-flight (`--set=camera.fly:` headless;
`--cam-fly` drifts along the WORLD's x/z instead). Stop it in a `finally` -- a script that dies leaves it flying. The probe's window mean of a bloom target over water is dominated by any lens ghost lying on it
-- set `bloom.ghostIntensity:0` for that read.
A FLICKER IS READ FRAME BY FRAME: two `get_readouts` calls land ~10 frames apart and see a drift, never
a flicker. `get_readouts {history: N}` (N <= 512) adds the newest N frames, one row EACH (frame,
adapted/metered EV100, pre-exposure, the probe's window mean rgb and max luminance, the sun probe) --
ExposureMetering's ring, filled as each readback slot retires. That is how the chrome star's 45 %
frame-to-frame flicker and the ghosts' DLSS ringing were found (2026-10-15). `set_settings` with
`resetExposure: false` changes a knob the way the Inspector does, without re-metering.

- **`save_level` writes the open level exactly as Ctrl+S does** -- only when the person asked
  (owner, 2026-10-23: «сам все сохраняй что надо»). Every other call leaves an unsaved edit that is the
  person's to keep or discard. The per-level editor camera is not saved with it.
- Each edit is one undo entry and shows in the command bar's transcript under **claude**.
- Only while the editor is open with a level; calls wait up to 60 s for a frame, then fail.
- Browser requests are refused (`Origin`/`Host` must be this machine), so a web page cannot
  drive it. A second editor cannot bind the port and says so in its panel.
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
- Cost, measured (Release, ~340 fps, 5 boats / 30 automatic pontoons): GPU `Pass_OceanReadback` 41 us
  per copy, but copies are capped at 30 a second -> ~3 us per frame averaged (every other frame at
  60 fps); CPU `Scene::TickBuoyancy` median 5 us (heights are re-sampled only when a copy lands). The
  floating objects come from a REGISTRY (`RenderableObject::SetBuoyant`), never a scene walk: a level
  with nothing floating costs 0 us CPU and no copy. The copy was tried on the async
  queue inside Main_ObjectCompute and cost the frame ~108 us: the ocean draw waits on that pass.

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
each LOD comes in; `LOD_K` = pixels per unit / PixelError).

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
  and inner surface), `glass.wall:0.004`. 0 = straight through. The only curved glass in the levels is those
  tumblers; the corner shelf's panes measure the same under every value (0.4 % of pixels, noise). Roughness blurs what is behind (`SampleBehindGlass`, SceneOpaque has one mip). Its reflection is the
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

## RT Sky Visibility -- the Sky Stops Lighting What It Cannot See

`Main_SkyVis` (`shaders/rt_sky_visibility_cs.hlsl`) is UE's Distance Field AO bent normal traced with inline
RayQuery on the TLAS: half resolution, `skyVis.rays` (8) cosine rays per pixel against the SHADOW casters' geometry
(a leaf card solid, below), out to `skyVis.maxDistance` (15 m), accumulated over frames in the same dispatch
(`skyVis.history` 0.98, reprojected through the G-buffer velocity) into `skyVisAccum`, then filtered in space by
`shaders/rt_sky_visibility_filter_cs.hlsl` (UE DFAO's spatial filter: a 5x5 half-resolution gather weighted by
relative depth `skyVis.filterDepth` 0.05 and normal agreement^`skyVis.filterNormalPower` 8; `skyVis.filter:0` = the
accumulation as it is) into `skyVis`, the target every consumer reads. The history is the UNFILTERED accumulation
(SVGF's rule: a filtered history widens the blur every frame). Its length is how much sky a point sees, its
direction where from. `shaders/sky_visibility.hlsli` applies it with UE's formulas, file and line cited there:
the sky diffuse takes min() with the AO (UE's Minimum combine) and a bent lookup normal, the sky specular a cone-cone
occlusion -- in lighting_cs AND compose_cs, the same line, because compose subtracts the sky specular lighting
added. Needs RT hardware (`decisions_.skyVis` also builds the TLAS); off = the open sky, the frame before it.

- Bindless scene slots 26-31 are this pass's (its own TLAS/GB1/depth copies -- the reflection trace that writes
  0/2/3 does not run unless reflections are RT).
- The body writes the target on EVERY path: traced, or cleared to the open sky while the TLAS is not ready --
  lighting and compose read it on the strength of `decisions_.skyVisTraced`, set by the builder.
- Cost is the ray count, not the reach: 4 rays 0.28-0.34 ms on wind_test's grove, 8 rays 0.63, 2 rays 0.18.
- Noise (2026-10-24, owner: "the wall is noisy"): a wall lit by a window that fills a few % of its hemisphere gets
  the sky from a few of its rays now and then. wind_test's second room at 24:00 (cam -21.92,1.07,89.75), per-pixel
  temporal std over 12 clean shots (scratchpad noise_map.py): 0.9 and no filter 9.4 % median (p90 18 %), 0.95 +
  the filter 3.2 % (5.7 %), 0.97 2.8 %, 0.97 + 8 rays 2.5 % (+0.075 ms); SkyVis off 0.35 %. Bindless slots 40-41
  are the filter's.
- 8 rays and 0.98 are the defaults since 2026-10-26 (owner's tunnel of boxes, cam -58.63,1.72,65.12 at 15:00, whose
  floor sees the sky only through the mouth; per-pixel temporal std over 16 shots, scratchpad tunnel_variants.py):
  4 rays / 0.95 8.6 % median (p90 12.3 %), 8 rays 5.6 %, 0.98 5.2 %, both 3.5 % (5.1 %); SkyVis off 0.2 %.
  Pass_SkyVis 0.14 -> 0.25 ms there. 0.98 is also the top of the history's range (`--set`, the settings file).
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
  shadow/sky rays) and the shadow rays of what a reflection hits see the GEOMETRY, a leaf card solid --
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
- Developer Controls > GI "Sky visibility": on/off, rays, history, reach, diffuse, specular, spatial filter --
  persisted in graphics_settings.json `skyVis` (`--set=skyVis.*` the same knobs, transient); "Reset GI to defaults"
  resets them with the probes. The filter's depth/normal weights stay `--set` only.
- A/B it with the exposure PINNED (`exposure.autoExposure:0` after metering): the metering brightens the whole
  frame to make up for the darker shade, which reads as "the sky got brighter".

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
- Bindless scene slots 32-39 are the trace's (`BindlessTable::kScenePerFrame` 48). Lighting binds t18-t20 whenever
  the volume exists and reads them when `ddgiParams.x` (the spacing) is non-zero -- `decisions_.giApplied`.
- `Pass_BuildAS`'s recording task now fans out to six RT passes; `TaskSystemLockFree` dependents_ holds 8 (4 aborted
  the first frame with a bare 0xC0000409 and no log line -- `--log-sync` did not help; the minidump's stack scan did).
- Cost (wind_test, 1440p DLSS Balanced): trace 0.10 ms in the bathroom, 0.18 ms on the grove; update 0.06 ms.
  `gi.debug:1` shows the bounce alone (the lamps' own passes still add), RED where no usable probe surrounds a
  point; `gi.debug:2` the probes' relocation and classification (below). `gi.enabled:0` = the frame before it.
  Developer Controls > GI: on/off, spacing, hysteresis, intensity -- persisted in graphics_settings.json `gi`,
  which overrides the compiled defaults at boot (a `--set gi.*` still wins, it is applied after) -- and the debug
  view (not persisted). `gi.divisor`, `gi.rayDistance`, `gi.infiniteBounce` stay --set / MCP only.
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
