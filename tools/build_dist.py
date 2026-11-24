#!/usr/bin/env python3
r"""The distribution build -- docs/distribution_build_plan.md §8.2, §8.3. One command from the tree to
dist/<Product>-<version>/ (and its zip), the same for the console, the editor's Build Distribution window and MCP.

    python tools/build_dist.py --profile=test                    # content: Cook up to date, cook + the folder, Shipping exe as built
    python tools/build_dist.py --profile=release --mode=full     # MSBuild Cook + Shipping, cook, folder, zip, symbols
    python tools/build_dist.py --profile=test --mode=check       # Cook up to date, the dependency walk and its report
    python tools/build_dist.py --profile=test --mode=record      # Release record runs -> shader_list.txt / vfs_record.txt

    --levels=a.json,b.json  --boot-level=a.json  --version=1.2.3  --no-encrypt
                            override the profile for this build (named in the report)
    --override-licences     this build ships the assets the profile's "strictLicences" refuses (no known licence in
                            CREDITS.md): a warning each, listed in the cook report and the build's history entry
                            ("licencesOverridden"); the profile keeps its rule
    --flavour=shipping|development
                            which exe goes into the folder (docs/scalability_plan.md §7): Shipping (the game, the developer
                            features stripped) or Development (the same packaged game with Developer Controls, the F keys,
                            the whole command line; x64/Development, the folder <product>-<version>-dev, its PDB beside the
                            exe, never encrypted, the profile's "devInclude" cooked too). Default: the profile's "flavour",
                            else shipping.
    --mark-published=<build>[,<build>...]   --unmark-published=<build>[,...]
                            a build handed to players (or a mark set by mistake), in <out>/build_history.json -- the
                            window's History "Published" column; nothing is built. A profile with "versionMustGrow" then
                            needs a version above every PUBLISHED build of it (its flavour); one built but not published
                            builds again, its folder and zip replaced
    --delete-build=<build>[,<build>...]
                            a build out of the history AND off the disk (the window's History "Delete"): its folder, zip
                            and kept cook report when they are still its own (a later build of the same version replaced
                            them: kept), and its symbols (dist_symbols/<a.b.c+build>). Only under the output folder and
                            dist_symbols/; a published build is refused (--unmark-published first). Nothing is built
    --skip-build            take the exes as they are: check/content do not bring x64/Cook up to date (an incremental
                            MSBuild, 0.5 s when nothing changed), full builds neither Cook nor the game's exe
    --key=<key.json>        the package key (default %USERPROFILE%\.test_cube\pak_key.json; made there if missing)
    --out=<folder>          instead of the profile's "out" (dist); its symbols are kept beside it
                            (<folder>/../dist_symbols), never in the repository's when the folder is elsewhere
    --progress=json         one JSON object a line on stdout: {"stage", "fraction", "message", "level", ...}
    --keep-temp             leave the staging folder (debugging)

Everything is built in .dist_tmp/ beside the output folder (<repo>/.dist_tmp for dist/: the same volume, so the finished
folder is renamed into place whole) -- a cancelled or failed build leaves nothing in the output folder; a killed one leaves
.dist_tmp, which the next run clears. Exit code 0 = built.
"""

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime
import glob
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
VCXPROJ = ROOT / "test_cube.vcxproj"
VERSION_HEADER = ROOT / "sources" / "app" / "generated" / "BuildVersion.g.h"
KEY_HEADER = ROOT / "sources" / "core" / "vfs" / "generated" / "PakKey.g.h"
DEFAULT_KEY = pathlib.Path(os.environ.get("USERPROFILE", str(pathlib.Path.home()))) / ".test_cube" / "pak_key.json"
EXE_NAME = "test_cube.exe"

DEV_VERSION_HEADER = """// The dev build's version: written by tools/build_dist.py after a distribution build (and by test_cube.vcxproj when missing).
#pragma once
#define TC_PRODUCT_NAME "test_cube"
#define TC_VERSION_STRING "0.0.0-dev"
#define TC_VERSION_MAJOR 0
#define TC_VERSION_MINOR 0
#define TC_VERSION_PATCH 0
#define TC_VERSION_BUILD 0
#define TC_GIT_HASH ""
#define TC_BUILD_DATE ""
"""

NO_KEY_HEADER = """// No package key: a distribution without encryption (tools/build_dist.py), or the dev tree's.
#pragma once
#define TC_PAK_HAS_EMBEDDED_KEY 0
"""

MIT_TEXT = """Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""

# What the shipped exe and its DLLs carry, and where each one's licence text is (§8.2: THIRD_PARTY_NOTICES.txt).
THIRD_PARTY = [
    ("Dear ImGui", "https://github.com/ocornut/imgui", "third_party/imgui/LICENSE.txt", None),
    ("ImGuizmo", "https://github.com/CedricGuillemet/ImGuizmo", None, "Copyright (c) 2016 Cedric Guillemet"),
    ("Jolt Physics", "https://github.com/jrouwe/JoltPhysics", "third_party/JoltPhysics/LICENSE", None),
    ("meshoptimizer", "https://github.com/zeux/meshoptimizer", "third_party/meshoptimizer/LICENSE.md", None),
    ("DirectXTex", "https://github.com/microsoft/DirectXTex", "third_party/DirectXTex/LICENSE", None),
    ("enkiTS", "https://github.com/dougbinks/enkiTS", "third_party/enkiTS/License.txt", None),
    ("zlib", "https://zlib.net", "third_party/zlib/LICENSE", None),
    ("Zstandard", "https://github.com/facebook/zstd", "third_party/zstd/LICENSE", None),
    ("cgltf", "https://github.com/jkuhlmann/cgltf", None, "Copyright (c) 2018-2021 Johannes Kuhlmann"),
    ("JSON for Modern C++", "https://github.com/nlohmann/json", None, "Copyright (c) 2013-2025 Niels Lohmann"),
    ("robin_hood", "https://github.com/martinus/robin-hood-hashing", None, "Copyright (c) 2018-2021 Martin Ankerl"),
    ("stb_truetype, stb_rect_pack", "https://github.com/nothings/stb", None, "Copyright (c) 2017 Sean Barrett"),
    ("mimalloc", "https://github.com/microsoft/mimalloc", "tools/dist/licenses/mimalloc_LICENSE.txt", None),
    ("oneTBB (tbb12.dll)", "https://github.com/uxlfoundation/oneTBB", "tools/dist/licenses/oneTBB_LICENSE.txt", None),
    ("NVIDIA Streamline (sl.*.dll)", "https://github.com/NVIDIA-RTX/Streamline", "tools/dist/licenses/Streamline_license.txt", None),
    ("NVIDIA DLSS (nvngx_*.dll)", "https://developer.nvidia.com/rtx/dlss", "third_party/streamline/bin/x64/nvngx_dlss.license.txt", None),
    ("DirectX Shader Compiler (dxcompiler.dll, dxil.dll)", "https://github.com/microsoft/DirectXShaderCompiler",
     "tools/dist/licenses/DirectXShaderCompiler_LICENSE.txt", None),
    ("DirectX 12 Agility SDK (D3D12\\D3D12Core.dll)", "https://www.nuget.org/packages/Microsoft.Direct3D.D3D12",
     "third_party/agility/LICENSE.txt", None),
    ("JetBrains Mono (font)", "https://github.com/JetBrains/JetBrainsMono", "fonts/JetBrainsMono-OFL.txt", None),
]


class BuildError(Exception):
    pass


class Progress:
    """The build's events: a line of text, or with --progress=json one JSON object a line (the window and MCP read them)."""

    STAGES = ["prepare", "build", "key", "cook", "stage", "modules", "notices", "check", "zip", "symbols", "finish"]

    def __init__(self, as_json):
        self.as_json = as_json
        self.stage = "prepare"
        self.started = time.time()
        self.stage_started = time.time()

    def _fraction(self):
        return round(self.STAGES.index(self.stage) / len(self.STAGES), 3) if self.stage in self.STAGES else 0.0

    def emit(self, message, level="info", **extra):
        if self.as_json:
            event = {"stage": self.stage, "fraction": self._fraction(), "message": message, "level": level,
                     "seconds": round(time.time() - self.started, 1)}
            event.update(extra)
            print(json.dumps(event, ensure_ascii=False), flush=True)
        else:
            tag = {"info": "", "warning": "WARNING ", "error": "ERROR "}.get(level, "")
            print(f"[{self.stage}] {tag}{message}", flush=True)

    def begin(self, stage, message):
        if self.stage != stage:
            self.emit(f"done in {time.time() - self.stage_started:.1f} s", level="info", event="stage_end")
        self.stage = stage
        self.stage_started = time.time()
        self.emit(message, event="stage")


