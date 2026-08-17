"""Compile the engine's compute shaders offline, so a syntax error cannot reach a run.

MSBuild does NOT compile HLSL in this project -- shaders are compiled at runtime, which means a
broken shader shows up as a *silently missing feature* in Release (the material is null, its pass
early-outs) and as an assert on a constant-buffer field name in Debug. Neither points at the actual
error. This does.

    python tools/check_shaders.py                 # every entry point listed below
    python tools/check_shaders.py exposure        # only shaders whose path contains "exposure"

Exit code is non-zero if anything failed, so it can gate a change set.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
SHADERS = PROJECT_ROOT / "shaders"

# (file, entry point). Only compute shaders are listed: the graphics ones take preprocessor
# permutations from the material system that are not reproducible from the command line.
COMPUTE_ENTRIES = [
    ("exposure_histogram_cs.hlsl", "CSClear"),
    ("exposure_histogram_cs.hlsl", "CSBuild"),
    ("exposure_solve_cs.hlsl", "CSMain"),
    ("exposure_baselum_cs.hlsl", "CSMain"),
    ("tonemap_cs.hlsl", "CSMain"),
    ("fxaa_cs.hlsl", "CSMain"),
    ("compose_cs.hlsl", "CSMain"),
    ("lighting_cs.hlsl", "CSMain"),
    ("ssr_cs.hlsl", "CSMain"),
    ("gtao_cs.hlsl", "CSMain"),
    ("gtao_filter_cs.hlsl", "CSMain"),
    ("gtao_temporal_cs.hlsl", "CSMain"),
    ("gtao_upsample_cs.hlsl", "CSMain"),
]


def find_dxc() -> Path | None:
    roots = [
        Path(r"C:\Program Files (x86)\Windows Kits\10\bin"),
        Path(r"C:\Program Files\Windows Kits\10\bin"),
    ]
    found = []
    for root in roots:
        if root.is_dir():
            found.extend(root.glob("*/x64/dxc.exe"))
            found.extend(root.glob("*/x86/dxc.exe"))
    return sorted(found)[-1] if found else None


def main() -> int:
    dxc = find_dxc()
    if dxc is None:
        print("dxc.exe not found in the Windows SDK; cannot check shaders")
        return 2

    needle = sys.argv[1].lower() if len(sys.argv) > 1 else ""
    failures = 0
    checked = 0
    for name, entry in COMPUTE_ENTRIES:
        if needle and needle not in name.lower():
            continue
        path = SHADERS / name
        if not path.exists():
            print(f"SKIP  {name} (missing)")
            continue
        checked += 1
        result = subprocess.run(
            [str(dxc), "-T", "cs_6_0", "-E", entry, str(path), "-Fo", "NUL"],
            capture_output=True, text=True, cwd=SHADERS,
        )
        if result.returncode == 0:
            print(f"ok    {name}:{entry}")
        else:
            failures += 1
            print(f"FAIL  {name}:{entry}")
            for line in (result.stderr or result.stdout).splitlines()[:12]:
                print(f"      {line}")

    print(f"\n{checked - failures}/{checked} compiled")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
