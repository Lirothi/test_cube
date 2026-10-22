#!/usr/bin/env python3
"""Memory profiling plan M1: every GPU allocation goes through rendering/core/GpuAlloc.h.

Scans sources/ for D3D12 calls that create memory-holding objects
  * CreateCommittedResource[1-3]( / CreatePlacedResource / CreateReservedResource / CreateHeap
  * CreateDescriptorHeap( / CreateQueryHeap(
  * render::CreateCommittedTexture(   (the enhanced-barrier primitive; callers use gpu::)
that are not the gpu:: wrapper, outside the files that implement the wrappers. Such a call makes an
object the GPU census never sees -- `gpu.untracked` on the memory line grows and nobody knows why.
Comment text is ignored. Exit code = number of findings.

    python tools/check_gpu_alloc.py
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1] / "sources"

# The wrappers themselves.
ALLOW = (
    "rendering/core/GpuAlloc.h",
    "rendering/core/GpuAlloc.cpp",
    "rendering/core/TextureCreate.h",
    "rendering/core/TextureCreate.cpp",
)

PATTERNS = [
    ("CreateCommittedResource", re.compile(r"(?<!gpu::)\bCreateCommittedResource[1-3]?\s*\(")),
    ("CreatePlacedResource", re.compile(r"\bCreatePlacedResource[1-2]?\s*\(")),
    ("CreateReservedResource", re.compile(r"\bCreateReservedResource[1-2]?\s*\(")),
    ("CreateHeap", re.compile(r"(?<![\w:])CreateHeap1?\s*\(|->CreateHeap1?\s*\(")),
    ("CreateDescriptorHeap", re.compile(r"(?<!gpu::)\bCreateDescriptorHeap\s*\(")),
    ("CreateQueryHeap", re.compile(r"(?<!gpu::)\bCreateQueryHeap\s*\(")),
    ("CreateCommittedTexture", re.compile(r"(?<!gpu::)\bCreateCommittedTexture\s*\(")),
]


def strip_comments(line: str) -> str:
    line = re.sub(r"/\*.*?\*/", "", line)
    at = line.find("//")
    return line if at < 0 else line[:at]


def main() -> int:
    findings = []
    for path in sorted(ROOT.rglob("*")):
        if path.suffix not in (".cpp", ".h"):
            continue
        rel = path.relative_to(ROOT).as_posix()
        if rel in ALLOW:
            continue
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
                if pattern.search(code):
                    findings.append((rel, number, name, raw.strip()))

    for rel, number, name, snippet in findings:
        print(f"{rel}:{number}: {name}: {snippet[:110]}")
    print(f"{len(findings)} finding(s)")
    return len(findings)


if __name__ == "__main__":
    sys.exit(main())
