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

The running Level Editor is an MCP server at `http://127.0.0.1:8128/mcp` (streamable HTTP,
localhost only; `sources/editor/intent/EditorMcpServer.*`), registered for Claude Code by
`.mcp.json` in the repo root. Tools: `editor_guide` (the local model's own system prompt, rebuilt
from the open level: every action, query and name — read it first), `run_action` /
`preview_action` (the same `{action, target, params}` JSON the local model emits, through the same
`ParseAnswer` → `BuildIntentPreview` → `ExecuteIntent`), `query`, `get_camera`, `set_camera`,
`screenshot` (the editor window as a PNG, taken at the `--shot` safe point), `undo`.

- **There is no save.** The level is saved by the person, and nothing over MCP can do it.
- Each edit is one undo entry and shows in the command bar's transcript under **claude**.
- Only while the editor is open with a level; calls wait up to 60 s for a frame, then fail.
- Browser requests are refused (`Origin`/`Host` must be this machine), so a web page cannot
  drive it. A second editor cannot bind the port and says so in its panel.
- Toggle and status: command bar settings → "Claude Code (MCP)". `mcpEnabled` / `mcpPort` live in
  `editor_state.json` → `levelEditor.intentModel`.
- The gate covers the protocol without a socket (`TestMcpDrivesTheSameRoad`); for a live check,
  launch `--editor --level=...` and POST JSON-RPC to the endpoint. Kill the process afterwards
  rather than closing it, so nothing can save the level.
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
photographed; `--edit-mesh=<models/x.mesh.json>` does the same for the Mesh Editor, and
`--edit-mesh=<models/x.mesh.json>#buoyancy` opens it with the Buoyancy section expanded.

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
