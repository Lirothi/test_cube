#!/usr/bin/env python3
"""Distribution plan D1: report runtime code that reads files around the VFS (sources/core/vfs/Vfs.h).

Every read of game content goes through vfs:: -- so a package can serve it later (D2) and a shipped build reads
nothing it was not given. This scans sources/ for the direct ways to touch a file:

  * std::ifstream / fopen / _wfopen / CreateFile[A|W|2]            -> vfs::InStream, vfs::ReadAll, vfs::Open
  * DXC LoadFile, D3DCompileFromFile, cgltf_parse_file             -> vfs::ReadAll + the API's from-memory form
  * WIC CreateDecoderFromFilename / InitializeFromFilename         -> IWICStream::InitializeFromMemory
  * ImGui AddFontFromFileTTF                                       -> AddFontFromMemoryTTF
  * GetFileAttributes*, FindFirstFile*, directory_iterator,
    std::filesystem exists / is_regular_file / is_directory / file_size / last_write_time
                                                                   -> vfs::Exists / IsFile / IsDirectory / GetStat / List

outside the code that is not the game: the VFS itself, the editor and the importer (they write the loose files
they read), logging and diagnostics, the headless harnesses. A line that must touch the disk directly -- a
writable file of the user's (settings, saves, the shader and PSO caches), a tool rewriting an asset -- says why
with a `vfs-ok: <reason>` comment on the same line. Comment text is ignored when matching. Exit code = number of
findings, so it can gate a commit hook or CI.

    python tools/check_file_io.py            # report
    python tools/check_file_io.py --list     # also print the allowlist
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1] / "sources"

# Path prefixes (posix, relative to sources/) that are not the game's runtime reads.
ALLOW = (
    "core/vfs/",                          # the one door itself
    "editor/",                            # the editor writes the loose files it reads
    "assets/",                            # the importer: source glTF / images in, baked files out
    "core/logging/",                      # session logs, rotation, the log stress harness
    "core/diagnostics/",                  # artifacts in logs/
    "core/profiling/",                    # traces/
    "core/memory/",                       # traces/, module paths
    "app/diagnostics/",                   # headless harnesses (scene stress, benchmarks)
    "rendering/diagnostics/",             # submission stress and friends
    "rendering/visibility/HzbCullSelfTest.cpp",
    "rendering/rt/RtSmoke.cpp",
    "physics/PhysicsSelfTest.cpp",
    "text/FontGenerator.cpp",             # writes an atlas (unreachable: FontManager returns before it)
    "rendering/core/Screenshot.cpp",      # writes --shot PNGs
)

PATTERNS = [
    ("ifstream", re.compile(r"\b(?:std::)?(?:w?ifstream|w?fstream|basic_ifstream|basic_fstream)\b")),
    ("fopen", re.compile(r"\b_?w?f(?:s)?open(?:_s)?\s*\(")),
    ("_open", re.compile(r"\b_w?s?open(?:_s)?\s*\(")),
    ("CreateFile", re.compile(r"\bCreateFile(?:[AW]|2|2FromAppW|FromAppW)?\s*\(")),
    ("CreateFileMapping", re.compile(r"\bCreateFileMapping[AW]?\s*\(")),
    ("D3DReadFileToBlob", re.compile(r"\bD3DReadFileToBlob\s*\(")),
    ("DirectXTex from a file", re.compile(r"\bLoadFrom(?:DDS|WIC|TGA|HDR|EXR)File\s*\(")),
    ("DXC blob from a file", re.compile(r"\bCreateBlobFromFile\s*\(")),
    ("DXC LoadFile", re.compile(r"(?:->|\.)LoadFile\s*\(")),
    ("D3DCompileFromFile", re.compile(r"\bD3DCompileFromFile\s*\(")),
    ("cgltf_parse_file", re.compile(r"\bcgltf_parse_file\s*\(")),
    ("WIC from a file", re.compile(r"\b(?:CreateDecoderFromFilename|InitializeFromFilename)\b")),
    ("ImGui font from a file", re.compile(r"\bAddFontFromFileTTF\b")),
    ("GetFileAttributes", re.compile(r"\bGetFileAttributes(?:Ex)?[AW]?\s*\(")),
    ("FindFirstFile", re.compile(r"\bFindFirstFile(?:Ex)?[AW]?\s*\(")),
    ("directory_iterator", re.compile(r"\b(?:recursive_)?directory_iterator\b")),
    ("filesystem query", re.compile(
        r"(?:\bstd::filesystem::|\bfs::|\.)(?:exists|is_regular_file|is_directory|file_size|last_write_time)\s*\(")),
    ("filesystem status", re.compile(r"(?:\bstd::filesystem::|\bfs::)(?:symlink_)?status\s*\(")),
]
# Not caught by a pattern: a vfs:: call given an ABSOLUTE path goes straight to the OS (dev; Shipping refuses one outside
# the game's and the user's folders -- Vfs.cpp AbsoluteAllowed). Build content paths from the content root, never a cwd.

LINE_COMMENT = re.compile(r"//.*$")
STRING = re.compile(r'"(?:\\.|[^"\\])*"')


def code_of(line, in_block):
    """The line without comments and string literals; whether a /* */ block is still open after it."""
    out = []
    i = 0
    while i < len(line):
        if in_block:
            end = line.find("*/", i)
            if end < 0:
                return "".join(out), True
            i = end + 2
            in_block = False
            continue
        start = line.find("/*", i)
        chunk = line[i:] if start < 0 else line[i:start]
        out.append(chunk)
        if start < 0:
            break
        i = start + 2
        in_block = True
    text = STRING.sub('""', "".join(out))
    return LINE_COMMENT.sub("", text), in_block


def main():
    if "--list" in sys.argv:
        print("allowed (not the game's runtime reads):")
        for prefix in ALLOW:
            print("   ", prefix)
    findings = 0
    for path in sorted(ROOT.rglob("*")):
        if path.suffix not in (".cpp", ".h", ".hpp", ".inl"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel.startswith(ALLOW):
            continue
        in_block = False
        for number, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            code, in_block = code_of(line, in_block)
            if "vfs-ok:" in line or code.lstrip().startswith("#"):
                continue
            for name, pattern in PATTERNS:
                if pattern.search(code):
                    print(f"sources/{rel}:{number}: {name}: {line.strip()[:160]}")
                    findings += 1
                    break
    print(f"check_file_io: {findings} finding(s)")
    return findings


if __name__ == "__main__":
    sys.exit(main())
