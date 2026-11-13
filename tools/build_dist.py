#!/usr/bin/env python3
r"""The distribution build -- docs/distribution_build_plan.md §8.2, §8.3. One command from the tree to
dist/<Product>-<version>/ (and its zip), the same for the console, the editor's Build Distribution window and MCP.

    python tools/build_dist.py --profile=test                    # content: cook + the folder, Shipping exe as built
    python tools/build_dist.py --profile=release --mode=full     # MSBuild Cook + Shipping, cook, folder, zip, symbols
    python tools/build_dist.py --profile=test --mode=check       # the dependency walk and its report, nothing built
    python tools/build_dist.py --profile=test --mode=record      # Release record runs -> shader_list.txt / vfs_record.txt

    --levels=a.json,b.json  --boot-level=a.json  --version=1.2.3  --no-encrypt
                            override the profile for this build (named in the report)
    --flavour=shipping|development
                            which exe goes into the folder (docs/scalability_plan.md §7): Shipping (the game, the developer
                            features stripped) or Development (the same packaged game with Developer Controls, the F keys,
                            the whole command line; x64/Development, the folder <product>-<version>-dev, its PDB beside the
                            exe, never encrypted, the profile's "devInclude" cooked too). Default: the profile's "flavour",
                            else shipping.
    --skip-build            content/full: take the exes as they are
    --key=<key.json>        the package key (default %USERPROFILE%\.test_cube\pak_key.json; made there if missing)
    --out=<folder>          instead of the profile's "out" (dist)
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


def load_history(out_dir):
    path = history_path(out_dir)
    if path.exists():
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except ValueError:
            return []
    return []


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
    settings\\graphics_settings.json   what you changed from the defaults
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
    ap.add_argument("--flavour", choices=["shipping", "development"])
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--progress", choices=["text", "json"], default="text")
    ap.add_argument("--keep-temp", action="store_true")
    ap.add_argument("--key")
    args = ap.parse_args()
    progress = Progress(args.progress == "json")
    try:
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
    if args.flavour:
        overrides.append(f"--flavour={args.flavour}")

    progress.begin("prepare", f"profile {profile['_name']} ({profile['_path']}), mode {args.mode}, {flavour}, version {version}"
                              f"{', encrypted' if encrypt else ''}{' ' + ' '.join(overrides) if overrides else ''}")
    if flavour == "development" and profile.get("encrypt", False) and not args.no_encrypt:
        progress.emit("a Development build is not encrypted (the profile's \"encrypt\" is for Shipping)")
    # The staging beside the output (the same volume: the folder is renamed into place whole). A killed build left one:
    # nothing of it is a distribution.
    tmp_root = out_dir.parent / ".dist_tmp"
    if tmp_root.exists():
        shutil.rmtree(tmp_root, ignore_errors=True)
    tmp_root.mkdir(parents=True)
    work = pathlib.Path(tempfile.mkdtemp(prefix="build_", dir=tmp_root))
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
        if dirty:
            progress.emit(f"uncommitted changes ({len(changed)}: {', '.join(changed[:5])}{'...' if len(changed) > 5 else ''})"
                          f" -- the version says -dirty", level="warning")
        history = load_history(out_dir)
        build_number = 1 + max([h.get("build", 0) for h in history] + [0])
        if profile.get("versionMustGrow") and args.mode in ("content", "full"):
            # Each flavour its own: a Development build of 1.2.0 does not use up 1.2.0 for Shipping.
            shipped = [h for h in history if h.get("profile") == profile["_name"] and h.get("result") == "built"
                       and h.get("flavour", "shipping") == flavour]
            if any(h.get("version") == version for h in shipped):
                raise BuildError(f"profile {profile['_name']}: version {version} was built before -- raise it (--version=)")
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
        elif not cook_exe.exists():
            raise BuildError(f"no {cook_exe}: build the Cook configuration (or --mode=full)")

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
        symbols = None
        if profile.get("symbols", False):
            progress.begin("symbols", "the PDB and the cook report into dist_symbols/")
            symbols = ROOT / "dist_symbols" / (version_text.split(" ")[0] + ("" if flavour == "shipping" else "-dev"))
            symbols.mkdir(parents=True, exist_ok=True)
            for name in (EXE_NAME, "test_cube.pdb"):
                if (ship_dir / name).exists():
                    shutil.copy2(ship_dir / name, symbols / name)
            shutil.copy2(report_kept, symbols / "cook_report.json")
            progress.emit(str(symbols))
        kept_report = symbols / "cook_report.json" if symbols else out_dir / f"{folder_name}.cook_report.json"
        if not symbols:
            shutil.copy2(report_kept, kept_report)
        if final.exists():
            os.replace(final, work / "replaced")
        os.replace(stage, final)
        if tmp_zip:
            zip_path = out_dir / f"{folder_name}.zip"
            os.replace(tmp_zip, zip_path)

        progress.begin("finish", f"built {final}")
        packages = {p["name"]: p.get("packageBytes") for p in report.get("packages", [])}
        history.append({"product": product, "version": version, "build": build_number, "versionString": version_text,
                        "profile": profile["_name"], "flavour": flavour, "git": git_hash, "dirty": dirty, "date": date,
                        "result": "built",
                        "folder": str(final), "bytes": total, "zip": str(zip_path) if zip_path else None,
                        "packages": packages, "levels": report.get("levels", []), "encrypted": encrypt})
        save_history(out_dir, history)
        progress.emit("done", event="done", folder=str(final), zip=str(zip_path) if zip_path else None, bytes=total,
                      report=str(kept_report), flavour=flavour,
                      version=version_text, warnings=len(report.get("warnings", [])))
        return 0
    finally:
        if not args.keep_temp:
            shutil.rmtree(work, ignore_errors=True)
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
