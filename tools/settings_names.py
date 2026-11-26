#!/usr/bin/env python3
"""Architecture plan R2.0 (a): every setting name the program takes by --set / --sweep / MCP set_settings, and which of
them it can read back -- the set a move into the settings registry (R2.1+) must keep whole.

Since R2.2 every name lives in the settings registry (sources/app/settings/*SettingRows.cpp):
  * a row, `TC_SETTING("a.b", TYPE, FLAGS, APPLY, READ)`: readable unless its READ is nullptr;
  * a family, `TC_SETTING_FAMILY("race.", "game::RaceKeys", TYPE, FLAGS, HAS, APPLY, READ, KEYS)`: the names are the
    prefix and every key of the table its label names (FAMILY_KEYS below; a family with a label this script does not
    know is an error, so a new one teaches it here).
App.cpp's old chains (ApplySweepValue / ReadSweepValue) are still read: a `setting == "a.b"` branch or a prefix block put
back there is a name outside the registry, and a name in both is a finding (the branch is dead).
tools/settings_names.txt is the set: one name a line, ` r` after a name that can be read. Exit code = differences.

    python tools/settings_names.py            # compare the sources with tools/settings_names.txt
    python tools/settings_names.py --write    # write tools/settings_names.txt from the sources (the baseline)
    python tools/settings_names.py --list     # print the set
"""

import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
SRC = REPO / "sources"
OUT = REPO / "tools" / "settings_names.txt"


def read(rel):
    return (SRC / rel).read_text(encoding="utf-8", errors="replace")


def strip_comments(text):
    out = []
    for line in text.split("\n"):
        at = line.find("//")
        # a "//" inside a string literal is rare in these chains; good enough
        out.append(line if at < 0 else line[:at])
    return out


def function_body(lines, header_re):
    """The lines of the first function whose header matches, its braces counted outside string literals."""
    rx = re.compile(header_re)
    for i, line in enumerate(lines):
        if not rx.search(line):
            continue
        depth = 0
        opened = False
        for k in range(i, len(lines)):
            text = re.sub(r'"(?:\\.|[^"\\])*"', '""', lines[k])
            for ch in text:
                if ch == "{":
                    depth += 1
                    opened = True
                elif ch == "}":
                    depth -= 1
            if opened and depth == 0:
                return lines[i:k + 1]
        break
    raise SystemExit(f"settings_names: no function matching {header_re}")


def table_keys(rel, function):
    """The "key" of every `{ "key", ...` row of a table function."""
    body = function_body(strip_comments(read(rel)), r"\b" + function + r"\s*\(\s*\)")
    keys = []
    for line in body:
        m = re.match(r'\s*\{\s*"([A-Za-z0-9_]+)"\s*,', line)
        if m:
            keys.append(m.group(1))
    if not keys:
        raise SystemExit(f"settings_names: no rows in {rel} {function}()")
    return keys


