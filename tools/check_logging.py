#!/usr/bin/env python3
"""Logging plan L9: report direct diagnostic output that bypasses core/logging.

Scans sources/ for
  * OutputDebugString[A|W](            -> events go through LOG_* / WriteRaw
  * printf( / fprintf(stderr / std::cout / std::cerr  (event-style console output)
  * fopen[_s]( ... diag::LogPath(      -> artifacts go through diag::ArtifactFile
  * diag::LogPath(                     -> only the allowlisted path-only users
outside a small allowlist (the logging core itself, and the harnesses that own their FILE*).
Comment text is ignored. Exit code = number of findings, so it can gate a commit hook or CI.

    python tools/check_logging.py            # report
    python tools/check_logging.py --list     # also print the allowlist
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1] / "sources"

# Path prefixes (posix, relative to sources/) allowed to contain each pattern.
ALLOW = {
    "OutputDebugString": ("core/logging/",),
    "console printf": ("core/logging/diagnostics/",),
    "fopen(diag::LogPath)": (),
    "diag::LogPath": (
        "core/diagnostics/DiagPaths.h",
        "core/diagnostics/ArtifactWriter.cpp",
        "app/main.cpp",                 # three harness outputs that own their FILE*
        "assets/AssetImporter.h",       # the importer's own log stream
        "editor/ui/ImportPanel.cpp",
    ),
}

PATTERNS = [
    ("OutputDebugString", re.compile(r"\bOutputDebugString[AW]?\s*\(")),
    ("console printf", re.compile(r"(?<![\w:.])(?:std::)?(?:printf|puts)\s*\(|\bfprintf\s*\(\s*std(?:err|out)\b|std::c(?:out|err)\b")),
    ("fopen(diag::LogPath)", re.compile(r"\bfopen(?:_s)?\s*\([^;]*diag::LogPath\s*\(")),
    ("diag::LogPath", re.compile(r"\bdiag::LogPath\s*\(")),
]


def strip_comments(line: str) -> str:
    # Good enough for this codebase: drop a trailing // comment and inline /* */ blocks.
    line = re.sub(r"/\*.*?\*/", "", line)
    at = line.find("//")
    return line if at < 0 else line[:at]


def main() -> int:
    findings = []
    for path in sorted(ROOT.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        in_block = False
        for number, raw in enumerate(text.splitlines(), 1):
            line = raw
            if in_block:
                end = line.find("*/")
                if end < 0:
                    continue
                line = line[end + 2:]
                in_block = False
            if "/*" in line and "*/" not in line[line.find("/*"):]:
                line = line[: line.find("/*")]
                in_block = True
            code = strip_comments(line)
            for name, pattern in PATTERNS:
                if not pattern.search(code):
                    continue
                if any(rel.startswith(prefix) for prefix in ALLOW[name]):
                    continue
                findings.append((rel, number, name, raw.strip()))

    if "--list" in sys.argv:
        for name, prefixes in ALLOW.items():
            print(f"allow {name}: {', '.join(prefixes) or '(nothing)'}")
    for rel, number, name, snippet in findings:
        print(f"{rel}:{number}: {name}: {snippet[:110]}")
    print(f"{len(findings)} finding(s)")
    return len(findings)


if __name__ == "__main__":
    sys.exit(main())
