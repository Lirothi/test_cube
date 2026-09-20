#!/usr/bin/env python3
"""Fetch the local model and llama.cpp server the editor's command bar talks to.

Neither goes in the repository (docs/editor_llm_plan.md, E2): the engine gains no new
dependency, the model and the runtime update on their own schedule, and a crash in
inference cannot take an unsaved level down with it.  The editor finds both through
`editor_state.json` -> `levelEditor.intentModel`, so swapping either is a settings edit
and not a rebuild.

Default target is OUTSIDE the repo (D:/llm_models), because a 38 GB file has no business
in a git working tree.

  python tools/fetch_intent_model.py                  # model + server, default quant
  python tools/fetch_intent_model.py --quant UD-Q4_K_M  # smaller, fits a 24 GB GPU
  python tools/fetch_intent_model.py --server-only
  python tools/fetch_intent_model.py --print-settings  # show the JSON to paste
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DEST = Path("D:/llm_models")

# Qwen3.6-35B-A3B: a 35B mixture-of-experts with only ~3B parameters active per token.
# That combination is why it is the pick here -- it reasons like a large model but runs
# at the speed of a small one on CPU, so the editor pays no VRAM to the renderer.  The
# task itself (pick one action and a few asset names off a list, constrained by a GBNF
# grammar) is not demanding; the headroom is for understanding the Russian phrase.
MODEL_REPO = "unsloth/Qwen3.6-35B-A3B-GGUF"
MODEL_QUANTS = {
    # name -> (filename, approx GiB)
    "UD-Q8_K_XL": ("Qwen3.6-35B-A3B-UD-Q8_K_XL.gguf", 38.5),
    "Q8_0":       ("Qwen3.6-35B-A3B-Q8_0.gguf", 36.9),
    "UD-Q6_K":    ("Qwen3.6-35B-A3B-UD-Q6_K.gguf", 29.3),
    "UD-Q5_K_M":  ("Qwen3.6-35B-A3B-UD-Q5_K_M.gguf", 26.5),
    "UD-Q4_K_M":  ("Qwen3.6-35B-A3B-UD-Q4_K_M.gguf", 22.1),
}
DEFAULT_QUANT = "UD-Q8_K_XL"

# THE OTHER CANDIDATE, and it is a different shape of bet.  Qwen3.8-27B is DENSE, not a
# mixture of experts, and it carries a vision encoder.  Two things follow that the 35B
# cannot offer at any quantisation:
#
#   - A 4-bit quant is about 15 GiB, which fits on this card ENTIRELY.  The 35B does not
#     and never will (Q4 is 22 GiB before the KV cache), which is why it runs with
#     `--cpu-moe` and pays 22 s for the first request while the experts sit in RAM.
#     Measured free VRAM with the editor rendering the atoll: ~19.6 GiB.
#   - `mmproj` is the vision projector.  The model the editor runs today is blind, and
#     when asked what it was missing most it said so itself and asked for a screenshot.
#
# Qwen 3.7 is NOT here because it does not exist in open weights -- that generation was
# API-only and skipped for open release.  3.8 is the next one that shipped.
VISION_MODEL_REPO = "unsloth/Qwen3.8-27B-GGUF"
VISION_MODEL_QUANTS = {
    "UD-IQ4_XS":  ("Qwen3.8-27B-UD-IQ4_XS.gguf", 13.3),
    "UD-Q4_K_S":  ("Qwen3.8-27B-UD-Q4_K_S.gguf", 14.3),
    "UD-Q4_K_M":  ("Qwen3.8-27B-UD-Q4_K_M.gguf", 15.4),
    "UD-Q4_K_XL": ("Qwen3.8-27B-UD-Q4_K_XL.gguf", 16.4),
    "UD-Q5_K_S":  ("Qwen3.8-27B-UD-Q5_K_S.gguf", 17.4),
    "UD-Q5_K_M":  ("Qwen3.8-27B-UD-Q5_K_M.gguf", 18.4),
}
# The projector is a separate file and llama-server wants it by path (--mmproj).
VISION_PROJECTOR = ("mmproj-F16.gguf", 0.9)

LLAMA_RELEASES = "https://api.github.com/repos/ggml-org/llama.cpp/releases?per_page=8"
# CUDA build for an NVIDIA box; the CPU build is the fallback and works everywhere.
SERVER_FLAVOURS = ("win-cuda-12.4-x64", "win-cpu-x64")


def human(num_bytes: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if abs(num_bytes) < 1024.0:
            return f"{num_bytes:.1f} {unit}"
        num_bytes /= 1024.0
    return f"{num_bytes:.1f} TiB"


def download(url: str, target: Path, expected_gib: float | None = None) -> None:
    """Resumable download.  A 38 GB file over a home connection WILL be interrupted at
    least once, and starting from zero each time is how a fetch script becomes a thing
    nobody runs."""
    target.parent.mkdir(parents=True, exist_ok=True)
    part = target.with_suffix(target.suffix + ".part")
    have = part.stat().st_size if part.exists() else 0

    request = urllib.request.Request(url, headers={"User-Agent": "test_cube-fetch/1"})
    if have:
        request.add_header("Range", f"bytes={have}-")
        print(f"  resuming at {human(have)}")

    with urllib.request.urlopen(request) as response:
        if have and response.status != 206:
            # The server ignored the range; start over rather than corrupt the file by
            # appending a second copy of the whole thing to a partial one.
            print("  server refused to resume, restarting from zero")
            have = 0
            part.unlink(missing_ok=True)
        total = int(response.headers.get("Content-Length", 0)) + have
        mode = "ab" if have else "wb"
        done = have
        started = time.time()
        last_report = 0.0
        with open(part, mode) as out:
            while True:
                chunk = response.read(4 * 1024 * 1024)
                if not chunk:
                    break
                out.write(chunk)
                done += len(chunk)
                now = time.time()
                if now - last_report > 5.0:
                    last_report = now
                    rate = (done - have) / max(now - started, 0.001)
                    pct = f"{100.0 * done / total:5.1f}%" if total else "  ?  "
                    eta = (total - done) / rate if rate > 0 and total else 0
                    print(f"  {pct}  {human(done)} / {human(total)}  "
                          f"{human(rate)}/s  eta {eta / 60:.0f} min", flush=True)

    part.replace(target)
    print(f"  done: {target} ({human(target.stat().st_size)})")


def fetch_model(dest: Path, quant: str, vision: bool = False) -> Path:
    repo = VISION_MODEL_REPO if vision else MODEL_REPO
    quants = VISION_MODEL_QUANTS if vision else MODEL_QUANTS
    if quant not in quants:
        raise SystemExit(f"unknown quant {quant!r}; pick one of {', '.join(quants)}")
    filename, size_gib = quants[quant]
    target = dest / filename
    if target.exists():
        print(f"model already present: {target} ({human(target.stat().st_size)})")
        return target

    free = shutil.disk_usage(dest if dest.exists() else dest.parent).free
    needed = size_gib * 1024 ** 3 * 1.05
    if free < needed:
        raise SystemExit(f"need ~{size_gib:.1f} GiB free at {dest}, have {human(free)}")

    url = f"https://huggingface.co/{repo}/resolve/main/{filename}?download=true"
    print(f"model {MODEL_REPO} [{quant}] ~{size_gib:.1f} GiB")
    download(url, target, size_gib)
    return target


def fetch_server(dest: Path) -> Path:
    server_dir = dest / "llama.cpp"
    exe = server_dir / "llama-server.exe"
    if exe.exists():
        print(f"server already present: {exe}")
        return exe

    with urllib.request.urlopen(urllib.request.Request(
            LLAMA_RELEASES, headers={"User-Agent": "test_cube-fetch/1"})) as response:
        releases = json.load(response)

    for release in releases:
        assets = {asset["name"]: asset["browser_download_url"] for asset in release.get("assets", [])}
        for flavour in SERVER_FLAVOURS:
            # `startswith("llama-")` matters: cudart-llama-bin-win-cuda-12.4-x64.zip ends
            # with the same suffix and is the CUDA runtime, not the binaries.
            match = next((n for n in assets
                          if n.startswith("llama-") and n.endswith(f"bin-{flavour}.zip")), None)
            if not match:
                continue
            print(f"server {release['tag_name']} [{flavour}]")
            archive = dest / match
            download(assets[match], archive)
            with zipfile.ZipFile(archive) as zf:
                zf.extractall(server_dir)
            archive.unlink(missing_ok=True)

            # The CUDA build needs the runtime DLLs from the matching cudart archive.
            if "cuda" in flavour:
                cuda_tag = flavour.replace("win-", "").replace("-x64", "")
                cudart = next((n for n in assets if n.startswith("cudart-") and cuda_tag in n), None)
                if cudart:
                    print(f"  cuda runtime: {cudart}")
                    cuda_zip = dest / cudart
                    download(assets[cudart], cuda_zip)
                    with zipfile.ZipFile(cuda_zip) as zf:
                        zf.extractall(server_dir)
                    cuda_zip.unlink(missing_ok=True)

            found = next(server_dir.rglob("llama-server.exe"), None)
            if not found:
                raise SystemExit(f"llama-server.exe not found inside {match}")
            if found != exe:
                # Some archives nest everything one level down.
                for item in found.parent.iterdir():
                    shutil.move(str(item), str(server_dir / item.name))
            return exe

    raise SystemExit("no Windows llama.cpp build found in the last 8 releases")


def settings_json(model: Path, server: Path) -> str:
    return json.dumps({
        "levelEditor": {
            "intentModel": {
                "enabled": True,
                "modelPath": str(model).replace("\\", "/"),
                "serverExe": str(server).replace("\\", "/"),
                "endpoint": "127.0.0.1:8127",
                "autoStart": True,
                # 0 = pure CPU.  The renderer owns the GPU; a 3B-active MoE answers in
                # about a second on a desktop CPU, which is fine for a box you type into
                # once a minute.  Raise it to offload layers if you would rather spend
                # VRAM than wait.
                "gpuLayers": 0,
                "contextTokens": 8192,
                "timeoutSeconds": 60,
            }
        }
    }, indent=2)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dest", type=Path, default=DEFAULT_DEST,
                        help=f"where to put the model and server (default {DEFAULT_DEST})")
    parser.add_argument("--quant", default=DEFAULT_QUANT,
                        help=f"one of {', '.join(MODEL_QUANTS)} (default {DEFAULT_QUANT})")
    parser.add_argument("--vision", action="store_true",
                        help="fetch Qwen3.8-27B (dense, fits the card whole, has eyes) "
                             "and its mmproj projector instead of the 35B MoE")
    parser.add_argument("--model-only", action="store_true")
    parser.add_argument("--server-only", action="store_true")
    parser.add_argument("--print-settings", action="store_true",
                        help="print the editor_state.json block and exit")
    args = parser.parse_args()

    args.dest.mkdir(parents=True, exist_ok=True)

    if args.print_settings:
        filename, _ = MODEL_QUANTS[args.quant]
        print(settings_json(args.dest / filename, args.dest / "llama.cpp" / "llama-server.exe"))
        return

    quants = VISION_MODEL_QUANTS if args.vision else MODEL_QUANTS
    if args.vision and args.quant not in quants:
        args.quant = "UD-Q4_K_M"
    model = args.dest / quants[args.quant][0]
    server = args.dest / "llama.cpp" / "llama-server.exe"
    if not args.server_only:
        model = fetch_model(args.dest, args.quant, vision=args.vision)
        if args.vision:
            # The weights alone are a blind model: without the projector llama-server
            # loads and answers text, and an image is simply not accepted.
            name, gib = VISION_PROJECTOR
            target = args.dest / name
            if target.exists():
                print(f"projector already present: {target}")
            else:
                download(f"https://huggingface.co/{VISION_MODEL_REPO}/resolve/main/{name}"
                         "?download=true", target, gib)
    if not args.model_only:
        server = fetch_server(args.dest)

    print()
    print("Paste into editor_state.json (the editor also writes these from its own UI):")
    print(settings_json(model, server))


if __name__ == "__main__":
    main()
