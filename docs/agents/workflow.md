# Working Rules -- Line Endings, Reading JSON, the Session Log and Threads, Reproducing a View

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

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

## A Field of Another Type, a File That Is Not There -- an Error, Not a Crash

2026-11-24 (the code review's second reading, docs/architecture_review_plan.md 10.13-10.18). nlohmann's
`object.value(key, fallback)` returns the fallback only when the key is ABSENT. A key that is there with another type --
`"zip": null`, `"version": 1.0` where a string is read, `"levels": "a.json"` where a list is -- throws `type_error 302`
(probed on the tree's `json.hpp`: five of five), and so does `value()` on anything that is not an object. Out of a frame
or a thread of its own that is `std::terminate`: in the editor, the level gone. There were 770 such calls in 53 files;
the ones a hand-edited or foreign file reaches in a frame are done, the levels' own are plan step R9.6.

- **`core/JsonField.h` `jsonfield::Get(object, key, fallback)`**: the field as its type, or the fallback -- never throws
  (bool, an integer, a float, a string, a list of strings, the json as it is). For a file another program writes or
  somebody edits by hand, read in a frame or on a thread: the Build Distribution window, `DistBuildJob`'s event lines,
  the player's `display.mode` (`"mode": 1` threw before the window was made, at every start), `config/boot.json`.
- **Where a wrong type must be an ERROR that names the file**, keep `value()` and catch `nlohmann::json::exception`
  around the read: a level (`JsonLevel::Load`), a cook profile (`Cook.cpp ReadProfile`), the scalability table, and a
  material -- `MaterialDataManager` `TryParseMaterialPreset` names the file, leaves that material out and loads the
  rest. One `"alphaCutoff": "0.5"` in any file of `data/materials` used to end the program before its first frame with
  no file named (the startup scan reads them all), and a preset loaded later took the editor down.
- **A sky file that is refused** -- not on the disk, cut short, a damaged header -- is an Error and a level without sky
  light. `TextureCube::CreateFromDDS` checks the header's numbers and the file's size against the layout BEFORE it
  creates or copies anything (a truncated file was read past its end), and `Skybox::Init` no longer throws its result
  away: every reader of the cube comes through `Skybox::FileCubeSrv()` / `EnvironmentSrv()`, which hand a null descriptor
  of the cube's own dimension (a second slot of the skybox's null heap; the 2D null there would be the wrong type).
  Until then a level that named a sky file that was simply not there died in the driver (`nvwgf2umx.dll`) a second
  after the "failed to read" line. **A loader that refuses is half the fix**: find who gets its `false` and what they
  bind instead -- the live run says, not the loader's code. GBV (`Skybox,Lighting,Compose,GBuffer,Transparent,
  Transparent.Water,Translucent` + the Skybox self-test, a level that names a sky file that is not there, Debug): only
  id=935, nothing alive at exit; `RTTrace` and `GlassReflections` were not in that run, and the editor on such a level
  opens, draws its panels and bakes thumbnails. That list patches Lighting, Compose and the G-buffer: 5 minutes to the
  first frame on a warm cache, 8 cold -- give such a run 20 minutes.
- Gates: `intent_regression` `TestJsonFieldsAreReadWithoutThrowing` (it also asserts that the library DOES throw, so
  nobody "simplifies" the readers back), `TestAMaterialOfTheWrongTypeIsAnErrorNotACrash`.

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
`RHI` (`rhi/RhiQueue.h`): every queue operation of a frame in flight goes through `Renderer::Rhi().Run`, never
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

The `mem:` line (Info, `core`, every 5 s; `rhi/MemoryReport.h`) puts process private
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
