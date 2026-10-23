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
`--set/--sweep=tod.<key>`; the HUD prints `Time: HH:MM` under `Cam:`. In the editor (command bar, MCP):
`setTime` ("18:30", "sunset", "+2h") and `setDate` ("2026-06-21", switches to real positions); every
`timeOfDay.*` key is in the document, so `setEnvironment` reaches the rest.

Night lamps (plan L, `rendering/lighting/NightLamps.h`): a point/spot light or a glowing mesh with `"onAt": "night"`
(or `"day"`) switches when the sun crosses `timeOfDay.lamps.sunElevationDeg` (0 = UDS's Sunset), UDS-style delay and
warm-up, instant when the clock is set by hand; a night lamp takes `nightBrightness` as the night comes in. Brightness,
never `enabled` (that rebuilds the lights behind a GPU wait). MCP: `setOnAt` -- lights through the selection.

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
  injected and the silence proves nothing.

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
document (project-wide switches -- `cloudTrace.*`, `cloudShadow.*`, `frame.*` -- are not in the
document and stay as last set); `get_readouts` returns the metered/adapted EV100, the pre-exposure,
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
`set_settings {set: "wind.freeze:1"}` holds the wind + ocean pose (`--wind-freeze` for a running
editor; `0` releases it, `reset_settings` does not): without it two looks of the sea are two different
seas. The probe's window mean of a bloom target over water is dominated by any lens ghost lying on it
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
  full sheet of blue glass.
- **No refraction, by the spec**: glTF without KHR_materials_volume is thin-walled (`kGlassMaterialThickness` 0);
  roughness blurs what is behind (`SampleBehindGlass`, SceneOpaque has one mip). Its reflection is the sky's (a slot
  is not in the glass-reflection G-buffer).
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
RayQuery on the TLAS: half resolution, `skyVis.rays` (4) cosine rays per pixel against the SHADOW casters, alpha-
tested like the RT reflections, out to `skyVis.maxDistance` (15 m), accumulated over frames in the same dispatch
(`skyVis.history` 0.9, reprojected through the G-buffer velocity). Its length is how much sky a point sees, its
direction where from. `shaders/sky_visibility.hlsli` applies it with UE's formulas, file and line cited there:
the sky diffuse takes min() with the AO (UE's Minimum combine) and a bent lookup normal, the sky specular a cone-cone
occlusion -- in lighting_cs AND compose_cs, the same line, because compose subtracts the sky specular lighting
added. Needs RT hardware (`decisions_.skyVis` also builds the TLAS); off = the open sky, the frame before it.

- Bindless scene slots 26-31 are this pass's (its own TLAS/GB1/depth copies -- the reflection trace that writes
  0/2/3 does not run unless reflections are RT).
- The body writes the target on EVERY path: traced, or cleared to the open sky while the TLAS is not ready --
  lighting and compose read it on the strength of `decisions_.skyVisTraced`, set by the builder.
- Cost is the ray count, not the reach: 4 rays 0.28-0.34 ms on wind_test's grove, 8 rays 0.63, 2 rays 0.18.
- A/B it with the exposure PINNED (`exposure.autoExposure:0` after metering): the metering brightens the whole
  frame to make up for the darker shade, which reads as "the sky got brighter".
