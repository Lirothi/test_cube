# Memory Profiling and Texture Streaming

Moved verbatim from `AGENTS.md` on 2026-11-24, when that file had grown to 513 KB; `AGENTS.md` now holds the rules in
short form and an index of these notes. This note keeps the full text: design, measurements, history and the reasons
behind each rule. Section titles are the old ones, so an old reference «AGENTS.md "<title>"» lands here.

## Memory Profiling — How to Run It

Plan and numbers: `docs/memory_profiling_plan.md` (UE's LLM + Memory Insights + `rhi.DumpResourceMemory`).
Pick the tool by the question:

- **"Who holds the memory?"** -- always on: the `mem:` line every 5 s (private, vram, the GPU census by
  pool, allocations per frame) and `--memreport[=<s>]` (one dump: the census as a tag tree, every GPU
  object by size). CPU owners need `--memtrack` (the pointer map: `mem:` gains `cpu X MB untagged Y`,
  ~+0.07 ms a frame). Live: MCP `get_memory` (snapshot / diff), Developer Controls > Memory
  (`--mem-window`). A new GPU owner gets a tag in `core/memory/MemTags.h` and creates through
  `gpu::Create*` (`python tools/check_gpu_alloc.py` = 0); a CPU owner wraps its work in `MEM_SCOPE`.
- **"Which allocation, from where?"** -- the trace:

  ```bash
  test_cube.exe --level=data/levels/wind_test.json --memtrace --profdump=p.txt --shot-delay=20
  ```

  `--memtrace[=all|cpu|gpu][,stacks=big|all|off|1/N][,start=<frame>]` writes
  `traces/memtrace_*.mtrace` (last 10 kept). Then `python tools/memtrace.py verify` (the replay must
  equal what the process counted alive -- PASS byte for byte, or the trace is not to be trusted),
  `summary`, `leaks A B C`, `growth A B`, `churn A B`, `tags A B`, `timeline` (Perfetto JSON);
  markers are frames (`600`) or seconds (`12.5s`). `stacks=big` costs nothing measurable,
  `stacks=all` ~0.5 ms a frame -- cap the frame rate (`--set=frame.maxFps:60`) for a long one.
  To look rather than grep: `python tools/memtrace.py view [file]` opens `tools/memtrace_viewer.html` on the
  trace (timeline by tag, markers A-D by click/drag, the same rules, stack trees, tag table, verify badge;
  served on 127.0.0.1 only) -- or drop a `.mtrace` on that page. Its core gives the CLI's numbers exactly.
  `memtrace.bat [app switches]` in the repo root does the three in one go: the run (wind_test unless a
  `--level=` is given, Release; `set MEMTRACE_CFG=Debug` for exact names, `MEMTRACE_OPTS` = the switch's value),
  `verify`, `view`.
  **Who freed it**: since trace format 2 every free carries the freeing stack (same `stacks=` policy) --
  `query AfB A B --by fstack|ftree|finverted`, the viewer's "free callstack" groupings and its Frees /
  Decline presets; a row's details show both "Allocated at" and "Freed at". Frees are negative sizes.
- **"Is there a leak?"** -- the gates:

  ```bash
  test_cube.exe --level=data/levels/occlusion_test.json --cam-pos=0,8,0 --cam-orbit=35,14 --mem-readout=30
  ```

  15 s warm-up, 30 s at 4 Hz, a least-squares slope per series (private, vram.local, every tag);
  `mem readout: PASS|FAIL <series> <MB/s>` in the session log, exit code = failing series. FAIL =
  above 1 MB/s AND climbing in every third of the window (a capacity step is not a leak).
  `--scene-stress=150` adds `mem tags: STABLE|DRIFT|NOT JUDGED` to `logs/scene_stress.log` (a tag
  that climbs from visit to visit of the same level; first visits are cold and resolution-sized
  tags are left out); fewer than 150 iterations is `NOT JUDGED`, never a pass.
- **Prove a gate catches before trusting its silence**: `--mem-selftest=leak@<tag>[,kb=<n>][,cpu|gpu|both]`
  loses 64 KB a frame under that tag. The line `mem self-test: injected ...` must be in the log;
  measured: the readout FAILs it at ~28 MB/s, the stress reports DRIFT on every level.

Reading numbers: `private` counts the VRAM twice on this machine (write-combined pages mirror it,
M1), so a GPU leak FAILs `private` and `vram.local` together. Release stack names of std templates
are folded by ICF (a vector of pointers shows up as nlohmann's) -- `memtrace.py` groups from the first
non-`std::` frame; for exact names use Debug.

**A texture its level left behind wants only its tail** (2026-11-04; `--scene-stress=150
--stress-level=data/levels/wind_test.json` said `mem tags: DRIFT -- tex.stream +1070 MB`, and it was not the budget: the
pool is 16.9 GB). `MaterialDataManager::cache_` outlives a level on purpose, so every texture stays registered on every
level, and a texture without a box read as an unknown ref: every mip. Leaving wind_test read all of it to full size in
the background -- 347 textures, 1373 MB, the plateau by iteration ~165, where what the four levels' cameras ask for comes
to 55 MB. The port had dropped UE's third term (`AsyncTextureStreaming.cpp:291-293`: unknown only if used since the last
reference was removed). Now a texture that lost its last box (`Persist::lastBoxed`) wants its tail and `TryKeepMips` holds
what it has, with no IO: `tex.stream` at the end 1436 -> 111 MB, read 1372 -> 53 MB, `STABLE` (+6.6 / +7.1 MB). One that
never had a box stays unknown (a LUT, the ocean, the emissive and transmission maps `BoundsBuilder` does not look at).
The readout: `[texstream] ... left behind N (M MB)`, `left-behind` in the `streaming.dumpRows` table (under `--set` it
fires again after every level load: a table per visit), Developer Controls > Streaming. What a level left behind keeps its
mips until the pool presses (`streaming.poolSizeMB:40`: used 28-35 MB, left behind 0-15 MB), so going back reads nothing:
the bathroom at arm's reach 472 MB, 12 s on demo no reads, back at the same 474 MB and the same picture (1.2 / 3.4 % of
pixels over 8 levels on the bathroom / the grove, the same arm twice 2.9 / 2.8 %). Proof of the holder and the price of
giving the memory back: releasing at a switch the materials only the cache holds (a probe, not kept) takes the table to
each level's own (338 / 82 / 18 / 9 textures) and `tex.stream` to 51 MB, and wind_test's switch 265-427 -> 574-659 ms
(texture loads 752 -> 6907). READING THE VERDICT: the rule wants growth in 2/3 of the steps, so a fill that ends inside
the run reads by the run's length -- that same climb said `DRIFT` at 150 iterations and `STABLE ... largest climb +1182 MB`
at 300. Read the largest climb.

**A level load shares its textures on its one upload list** (2026-11-04). `Texture2D::CreateFromFile` handed out no cache
hit in the frame a texture was uploaded -- the transition lives on the uploader's list, and another list may run first (the
editor's thumbnail jobs) -- and a level load is one list in one frame, so the cache shared nothing where it mattered:
`Wall_Trim_*.dds` 20 copies, 347 textures over 171 files, 875 of their 1373 MB at full size. The same list now shares at
once; another list in that frame still loads its own copy. wind_test in the editor: 339 -> 163 textures; the bathroom at
arm's reach `tex.stream` 472 -> 166 MB, read 459 -> 161 MB, GPU objects 273 -> 106. Stress 150 (alternated twice): the
`streaming.dumpRows` table 347 -> 171 rows, the 171 files; `[texcache]` 752 loads / 78 shared -> 576 / 254; the boot's texture
loads 343 -> 167, 172-191 -> 96-100 ms; the boot 4056-4315 -> 3876 ms; 50 switches 6242-6324 -> 6089-6154 ms. The same picture:
the bathroom 1.25-2.38 % of pixels over 8 levels against the same arm across processes 1.21 % (within a process up to 2.31 %,
its candles), the grove 0.10-0.40 % against 0.50 %.

**A stream-in whose file no longer matches its mip table is refused** (2026-11-24, architecture review M14). The IO thread
checks every request before it reads (`TextureStreamingIo::CheckLayout`): the file the size its table says, every row of
every new mip inside what is read and inside the ring range the request owns. A mismatch is an Error ("texture stream-in
refused: ..."), nothing reaches the ring, and the texture goes off streaming with the mips it has (a Warning; `failed`
and the manager's `refused` count in `[texstream]`) until it is registered again. `--texstream-selftest=layout|size`
builds the first stream-in wrong on purpose.