def kill_tree(pid):
    subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True)


def run(cmd, progress, log_path=None, timeout=3600, cwd=ROOT, env=None):
    """A child process; its output into `log_path` (or inherited). Returns its exit code; past `timeout` its whole tree
    is ended (MSBuild's compilers too, not only MSBuild) and BuildError raised."""
    out = open(log_path, "w", encoding="utf-8", errors="replace") if log_path else None
    # With --progress=json stdout is the event stream: a child's own output goes to its log or nowhere.
    stdout = out if out else (subprocess.DEVNULL if progress.as_json else None)
    try:
        proc = subprocess.Popen(cmd, cwd=cwd, stdout=stdout, stderr=subprocess.STDOUT if stdout else None, env=env)
        try:
            code = proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            kill_tree(proc.pid)
            proc.wait()
            raise BuildError(f"{pathlib.Path(cmd[0]).name} ran past {timeout} s and was ended")
    finally:
        if out:
            out.close()
    return code


def find_msbuild():
    vswhere = pathlib.Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        found = subprocess.run([str(vswhere), "-latest", "-prerelease", "-requires", "Microsoft.Component.MSBuild", "-find",
                                r"MSBuild\**\Bin\MSBuild.exe"], capture_output=True, text=True).stdout.strip().splitlines()
        if found:
            return found[0]
    fallback = r"C:/Program Files/Microsoft Visual Studio/18/Professional/MSBuild/Current/Bin/MSBuild.exe"
    if os.path.exists(fallback):
        return fallback
    raise BuildError("MSBuild not found (vswhere and the known path)")


def msbuild(config, progress, logs, generated=None):
    # test_cube.vcxproj, the configuration, x64, /m /nologo /v:minimal -- and two things so that ending this build (Cancel,
    # the editor closing: its job object) ends nothing anybody else uses: no reusable MSBuild nodes, and a PDB server of
    # this build's own (_MSPDBSRV_ENDPOINT_), not the per-user one another build may be linking through. `generated`:
    # the stamped version / key headers' root (/p:TcGeneratedRoot, test_cube.vcxproj): the tree's own stay the dev ones.
    log = logs / f"msbuild_{config}.log"
    progress.emit(f"MSBuild {config} (log {log})")
    env = dict(os.environ)
    env["_MSPDBSRV_ENDPOINT_"] = f"test_cube_build_dist_{os.getpid()}"
    cmd = [find_msbuild(), str(VCXPROJ), f"/p:Configuration={config}", "/p:Platform=x64", "/m", "/nologo", "/v:minimal",
           "/nodeReuse:false"]
    if generated:
        cmd.append(f"/p:TcGeneratedRoot={generated}")
    code = run(cmd, progress, log_path=log, env=env)
    if code != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-15:]
        for line in tail:
            if " error " in line or "error " in line:
                progress.emit(line.strip()[:300], level="error")
        raise BuildError(f"MSBuild {config} failed (exit {code}); the log: {log}")


def write_if_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == text:
        return
    path.write_text(text, encoding="utf-8", newline="\n")


def version_header(product, version, build, git_hash, dirty, date):
    parts = (version.split("+")[0].split("-")[0].split(".") + ["0", "0", "0"])[:3]
    nums = [int(p) if p.isdigit() else 0 for p in parts]
    text_version = f"{version}+{build} ({git_hash}{'-dirty' if dirty else ''})" if git_hash else f"{version}+{build}"
    return text_version, f"""// Written by tools/build_dist.py for a distribution build ({date}); a dev build gets "0.0.0-dev" back afterwards.
#pragma once
#define TC_PRODUCT_NAME "{product}"
#define TC_VERSION_STRING "{text_version}"
#define TC_VERSION_MAJOR {nums[0]}
#define TC_VERSION_MINOR {nums[1]}
#define TC_VERSION_PATCH {nums[2]}
#define TC_VERSION_BUILD {build}
#define TC_GIT_HASH "{git_hash}{'-dirty' if dirty else ''}"
#define TC_BUILD_DATE "{date}"
"""


def recorded_levels():
    """The levels tools/dist/shader_list.txt says its record runs went over; None when it does not say (an old list)."""
    path = ROOT / "tools" / "dist" / "shader_list.txt"
    if not path.exists():
        return None
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("# levels:"):
            return set(line[len("# levels:"):].split())
        if not line.startswith("#"):
            break
    return None


def git_state():
    try:
        head = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        status = subprocess.run(["git", "status", "--porcelain", "--untracked-files=no"], cwd=ROOT, capture_output=True,
                                text=True).stdout.splitlines()
    except OSError:
        return "", False, []
    # graphics_settings.json is the developer's own settings file: the build ships tools/dist/graphics_defaults.json.
    changed = [l[3:] for l in status if l[3:].strip() not in ("graphics_settings.json",)]
    return head, bool(changed), changed


def load_profile(name):
    path = pathlib.Path(name)
    if not path.suffix:
        path = ROOT / "tools" / "dist" / f"{name}.json"
    elif not path.is_absolute():
        path = (ROOT / path) if (ROOT / path).exists() else path.resolve()
    if not path.exists():
        raise BuildError(f"no profile {path}")
    profile = json.loads(path.read_text(encoding="utf-8"))
    profile["_path"] = path
    profile["_name"] = path.stem
    return profile


def history_path(out_dir):
    return out_dir / "build_history.json"


def load_history(out_dir, strict=True):
    """The builds of this output folder, oldest first; [] when there is no file yet. A file that is there and is not a
    list of builds is DAMAGED, not empty: read as [] (as it was until 2026-11-18) the next build was numbered 1, laid its
    symbols over the real first build's (dist_symbols/<version>+1) and replaced the file, the published marks with it.
    `strict` False (a check: it writes nothing of it) returns None for a damaged file instead of refusing."""
    path = history_path(out_dir)
    if not path.exists():
        return []
    try:
        text = path.read_text(encoding="utf-8")
        if not text.strip():
            return []
        history = json.loads(text)
        if isinstance(history, list) and all(isinstance(h, dict) for h in history):
            return history
        why = "not a list of builds"
    except (ValueError, OSError) as e:
        why = str(e)
    if strict:
        raise BuildError(f"{path} is damaged ({why}): repair it or move it away -- it holds the build numbers and what "
                         f"was published. Nothing was built or changed")
    return None


