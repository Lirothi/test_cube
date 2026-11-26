#!/usr/bin/env python3
"""Architecture plan R0.2: the layer ratchet. An #include "..." goes down the layers of
docs/architecture_review_plan.md section 4, never up.

Every project file under sources/ belongs to a ROW of the table below (longest folder prefix wins); every row sits
on a LAYER. An include from a file of row A into a file of row B is
  * fine when A == B, or B's layer is lower than A's;
  * fine when both rows share a layer that says peers=True (render-core: materials, text, ImGui and the renderer's
    core are one knot today, R4 cuts it);
  * a violation otherwise: B's layer is higher ("up"), or B is another row of a layer without peers ("sideways":
    physics, ocean and vfx sit beside the world and call only down).
Today's violations are the allowance, tools/arch_layers_allow.txt (one `source -> target` per line, paths relative to
sources/). A violation that is not in it is a finding; an allowance line whose include is gone is a finding too (the
ratchet only tightens: --update removes it). Exit code = findings.

    python tools/check_layers.py              # check
    python tools/check_layers.py --update     # drop allowance lines whose include is gone (never adds one)
    python tools/check_layers.py --summary    # violations by row pair, allowed or not
    python tools/check_layers.py --selftest   # an injected core -> app include is caught, an allowed one is not

Include resolution follows MSVC's quoted form for this project: the including file's folder, then sources/, then the
repository root ($(ProjectDir)); an include that resolves to none of them is third-party and ignored.
"""

import collections
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
SRC = REPO / "sources"
ALLOW_FILE = REPO / "tools" / "arch_layers_allow.txt"
EXTENSIONS = (".cpp", ".h", ".hpp", ".inl")

# Section 4, bottom up. (layer rank, layer name, peers, {row: [folder prefixes relative to sources/]}).
# rhi (1): the device, queues, frames, submission, descriptors, upload, barriers -- sources/rhi/ (R4.5b).
LAYERS = [
    (0.0, "core", False, {
        "core": ["core/"],
    }),
    (0.5, "platform", False, {
        "input": ["input/"],
        "assets": ["assets/"],
    }),
    (1.0, "rhi", False, {
        "rhi": ["rhi/"],
    }),
    (2.0, "render-core", True, {
        "rendering-core": ["rendering/core/", "rendering/descriptors/", "rendering/meshes/",
                           "rendering/streaming/", "rendering/debug/", "rendering/view/", "rendering/wind/",
                           "rendering/RenderLayers.h"],
        "materials": ["materials/"],
        "text": ["text/"],
        "ui": ["ui/"],
    }),
    (3.0, "render-feat", False, {
        "rendering": ["rendering/lighting/", "rendering/shadows/", "rendering/post/", "rendering/rt/",
                      "rendering/visibility/", "rendering/renderables/", "rendering/diagnostics/",
                      "rendering/scene/"],
    }),
    (3.5, "world-side", False, {
        "physics": ["physics/"],
        "ocean": ["ocean/"],
        "vfx": ["vfx/"],
    }),
    (4.0, "world", False, {
        "world": ["app/scene/", "app/camera/", "app/levels/"],
    }),
    (5.0, "game", False, {
        "game": ["app/game/"],
    }),
    (6.0, "app", False, {
        "app": ["app/"],
    }),
    (7.0, "editor", False, {
        "editor": ["editor/"],
    }),
]

ROOT_ROW = "(root)"  # pch.h / pch.cpp: platform headers only, outside the layers

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')


def load_sources(src=SRC):
    """{posix path relative to sources/: list of lines} for every project source file."""
    files = {}
    for path in sorted(src.rglob("*")):
        if path.suffix in EXTENSIONS and path.is_file():
            rel = path.relative_to(src).as_posix()
            files[rel] = path.read_text(encoding="utf-8", errors="replace").split("\n")
    return files


def resolve(files, source, include):
    """The sources/-relative file an include names, or None when it is not a project source."""
    inc = include.replace("\\", "/")
    here = pathlib.PurePosixPath(source).parent
    for candidate in (here / inc, pathlib.PurePosixPath(inc)):
        norm = _normalize(str(candidate))
        if norm in files:
            return norm
    # $(ProjectDir): "sources/x.h" from the repository root.
    if inc.startswith("sources/"):
        norm = _normalize(inc[len("sources/"):])
        if norm in files:
            return norm
    return None


