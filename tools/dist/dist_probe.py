# Probes of tools/build_dist.py's locks and of its history reader (review 10.14, fixes of 2026-11-24). Everything is
# written under a scratch output folder: nothing of the repository's dist/ is touched.
#   python tools/dist/dist_probe.py <repo root> <scratch folder>   (the scratch folder: a session's own, never under dist/)
import json
import pathlib
import subprocess
import sys
import time

repo = pathlib.Path(sys.argv[1]).resolve()
scratch = pathlib.Path(sys.argv[2]).resolve()
sys.path.insert(0, str(repo / "tools"))
import build_dist as bd  # noqa: E402

failures = []


def check(ok, what):
    print(("PASS  " if ok else "FAIL  ") + what)
    if not ok:
        failures.append(what)


class Quiet:
    as_json = False

    def emit(self, message, level="info", **extra):
        print(f"        [{level}] {message}")

    def begin(self, stage, message):
        print(f"        [{stage}] {message}")


def fresh(name):
    out = scratch / name / "dist"
    if out.parent.exists():
        import shutil
        shutil.rmtree(out.parent, ignore_errors=True)
    out.mkdir(parents=True)
    return out


HOLDER = "import sys, time\nf = open(sys.argv[1], 'w')\nprint('held', flush=True)\ntime.sleep(60)\n"

# ---- 1. the history: damaged is not empty ----
out = fresh("history")
check(bd.load_history(out) == [], "no history file: an empty history")
bd.history_path(out).write_text("   \n", encoding="utf-8")
check(bd.load_history(out) == [], "a blank file: an empty history")
good = [{"build": 3, "result": "built", "version": "0.1.0", "published": True}]
bd.history_path(out).write_text(json.dumps(good), encoding="utf-8")
check(bd.load_history(out) == good, "a list of builds is read")
for label, text in (("cut short", '[{"build": 3, "result": "bui'), ("an object, not a list", '{"build": 3}'),
                    ("a list of numbers", "[1, 2]"), ("merge markers", "<<<<<<< HEAD\n[]\n=======\n[]\n>>>>>>> x\n")):
    bd.history_path(out).write_text(text, encoding="utf-8")
    try:
        bd.load_history(out)
        check(False, f"a damaged history ({label}) refuses")
    except bd.BuildError as e:
        check("damaged" in str(e), f"a damaged history ({label}) refuses: {str(e)[:70]}...")
    check(bd.load_history(out, strict=False) is None, f"... and a check reads it as damaged, not as empty ({label})")

# ---- 2. the staging: a live run's folder stays, a dead run's goes ----
out = fresh("staging")
tmp_root = out.parent / ".dist_tmp"
live = tmp_root / "build_live"
dead = tmp_root / "build_dead"
old = tmp_root / "build_old"  # a folder of a build before the locks: no lock file at all
for d in (live, dead, old):
    (d / "logs").mkdir(parents=True)
    (d / "logs" / "x.log").write_text("x", encoding="utf-8")
(dead / bd.WORK_LOCK).write_text("", encoding="utf-8")  # its owner is gone: nobody holds it
holder = subprocess.Popen([sys.executable, "-c", HOLDER, str(live / bd.WORK_LOCK)], stdout=subprocess.PIPE, text=True)
holder.stdout.readline()
bd.clear_stale_work(tmp_root, Quiet())
check(live.exists() and (live / "logs" / "x.log").exists(), "a live run's staging is left alone")
check(not dead.exists() and not old.exists(), "a dead run's staging and one without a lock are removed")
holder.kill()
holder.wait()
time.sleep(0.2)
bd.clear_stale_work(tmp_root, Quiet())
check(not live.exists(), "once its process is gone (killed), the staging is removed")

# ---- 3. one writer of an output folder ----
out = fresh("writer")
first = bd.acquire_writer(out)
try:
    bd.acquire_writer(out)
    check(False, "a second writer in the same process is refused")
except bd.BuildError as e:
    check("another build is writing" in str(e), "a second writer is refused while the first holds the folder")
