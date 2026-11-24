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

# ---- 5. a publication killed anywhere: the next writer finishes it, and the parts are of ONE build (M12) ----
# build 1 is in place (folder, symbols, zip, history); build 2 is published (bd.publish) and the run dies at the k-th
# os.replace of it -- every point: the record, each move, each note of a move, the history. The death is real: the
# publication runs in a child process that ends itself (os._exit) there, so no except and no finally of build_dist runs,
# as when the editor's Cancel or a crash ends it (an exception is branch 6's road). Then the next writer
# (bd.finish_publication, as main / --delete-build / --mark-published call it) and the four parts are compared: the
# folder's manifest against the history's last entry of the version, the exe of dist_symbols/<version+build> against the
# manifest's hash, the zip's manifest against the folder's. The control: the same death, nobody finishing it.
import hashlib  # noqa: E402
import shutil  # noqa: E402
import zipfile  # noqa: E402

VERSION, NAME = "0.1.0", "test_cube-0.1.0"


def make_build(root, build):
    """A build's parts as build_dist leaves them in its staging: the folder, the symbols, the zip; and its entry."""
    folder = root / NAME
    folder.mkdir(parents=True)
    exe = f"exe of build {build}".encode()
    (folder / "test_cube.exe").write_bytes(exe)
    manifest = {"version": VERSION, "build": build, "versionString": f"{VERSION}+{build} (probe)",
                "files": {"test_cube.exe": {"size": len(exe), "sha256": hashlib.sha256(exe).hexdigest()}}}
    (folder / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    symbols = root / "symbols"
    symbols.mkdir()
    (symbols / "test_cube.exe").write_bytes(exe)
    (symbols / "test_cube.pdb").write_text(f"pdb of build {build}", encoding="utf-8")
    zip_path = root / f"{NAME}.zip"
    with zipfile.ZipFile(zip_path, "w") as z:
        for p in folder.iterdir():
            z.write(p, f"{NAME}/{p.name}")
    entry = {"product": "test_cube", "version": VERSION, "build": build, "versionString": manifest["versionString"],
             "profile": "test", "flavour": "shipping", "result": "built"}
    return folder, symbols, zip_path, entry


def moves_for(out, work, build, folder, symbols, zip_path):
    final, symbols_final = out / NAME, bd.symbols_root(out) / f"{VERSION}+{build}"
    moves = []
    if final.exists():
        moves.append((final, work / "replaced"))
    moves.append((folder, final))
    if symbols_final.exists():
        moves.append((symbols_final, work / "replaced_symbols"))
    moves.append((symbols, symbols_final))
    if (out / f"{NAME}.zip").exists():  # the last build's zip, aside first (build_dist.build does the same)
        moves.append((out / f"{NAME}.zip", work / "replaced_zip"))
    moves.append((zip_path, out / f"{NAME}.zip"))
    return moves


def one_build(out):
    """'' when the folder, the symbols, the zip and the history are of one build, else what differs."""
    manifest_path = out / NAME / "manifest.json"
    if not manifest_path.exists():
        return "no version folder in the output"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    entries = [h for h in bd.load_history(out) if h.get("version") == VERSION]
    if not entries or entries[-1].get("build") != manifest["build"] or entries[-1].get("versionString") != manifest["versionString"]:
        return f"the folder is build {manifest['build']}, the history's last is {entries[-1].get('build') if entries else None}"
    symbols_exe = bd.symbols_root(out) / f"{VERSION}+{manifest['build']}" / "test_cube.exe"
    if not symbols_exe.exists() or hashlib.sha256(symbols_exe.read_bytes()).hexdigest() != manifest["files"]["test_cube.exe"]["sha256"]:
        return f"dist_symbols/{VERSION}+{manifest['build']} is not that exe's"
    if not (out / f"{NAME}.zip").exists():
        return "no zip beside the folder"
    with zipfile.ZipFile(out / f"{NAME}.zip") as z:
        if json.loads(z.read(f"{NAME}/manifest.json").decode("utf-8")) != manifest:
            return "the zip holds another build's manifest"
    return ""


KILL_CHILD = """
import json, os, pathlib, sys
sys.path.insert(0, sys.argv[1])
import build_dist as bd
out, work, at = pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3]), int(sys.argv[4])
moves = [(pathlib.Path(s), pathlib.Path(d)) for s, d in json.loads(sys.argv[5])]
real, calls = os.replace, [0]
def dying(src, dst):
    calls[0] += 1
    if calls[0] == at:
        os._exit(9)  # the process ends here: nothing after it runs
    return real(src, dst)
os.replace = dying
class Quiet:
    def emit(self, *a, **k): pass
    def begin(self, *a, **k): pass
bd.publish(out, moves, json.loads(sys.argv[6]), work, Quiet())
"""


def build_one_in_place(name):
    """A fresh output with build 1 published, and build 2's parts in a staging beside it: (out, staging, moves, entry)."""
    out = fresh(name)
    old_work, new_work = out.parent / "old_work", out.parent / ".dist_tmp" / "build_x"
    old_work.mkdir()
    new_work.mkdir(parents=True)
    f1, s1, z1, e1 = make_build(old_work, 1)
    bd.publish(out, moves_for(out, old_work, 1, f1, s1, z1), e1, old_work, Quiet())
    f2, s2, z2, e2 = make_build(new_work, 2)
    return out, new_work, moves_for(out, new_work, 2, f2, s2, z2), e2


def build_of(out):
    path = out / NAME / "manifest.json"
    return json.loads(path.read_text(encoding="utf-8")).get("build") if path.exists() else None


points = None
for kill_at in range(1, 64):
    out, new_work, moves, e2 = build_one_in_place(f"publish_{kill_at}")
    check_first = one_build(out)
    child = subprocess.run([sys.executable, "-I", "-c", KILL_CHILD, str(repo / "tools"), str(out), str(new_work), str(kill_at),
                            json.dumps([[str(s), str(d)] for s, d in moves]), json.dumps(e2)],
                           capture_output=True, text=True, timeout=60)
    if child.returncode == 0:
        points = kill_at - 1
        check(check_first == "" and one_build(out) == "", f"publication: {points} os.replace calls, all made, one build")
        break
    if child.returncode != 9:
        check(False, f"publication killed at its os.replace #{kill_at}: the child ended otherwise ({child.returncode}): "
                     f"{child.stderr.strip()[-300:]}")
        continue
    left = one_build(out)  # the control: what the death left, nobody finishing it
    record_left = bd.publication_path(out).exists()
    bd.finish_publication(out, Quiet(), recovering=True)  # the next writer
    after = one_build(out)
    # A record names the dead staging, which the finish removes; without one (dead before the record) clear_stale_work does.
    check(after == "" and not bd.publication_path(out).exists() and (not record_left or not new_work.exists()),
          f"publication killed at its os.replace #{kill_at}: left {('one build' if not left else repr(left))}; the next writer "
          f"finished it: {'one build, ' + str(build_of(out)) if after == '' else repr(after)}")
check(points is not None and points >= 9, f"the publication's points were all tried ({points})")

# ---- 6. a publication that fails by an EXCEPTION inside build(): undone, build 1 stays (M review, 2026-11-24) ----
# A move refused (the old folder held by its running exe or a console in it -- PermissionError), Ctrl+C. build() ends with
# clear_own_work, which removes its staging. Before the undo the record stayed and the staging went, and the next writer
# "finished" a publication whose pieces were gone: it moved the old folder into the dead staging and deleted it, and
# listed build 2, which never existed. First every point fails once (the undo puts build 1 back, the record goes); then
# every point fails and so does every os.replace after it (the undo cannot be made: the record and the staging stay, and
# the next writer, the disk free again, finishes the publication). The control: the same failures without the undo.
real_replace = bd.os.replace
for stuck in (False, True):
    points6 = None
    for fail_at in range(1, 64):
        out, new_work, moves, e2 = build_one_in_place(f"publish_fail_{int(stuck)}_{fail_at}")
        calls = [0]

        def failing_replace(src, dst, _calls=calls, _at=fail_at, _stuck=stuck):
            _calls[0] += 1
            if _calls[0] == _at or (_stuck and _calls[0] > _at):
                raise PermissionError(13, "the process cannot access the file because another process is using it", str(src))
            return real_replace(src, dst)

        bd.os.replace = failing_replace
        failed = False
        try:
            bd.publish(out, moves, e2, new_work, Quiet())
        except PermissionError:
            failed = True
        finally:
            bd.clear_own_work(out, new_work, Quiet())  # build()'s finally
            bd.os.replace = real_replace
        if not failed:
            points6 = fail_at - 1
            check(one_build(out) == "" and build_of(out) == 2, f"publication ({'stuck' if stuck else 'one failure'}): "
                                                                f"{points6} os.replace calls, all made, build 2")
            break
        left, left_build = one_build(out), build_of(out)
        record_left, staging_left = bd.publication_path(out).exists(), new_work.exists()
        bd.finish_publication(out, Quiet(), recovering=True)  # the next writer
        after, after_build = one_build(out), build_of(out)
        what = (f"left {('build ' + str(left_build)) if not left else repr(left)}, record {'kept' if record_left else 'gone'}, "
                f"staging {'kept' if staging_left else 'gone'}; the next writer: "
                f"{('build ' + str(after_build)) if not after else repr(after)}")
        if not stuck:
            ok = left == "" and left_build == 1 and not record_left and after == "" and after_build == 1
        else:
            ok = (staging_left or not record_left) and after == "" and not bd.publication_path(out).exists()
        check(ok and not new_work.exists(),
              f"publication {'stuck' if stuck else 'refused'} at its os.replace #{fail_at} (PermissionError): {what}")
    check(points6 is not None and points6 >= 9, f"the failing publication's points were all tried ({points6})")

print()
print("dist probe:", "PASS" if not failures else f"FAIL ({len(failures)})")
sys.exit(len(failures))
