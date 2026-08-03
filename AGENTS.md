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
