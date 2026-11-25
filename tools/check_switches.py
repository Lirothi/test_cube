#!/usr/bin/env python3
"""Architecture plan R3.2: the switch table (sources/app/BootSwitches.cpp) and the code that reads the switches agree.

  * every "--switch" literal in the code of sources/ is a row of the table (a switch nobody can find in --help is
    a finding), except the few that are other programs' switches this one writes for them (NOT_OURS);
  * every row of the table is read somewhere in the code (a row for a switch nothing reads lies in --help);
  * no row is there twice.
Comment text is ignored. Exit code = findings.

    python tools/check_switches.py              # check
    python tools/check_switches.py --list       # the table's rows, the ones a Shipping build keeps marked
    python tools/check_switches.py --selftest   # an unlisted switch, a dead row and a duplicate are caught
"""

import collections
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1] / "sources"
TABLE = "app/BootSwitches.cpp"

# Switches of other programs, written by this one onto their command lines.
NOT_OURS = {
    "--reap-idle", "--reap-model", "--reap-model-pid",  # model_reaper.exe (editor/intent/LlmReaper.cpp)
    "--profile",                                       # tools/build_dist.py (editor/dist/DistBuildJob.cpp)
}
# Switches read without their dashes: strstr(cmd, "scene-stress") catches --scene-stress and --scene-stress-gbv.
BARE_READS = {"scene-stress": ("--scene-stress",), "scene-stress-gbv": ("--scene-stress-gbv",)}

LITERAL = re.compile(r'L?"(--[a-z0-9][a-z0-9-]*)')
BARE = re.compile(r'"(' + "|".join(re.escape(k) for k in BARE_READS) + r')"')
ROW = re.compile(r'\{\s*"(--[a-z0-9][a-z0-9-]*)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*(true|false)\s*\}')


def strip_comments(text):
    """The text with // and /* */ comments blanked (string literals kept; a "//" inside one is rare here)."""
    out = []
    i = 0
    in_block = False
    in_string = False
    while i < len(text):
        c = text[i]
        if in_block:
            if text.startswith("*/", i):
                in_block = False
                i += 2
                continue
            out.append("\n" if c == "\n" else " ")
            i += 1
            continue
        if in_string:
            out.append(c)
            if c == "\\" and i + 1 < len(text):
                out.append(text[i + 1])
                i += 2
                continue
            if c == '"' or c == "\n":
                in_string = False
            i += 1
            continue
        if c == '"':
            in_string = True
            out.append(c)
            i += 1
            continue
        if text.startswith("//", i):
            while i < len(text) and text[i] != "\n":
                i += 1
            continue
        if text.startswith("/*", i):
            in_block = True
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def table_rows(text):
    return [(m.group(1), m.group(2), m.group(3), m.group(4) == "true") for m in ROW.finditer(strip_comments(text))]


def reads(files):
    """{switch: [file:line, ...]} of the switches the code reads."""
    found = collections.defaultdict(list)
    for rel, text in files.items():
        if rel == TABLE:
            continue
        code = strip_comments(text)
        for number, line in enumerate(code.split("\n"), 1):
            for m in LITERAL.finditer(line):
                found[m.group(1)].append(f"{rel}:{number}")
            for m in BARE.finditer(line):
                for name in BARE_READS[m.group(1)]:
                    found[name].append(f"{rel}:{number}")
    return found


def check(files, out=print):
    rows = table_rows(files.get(TABLE, ""))
    names = collections.Counter(name for name, _, _, _ in rows)
    used = reads(files)
    findings = 0
    for name, n in sorted(names.items()):
        if n > 1:
            out(f"{TABLE}: {name} is in the table {n} times")
            findings += 1
    for name in sorted(used):
        if name in NOT_OURS or name in names:
            continue
        out(f"sources/{used[name][0]}: {name} is read but not in the switch table ({TABLE})")
        findings += 1
    for name in sorted(names):
        if name == "--help":
            # read in main.cpp like any other; listed here only to say it is not special
            pass
        if name not in used:
            out(f"{TABLE}: {name} is in the table but nothing reads it")
            findings += 1
    return findings, rows


def load(root=ROOT):
    files = {}
    for path in sorted(root.rglob("*")):
        if path.suffix in (".cpp", ".h") and path.is_file():
            files[path.relative_to(root).as_posix()] = path.read_text(encoding="utf-8", errors="replace")
    return files


def selftest():
    table = ('constexpr Switch kSwitches[] = {\n'
             '    { "--shot", "=<path>", "a shot", true },\n'
             '    { "--dead", "", "nothing reads it", false },\n'
             '    { "--twice", "", "one", false },\n'
             '    { "--twice", "", "two", false },\n'
             '    { "--scene-stress", "", "bare read", false },\n'
             '    // { "--commented", "", "not a row", false },\n'
             '};\n')
    files = {
        TABLE: table,
        "app/main.cpp": ('if (strstr(cmd, "--shot=")) {}\n'
                         'if (strstr(cmd, "--unlisted")) {}\n'
                         '// "--in-a-comment" is not a read\n'
                         'if (strstr(cmd, "scene-stress")) {}\n'
                         'if (strstr(cmd, "--twice")) {}\n'
                         'spawn(L"--reap-idle=5");\n'),
    }
    lines = []
    findings, rows = check(files, out=lines.append)
    expected = [
        f"{TABLE}: --twice is in the table 2 times",
        f"sources/app/main.cpp:2: --unlisted is read but not in the switch table ({TABLE})",
        f"{TABLE}: --dead is in the table but nothing reads it",
    ]
    failures = 0
    for e in expected:
        ok = e in lines
        failures += not ok
        print(f"  {'ok' if ok else 'FAIL':4s} {e}")
    extra = [l for l in lines if l not in expected]
    for l in extra:
        print(f"  FAIL unexpected: {l}")
    failures += len(extra)
    ok = len(rows) == 5
    failures += not ok
    print(f"  {'ok' if ok else 'FAIL':4s} a commented-out row is not a row ({len(rows)} rows)")
    print(f"check_switches selftest: {'PASS' if failures == 0 else 'FAIL'} ({failures} failing)")
    return failures


def main(argv):
    if "--selftest" in argv:
        return selftest()
    files = load()
    findings, rows = check(files)
    if "--list" in argv:
        for name, value, help_text, shipping in sorted(rows):
            print(f"  {name + value:60s} {'[shipping] ' if shipping else ''}{help_text}")
    shipping = sorted(name for name, _, _, s in rows if s)
    print(f"check_switches: {findings} finding(s); {len(rows)} switches in the table, {len(shipping)} kept by Shipping")
    return findings


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
