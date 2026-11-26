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
ROW = re.compile(r'\{\s*"(--[a-z0-9][a-z0-9-]*)",\s*"((?:[^"\\]|\\.)*)",\s*"((?:[^"\\]|\\.)*)",\s*(true|false)\s*'
                 r'(?:,\s*&parse::(\w+)\s*)?\}')
PARSE = "app/BootParse.cpp"


def parse_check(files, out=print):
    """R3.2b: a row that names its parse function (&parse::Fn) -- the function is in BootParse.cpp and reads that switch."""
    findings = 0
    code = strip_comments(files.get(PARSE, ""))
    for m in ROW.finditer(strip_comments(files.get(TABLE, ""))):
        name, fn = m.group(1), m.group(5)
        if not fn:
            continue
        body = re.search(r"^void " + fn + r"\(const char\* cmd\)\n\{\n(.*?)^\}", code, re.S | re.M)
        if not body:
            out(f"{TABLE}: {name} names parse::{fn}, which is not in sources/{PARSE}")
            findings += 1
        elif f'"{name}' not in body.group(1):
            out(f"{TABLE}: {name} names parse::{fn}, which does not read {name}")
            findings += 1
    return findings


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


OPTIONS = "app/BootOptions.h"
DUMP = "app/BootOptionsDump.cpp"
STRUCT = re.compile(r"^    struct (\w+)\s*$")
FIELD = re.compile(r"^        [\w:<>, ]+?[ *&]+(\w+)(?:\[\d+\])?(?:\s*=[^;]*)?;")
CLOSE = re.compile(r"^    \} (\w+);")


def dump_check(files, out=print):
    """R3.2b: every field of BootOptions.h is written by --boot-dump (BootOptionsDump.cpp), but bootDumpPath."""
    if OPTIONS not in files or DUMP not in files:
        return 0
    findings, fields = 0, []
    for line in strip_comments(files[OPTIONS]).split("\n"):
        m = FIELD.match(line)
        if m:
            fields.append(m.group(1))
        c = CLOSE.match(line)
        if c:
            for name in fields:
                if name != "bootDumpPath" and f'"{c.group(1)}.{name}"' not in files[DUMP]:
                    out(f"sources/{DUMP}: {c.group(1)}.{name} (sources/{OPTIONS}) is not written by --boot-dump")
                    findings += 1
            fields = []
    return findings


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
    opts = ("struct BootOptions\n{\n    struct Frame\n    {\n        std::string levelPath;\n"
            "        float camPos[3] = { 0.0f, 0.0f, 0.0f };\n        // bool commented = false;\n    } frame;\n};\n")
    dump_lines = []
    dump_findings = dump_check({OPTIONS: opts, DUMP: 'line("frame.levelPath", f.levelPath);'}, out=dump_lines.append)
    expected_dump = [f"sources/{DUMP}: frame.camPos (sources/{OPTIONS}) is not written by --boot-dump"]
    ok = dump_lines == expected_dump and dump_findings == 1
    failures += not ok
    print(f"  {'ok' if ok else 'FAIL':4s} a BootOptions field --boot-dump does not write ({dump_lines})")
    ptable = ('    { "--a", "", "x", false, &parse::A },\n'
              '    { "--b", "", "y", false, &parse::Missing },\n'
              '    { "--c", "", "z", false, &parse::C },\n')
    pcode = ('void A(const char* cmd)\n{\n    if (std::strstr(cmd, "--a")) {}\n}\n\n'
             'void C(const char* cmd)\n{\n    if (std::strstr(cmd, "--a")) {}\n}\n')
    plines = []
    pfindings = parse_check({TABLE: ptable, PARSE: pcode}, out=plines.append)
    expected_parse = [f"{TABLE}: --b names parse::Missing, which is not in sources/{PARSE}",
                      f"{TABLE}: --c names parse::C, which does not read --c"]
    ok = plines == expected_parse and pfindings == 2
    failures += not ok
    print(f"  {'ok' if ok else 'FAIL':4s} a row's parse function missing, or reading another switch ({plines})")
    print(f"check_switches selftest: {'PASS' if failures == 0 else 'FAIL'} ({failures} failing)")
    return failures


def main(argv):
    if "--selftest" in argv:
        return selftest()
    files = load()
    findings, rows = check(files)
    findings += dump_check(files)
    findings += parse_check(files)
    if "--list" in argv:
        for name, value, help_text, shipping in sorted(rows):
            print(f"  {name + value:60s} {'[shipping] ' if shipping else ''}{help_text}")
    shipping = sorted(name for name, _, _, s in rows if s)
    print(f"check_switches: {findings} finding(s); {len(rows)} switches in the table, {len(shipping)} kept by Shipping")
    return findings


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
