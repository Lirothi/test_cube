#!/usr/bin/env python3
"""docs/architecture_review_plan.md R15.8: report a detached std::thread that does not say what catches its exceptions.

An exception that leaves a detached thread's function is std::terminate -- the whole process, from a thread nobody
waits for. So every `.detach()` names the place that catches: a `detached-ok: <where it catches>` comment on the same
line, or on a comment line right above it (RunRequestWorker's try, say). Matched in any spelling the compiler takes:
`t.detach()`, `p->detach()`, `}).detach()`, `.detach ( )`, a `.detach()` continued onto a line of its own. Comment
text is ignored when matching (a comment that mentions detach is not a call), but the marker is read from comments.
Scans sources/ and tools/ (C++). Exit code = number of findings, so it can gate a commit hook or CI.

    python tools/check_threads.py          # report
    python tools/check_threads.py --list   # also print the marked sites
"""

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
ROOTS = (REPO / "sources", REPO / "tools")
SUFFIXES = {".cpp", ".h", ".hpp", ".inl", ".cc", ".cxx"}
SKIP_PARTS = {"third_party"}

DETACH = re.compile(r"(?:\.|->)\s*detach\s*\(\s*\)")
# The member access ending one line and `detach()` opening the next.
DETACH_ALONE = re.compile(r"^\s*detach\s*\(\s*\)")
ENDS_IN_ACCESS = re.compile(r"(?:\.|->)\s*$")
MARKER = re.compile(r"detached-ok:\s*\S")


def strip_code(line, in_block):
    """The line without comments and string literals (their text is not code), and whether a /* */ is still open."""
    out = []
    i = 0
    n = len(line)
    while i < n:
        if in_block:
            end = line.find("*/", i)
            if end < 0:
                return "".join(out), True
            i = end + 2
            in_block = False
            continue
        c = line[i]
        if line.startswith("//", i):
            break
        if line.startswith("/*", i):
            in_block = True
            i += 2
            continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n and line[i] != quote:
                i += 2 if line[i] == "\\" else 1
            i += 1
            out.append(quote + quote)
            continue
        out.append(c)
        i += 1
    return "".join(out), in_block


def scan(path):
    findings, marked = [], []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return findings, marked
    in_block = False
    previous_code = ""
    for number, line in enumerate(lines, 1):
        code, in_block = strip_code(line, in_block)
        split = DETACH_ALONE.search(code) and ENDS_IN_ACCESS.search(previous_code)
        if code.strip():
            previous_code = code
        if not (DETACH.search(code) or split):
            continue
        above = lines[number - 2].strip() if number >= 2 else ""
        ok = MARKER.search(line) or (above.startswith("//") and MARKER.search(above))
        (marked if ok else findings).append((number, line.strip()))
    return findings, marked


def main():
    show = "--list" in sys.argv
    total = 0
    for root in ROOTS:
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() not in SUFFIXES or SKIP_PARTS & set(path.parts):
                continue
            findings, marked = scan(path)
            rel = path.relative_to(REPO).as_posix()
            for number, text in findings:
                print(f"{rel}:{number}: detached thread without `// detached-ok: <where it catches>`: {text}")
            if show:
                for number, text in marked:
                    print(f"  ok {rel}:{number}: {text}")
            total += len(findings)
    print(f"check_threads: {total} finding(s)")
    return total


if __name__ == "__main__":
    sys.exit(main())