def _normalize(path):
    parts = []
    for part in path.split("/"):
        if part in ("", "."):
            continue
        if part == "..":
            if parts:
                parts.pop()
            continue
        parts.append(part)
    return "/".join(parts)


def include_edges(files):
    """[(source, target, line number)] for every quoted include that resolves to a project file."""
    edges = []
    for rel, lines in files.items():
        for number, line in enumerate(lines, 1):
            match = INCLUDE_RE.match(line)
            if not match:
                continue
            target = resolve(files, rel, match.group(1))
            if target and target != rel:
                edges.append((rel, target, number))
    return edges


def _row_table():
    table = []  # (prefix, row, rank, layer, peers)
    for rank, layer, peers, rows in LAYERS:
        for row, prefixes in rows.items():
            for prefix in prefixes:
                table.append((prefix, row, rank, layer, peers))
    table.sort(key=lambda item: -len(item[0]))
    return table


ROW_TABLE = _row_table()


def row_of(rel):
    """(row, rank, layer, peers) of a sources/-relative path."""
    for prefix, row, rank, layer, peers in ROW_TABLE:
        if rel == prefix or rel.startswith(prefix):
            return row, rank, layer, peers
    if "/" not in rel:
        return ROOT_ROW, -1.0, ROOT_ROW, True
    raise KeyError(f"{rel}: no row in the layer table of tools/check_layers.py")


def violation_kind(source, target):
    """'up', 'sideways' or None for an include from source to target."""
    s_row, s_rank, s_layer, s_peers = row_of(source)
    t_row, t_rank, t_layer, _ = row_of(target)
    if s_row == t_row or ROOT_ROW in (s_row, t_row):
        return None
    if t_rank > s_rank:
        return "up"
    if t_rank == s_rank and not s_peers:
        return "sideways"
    return None


def violations(files, edges=None):
    """{"source -> target": (kind, line number)} over the tree."""
    found = {}
    for source, target, number in edges if edges is not None else include_edges(files):
        kind = violation_kind(source, target)
        if kind:
            found.setdefault(f"{source} -> {target}", (kind, number))
    return found


def read_allowance(path=ALLOW_FILE):
    allowed = []
    if path.exists():
        for line in path.read_text(encoding="utf-8").split("\n"):
            line = line.strip()
            if line and not line.startswith("#"):
                allowed.append(line)
    return allowed


ALLOW_HEADER = """\
# Architecture plan R0.2: today's include violations of the layer table in tools/check_layers.py.
# One `source -> target` per line (paths relative to sources/). The list only shrinks:
# `python tools/check_layers.py --update` removes lines whose include is gone and never adds one.
# A step that removes an edge says so in its "Сделано" (docs/architecture_review_plan.md section 11).
"""


def write_allowance(lines, path=ALLOW_FILE):
    path.write_text(ALLOW_HEADER + "".join(line + "\n" for line in sorted(lines)), encoding="utf-8", newline="\n")


def check(files, allowed, out=print):
    """(new violations, stale allowance lines)."""
    found = violations(files)
    allowed_set = set(allowed)
    new = sorted(key for key in found if key not in allowed_set)
    stale = sorted(line for line in allowed_set if line not in found)
    for key in new:
        source, target = key.split(" -> ")
        kind, number = found[key]
        s_row, _, s_layer, _ = row_of(source)
        t_row, _, t_layer, _ = row_of(target)
        out(f"sources/{source}:{number}: includes {target} -- {kind}: {s_row} ({s_layer}) -> {t_row} ({t_layer})")
    for key in stale:
        out(f"allowance: '{key}' no longer included -- tighten the ratchet: python tools/check_layers.py --update")
    return new, stale


def summary(files, allowed):
    found = violations(files)
    pairs = collections.Counter()
    sources = collections.defaultdict(set)
    for key, (kind, _) in found.items():
        source, target = key.split(" -> ")
        pair = (row_of(source)[0], row_of(target)[0], kind)
        pairs[pair] += 1
        sources[pair].add(source)
    print(f"{len(found)} include violations ({len(set(allowed))} allowed); by rows:")
    for (s_row, t_row, kind), n in sorted(pairs.items(), key=lambda item: (-item[1], item[0])):
        print(f"  {n:4d} in {len(sources[(s_row, t_row, kind)]):3d} files  {s_row} -> {t_row} ({kind})")


