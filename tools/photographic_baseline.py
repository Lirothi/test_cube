"""Capture and measure the canonical image-quality baseline (photographic plan, step P0).

Reads docs/photographic_baseline_manifest.json, runs the engine once per (view, mode) with the
camera pinned from the command line, and writes the captures, the profiler dumps and a measurement
report under logs/baseline/ (gitignored).

    python tools/photographic_baseline.py                # capture, then measure
    python tools/photographic_baseline.py --analyze-only # re-measure existing captures
    python tools/photographic_baseline.py --views overview sun_glint

Run it from the project root; the engine resolves its data paths relative to the working directory.

The measurements are deliberately taken from the final SDR PNG rather than from the HDR scene: the
renderer has no HDR readback path, and the plan's P0 names the screenshot histogram as the accepted
fallback. Once P1 lands a metering resource, add the HDR numbers next to these instead of replacing
them -- the clipped-pixel ratio is a display-referred quantity and stays meaningful either way.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

PROJECT_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = PROJECT_ROOT / "docs" / "photographic_baseline_manifest.json"

# Rec. 709 luma weights; the captures are sRGB-encoded, so linearise before weighting.
LUMA_WEIGHTS = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)
# A channel at or above this 8-bit value is treated as clipped. 254 rather than 255 because the
# tonemap's saturate() plus sRGB encoding lands a hair under full scale on some highlights.
CLIP_LEVEL = 254

# Rows kept from the profiler dump. Pass_Tonemap is the one to read carefully: the DLSS
# slEvaluateFeature call is recorded INSIDE that scope, so most of its GPU cost is the upscaler,
# not the tone curve. The photographic plan replaces the curve, which will barely move this row.
PROFILED_ROWS = [
    "CPU.Frame", "GPU.Frame",
    "Pass_Lighting", "Pass_Compose", "Pass_Tonemap", "Pass_Skybox",
    "Pass_OceanReflection", "Pass_RTReflections", "Pass_GlassReflections",
    "Pass_Transparent", "Pass_VsmPageRender", "Ocean.Surface",
    "RenderObjectBatch", "ExecuteBundles",
]


def srgb_to_linear(x: np.ndarray) -> np.ndarray:
    return np.where(x <= 0.04045, x / 12.92, ((x + 0.055) / 1.055) ** 2.4)


def load_manifest() -> dict:
    with MANIFEST.open("r", encoding="utf-8") as f:
        return json.load(f)


def capture(manifest: dict, view: dict, mode: dict, out_png: Path, out_prof: Path) -> bool:
    """Run the engine once; it writes the PNG and the profiler dump, then exits by itself."""
    recipe = manifest["recipe"]
    exe = PROJECT_ROOT / recipe["exe"]
    if not exe.exists():
        sys.exit(f"engine not built: {exe}")

    pos = ",".join(f"{v:g}" for v in view["camPos"])
    rot = ",".join(f"{v:g}" for v in view["camRot"])
    args = [
        str(exe),
        f"--level={recipe['level']}",
        f"--shot={out_png}",
        f"--profdump={out_prof}",
        f"--shot-delay={recipe['shotDelaySec']:g}",
        f"--wind-freeze={recipe['windFreezeSec']:g}",
        f"--cam-pos={pos}",
        f"--cam-rot={rot}",
        mode["dlssArg"],
        *recipe["commonArgs"],
    ]

    out_png.unlink(missing_ok=True)
    out_prof.unlink(missing_ok=True)
    started = time.time()
    subprocess.run(args, cwd=PROJECT_ROOT, check=False)
    ok = out_png.exists()
    print(f"  {'ok ' if ok else 'FAILED'} {out_png.name}  ({time.time() - started:.1f}s)")
    return ok


def measure(png: Path) -> dict:
    img = np.asarray(Image.open(png).convert("RGB"), dtype=np.float64) / 255.0
    lin = srgb_to_linear(img)
    luma = lin @ LUMA_WEIGHTS

    raw = np.asarray(Image.open(png).convert("RGB"))
    clipped_any = np.any(raw >= CLIP_LEVEL, axis=2)
    clipped_all = np.all(raw >= CLIP_LEVEL, axis=2)

    # 16-bin display-referred histogram: enough to see the tonal distribution move without
    # checking in a 256-column table per view.
    hist, _ = np.histogram(np.mean(img, axis=2), bins=16, range=(0.0, 1.0))

    return {
        "resolution": [int(img.shape[1]), int(img.shape[0])],
        "lumaMean": float(luma.mean()),
        "lumaMedian": float(np.median(luma)),
        "lumaP02": float(np.percentile(luma, 2)),
        "lumaP95": float(np.percentile(luma, 95)),
        "lumaP99": float(np.percentile(luma, 99)),
        "clippedAnyChannelPct": float(clipped_any.mean() * 100.0),
        "clippedAllChannelsPct": float(clipped_all.mean() * 100.0),
        "shadowPixelsBelow2Pct": float((luma < 0.02).mean() * 100.0),
        "histogram16": [int(v) for v in hist],
    }


def parse_profdump(path: Path, wanted: list[str]) -> dict:
    """Pull the named rows out of a --profdump text file, keyed by section.

    The dump is the profiler overlay verbatim: a "[CPU] frame=N" block followed by a "[GPU] one,
    each row "Name  avg: X max: Y usages: Z". The two sections carry the SAME pass names for very
    different quantities -- the CPU row is the cost of recording the pass, the GPU row is the cost
    of running it -- so the section must be tracked rather than matching the first hit.
    """
    if not path.exists():
        return {}
    out: dict[str, dict] = {"cpu": {}, "gpu": {}, "frame": None}
    section = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if line.startswith("[CPU]") or line.startswith("[GPU]"):
            section = "cpu" if line.startswith("[CPU]") else "gpu"
            if out["frame"] is None and "frame=" in line:
                out["frame"] = int(line.split("frame=")[1].split()[0])
            continue
        if section is None or "avg:" not in line:
            continue
        name = line.split("avg:")[0].strip()
        if name not in wanted:
            continue
        try:
            avg = float(line.split("avg:")[1].split("max:")[0])
            mx = float(line.split("max:")[1].split("usages:")[0])
        except (IndexError, ValueError):
            continue
        out[section][name] = {"avgMs": avg, "maxMs": mx}
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--analyze-only", action="store_true", help="skip capture, measure what is on disk")
    ap.add_argument("--views", nargs="*", default=None, help="subset of view ids")
    ap.add_argument("--modes", nargs="*", default=None, help="subset of mode ids")
    args = ap.parse_args()

    manifest = load_manifest()
    out_dir = PROJECT_ROOT / manifest["recipe"]["outputDir"]
    out_dir.mkdir(parents=True, exist_ok=True)

    views = [v for v in manifest["views"] if not args.views or v["id"] in args.views]
    modes = [m for m in manifest["modes"] if not args.modes or m["id"] in args.modes]

    results = []
    for view in views:
        for mode in modes:
            stem = f"{view['id']}_{mode['id']}"
            png = out_dir / f"{stem}.png"
            prof = out_dir / f"{stem}.prof.txt"
            if not args.analyze_only:
                print(f"capturing {stem}")
                capture(manifest, view, mode, png, prof)
            if not png.exists():
                print(f"  missing capture: {png}")
                continue
            entry = {"view": view["id"], "mode": mode["id"], "png": png.name}
            entry.update(measure(png))
            entry["prof"] = parse_profdump(prof, PROFILED_ROWS)
            results.append(entry)

    report = out_dir / "measurements.json"
    with report.open("w", encoding="utf-8") as f:
        json.dump({"manifestVersion": manifest["version"], "results": results}, f, indent=2)
    print(f"\nwrote {report}")

    print(f"\n{'view/mode':28} {'median':>8} {'p95':>8} {'p99':>8} {'clip%':>7} {'dark%':>7}")
    for r in results:
        print(f"{r['view'] + '/' + r['mode']:28} {r['lumaMedian']:8.4f} {r['lumaP95']:8.4f} "
              f"{r['lumaP99']:8.4f} {r['clippedAnyChannelPct']:7.3f} {r['shadowPixelsBelow2Pct']:7.3f}")


if __name__ == "__main__":
    main()