def symbols_root(out_dir):
    """Where the builds of an output folder keep their symbols: dist_symbols/ BESIDE it -- for the profiles' own
    <repo>/dist that is <repo>/dist_symbols, as it always was. It used to be <repo>/dist_symbols whatever --out said, so
    a run against another output folder read and DELETED the repository's symbols: on 2026-11-24 a probe of
    --delete-build with --out in a temp folder took the real dist_symbols/0.1.0+1 with it."""
    return out_dir.parent / "dist_symbols"


# ---- who is working beside this run ----
# An open file is a process's claim. Windows refuses to delete a file somebody holds open (Python opens without
# FILE_SHARE_DELETE) and lets it go the moment that process is gone, killed included -- no pid to be reused, nothing to
# clean after a crash. Until 2026-11-18 every run began by deleting <out>/../.dist_tmp whole, the check the Build
# Distribution window starts when it opens included: a build from the console lost its staging to it.

WORK_LOCK = "owner.lock"    # in a run's own folder under .dist_tmp: the run is alive
WRITER_LOCK = "writer.lock"  # in .dist_tmp: somebody is writing the output folder (its history, folders, zips)


def held_by_a_live_process(path):
    try:
        path.unlink()
        return False  # nobody held it (it is gone now: a stale claim)
    except FileNotFoundError:
        return False
    except OSError:
        return True


def clear_stale_work(tmp_root, progress, keep=()):
    """The staging folders of runs that are gone (killed, cancelled: nothing of them is a distribution); a live run's
    folder is left alone, and so is one an unfinished publication still moves things out of (`keep`)."""
    keep = {pathlib.Path(k).resolve() for k in keep}
    for entry in tmp_root.iterdir():
        if not entry.is_dir():
            continue
        if entry.resolve() in keep:
            progress.emit(f"{entry} holds an unfinished publication: left for the next build to finish")
            continue
        if held_by_a_live_process(entry / WORK_LOCK):
            progress.emit(f"another build is at work in {entry}: left alone")
            continue
        shutil.rmtree(entry, ignore_errors=True)


# ---- the publication: one build into place, or the next writer finishes it ----
# The folder, its symbols, its zip and its history entry were four separate steps (the symbols written in place first,
# then two renames of the folder, the zip, the history), and a run killed between any two left parts of two builds: a new
# exe beside an old PDB, dist/ without the version, a folder the history does not list (docs/architecture_review_plan.md
# M12). Now every move and the history entry are written down FIRST (<out>/.publishing.json), then made one by one, the
# record noting each one done; a run killed anywhere leaves the record, and the next writer (content, full, delete, mark)
# makes the rest -- a check names it. The pieces wait in the dead run's staging, which clear_stale_work then leaves alone.
# A run that is not killed but meets an EXCEPTION on the way (a move refused: the old folder held by its running exe or a
# console in it; Ctrl+C) undoes the publication instead (undo_publication): build()'s end removes its staging, and a
# record of moves whose pieces are gone was "finished" by the next writer into the old folder deleted and a build listed
# that never existed (found by the review of the M steps, 2026-11-24).

PUBLISHING = ".publishing.json"


def publication_path(out_dir):
    return out_dir / PUBLISHING


def write_publication(out_dir, record):
    tmp = publication_path(out_dir).with_suffix(".tmp")
    tmp.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, publication_path(out_dir))


def unfinished_publication(out_dir):
    """The record a killed publication left, or None."""
    path = publication_path(out_dir)
    if not path.exists():
        return None
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
        return record if isinstance(record, dict) and isinstance(record.get("moves"), list) else None
    except (ValueError, OSError):
        return None


def publish(out_dir, moves, entry, work, progress):
    """`moves`: [(source, destination), ...] in order -- a destination that is there must have been moved away by an
    earlier move. Written down, then made (finish_publication). An exception on the way undoes it (undo_publication: the
    build before is in place again, the record gone) and goes on up; only an undo that fails too leaves the record -- and
    clear_own_work the staging -- for the next writer to finish."""
    write_publication(out_dir, {"work": str(work), "moves": [[str(s), str(d)] for s, d in moves], "done": 0,
                                "entry": entry})
    try:
        finish_publication(out_dir, progress)
    except BaseException:
        try:
            undo_publication(out_dir, progress)
        except Exception as error:
            progress.emit(f"the publication could not be undone either ({error}); its record and staging are kept, and the "
                          f"next content or full build finishes it", level="error")
        raise


def finish_publication(out_dir, progress, recovering=False):
    """Make what the record says is not done yet, then the history entry, then forget the record. A move whose source is
    gone was made (the run died before noting it); each is made once, in its order."""
    record = unfinished_publication(out_dir)
    if record is None:
        return False
    if recovering:
        entry = record.get("entry", {})
        progress.emit(f"finishing the publication of {entry.get('versionString', entry.get('version'))} (build "
                      f"{entry.get('build')}) that a killed build left: {record['done']} of {len(record['moves'])} moves made",
                      level="warning")
    moves = record["moves"]
    for i in range(int(record.get("done", 0)), len(moves)):
        source, destination = pathlib.Path(moves[i][0]), pathlib.Path(moves[i][1])
        if source.exists():
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(source, destination)
        record["done"] = i + 1
        write_publication(out_dir, record)
    history = load_history(out_dir)
    entry = record.get("entry")
    if entry and not any(h.get("build") == entry.get("build") for h in history):
        history.append(entry)
        save_history(out_dir, history)
    publication_path(out_dir).unlink()
    if recovering and record.get("work"):
        shutil.rmtree(record["work"], ignore_errors=True)  # the dead run's staging: nothing left in it to publish
    return True


def undo_publication(out_dir, progress):
    """Put back what this run's unfinished publication moved, the last move first, take its history entry out, and forget
    the record: the build before it is in place again. The record is lowered to each move BEFORE that move is put back,
    so a death inside the undo leaves a record the next writer finishes like any other (a move whose source is gone was
    made). The move the record says is next may have been made too (dead before its note): its destination is there and
    its source is not."""
    record = unfinished_publication(out_dir)
    if record is None:
        return
    moves = record["moves"]
    for i in reversed(range(min(int(record.get("done", 0)) + 1, len(moves)))):
        record["done"] = i
        write_publication(out_dir, record)
        source, destination = pathlib.Path(moves[i][0]), pathlib.Path(moves[i][1])
        if destination.exists() and not source.exists():
            os.replace(destination, source)
    entry = record.get("entry") or {}
    history = load_history(out_dir)
    kept = [h for h in history if h.get("build") != entry.get("build")]
    if len(kept) != len(history):
        save_history(out_dir, kept)
    publication_path(out_dir).unlink()
    progress.emit(f"the publication of build {entry.get('build')} was undone: what was in place before it is in place again",
                  level="warning")


def clear_own_work(out_dir, work, progress):
    """build()'s end: its staging goes -- unless an unfinished publication still moves things out of it (one an exception
    cut short and the undo could not put back): then it stays for the next writer to finish from, as a killed run's
    does."""
    record = unfinished_publication(out_dir)
    if record and record.get("work") and pathlib.Path(record["work"]).resolve() == pathlib.Path(work).resolve():
        progress.emit(f"{work} is kept: the publication from it is unfinished; the next content or full build finishes it",
                      level="warning")
        return
    shutil.rmtree(work, ignore_errors=True)