def selftest():
    files = {
        "core/A.h": ['#include "core/B.h"'],
        "core/B.h": [],
        "core/C.cpp": ['#include "app/App.h"', '#include "rendering/core/Renderer.h"'],
        "app/App.h": ['#include "core/A.h"', '#include "editor/E.h"'],
        "editor/E.h": ['#include "app/App.h"'],
        "rendering/core/Renderer.h": ['#include "materials/M.h"', '#include "rendering/lighting/L.h"'],
        "materials/M.h": ['#include "rendering/core/Renderer.h"'],
        "rendering/lighting/L.h": ['#include "../core/Renderer.h"'],
        "physics/P.cpp": ['#include "ocean/O.h"'],
        "ocean/O.h": [],
        "pch.h": [],
    }
    cases = [
        ("core -> app is up", "core/C.cpp -> app/App.h", "up"),
        ("core -> rendering is up", "core/C.cpp -> rendering/core/Renderer.h", "up"),
        ("app -> editor is up", "app/App.h -> editor/E.h", "up"),
        ("editor -> app is down", "editor/E.h -> app/App.h", None),
        ("render-core peers", "rendering/core/Renderer.h -> materials/M.h", None),
        ("render-core peers both ways", "materials/M.h -> rendering/core/Renderer.h", None),
        ("render-core -> render-feat is up", "rendering/core/Renderer.h -> rendering/lighting/L.h", "up"),
        ("a relative include resolves", "rendering/lighting/L.h -> rendering/core/Renderer.h", None),
        ("world-side siblings are sideways", "physics/P.cpp -> ocean/O.h", "sideways"),
        ("inside a row", "core/A.h -> core/B.h", None),
    ]
    found = violations(files)
    failures = 0
    for name, key, expected in cases:
        got = found.get(key, (None,))[0]
        if key not in [f"{s} -> {t}" for s, t, _ in include_edges(files)]:
            got = "unresolved"
        status = "ok" if got == expected else "FAIL"
        failures += status != "ok"
        print(f"  {status:4s} {name}: {key} -> {got} (expected {expected})")
    # The ratchet: an allowed violation passes, a new one and a stale allowance line are findings.
    allowed = ["core/C.cpp -> rendering/core/Renderer.h", "app/App.h -> editor/E.h", "core/A.h -> app/Gone.h"]
    new, stale = check(files, allowed, out=lambda line: None)
    for name, ok in (("a violation outside the allowance is new",
                      new == ["core/C.cpp -> app/App.h", "physics/P.cpp -> ocean/O.h",
                              "rendering/core/Renderer.h -> rendering/lighting/L.h"]),
                     ("an allowance line whose include is gone is stale", stale == ["core/A.h -> app/Gone.h"])):
        print(f"  {'ok' if ok else 'FAIL':4s} {name}")
        failures += not ok
    try:
        row_of("nowhere/X.h")
        print("  FAIL a folder outside the table is an error")
        failures += 1
    except KeyError:
        print("  ok   a folder outside the table is an error")
    print(f"check_layers selftest: {'PASS' if failures == 0 else 'FAIL'} ({failures} failing)")
    return failures


def main(argv):
    if "--selftest" in argv:
        return selftest()
    files = load_sources()
    allowed = read_allowance()
    if "--summary" in argv:
        summary(files, allowed)
        return 0
    if "--update" in argv:
        found = violations(files)
        kept = [line for line in allowed if line in found]
        removed = sorted(set(allowed) - set(kept))
        for line in removed:
            print(f"removed: {line}")
        write_allowance(kept)
        print(f"check_layers: allowance {len(set(allowed))} -> {len(set(kept))} lines")
        allowed = kept
    new, stale = check(files, allowed)
    findings = len(new) + len(stale)
    print(f"check_layers: {findings} finding(s); {len(set(allowed))} allowed include(s) up or sideways")
    return findings


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
