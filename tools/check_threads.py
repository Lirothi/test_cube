#!/usr/bin/env python3
"""docs/architecture_review_plan.md R15.8: report a detached std::thread that does not say what catches its exceptions.

An exception that leaves a detached thread's function is std::terminate -- the whole process, from a thread nobody
waits for. So every `.detach()` names the place that catches: a `detached-ok: <where it catches>` comment on the same
line, or on a comment-only line right above it (RunRequestWorker's try, say). Matched in any spelling the compiler
takes: `t.detach()`, `p->detach()`, `}).detach()`, `.detach ( )`, a `.detach()` continued onto a line of its own.
A Win32 thread (`CreateThread`, `_beginthreadex`, `_beginthread`) is the same: what leaves its function ends the
process. Its start says `thread-ok: <where it catches>` the same way (the second review of wave 1: R15.8 left them out).
Comment and literal text is not code (a comment or a string that mentions detach is not a call), and the marker counts
only in a comment (a string that says it marks nothing). The lexer knows a digit separator (`1'000`) from a character
literal and follows a raw string (`R"x(...)x"`) across lines. Scans sources/ and tools/ (C++). Exit code = number of
findings, so it can gate a commit hook or CI.

    python tools/check_threads.py              # report
    python tools/check_threads.py --list       # also print the marked sites
    python tools/check_threads.py --selftest   # the lexer's cases (exit code = cases that fail)
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
WIN32_START = re.compile(r"(?<![\w.>])(?:::)?(?:CreateThread|_beginthreadex|_beginthread)\s*\(")
WIN32_MARKER = re.compile(r"thread-ok:\s*\S")
RAW_PREFIXES = {"R", "LR", "uR", "UR", "u8R"}
RAW_DELIMITER = re.compile(r'[^\s()\\"]{0,16}')


def run_before(line, i, chars):
    """The characters of `chars` (a predicate) right before position i."""
    j = i
    while j > 0 and chars(line[j - 1]):
        j -= 1
    return line[j:i]


def is_ident(c):
    return c.isalnum() or c == "_"


def is_number_part(c):
    return c.isalnum() or c in "_."


class State:
    """What a line ends inside of: a /* */ comment, or a raw string (its closing `)delim"`)."""

    def __init__(self):
        self.in_block = False
        self.raw_end = None


def lex(line, state):
    """The line's code (literals emptied, comments out) and its comment text; `state` carries over to the next line."""
    code, comments = [], []
    i, n = 0, len(line)
    while i < n:
        if state.raw_end is not None:
            end = line.find(state.raw_end, i)
            if end < 0:
                return "".join(code), " ".join(comments)
            i = end + len(state.raw_end)
            state.raw_end = None
            continue
        if state.in_block:
            end = line.find("*/", i)
            if end < 0:
                comments.append(line[i:])
                return "".join(code), " ".join(comments)
            comments.append(line[i:end])
            i = end + 2
            state.in_block = False
            continue
        c = line[i]
        if line.startswith("//", i):
            comments.append(line[i + 2:])
            break
        if line.startswith("/*", i):
            state.in_block = True
            i += 2
            continue
        if c == "'":
            number = run_before(line, i, is_number_part)
            if number and number[0].isdigit():
                # A digit separator (1'000, 0xFF'FF): part of the number, not a character literal.
                i += 1
                continue
        if c == '"' and run_before(line, i, is_ident) in RAW_PREFIXES:
            delimiter = RAW_DELIMITER.match(line, i + 1).group(0)
            if line.startswith("(", i + 1 + len(delimiter)):
                terminator = ")" + delimiter + '"'
                code.append('""')
                end = line.find(terminator, i + 2 + len(delimiter))
                if end < 0:
                    state.raw_end = terminator
                    return "".join(code), " ".join(comments)
                i = end + len(terminator)
                continue
        if c in "\"'":
            quote = c
            i += 1
            while i < n and line[i] != quote:
                i += 2 if line[i] == "\\" else 1
            i += 1
            code.append(quote + quote)
            continue
        code.append(c)
        i += 1
    return "".join(code), " ".join(comments)


def scan_lines(lines):
    findings, marked = [], []
    state = State()
    previous_code = ""
    above_comment = ""  # the comment of the line above when that line holds no code
    for number, line in enumerate(lines, 1):
        code, comment = lex(line, state)
        split = DETACH_ALONE.search(code) and ENDS_IN_ACCESS.search(previous_code)
        if code.strip():
            previous_code = code
        if DETACH.search(code) or split:
            ok = MARKER.search(comment) or MARKER.search(above_comment)
            (marked if ok else findings).append((number, line.strip()))
        elif WIN32_START.search(code):
            ok = WIN32_MARKER.search(comment) or WIN32_MARKER.search(above_comment)
            (marked if ok else findings).append((number, line.strip()))
        above_comment = comment if not code.strip() else ""
    return findings, marked