def acquire_writer(out_dir):
    """One writer of an output folder at a time: two would give their builds the same number and the later save would
    drop the earlier's entry. Returns what release_writer takes."""
    tmp_root = out_dir.parent / ".dist_tmp"
    tmp_root.mkdir(parents=True, exist_ok=True)
    path = tmp_root / WRITER_LOCK
    busy = BuildError(f"another build is writing {out_dir} (its lock: {path}): wait for it or cancel it")
    if held_by_a_live_process(path):
        raise busy
    try:
        return os.open(path, os.O_CREAT | os.O_EXCL | os.O_WRONLY), path
    except FileExistsError:
        raise busy


def release_writer(writer):
    if writer is None:
        return
    fd, path = writer
    os.close(fd)
    try:
        path.unlink()
        path.parent.rmdir()  # .dist_tmp, when nothing else is in it
    except OSError:
        pass


def version_key(version):
    """a.b.c[-suffix] as something that sorts: a release (no suffix) after its pre-releases, suffixes by their text."""
    m = re.match(r"^([0-9]+)\.([0-9]+)\.([0-9]+)(?:-(.+))?$", version or "")
    if not m:
        return None
    return (int(m[1]), int(m[2]), int(m[3]), 1 if m[4] is None else 0, m[4] or "")


def version_blockers(history, profile_name, flavour, version):
    """versionMustGrow (owner, 2026-11-17: "if I don't bump the version I cannot build"): the PUBLISHED builds of this
    profile and flavour whose version this one does not grow past. A version a player may hold is one build; one that was
    only built (tried, checked, never handed out) builds again."""
    key = version_key(version)
    return [h for h in history if h.get("profile") == profile_name and h.get("result") == "built"
            and h.get("flavour", "shipping") == flavour and h.get("published")
            and (key is None or version_key(h.get("version", "")) is None or version_key(h["version"]) >= key)]