bd.release_writer(first)
second = bd.acquire_writer(out)
check(second is not None, "after the release the folder can be taken again")
bd.release_writer(second)
check(not (out.parent / ".dist_tmp").exists(), "the release leaves no .dist_tmp behind")
# A writer in ANOTHER process, then that process killed: its lock is stale and the folder is free.
lock = out.parent / ".dist_tmp" / bd.WRITER_LOCK
lock.parent.mkdir(parents=True)
holder = subprocess.Popen([sys.executable, "-c", HOLDER, str(lock)], stdout=subprocess.PIPE, text=True)
holder.stdout.readline()
try:
    bd.acquire_writer(out)
    check(False, "a writer in another process is seen")
except bd.BuildError:
    check(True, "a writer in another process is seen")
holder.kill()
holder.wait()
time.sleep(0.2)
third = bd.acquire_writer(out)
check(third is not None, "a killed writer's lock is stale: the folder is free")
bd.release_writer(third)

# ---- 4. the script itself: a content build over a damaged history refuses before it does anything ----
out = fresh("script")
bd.history_path(out).write_text('[{"build": 1, "resu', encoding="utf-8")
run = subprocess.run([sys.executable, str(repo / "tools" / "build_dist.py"), "--profile=test", "--mode=content", "--skip-build",
                      f"--out={out}"], capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=repo)
text = run.stdout + run.stderr
check(run.returncode == 1 and "is damaged" in text, f"build_dist.py --mode=content: exit {run.returncode}, refuses a damaged history")
check(bd.history_path(out).read_text(encoding="utf-8") == '[{"build": 1, "resu', "... and leaves the file as it was")
check(not any(out.iterdir().__next__().name != "build_history.json" for _ in [0]) and len(list(out.iterdir())) == 1,
      "... and writes nothing else into the output folder")
check(not (out.parent / ".dist_tmp").exists() or not any((out.parent / ".dist_tmp").iterdir()),
      "... and leaves no staging and no lock behind")

# ---- 5. delete: the root itself is never a build's file, and the symbols are the OUTPUT FOLDER's, not the repository's ----
# The first cut of this step (2026-11-24) named its build "0.1.0+1" and the script looked for symbols under the
# repository whatever --out said: it deleted the real dist_symbols/0.1.0+1. The version below cannot exist, the
# repository's folder is listed before and after, and the script keeps symbols beside its own output folder now.
def listing(folder):
    return sorted((p.name, p.stat().st_mtime_ns) for p in folder.iterdir()) if folder.exists() else None


repo_symbols = repo / "dist_symbols"
before = listing(repo_symbols), (repo_symbols.stat().st_mtime_ns if repo_symbols.exists() else None)
check(bd.symbols_root(repo / "dist") == repo_symbols, "the repository's own output folder keeps its symbols where they were")
out = fresh("delete")
check(bd.symbols_root(out) == out.parent / "dist_symbols" and repo not in bd.symbols_root(out).parents,
      "another output folder keeps its symbols beside itself")
(out / "keep.txt").write_text("the output folder's own file", encoding="utf-8")
own_symbols = bd.symbols_root(out) / "9.9.9+999"
own_symbols.mkdir(parents=True)
(own_symbols / "test_cube.pdb").write_text("a probe's", encoding="utf-8")
history = [{"build": 999, "result": "built", "version": "9.9.9", "versionString": "9.9.9+999 (probe)", "folder": str(out), "zip": None}]
bd.history_path(out).write_text(json.dumps(history), encoding="utf-8")
run = subprocess.run([sys.executable, str(repo / "tools" / "build_dist.py"), "--profile=test", "--delete-build=999", f"--out={out}"],
                     capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=repo)
check(run.returncode == 0 and (out / "keep.txt").exists(),
      f"--delete-build of an entry whose folder is the output folder itself: exit {run.returncode}, the folder stays")
check(not own_symbols.exists() and bd.symbols_root(out).exists(), "its own symbols are deleted, their root is not")
check(bd.load_history(out) == [], "... and the entry leaves the history")
after = listing(repo_symbols), (repo_symbols.stat().st_mtime_ns if repo_symbols.exists() else None)
check(before == after, "the repository's dist_symbols is as it was: nothing listed, added or removed")

print()
print("dist probe:", "PASS" if not failures else f"FAIL ({len(failures)})")
sys.exit(len(failures))