def scan(path):
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return [], []
    return scan_lines(lines)


# (what the case shows, the source, the findings it must give). The first group failed before the second review of
# wave 1 (item 6): a digit separator swallowed the rest of its line, a raw string's text was taken for code, and a
# marker inside a string literal counted.
SELFTEST_CASES = [
    ("a digit separator before a call", "int n = 1'000; t.detach();", 1),
    ("a hex digit separator before a call", "auto m = 0xFF'FF; t.detach();", 1),
    ("a raw string's text is not a call", 'auto s = R"(\n    t.detach();\n)";', 0),
    ("a raw string's quote and delimiter", 'auto s = R"x( ")" t.detach(); )x"; t.detach();', 1),
    ("a call after a raw string over lines", 'auto s = LR"(\n" \n)"; t.detach();', 1),
    ("a marker inside a string does not count", 't.detach(); std::puts("detached-ok: nothing");', 1),
    ("a marker inside a raw string does not count", 't.detach(); auto s = R"(// detached-ok: nothing)";', 1),
    # The second group held before and must still hold.
    ("a character literal is still a literal", "char c = '.'; t.detach(); // detached-ok: the body's try", 0),
    ("a prefixed character literal", "auto c = u8'a'; t.detach();", 1),
    ("a quote in a character literal", "char q = '\"'; t.detach();", 1),
    ("a marker on the line", "t.detach(); // detached-ok: RunRequestWorker's try", 0),
    ("a marker on a comment line above", "// detached-ok: RunRequestWorker's try\nt.detach();", 0),
    ("a marker above a code line does not reach past it", "// detached-ok: x\nint a = 0;\nt.detach();", 1),
    ("a marker in a block comment", "t.detach(); /* detached-ok: the body's try */", 0),
    ("a call split over two lines", "worker.\n    detach();", 1),
    ("an arrow call", "p->detach ( );", 1),
    ("a call in a comment is not a call", "// t.detach();", 0),
    ("a call in a block comment is not a call", "/* t.detach();\n   still a comment */", 0),
    ("a call in a string is not a call", 'std::puts("t.detach()");', 0),
    # Win32 threads (R15.8's gap, the same review): their start names where their exceptions go.
    ("a CreateThread without a marker", "HANDLE h = CreateThread(nullptr, 0, &Main, nullptr, 0, nullptr);", 1),
    ("a ::CreateThread with its marker above", "// thread-ok: Main catches all\nHANDLE h = ::CreateThread(nullptr, 0, &Main, p, 0, nullptr);", 0),
    ("a _beginthreadex with its marker on the line", "auto h = _beginthreadex(nullptr, 0, &Main, p, 0, &id); // thread-ok: Main's try", 0),
    ("a detached-ok marker does not mark a Win32 thread", "auto h = _beginthread(&Main, 0, p); // detached-ok: no", 1),
    ("a function whose name ends in CreateThread is not one", "MyCreateThread(p);", 0),
    ("a CreateThread in a comment is not a call", "// CreateThread(nullptr, 0, &Main, p, 0, nullptr);", 0),
]


def selftest():
    failed = 0
    for name, source, expected in SELFTEST_CASES:
        findings, _ = scan_lines(source.split("\n"))
        verdict = "PASS" if len(findings) == expected else "FAIL"
        if verdict == "FAIL":
            failed += 1
        print(f"check_threads self-test {verdict}: {name} -- {len(findings)} finding(s), expected {expected}")
    print(f"check_threads self-test: {len(SELFTEST_CASES) - failed} of {len(SELFTEST_CASES)} cases pass")
    return failed


def main():
    if "--selftest" in sys.argv:
        return selftest()
    show = "--list" in sys.argv
    total = 0
    for root in ROOTS:
        for path in sorted(root.rglob("*")):
            if path.suffix.lower() not in SUFFIXES or SKIP_PARTS & set(path.parts):
                continue
            findings, marked = scan(path)
            rel = path.relative_to(REPO).as_posix()
            for number, text in findings:
                marker = "thread-ok" if WIN32_START.search(text) and not DETACH.search(text) else "detached-ok"
                print(f"{rel}:{number}: thread without `// {marker}: <where it catches>`: {text}")
            if show:
                for number, text in marked:
                    print(f"  ok {rel}:{number}: {text}")
            total += len(findings)
    print(f"check_threads: {total} finding(s)")
    return total


if __name__ == "__main__":
    sys.exit(main())
