#!/usr/bin/env python3
"""The gate of build_dist.py's check mode (docs/distribution_build_plan.md, the leftovers of 2026-11-16): the dependency walk
on the test profile must pass, and three control arms must FAIL and say why -- or the gate's silence proves nothing.

    1. the test profile as it is              -> exit 0, no error
    2. its include + a file that is not there  -> exit 3, "missing file data/__check_gate_missing__.json (named by profile include)"
    3. a level whose object names a missing mesh -> exit 3, the error names the level AND the object -- what the Build
       Distribution window turns into a button that selects it
    4. a level in a folder no package carries (logs/) -> exit 3, "lies in a folder no package carries" (it shipped as
       nothing, silently, before 2026-11-16)

Needs x64/Cook/test_cube.exe (build the Cook configuration). Writes nothing it does not remove: the control profiles and
the level in out/check_gate/ (gitignored; logs/ is a folder no package carries), the reports in a temp folder. Exit = the
arms that failed.

    python tools/dist/check_gate.py
"""

import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
MISSING_INCLUDE = "data/__check_gate_missing__.json"
MISSING_MESH = "models/__check_gate__/__check_gate__.mesh.json"
GATE_LEVEL = "out/check_gate/level.json"
GATE_OBJECT = "check gate object"
UNSHIPPABLE_LEVEL = "logs/check_gate_level.json"


def check(profile, out_dir):
    """build_dist.py --mode=check: (exit code, the done event or None, the error messages)."""
    cmd = [sys.executable, str(ROOT / "tools" / "build_dist.py"), f"--profile={profile}", "--mode=check",
           f"--out={out_dir}", "--progress=json"]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=900)
    done, errors = None, []
    for line in proc.stdout.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if event.get("event") == "done":
            done = event
        if event.get("level") == "error":
            errors.append(event.get("message", ""))
    return proc.returncode, done, errors


def main():
    if not (ROOT / "x64" / "Cook" / "test_cube.exe").exists():
        print("check gate: no x64/Cook/test_cube.exe -- build the Cook configuration first")
        return 1
    failed = 0
    work = pathlib.Path(tempfile.mkdtemp(prefix="check_gate_"))
    gate_dir = ROOT / "out" / "check_gate"
    try:
        gate_dir.mkdir(parents=True, exist_ok=True)
        base = json.loads((ROOT / "tools" / "dist" / "test.json").read_text(encoding="utf-8"))

        # 1. The test profile as it is.
        code, done, errors = check("test", work / "out1")
        ok = code == 0 and done is not None and done.get("errors") == 0
        print(f"check gate: test profile -- exit {code}, errors {done.get('errors') if done else '?'}: {'PASS' if ok else 'FAIL'}")
        for e in errors[:5]:
            print(f"    {e}")
        failed += 0 if ok else 1

        # 2. A missing file in the include: must fail and name it.
        profile = dict(base)
        profile["include"] = list(base.get("include", [])) + [MISSING_INCLUDE]
        path = gate_dir / "missing_include.json"
        path.write_text(json.dumps(profile, indent=2), encoding="utf-8")
        code, done, errors = check(str(path), work / "out2")
        want = f"missing file {MISSING_INCLUDE} (named by profile include)"
        ok = code == 3 and want in errors
        print(f"check gate: control, a missing include -- exit {code}: {'PASS (refused, named)' if ok else 'FAIL'}")
        if not ok:
            print(f"    wanted the error: {want}")
            for e in errors[:5]:
                print(f"    got: {e}")
        failed += 0 if ok else 1

        # 3. A level whose object names a missing mesh: must fail and name the level and the object.
        level = {"objects": [{"enabled": True, "id": 1, "name": GATE_OBJECT, "type": "staticMesh", "mesh": MISSING_MESH,
                              "position": [0.0, 0.0, 0.0], "rotationDeg": [0.0, 0.0, 0.0], "scale": [1.0, 1.0, 1.0]}]}
        (ROOT / GATE_LEVEL).write_text(json.dumps(level, indent=2), encoding="utf-8")
        profile = dict(base)
        profile["levels"] = [GATE_LEVEL]
        profile["bootLevel"] = GATE_LEVEL
        path = gate_dir / "missing_mesh.json"
        path.write_text(json.dumps(profile, indent=2), encoding="utf-8")
        code, done, errors = check(str(path), work / "out3")
        want = f"missing file {MISSING_MESH} (named by {GATE_LEVEL}, object '{GATE_OBJECT}')"
        ok = code == 3 and want in errors
        print(f"check gate: control, an object's missing mesh -- exit {code}: {'PASS (refused, the object named)' if ok else 'FAIL'}")
        if not ok:
            print(f"    wanted the error: {want}")
            for e in errors[:5]:
                print(f"    got: {e}")
        failed += 0 if ok else 1

        # 4. A level in a folder no package carries: must fail and say so.
        (ROOT / UNSHIPPABLE_LEVEL).write_text(json.dumps(level, indent=2), encoding="utf-8")
        profile = dict(base)
        profile["levels"] = [UNSHIPPABLE_LEVEL]
        profile["bootLevel"] = UNSHIPPABLE_LEVEL
        path = gate_dir / "unshippable_level.json"
        path.write_text(json.dumps(profile, indent=2), encoding="utf-8")
        code, done, errors = check(str(path), work / "out4")
        want = f"level {UNSHIPPABLE_LEVEL} lies in a folder no package carries -- move it under data/levels/"
        ok = code == 3 and want in errors
        print(f"check gate: control, a level under logs/ -- exit {code}: {'PASS (refused)' if ok else 'FAIL'}")
        if not ok:
            print(f"    wanted the error: {want}")
            for e in errors[:5]:
                print(f"    got: {e}")
        failed += 0 if ok else 1
    finally:
        (ROOT / UNSHIPPABLE_LEVEL).unlink(missing_ok=True)
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(gate_dir, ignore_errors=True)
        try:
            (ROOT / "out").rmdir()  # only if the gate made it and left it empty
        except OSError:
            pass
    print(f"check gate: {'PASS' if failed == 0 else f'FAIL ({failed} arm(s))'}")
    return failed


if __name__ == "__main__":
    sys.exit(main())