def write_chain():
    body = function_body(strip_comments(read("app/App.cpp")), r"bool ApplySweepValue\(")
    names = set()
    prefix_stack = []  # (prefix, depth at which its block opened)
    depth = 0
    pending_prefix = None
    for line in body:
        for m in re.finditer(r'setting\s*==\s*"([^"]+)"', line):
            names.add(m.group(1))
        m = re.search(r'setting\.rfind\("([^"]+)",\s*0\)\s*==\s*0', line)
        if m:
            pending_prefix = m.group(1)
        if prefix_stack:
            for m in re.finditer(r'\b(?:key|group)\s*==\s*"([^"]+)"', line):
                names.add(prefix_stack[-1][0] + m.group(1))
        text = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
        for ch in text:
            if ch == "{":
                depth += 1
                if pending_prefix is not None:
                    prefix_stack.append((pending_prefix, depth))
                    pending_prefix = None
            elif ch == "}":
                if prefix_stack and prefix_stack[-1][1] == depth:
                    prefix_stack.pop()
                depth -= 1
        if pending_prefix is not None and text.rstrip().endswith(")") is False and "{" not in text:
            # `if (setting.rfind("vessel.", 0) == 0 && ...) { return true; }` style one-liners open no block
            pass
    # the tables a prefix block still in the chain walks
    for prefix, label in (("race.", "game::RaceKeys"), ("bot.", "game::BotKeys"), ("player.", "game::PlayerKeys"),
                          ("vessel.", "physics::VesselKeys"), ("ocean.wake", "ocean::WakeKnobs"), ("sg.", "scalability groups"),
                          ("ocean.", "ocean colours")):
        if any(re.search(r'setting\.rfind\("' + re.escape(prefix) + r'",\s*0\)', line) for line in body):
            names |= {("ocean." if label == "ocean::WakeKnobs" else prefix) + k for k in FAMILY_KEYS[label]()}
    if any("exposure.curveEv" in line for line in body):
        names |= {p + k for p in ("exposure.curveEv", "exposure.curveComp") for k in FAMILY_KEYS["exposure curve points"]()}
    return names


def read_chain():
    body = function_body(strip_comments(read("app/App.cpp")), r"bool ReadSweepValue\(")
    names = set()
    for line in body:
        for m in re.finditer(r'\bname\s*==\s*"([^"]+)"', line):
            names.add(m.group(1))
    return names


REGISTERED_TWICE = []


def scalability_groups():
    scal = json.loads((REPO / "data" / "config" / "scalability.json").read_text(encoding="utf-8"))
    groups = scal.get("groups", {})
    return [g for g in (groups.keys() if isinstance(groups, dict) else [g.get("key") for g in groups]) if g]


def curve_points():
    m = re.search(r"kExposureCurveMaxPoints\s*=\s*(\d+)", read("rendering/core/PhotographicSettings.h"))
    return [str(i) for i in range(int(m.group(1)) if m else 0)]


# A family's label -> its keys, read from where the program reads them.
FAMILY_KEYS = {
    "game::RaceKeys": lambda: table_keys("app/game/Race.cpp", "RaceKeys"),
    "game::BotKeys": lambda: table_keys("app/game/RaceBot.cpp", "BotKeys"),
    "game::PlayerKeys": lambda: table_keys("app/game/PlayerStart.cpp", "PlayerKeys"),
    "physics::VesselKeys": lambda: table_keys("physics/CollisionJson.h", "VesselKeys"),
    "ocean::WakeKnobs": lambda: table_keys("ocean/OceanWakeKnobs.h", "WakeKnobs"),
    "scalability groups": scalability_groups,
    "ocean colours": lambda: [s + c for s in ("deepScatter", "diffuse", "sss", "absorption0", "absorption1", "absorption2",
                                              "absorption3") for c in "RGB"],
    "exposure curve points": curve_points,
}


