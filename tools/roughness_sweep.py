"""Render data/levels/roughness_sweep.json and measure the reflection across the roughness row.

    python tools/roughness_sweep.py                 # render, then measure
    python tools/roughness_sweep.py --analyze-only  # re-measure the existing capture

Why this exists. Every change to the environment path -- the prefiltered bake, the roughness/mip
mapping, the split-sum evaluation, a new sky format -- is invisible on the canonical wind_test views,
because that scene is sand, foliage and near-mirror water: nothing sits in the roughness 0.1..0.3
band where a reflection still reads AS a reflection. Measuring there produced "mean |delta| 0.01/255"
for a change that was structurally significant, which is not a gate, it is a coin toss.

The level is 3 rows x 8 spheres against the sky and nothing else:
  row 0  dielectric, 0.18 grey   -- F0 ~ 0.04, so this row is mostly the BRDF LUT's bias term
  row 1  white metal             -- F0 = albedo, the row where the LUT's scale term shows
  row 2  copper metal            -- a wrong split-sum shows up as a HUE shift, easier to see
The eight roughness stops are the exact values mips 1..6 are prefiltered for, plus a near-mirror and
a full-rough bookend, so a mapping change moves specific spheres rather than smearing everything.

The metric is per-sphere HIGH-FREQUENCY energy: the luminance inside the disc minus a blurred copy
of itself, then its standard deviation. Plain contrast (std of the luminance) was tried first and is
the WRONG metric -- it conflates three things, and reported a false alarm on a correct build:
  * the reflection's detail, which does fall with roughness (what we want)
  * the Fresnel rim, which GROWS with roughness as the LUT's bias term rises at grazing angles
  * which part of the sky is being averaged -- a mirror shows one patch, a rough lobe averages the
    whole bright upper hemisphere, so even the MEAN rises with roughness here
Subtracting the blur removes the last two, which are smooth across the sphere, and leaves the
detail. That must fall monotonically along each row; if it does not, the prefilter and the sampler
disagree about which mip is which roughness -- the one failure that looks fine in a screenshot.
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
LEVEL = ROOT / "data" / "levels" / "roughness_sweep.json"
EXE = ROOT / "x64" / "Release" / "test_cube.exe"
SHOT = ROOT / "logs" / "roughness_sweep.png"

LUMA = np.array([0.2126, 0.7152, 0.0722])


def box_blur(a: np.ndarray, radius: int) -> np.ndarray:
    """Separable box blur, numpy only. Edge-clamped so the disc's border does not manufacture
    detail that the reflection does not have."""
    pad = np.pad(a, radius, mode="edge")
    k = 2 * radius + 1
    csum = np.cumsum(pad, axis=0)
    horiz = np.empty_like(pad)
    horiz[radius:-radius] = (csum[k - 1:] - np.pad(csum[:-k], ((1, 0), (0, 0)))) / k
    horiz[:radius] = horiz[radius]
    horiz[-radius:] = horiz[-radius - 1]
    csum = np.cumsum(horiz, axis=1)
    out = np.empty_like(horiz)
    out[:, radius:-radius] = (csum[:, k - 1:] - np.pad(csum[:, :-k], ((0, 0), (1, 0)))) / k
    out[:, :radius] = out[:, radius][:, None]
    out[:, -radius:] = out[:, -radius - 1][:, None]
    return out[radius:-radius, radius:-radius]


def srgb_to_linear(x: np.ndarray) -> np.ndarray:
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def capture() -> None:
    SHOT.parent.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(EXE),
        "--no-hud",
        "--shadow-mode=vsm",
        "--dlss=off",  # the upscaler would blur exactly the detail being measured
        f"--level={LEVEL.relative_to(ROOT).as_posix()}",
        f"--shot={SHOT.relative_to(ROOT).as_posix()}",
        "--shot-delay=8",
    ]
    print("capturing", SHOT.name)
    subprocess.run(cmd, cwd=ROOT, check=True)


def sphere_centres(level: dict, width: int, height: int):
    """Project the sphere centres. The level is the source of truth for the layout, so this keeps
    working if the grid is edited -- hardcoded pixel boxes would not."""
    cam = next(o for o in level["objects"] if o["type"] == "freeCameraStart")
    cx, cy, cz = cam["position"]
    hfov = math.radians(level["camera"]["hfovDeg"])
    tan_h = math.tan(hfov * 0.5)
    tan_v = tan_h * height / width

    out = []
    for obj in level["objects"]:
        if obj["type"] != "staticMesh":
            continue
        x, y, z = obj["position"]
        vz = z - cz
        if vz <= 0.01:
            continue
        ndc_x = (x - cx) / (vz * tan_h)
        ndc_y = (y - cy) / (vz * tan_v)
        px = (ndc_x * 0.5 + 0.5) * width
        py = (0.5 - ndc_y * 0.5) * height
        radius_px = 0.5 / (vz * tan_h) * 0.5 * width  # mesh radius is 0.5 world units
        out.append((obj["name"], px, py, radius_px))
    return out


def analyze() -> int:
    level = json.loads(LEVEL.read_text(encoding="utf-8"))
    img = np.asarray(Image.open(SHOT).convert("RGB"), dtype=np.float64) / 255.0
    h, w, _ = img.shape
    lin = srgb_to_linear(img)
    lum = lin @ LUMA

    rows: dict[str, list] = {}
    for name, px, py, r in sphere_centres(level, w, h):
        # Sample well inside the disc: the silhouette mixes with the sky and would report the
        # background's contrast as if it were the sphere's.
        rr = r * 0.7
        y0, y1 = int(py - rr), int(py + rr)
        x0, x1 = int(px - rr), int(px + rr)
        if x0 < 0 or y0 < 0 or x1 >= w or y1 >= h:
            print(f"  {name}: off screen, skipped")
            continue
        patch = lum[y0:y1, x0:x1]
        yy, xx = np.mgrid[0:patch.shape[0], 0:patch.shape[1]]
        mask = ((yy - patch.shape[0] / 2) ** 2 + (xx - patch.shape[1] / 2) ** 2) <= (rr * 0.95) ** 2
        detail = patch - box_blur(patch, 3)
        vals = patch[mask]
        rgb = lin[y0:y1, x0:x1][mask]
        key = name.split()[1][0]
        rows.setdefault(key, []).append((name, float(vals.mean()), float(detail[mask].std()),
                                         rgb.mean(0)))

    labels = {"d": "dielectric 0.18 grey", "m": "white metal", "c": "copper metal",
              "a": "F9 material AO sweep (roughness fixed)"}
    failures = 0
    for key, entries in rows.items():
        print(f"\n{labels.get(key, key)}")
        print(f"  {'sphere':>22} {'mean':>8} {'detail':>9} {'R/B':>7}")
        detail = []
        for name, mean, std, rgb in entries:
            rb = rgb[0] / max(rgb[2], 1e-6)
            print(f"  {name:>22} {mean:>8.4f} {std:>9.4f} {rb:>7.3f}")
            detail.append(std)
        # Each row asserts on the quantity IT is about.
        if key == "a":
            # AO row: roughness is fixed, so detail is not the subject -- and it actually RISES as
            # the spheres darken, because these numbers come from the final image and the tone
            # curve is steeper down there, so the same scene-linear variation reads as a larger
            # display-linear one. The property under test is that occlusion darkens the sphere.
            means = [m for _, m, _, _ in entries]
            bad = [i for i in range(1, len(means)) if means[i] > means[i - 1]]
            if bad:
                failures += 1
                print(f"  MEAN NOT FALLING at index {bad} -- material AO is not reaching indirect light?")
            else:
                print(f"  mean falls {means[0]:.4f} -> {means[-1]:.4f} as AO 1 -> 0, monotonic  OK")
        else:
            # Roughness rows: detail must fall. Small tolerance -- the sky is not uniform, so a
            # sphere can catch a slightly busier patch than its neighbour.
            bad = [i for i in range(1, len(detail)) if detail[i] > detail[i - 1] * 1.15]
            if bad:
                failures += 1
                print(f"  DETAIL NOT MONOTONIC at index {bad} -- prefilter and sampler disagree?")
            else:
                print(f"  detail falls {detail[0]:.4f} -> {detail[-1]:.4f}, monotonic  OK")
    return failures


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--analyze-only", action="store_true")
    args = ap.parse_args()
    if not args.analyze_only:
        capture()
    return 1 if analyze() else 0


if __name__ == "__main__":
    sys.exit(main())
