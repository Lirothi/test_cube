# Repository Instructions

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