def registry():
    names, readable = set(), set()
    root = SRC / "core" / "settings"
    if not root.exists():
        return names, readable
    for path in sorted(SRC.rglob("*")):
        if path.suffix not in (".cpp", ".h") or not path.is_file():
            continue
        # Comments out: the registry's own header shows a row in its comments.
        text = "\n".join(strip_comments(path.read_text(encoding="utf-8", errors="replace")))
        for m in re.finditer(r'\bTC_SETTING\(', text):
            args = macro_args(text, m.end())
            if not args or not re.fullmatch(r'"[^"]+"', args[0]):
                continue
            name = args[0][1:-1]
            if name in names:
                REGISTERED_TWICE.append(f"{name} ({path.relative_to(SRC).as_posix()})")
            names.add(name)
            # TC_SETTING(NAME, TYPE, FLAGS, APPLY, READ): a row reads back unless its READ is nullptr.
            if len(args) == 5 and args[4] not in ("nullptr", "NULL", "0"):
                readable.add(name)
        for m in re.finditer(r'\bTC_SETTING_FAMILY\(', text):
            args = macro_args(text, m.end())
            if not args or len(args) != 8 or not re.fullmatch(r'"[^"]+"', args[0]) or not re.fullmatch(r'"[^"]+"', args[1]):
                continue
            prefix, label = args[0][1:-1], args[1][1:-1]
            if label not in FAMILY_KEYS:
                raise SystemExit(f"settings_names: family {prefix} ({label}) in {path.relative_to(SRC).as_posix()} -- "
                                 "teach FAMILY_KEYS where its keys live")
            for key in FAMILY_KEYS[label]():
                name = prefix + key
                if name in names:
                    REGISTERED_TWICE.append(f"{name} ({path.relative_to(SRC).as_posix()}, family {label})")
                names.add(name)
                # TC_SETTING_FAMILY(PREFIX, TABLE, TYPE, FLAGS, HAS, APPLY, READ, KEYS)
                if args[6] not in ("nullptr", "NULL", "0"):
                    readable.add(name)
    return names, readable


def macro_args(text, at):
    """The top-level arguments of a call whose '(' ends just before `at`, split at commas outside (), [], {} and strings."""
    args, depth, cur, i, quote = [], 0, [], at, None
    while i < len(text):
        c = text[i]
        if quote:
            cur.append(c)
            if c == "\\":
                cur.append(text[i + 1])
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            cur.append(c)
        elif c in "([{":
            depth += 1
            cur.append(c)
        elif c in ")]}":
            if depth == 0:
                args.append("".join(cur).strip())
                return args
            depth -= 1
            cur.append(c)
        elif c == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    return None


def current():
    writes = write_chain()
    reads = read_chain()
    reg, reg_reads = registry()
    # A name lives in the registry or in the chains, never both: the registry is asked first, the branch would be dead.
    BOTH.extend(sorted(reg & (writes | reads)))
    names = writes | reads | reg
    return {n: (n in reads or n in reg_reads) for n in names}


BOTH = []


def render(table):
    return "".join(f"{n}{' r' if r else ''}\n" for n, r in sorted(table.items()))


HEADER = ("# Architecture plan R2.0: every setting name (--set / --sweep / MCP set_settings), ' r' = it can be read back.\n"
          "# Written by `python tools/settings_names.py --write`; a move into the registry must keep the set.\n")


def main(argv):
    table = current()
    if "--list" in argv:
        sys.stdout.write(render(table))
    if "--write" in argv:
        OUT.write_text(HEADER + render(table), encoding="utf-8", newline="\n")
        print(f"settings_names: {len(table)} names ({sum(table.values())} readable) -> {OUT.relative_to(REPO)}")
        return 0
    saved = {}
    for line in OUT.read_text(encoding="utf-8").split("\n"):
        if line and not line.startswith("#"):
            parts = line.split()
            saved[parts[0]] = len(parts) > 1 and parts[1] == "r"
    lost = sorted(set(saved) - set(table))
    new = sorted(set(table) - set(saved))
    unread = sorted(n for n in saved if n in table and saved[n] and not table[n])
    for n in lost:
        print(f"settings_names: {n} is gone (it was taken; neither the old chain nor the registry knows it now)")
    for n in new:
        print(f"settings_names: {n} is new -- add it with --write once it is meant")
    for n in unread:
        print(f"settings_names: {n} can no longer be read back")
    for n in REGISTERED_TWICE:
        print(f"settings_names: {n} is a second TC_SETTING row of the name (the registry refuses it at boot)")
    for n in BOTH:
        print(f"settings_names: {n} is both a registry row and a branch of App.cpp's chains (the branch is dead)")
    findings = len(lost) + len(new) + len(unread) + len(REGISTERED_TWICE) + len(BOTH)
    print(f"settings_names: {findings} difference(s); {len(table)} names, {sum(table.values())} readable")
    return findings


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