def mark_published(args, progress):
    """--mark-published / --unmark-published: the history's "published" flag, nothing built."""
    on = args.mark_published is not None
    spec = args.mark_published if on else args.unmark_published
    out_dir = pathlib.Path(args.out) if args.out else ROOT / load_profile(args.profile).get("out", "dist")
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    try:
        builds = [int(b) for b in spec.split(",") if b.strip()]
    except ValueError:
        raise BuildError(f"build numbers, comma-separated: {spec!r}")
    writer = acquire_writer(out_dir)
    try:
        finish_publication(out_dir, progress, recovering=True)  # a killed build's entry first
        history = load_history(out_dir)
        for number in builds:
            entry = next((h for h in history if h.get("build") == number and h.get("result") == "built"), None)
            if entry is None:
                raise BuildError(f"no built build {number} in {history_path(out_dir)}")
            if on:
                entry["published"] = True
                entry["publishedDate"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
            else:
                entry.pop("published", None)
                entry.pop("publishedDate", None)
            progress.emit(f"build {number} ({entry.get('versionString', entry.get('version'))}, {entry.get('profile')}): "
                          f"{'published' if on else 'not published'}")
        save_history(out_dir, history)
    finally:
        release_writer(writer)
    progress.emit("done", event="done")
    return 0


def delete_builds(args, progress):
    """--delete-build: builds out of the history and their files off the disk (owner, 2026-11-17)."""
    out_dir = pathlib.Path(args.out) if args.out else ROOT / load_profile(args.profile).get("out", "dist")
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    writer = acquire_writer(out_dir)
    try:
        finish_publication(out_dir, progress, recovering=True)  # a killed build's entry first
        return delete_builds_locked(args, progress, out_dir)
    finally:
        release_writer(writer)


def delete_builds_locked(args, progress, out_dir):
    out_root = out_dir.resolve()
    symbols_dir = symbols_root(out_dir).resolve()  # beside THIS output folder, never the repository's by default
    history = load_history(out_dir)
    try:
        numbers = [int(b) for b in args.delete_build.split(",") if b.strip()]
    except ValueError:
        raise BuildError(f"build numbers, comma-separated: {args.delete_build!r}")

    def inside(path, parent):
        # Strictly inside: the root itself is not a build's file (an entry whose folder is the output folder, or whose
        # versionString starts with a space -- its symbols would be dist_symbols/ whole -- deletes nothing).
        try:
            return path.resolve().relative_to(parent) != pathlib.Path(".")
        except ValueError:
            return False

    def writable(func, path, _):
        os.chmod(path, 0o666)  # a read-only file (copied from a read-only source) is deleted all the same
        func(path)

    for number in numbers:
        progress.begin("stage", f"build {number}")
        entry = next((h for h in history if h.get("build") == number), None)
        if entry is None:
            raise BuildError(f"no build {number} in {history_path(out_dir)}")
        if entry.get("published"):
            raise BuildError(f"build {number} ({entry.get('version')}) is published -- players may hold it; unmark it first "
                             f"(--unmark-published={number}) if it never reached anybody")
        # Each build replaces the folder and the zip of its version: whatever lies there now is the newest build's.
        later = [h for h in history if h.get("build", 0) > number and h.get("result") == "built"]
        targets = []
        folder = entry.get("folder")
        if folder:
            path = pathlib.Path(folder)
            owner = next((h for h in later if h.get("folder") == folder), None)
            manifest = path / "manifest.json"
            manifest_build = None
            if manifest.exists():
                try:
                    manifest_build = json.loads(manifest.read_text(encoding="utf-8")).get("build")
                except ValueError:
                    pass
            if not path.exists():
                pass
            elif owner is not None or (manifest_build is not None and manifest_build != number):
                progress.emit(f"kept {path}: build {owner.get('build') if owner else manifest_build}'s now")
            elif not inside(path, out_root):
                progress.emit(f"kept {path}: not under {out_root}", level="warning")
            else:
                targets.append(path)
            report = out_dir / f"{path.name}.cook_report.json"
            if report.exists() and owner is None and inside(report, out_root):
                targets.append(report)
        zip_file = entry.get("zip")
        if zip_file:
            path = pathlib.Path(zip_file)
            owner = next((h for h in later if h.get("zip") == zip_file), None)
            if not path.exists():
                pass
            elif owner is not None:
                progress.emit(f"kept {path}: build {owner.get('build')}'s now")
            elif not inside(path, out_root):
                progress.emit(f"kept {path}: not under {out_root}", level="warning")
            else:
                targets.append(path)
        version_text = entry.get("versionString", "")
        if version_text:
            symbols = symbols_dir / (version_text.split(" ")[0] + ("" if entry.get("flavour", "shipping") == "shipping" else "-dev"))
            if symbols.exists() and inside(symbols, symbols_dir):
                targets.append(symbols)
        for path in targets:
            try:
                if path.is_dir():
                    if sys.version_info >= (3, 12):
                        shutil.rmtree(path, onexc=writable)  # onerror is deprecated from 3.12
                    else:
                        shutil.rmtree(path, onerror=writable)
                else:
                    path.unlink()
            except OSError as e:
                raise BuildError(f"cannot delete {path} ({e}) -- is the game running from it? The history keeps build {number}")
            progress.emit(f"deleted {path}")
        history.remove(entry)
        save_history(out_dir, history)
        progress.emit(f"build {number} ({version_text or entry.get('version')}) out of the history"
                      f"{'' if targets else ' (none of its files deleted)'}")
    progress.begin("finish", "deleted")
    progress.emit("done", event="done")
    return 0


def save_history(out_dir, history):
    out_dir.mkdir(parents=True, exist_ok=True)
    tmp = history_path(out_dir).with_suffix(".tmp")
    tmp.write_text(json.dumps(history, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    os.replace(tmp, history_path(out_dir))


# ---- the modules a running exe has loaded (ctypes: EnumProcessModulesEx + GetModuleFileNameExW) ----

def process_modules(pid):
    PROCESS_QUERY_INFORMATION, PROCESS_VM_READ = 0x0400, 0x0010
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    k32.CloseHandle.argtypes = [wt.HANDLE]
    psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(wt.HMODULE), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
    psapi.GetModuleFileNameExW.argtypes = [wt.HANDLE, wt.HMODULE, wt.LPWSTR, wt.DWORD]
    handle = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not handle:
        return set()
    try:
        modules = (wt.HMODULE * 1024)()
        needed = wt.DWORD()
        if not psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 0x03):
            return set()
        names = set()
        for i in range(min(needed.value // ctypes.sizeof(wt.HMODULE), 1024)):
            buf = ctypes.create_unicode_buffer(32768)
            if psapi.GetModuleFileNameExW(handle, modules[i], buf, 32768):
                names.add(buf.value)
        return names
    finally:
        k32.CloseHandle(handle)


def run_and_sample(exe, args, cwd, env, timeout, sample_every=0.5):
    """Runs the staged exe, sampling its loaded modules; (exit code, every module path seen)."""
    proc = subprocess.Popen([str(exe)] + args, cwd=cwd, env=env)
    seen = set()
    t0 = time.time()
    while proc.poll() is None:
        seen |= process_modules(proc.pid)
        if time.time() - t0 > timeout:
            proc.kill()
            proc.wait()
            raise BuildError(f"{exe.name} did not finish in {timeout} s")
        time.sleep(sample_every)
    return proc.returncode, seen


def ota_substitute(path):
    """The file of the game's folder an NVIDIA over-the-air copy stands in for: NGX loads the newest Streamline plugin and
    DLSS model it has (C:\\ProgramData\\NVIDIA\\NGX\\models\\sl_common_0\\versions\\...\\x.dll) instead of the one beside the
    exe, which it still needs where no update came -- so a module seen there keeps its local file."""
    m = re.search(r"\\NGX\\models\\([a-z0-9_]+?)(?:_\d+)?\\versions\\", path, re.IGNORECASE)
    if not m:
        return None
    name = m.group(1).lower()
    return f"sl.{name[3:]}.dll" if name.startswith("sl_") else f"nvngx_{name}.dll"


def vc_runtime_folder():
    roots = sorted(glob.glob(r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*\x64\Microsoft.VC14*.CRT"),
                   key=lambda p: [int(x) if x.isdigit() else 0 for x in pathlib.Path(p).parents[1].name.split(".")])
    roots = [r for r in roots if os.path.exists(os.path.join(r, "msvcp140.dll"))]
    return pathlib.Path(roots[-1]) if roots else None


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def notices_text():
    parts = ["THIRD-PARTY SOFTWARE NOTICES\n\nThis program includes the following third-party software. Each component's licence "
             "follows its name.\n"]
    for name, url, licence_file, copyright_line in THIRD_PARTY:
        parts.append("\n" + "=" * 100 + f"\n{name}\n{url}\n" + "=" * 100 + "\n\n")
        if licence_file:
            path = ROOT / licence_file
            if not path.exists():
                raise BuildError(f"the licence text of {name} is missing: {licence_file}")
            parts.append(path.read_text(encoding="utf-8", errors="replace").strip() + "\n")
        else:
            parts.append(f"MIT License\n\n{copyright_line}\n\n{MIT_TEXT}")
    return "".join(parts)


def readme_text(product, version_text, profile, report, flavour="shipping"):
    levels = ", ".join(pathlib.Path(l).stem for l in report.get("levels", []))
    user_folder = product if flavour == "shipping" else f"{product}_dev"
    development = "" if flavour == "shipping" else f"""
DEVELOPMENT BUILD
  This is the packaged game with the developer features left in -- not for players. The screen says DEVELOPMENT BUILD.
  F1 opens Developer Controls, F9 the profiler overlay, F11 a trace (there is no level editor in it); the whole command
  line works (--set=<key>:<value>, --sweep, --log-level=debug, the harnesses).
  The tools that rewrite content (--import, --reimport, --cook, ...) need the source tree and refuse here.
  Its files are kept apart from the Shipping build's (below), so the two never share settings.
"""
    return f"""{product} {version_text}

Start {EXE_NAME}. Shipped levels: {levels}.
{development}
REQUIREMENTS
  Windows 10 (version 1909 or newer) or Windows 11, 64-bit.
  A DirectX 12 graphics card with hardware ray tracing (DXR 1.1) and shader model 6.6: NVIDIA GeForce RTX 20 series or
  newer. Tested on NVIDIA only; AMD Radeon RX 6000+ and Intel Arc meet the requirements but are not checked.
  A recent graphics driver. The game says so at start when the card or driver falls short.

WHERE THINGS ARE
  The game writes nothing beside the exe. Its files are in %LOCALAPPDATA%\\{user_folder}:
    settings\\user_settings.json       your settings: what you changed from the defaults, the quality levels,
                                       the window, brightness, field of view, mouse (delete it for the defaults)
    logs\\                             the session logs (logs\\latest.txt names the newest)
    crashes\\                          crash reports (minidumps) -- send one with the log if the game stops
    cache\\                            shader and pipeline caches (safe to delete)
    bot_memory\\                       what the race bots learned
  --user-dir=<folder> puts them elsewhere (a portable copy).

CREDITS
  See CREDITS.txt (the assets and their authors) and THIRD_PARTY_NOTICES.txt (the libraries).
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", default="test")
    ap.add_argument("--mode", choices=["check", "record", "content", "full"], default="content")
    ap.add_argument("--levels")
    ap.add_argument("--boot-level")
    ap.add_argument("--version")
    ap.add_argument("--no-encrypt", action="store_true")
    ap.add_argument("--override-licences", action="store_true")
    ap.add_argument("--flavour", choices=["shipping", "development"])
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--mark-published")
    ap.add_argument("--unmark-published")
    ap.add_argument("--delete-build")
    ap.add_argument("--out")
    ap.add_argument("--progress", choices=["text", "json"], default="text")
    ap.add_argument("--keep-temp", action="store_true")
    ap.add_argument("--key")
    args = ap.parse_args()
    progress = Progress(args.progress == "json")
    try:
        if args.mark_published is not None or args.unmark_published is not None:
            return mark_published(args, progress)
        if args.delete_build is not None:
            return delete_builds(args, progress)
        return build(args, progress)
    except BuildError as e:
        progress.emit(str(e), level="error", event="failed")
        return 1
    except KeyboardInterrupt:
        progress.emit("cancelled", level="error", event="failed")
        return 2
    except Exception as e:  # an OSError, a bad JSON, a bug: still a "failed" event the window and MCP can show
        import traceback
        for line in traceback.format_exc().splitlines()[-6:]:
            progress.emit(line, level="error")
        progress.emit(f"{type(e).__name__}: {e}", level="error", event="failed")
        return 1


VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z][0-9A-Za-z.-]*)?$")


def repair_tree_headers():
    """The tree's generated headers are the dev ones (test_cube.vcxproj writes them when missing). Builds before the
    staging root stamped them in place, and a killed one left the release version -- or the embedded KEY -- for every dev
    build after it: put the dev ones back."""
    if VERSION_HEADER.exists() and "build_dist.py for a distribution build" in VERSION_HEADER.read_text(encoding="utf-8"):
        write_if_changed(VERSION_HEADER, DEV_VERSION_HEADER)
    if KEY_HEADER.exists() and "TC_PAK_HAS_EMBEDDED_KEY 1" in KEY_HEADER.read_text(encoding="utf-8", errors="replace"):
        write_if_changed(KEY_HEADER, NO_KEY_HEADER)


def build(args, progress):
    repair_tree_headers()
    profile = load_profile(args.profile)
    product = profile.get("product", "test_cube")
    version = args.version or profile.get("version", "0.0.0")
    # It becomes a folder name, a C string (BuildVersion.g.h, the .rc) and a command-line value: digits, dots, a suffix.
    if not VERSION_RE.match(version):
        raise BuildError(f"version {version!r}: digits a.b.c, optionally -suffix of letters, digits, dots and dashes")
    flavour = args.flavour or profile.get("flavour", "shipping")
    if flavour not in ("shipping", "development"):
        raise BuildError(f"flavour {flavour!r}: shipping or development")
    # A Development distribution is the team's, never the players': no encryption (docs/scalability_plan.md §10 item 11).
    encrypt = bool(profile.get("encrypt", False)) and not args.no_encrypt and flavour == "shipping"
    exe_config = "Shipping" if flavour == "shipping" else "Development"
    out_dir = pathlib.Path(args.out) if args.out else ROOT / profile.get("out", "dist")
    if not out_dir.is_absolute():
        out_dir = ROOT / out_dir
    overrides = []
    if args.levels:
        overrides.append(f"--levels={args.levels}")
    if args.boot_level:
        overrides.append(f"--boot-level={args.boot_level}")
    if args.version:
        overrides.append(f"--version={args.version}")
    if args.no_encrypt:
        overrides.append("--no-encrypt")
    if args.override_licences:
        overrides.append("--override-licences")
    if args.flavour:
        overrides.append(f"--flavour={args.flavour}")

    progress.begin("prepare", f"profile {profile['_name']} ({profile['_path']}), mode {args.mode}, {flavour}, version {version}"
                              f"{', encrypted' if encrypt else ''}{' ' + ' '.join(overrides) if overrides else ''}")
    if flavour == "development" and profile.get("encrypt", False) and not args.no_encrypt:
        progress.emit("a Development build is not encrypted (the profile's \"encrypt\" is for Shipping)")
    if args.override_licences:
        if profile.get("strictLicences", False):
            progress.emit("the licence check is overridden for this build: an asset without a known licence ships (a warning "
                          "each, listed in the cook report and the history)", level="warning")
        else:
            progress.emit("--override-licences: the profile does not require every licence -- nothing to override")
    # The staging beside the output (the same volume: the folder is renamed into place whole). A killed build left one:
    # nothing of it is a distribution.
    # A build that writes the output folder is the only one that does (a check or a record beside it is fine).
    writer = acquire_writer(out_dir) if args.mode in ("content", "full") else None
    tmp_root = out_dir.parent / ".dist_tmp"
    tmp_root.mkdir(parents=True, exist_ok=True)
    pending = unfinished_publication(out_dir)
    if pending is not None and writer is not None:
        finish_publication(out_dir, progress, recovering=True)
        pending = None
    elif pending is not None:
        entry = pending.get("entry", {})
        progress.emit(f"{publication_path(out_dir)}: the publication of {entry.get('versionString', entry.get('version'))} "
                      f"was cut short -- the next content or full build finishes it", level="warning")
    # only the folders of runs that are gone: a live run keeps its staging, an unfinished publication its pieces
    clear_stale_work(tmp_root, progress, keep=[pending["work"]] if pending and pending.get("work") else ())
    work = pathlib.Path(tempfile.mkdtemp(prefix="build_", dir=tmp_root))
    work_lock = open(work / WORK_LOCK, "w")  # held open to the end: this run is alive (clear_stale_work)
    logs = work / "logs"
    logs.mkdir()
    cook_exe = ROOT / "x64" / "Cook" / EXE_NAME
    ship_dir = ROOT / "x64" / exe_config  # the exe that goes into the folder: Shipping or Development
    try:
        if args.mode == "record":
            if not args.skip_build:
                progress.begin("build", "MSBuild Release (the record runs)")
                msbuild("Release", progress, logs)
            progress.begin("cook", "record runs over the profile's levels (minutes)")
            levels = args.levels.split(",") if args.levels else profile.get("levels", [])
            code = run([sys.executable, str(ROOT / "tools" / "dist" / "record_shaders.py"), "--levels"] + levels, progress)
            if code != 0:
                raise BuildError(f"record_shaders.py: {code} run(s) failed")
            progress.begin("finish", "the record changed tools/dist/shader_list.txt and vfs_record.txt: review the diff, commit")
            progress.emit("done", event="done")
            return 0

        git_hash, dirty, changed = git_state()
        # The shader record names the levels it ran over (tools/dist/shader_list.txt "# levels:"): a level outside them may
        # ask for permutations nobody cooked -- compiled from the packaged HLSL at first sight, a stall and "not cooked".
        recorded = recorded_levels()
        build_levels = [l.replace("\\", "/") for l in (args.levels.split(",") if args.levels else profile.get("levels", []))]
        unrecorded = [l for l in build_levels if recorded is not None and l not in recorded]
        if unrecorded:
            progress.emit(f"not in the shader record runs: {', '.join(unrecorded)} -- its permutations may be compiled at "
                          f"first sight; python tools/dist/record_shaders.py --levels {' '.join(sorted(set(build_levels) | recorded))}"
                          f" (or --mode=record), then review the diff", level="warning")
        if dirty:
            progress.emit(f"uncommitted changes ({len(changed)}: {', '.join(changed[:5])}{'...' if len(changed) > 5 else ''})"
                          f" -- the version says -dirty", level="warning")
        history = load_history(out_dir, strict=args.mode in ("content", "full"))
        if history is None:  # a check writes nothing of it: say so and go on
            progress.emit(f"{history_path(out_dir)} is damaged: a content or full build is refused until it is repaired "
                          f"or moved away", level="warning")
            history = []
        build_number = 1 + max([h.get("build", 0) for h in history] + [0])
        if profile.get("versionMustGrow") and args.mode in ("content", "full"):
            # Above every PUBLISHED build of the profile; each flavour its own (a Development build of 1.2.0 does not use
            # up 1.2.0 for Shipping). Until 2026-11-17 any build used its version up, published or not.
            blockers = version_blockers(history, profile["_name"], flavour, version)
            if blockers:
                top = max(blockers, key=lambda h: version_key(h.get("version", "")) or ())
                raise BuildError(f"profile {profile['_name']}: version {version} does not grow past {top.get('version')}, "
                                 f"published (build {top.get('build')}, {top.get('publishedDate', '?')}) -- raise it "
                                 f"(--version=), or unmark that build if it never reached anybody (--unmark-published=)")
            before = [h.get("build") for h in history if h.get("profile") == profile["_name"] and h.get("result") == "built"
                      and h.get("flavour", "shipping") == flavour and h.get("version") == version]
            if before:
                progress.emit(f"version {version} was built before (build {', '.join(str(b) for b in before)}), not published: "
                              f"built again, its folder and zip replaced")
        date = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
        version_text, header = version_header(product, version, build_number, git_hash, dirty, date)
        # The stamped headers live under the staging folder only (test_cube.vcxproj TcGeneratedRoot): ending this build
        # any way at all leaves the tree's dev headers as they were.
        generated = work / "generated"
        gen_version = generated / "app" / "generated" / "BuildVersion.g.h"
        gen_key = generated / "core" / "vfs" / "generated" / "PakKey.g.h"
        write_if_changed(gen_version, header)
        write_if_changed(gen_key, NO_KEY_HEADER)

        # ---- the exes ----
        if args.mode in ("full",) and not args.skip_build:
            progress.begin("build", f"MSBuild Cook (version {version_text})")
            msbuild("Cook", progress, logs, generated)
        elif args.mode in ("check", "content") and not args.skip_build:
            # The cook as the tree has it NOW (owner, 2026-11-17): a check or a content build took x64/Cook as it lay, and
            # one from before the last change to the cook ran the old code -- a new switch unknown (--override-licences: its
            # 4 licence errors stayed), a rule the tree had changed still applied. Incremental, with the tree's own (dev)
            # headers: 0.5 s when nothing changed; the first one after a full build recompiles the stamped version files and
            # links (~20 s). The game's exe stays as built (content's "as built" is the point of that mode).
            progress.begin("build", "MSBuild Cook (incremental: the cook as the tree has it)")
            msbuild("Cook", progress, logs)
        if not cook_exe.exists():
            raise BuildError(f"no {cook_exe}: build the Cook configuration (or drop --skip-build)")

        if args.mode == "check":
            progress.begin("cook", "the dependency walk (--cook-check)")
            cook_out = work / "check"
            code = run([str(cook_exe), f"--cook={profile['_path']}", f"--cook-out={cook_out}", "--cook-check"] + overrides,
                       progress, log_path=logs / "cook.log")
            if not (cook_out / "cook_report.json").exists():
                raise BuildError(f"the check wrote no report (exit {code}); the log: {logs / 'cook.log'}")
            report = json.loads((cook_out / "cook_report.json").read_text(encoding="utf-8"))
            summarise_report(report, progress)
            keep = out_dir / f"check_{profile['_name']}_report.json"
            out_dir.mkdir(parents=True, exist_ok=True)
            shutil.copy2(cook_out / "cook_report.json", keep)
            progress.begin("finish", "check done" if code == 0 else f"check: {len(report['errors'])} error(s)")
            progress.emit("done", event="done", report=str(keep), errors=len(report["errors"]),
                          warnings=len(report["warnings"]))
            return 0 if code == 0 else 3

        # ---- the key (§4) ----
        key_file = pathlib.Path(args.key).resolve() if args.key else DEFAULT_KEY
        if encrypt:
            progress.begin("key", f"the package key {key_file}")
            if not key_file.exists():
                key_file.parent.mkdir(parents=True, exist_ok=True)
                if run([str(cook_exe), f"--pak-keygen={key_file}"], progress, log_path=logs / "keygen.log") != 0:
                    raise BuildError(f"cannot make the key {key_file}")
                progress.emit(f"a NEW key was made: {key_file} -- keep a copy somewhere safe: without it no package built with "
                              f"it can be read or patched", level="warning")
            guid = json.loads(key_file.read_text(encoding="utf-8")).get("guid", "?")
            progress.emit(f"key {guid}")
            if args.mode == "full" and not args.skip_build:
                if run([str(cook_exe), f"--pak-key-header={key_file},{gen_key}"], progress, log_path=logs / "keyheader.log") != 0:
                    raise BuildError("cannot write the embedded key header")

        if args.mode == "full" and not args.skip_build:
            progress.begin("build", f"MSBuild {exe_config} (version {version_text})")
            msbuild(exe_config, progress, logs, generated)
        if not (ship_dir / EXE_NAME).exists():
            raise BuildError(f"no {ship_dir / EXE_NAME}: build the {exe_config} configuration (or --mode=full)")

        # ---- the content ----
        # -dev: a Development build of a version must not replace the Shipping one of the same version.
        folder_name = f"{product}-{version}" + ("" if flavour == "shipping" else "-dev")
        stage = work / folder_name
        content = stage / "content"
        progress.begin("cook", f"cook {profile['_name']} -> {content}")
        cook_cmd = [str(cook_exe), f"--cook={profile['_path']}", f"--cook-out={content}", f"--version={version}"] + \
                   [o for o in overrides if not o.startswith("--version")]
        if encrypt:
            cook_cmd.append(f"--pak-key={key_file}")
        code = run(cook_cmd, progress, log_path=logs / "cook.log")
        report_path = content / "cook_report.json"
        report = json.loads(report_path.read_text(encoding="utf-8")) if report_path.exists() else {"errors": ["no report"]}
        summarise_report(report, progress)
        if code != 0:
            raise BuildError(f"the cook failed with {len(report.get('errors', []))} error(s); the report: {report_path}")
        report_kept = work / "cook_report.json"
        shutil.move(str(report_path), report_kept)
        overridden = report.get("licencesOverridden", [])
        if overridden:
            progress.emit(f"shipped without a known licence ({len(overridden)}, --override-licences): {', '.join(overridden)}",
                          level="warning")

        # ---- the folder ----
        progress.begin("stage", f"the folder {folder_name}")
        shutil.copy2(ship_dir / EXE_NAME, stage / EXE_NAME)
        if flavour == "development" and (ship_dir / "test_cube.pdb").exists():
            # Its crash reports and traces read with names at once (Shipping keeps the PDB in dist_symbols/).
            shutil.copy2(ship_dir / "test_cube.pdb", stage / "test_cube.pdb")
        for dll in sorted(ship_dir.glob("*.dll")):
            shutil.copy2(dll, stage / dll.name)
        (stage / "D3D12").mkdir()
        shutil.copy2(ROOT / "third_party" / "agility" / "x64" / "D3D12Core.dll", stage / "D3D12" / "D3D12Core.dll")

        # Exactly the DLLs the game loads from its folder (§7): a run of the staged exe, its modules sampled; then the
        # VC++ runtime app-local (from Visual Studio's Redist) and a second run that must load it from the folder.
        progress.begin("modules", "a run of the staged exe: which DLLs it loads from its folder")
        user = work / "user"
        env = dict(os.environ)
        env["TEST_CUBE_MESSAGE_TIMEOUT_MS"] = "5000"
        check_args = [f"--user-dir={user}", "--shot=check.png", "--shot-delay=8", "--wind-freeze=5", "--time=15:00"]
        code, seen = run_and_sample(stage / EXE_NAME, check_args, cwd=work, env=env, timeout=600)
        if code != 0:
            raise BuildError(f"the staged exe exited with {code} (its log: {user / 'logs'})")
        local = {pathlib.Path(p).name.lower() for p in seen if pathlib.Path(p).parent.resolve() == stage.resolve()}
        # DXC is loaded for reflection; dxil.dll only when a permutation the package lacks is compiled -- the safety net
        # (§5 item 4) needs both even when this run compiled nothing.
        substitutes = {s for s in (ota_substitute(p) for p in seen) if s}
        keep = local | substitutes | {"dxcompiler.dll", "dxil.dll"}
        removed = [d.name for d in stage.glob("*.dll") if d.name.lower() not in keep]
        if substitutes:
            progress.emit(f"NVIDIA's over-the-air copies stood in for: {', '.join(sorted(substitutes))} (kept: needed without them)")
        for name in removed:
            (stage / name).unlink()
        progress.emit(f"loaded from the folder: {', '.join(sorted(local))}; not loaded, left out: {', '.join(removed) or 'none'}")
        crt_needed = sorted({pathlib.Path(p).name for p in seen
                             if pathlib.Path(p).name.lower().startswith(("msvcp140", "vcruntime140", "concrt140"))})
        crt = vc_runtime_folder()
        if crt_needed and not crt:
            raise BuildError("the VC++ runtime is loaded but Visual Studio's Redist folder was not found")
        for name in crt_needed:
            src = crt / name
            if not src.exists():
                raise BuildError(f"{name} not in {crt}")
            shutil.copy2(src, stage / name)
        progress.emit(f"VC++ runtime app-local from {crt}: {', '.join(crt_needed)}")

        progress.begin("check", "the staged build, again, with its own runtime: exit 0, no Error, nothing compiled")
        shutil.rmtree(user, ignore_errors=True)
        code, seen = run_and_sample(stage / EXE_NAME, check_args, cwd=work, env=env, timeout=600)
        log = latest_log(user)
        errors = [l for l in log.splitlines() if "][ERROR][" in l or "][FATAL][" in l]
        not_cooked = [l for l in log.splitlines() if "shader permutation not cooked" in l]
        # DLSS must come up: a Streamline plugin left out shows here (Renderer.cpp's line), not as an error.
        errors += [l for l in log.splitlines() if "streamline: slInit failed" in l]
        foreign = sorted({p for p in seen if pathlib.Path(p).name.lower().startswith(("msvcp140", "vcruntime140"))
                          and pathlib.Path(p).parent.resolve() != stage.resolve()})
        if code != 0 or errors or not_cooked or not (user / "check.png").exists():
            for l in (errors + not_cooked)[:10]:
                progress.emit(l[:300], level="error")
            raise BuildError(f"the staged build's check failed: exit {code}, {len(errors)} Error, {len(not_cooked)} not cooked")
        if foreign:
            progress.emit(f"the VC++ runtime came from outside the folder: {foreign}", level="warning")
        progress.emit("the staged build runs: exit 0, 0 Error, 0 not cooked")

        progress.begin("notices", "CREDITS.txt, THIRD_PARTY_NOTICES.txt, README.txt, manifest.json")
        (stage / "CREDITS.txt").write_text((ROOT / "CREDITS.md").read_text(encoding="utf-8"), encoding="utf-8")
        (stage / "THIRD_PARTY_NOTICES.txt").write_text(notices_text(), encoding="utf-8")
        (stage / "README.txt").write_text(readme_text(product, version_text, profile, report, flavour), encoding="utf-8")
        files = {}
        for path in sorted(stage.rglob("*")):
            if path.is_file():
                rel = path.relative_to(stage).as_posix()
                files[rel] = {"size": path.stat().st_size, "sha256": sha256(path)}
        manifest = {"product": product, "version": version, "build": build_number, "versionString": version_text,
                    "git": git_hash + ("-dirty" if dirty else ""), "date": date, "profile": profile["_name"],
                    "flavour": flavour, "encrypted": encrypt, "files": files}
        (stage / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        total = sum(f["size"] for f in files.values())
        progress.emit(f"{len(files)} files, {total / 1048576:.1f} MB")

        # ---- the zip and the symbols in the staging, then into place: nothing of a failure lands in dist/ ----
        out_dir.mkdir(parents=True, exist_ok=True)
        final = out_dir / folder_name
        zip_path = None
        tmp_zip = None
        if profile.get("zip", False):
            progress.begin("zip", f"{folder_name}.zip")
            tmp_zip = work / f"{folder_name}.zip"
            with zipfile.ZipFile(tmp_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as z:
                for path in sorted(stage.rglob("*")):
                    if path.is_file():
                        # The packages are compressed already (zstd): stored, not deflated again.
                        kind = zipfile.ZIP_STORED if path.suffix == ".tcpak" else zipfile.ZIP_DEFLATED
                        z.write(path, f"{folder_name}/{path.relative_to(stage).as_posix()}", compress_type=kind)
            progress.emit(f"{tmp_zip.name} ({tmp_zip.stat().st_size / 1048576:.1f} MB)")
        # The symbols too are made in the staging (they were written into dist_symbols/ first, before anything else of the
        # build was in place) and published with the folder, its zip and its history entry as one (publish).
        symbols = None
        if profile.get("symbols", False):
            progress.begin("symbols", "the PDB and the cook report, in the staging for dist_symbols/")
            symbols = symbols_root(out_dir) / (version_text.split(" ")[0] + ("" if flavour == "shipping" else "-dev"))
            work_symbols = work / "symbols"
            work_symbols.mkdir()
            for name in (EXE_NAME, "test_cube.pdb"):
                if (ship_dir / name).exists():
                    shutil.copy2(ship_dir / name, work_symbols / name)
            shutil.copy2(report_kept, work_symbols / "cook_report.json")
            progress.emit(str(symbols))
        kept_report = symbols / "cook_report.json" if symbols else out_dir / f"{folder_name}.cook_report.json"
        # Whatever is in a destination's place is moved aside first (publish's rule), the files too: os.replace would
        # overwrite the last build's zip or report, and an undone publication could not put it back.
        moves = []
        if final.exists():
            moves.append((final, work / "replaced"))
        moves.append((stage, final))
        if symbols:
            if symbols.exists():
                moves.append((symbols, work / "replaced_symbols"))
            moves.append((work / "symbols", symbols))
        else:
            if kept_report.exists():
                moves.append((kept_report, work / "replaced_report"))
            moves.append((report_kept, kept_report))
        if tmp_zip:
            zip_path = out_dir / f"{folder_name}.zip"
            if zip_path.exists():
                moves.append((zip_path, work / "replaced_zip"))
            moves.append((tmp_zip, zip_path))

        progress.begin("finish", f"built {final}")
        packages = {p["name"]: p.get("packageBytes") for p in report.get("packages", [])}
        entry = {"product": product, "version": version, "build": build_number, "versionString": version_text,
                 "profile": profile["_name"], "flavour": flavour, "git": git_hash, "dirty": dirty, "date": date,
                 "result": "built",
                 "folder": str(final), "bytes": total, "zip": str(zip_path) if zip_path else None,
                 "packages": packages, "levels": report.get("levels", []), "encrypted": encrypt,
                 **({"licencesOverridden": overridden} if overridden else {})}
        publish(out_dir, moves, entry, work, progress)
        progress.emit("done", event="done", folder=str(final), zip=str(zip_path) if zip_path else None, bytes=total,
                      report=str(kept_report), flavour=flavour, licencesOverridden=len(overridden),
                      version=version_text, warnings=len(report.get("warnings", [])))
        return 0
    finally:
        work_lock.close()
        if not args.keep_temp:
            clear_own_work(out_dir, work, progress)
        release_writer(writer)
        if not args.keep_temp:
            try:
                tmp_root.rmdir()
            except OSError:
                pass


def latest_log(user):
    hint = user / "logs" / "latest.txt"
    if not hint.exists():
        return ""
    name = hint.read_text(encoding="utf-8").strip()
    path = pathlib.Path(name) if os.path.isabs(name) else user / name
    return path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""


def summarise_report(report, progress):
    totals = report.get("totals", {})
    progress.emit(f"{totals.get('files', 0)} files, {totals.get('rawBytes', 0) / 1048576:.1f} MB before compression"
                  + (f", {totals.get('storedBytes', 0) / 1048576:.1f} MB stored" if totals.get("storedBytes") else
                     f", about {totals.get('estimatedBytes', 0) / 1048576:.0f} MB after"), event="totals", totals=totals)
    for e in report.get("errors", []):
        progress.emit(e, level="error")
    for w in report.get("warnings", []):
        progress.emit(w, level="warning")


if __name__ == "__main__":
    sys.exit(main())
