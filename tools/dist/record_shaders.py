#!/usr/bin/env python3
"""Distribution plan D4 (docs/distribution_build_plan.md §5 item 3): record the shader permutations a shipped build asks for.

Runs the Release exe (the Shipping defines: no WITH_EDITOR) over every level and every arm -- time of day, window size,
DLSS mode, Play -- with --shader-model=6.6 (the shipped target) and --shader-record / --vfs-record, then merges the
records into tools/dist/shader_list.txt (what --shader-cook compiles) and tools/dist/vfs_record.txt (every content file
the runs opened: what the cook's static walk misses -- files named only in code). Both lists are sorted; the script
prints what changed against the committed ones, so an update is reviewed, not taken blindly.

    python tools/dist/record_shaders.py                              # wind_test, the default arms
    python tools/dist/record_shaders.py --levels data/levels/a.json data/levels/b.json
"""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]

ARMS = [
    ("day 1440p", ["--time=15:00", "--window=2560x1440"]),
    ("night 1440p", ["--time=00:00", "--window=2560x1440"]),
    ("twilight DLAA", ["--time=19:30", "--dlss=dlaa"]),
    ("day 1080p no DLSS", ["--time=15:00", "--window=1920x1080", "--dlss=off"]),
    ("day 4K", ["--time=15:00", "--window=3840x2160"]),
    ("Play", ["--time=15:00", "--play"]),
]


def read_lines(path):
    if not path.exists():
        return set()
    return {l.rstrip("\r\n") for l in path.read_text(encoding="utf-8").splitlines() if l.strip()}


def write_sorted(path, lines, header):
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        for line in sorted(lines):
            f.write(line + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "x64" / "Release" / "test_cube.exe"))
    ap.add_argument("--levels", nargs="+", default=["data/levels/wind_test.json"])
    ap.add_argument("--shader-list", default=str(ROOT / "tools" / "dist" / "shader_list.txt"))
    ap.add_argument("--vfs-list", default=str(ROOT / "tools" / "dist" / "vfs_record.txt"))
    ap.add_argument("--delay", default="8")
    args = ap.parse_args()

    shaders, files = set(), set()
    failed = 0
    with tempfile.TemporaryDirectory(prefix="tc_record_", ignore_cleanup_errors=True) as tmp:
        tmp = pathlib.Path(tmp)
        n = 0
        for level in args.levels:
            for name, extra in ARMS:
                n += 1
                rec, vrec, shot = tmp / f"s{n}.txt", tmp / f"v{n}.txt", tmp / f"shot{n}.png"
                cmd = [args.exe, f"--level={level}", "--shader-model=6.6", f"--shader-record={rec}", f"--vfs-record={vrec}",
                       f"--shot={shot}", f"--shot-delay={args.delay}", "--wind-freeze=5"] + extra
                code = subprocess.run(cmd, cwd=ROOT, timeout=900).returncode
                got_s, got_v = read_lines(rec), read_lines(vrec)
                print(f"{level} / {name}: exit {code}, {len(got_s)} permutations, {len(got_v)} files", flush=True)
                if code != 0 or not got_s:
                    failed += 1
                shaders |= got_s
                files |= got_v

    header = "# docs/distribution_build_plan.md §5: the shader permutations a shipped build asks for -- file|entry|stage|defines.\n" \
             "# Written by tools/dist/record_shaders.py; --shader-cook compiles exactly these.\n"
    old_s = {l for l in read_lines(pathlib.Path(args.shader_list)) if not l.startswith("#")}
    old_v = {l for l in read_lines(pathlib.Path(args.vfs_list)) if not l.startswith("#")}
    write_sorted(pathlib.Path(args.shader_list), shaders, header)
    write_sorted(pathlib.Path(args.vfs_list), files,
                 "# docs/distribution_build_plan.md §8.1: every content file the record runs opened.\n")
    for label, new, old in (("permutations", shaders, old_s), ("files", files, old_v)):
        added, removed = sorted(new - old), sorted(old - new)
        print(f"{label}: {len(new)} ({len(added)} added, {len(removed)} removed against the committed list)")
        for line in added[:40]:
            print("  +", line[:200])
        for line in removed[:40]:
            print("  -", line[:200])
    return failed


if __name__ == "__main__":
    sys.exit(main())
