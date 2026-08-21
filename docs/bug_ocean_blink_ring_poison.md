# Ocean blink: NaN poison in the per-frame upload ring

**Status: RESOLVED 2026-08-21. Root cause found to the field name and fixed; verification
below.**

## Root cause (the whole chain)

1. The legacy ocean binder wrote only the USED absorption-gradient keys into the per-frame CB
   (`for i < absorptionCount` — the level uses <= 4 of `absorptionColors[8]`), so keys [4..7]
   (CB bytes [1168..1232) of the 1792-byte OceanCB) were NEVER written.
2. The shader evaluates the gradient touching all 8 keys and masking the unused ones — but a
   mask MULTIPLY is not a branch: finite garbage * 0 = 0 (invisible), **NaN * 0 = NaN**, and
   the NaN eats the whole absorption term = the deep-water colour -> black water. Foam and
   sun sparkle are separate terms and survive — exactly the observed blink anatomy.
3. Normally the unwritten bytes held whatever the upload ring last carried at that offset —
   quasi-stable finite leftovers, masked to zero, invisible for months. When the ring's
   allocation ORDER shifts for a frame (bundle/graph workers bump the ring's atomic
   concurrently; the interleave is nondeterministic), the ocean CB lands at a different
   offset and its unwritten keys overlay ANOTHER pass's stale bytes.
4. f0a01c4's `RefreshChunkGroupLods` began writing `groupLodOverride_.fill(-1)` -> 64 int32s
   of 0xFFFFFFFF into the VSM SetupCB every frame. 0xFFFFFFFF **as float is NaN**. On a
   shifted frame the ocean's unwritten keys could land on those bytes -> NaN gradient -> one
   black frame; the next frame the layout snaps back and it recovers. The commit did not
   create the hole — it changed what leaked through it (the parent's bias table carried
   mostly zeros). DLSS was never the cause; it only modulates thread timing (rate x5-10).

## The proof (key experiments, each = shot-series metric)

- `fill(-1)` alone blinks; `fill(0)` alone is clean — same code, same cost, same timing.
- Setup shader FORCING `groupLod = 0` (table read dead-code-eliminated) still blinks while
  the CPU array holds -1s: the legitimate consumer is irrelevant, the BYTES are the poison.
- Ring NaN-flood (memset the whole upload ring to 0xFF at frame start) makes the black water
  PERMANENT -> deterministic repro. Ocean off -> frame normal (the ocean is the reader).
- CB-hole dump under the flood: ocean holes = [1028..1040) `_fogDebugPad` (benign),
  **[1168..1232) `absorptionColors[4..7]`**, [1576..1792) tail padding (benign). gbuffer has
  a 32B tail hole, skybox a 124B tail hole + strays — same class, currently benign.
- Sniper probe: under the full ring flood, zeroing ONLY bytes [1168..1232) of the ocean CB
  restores a pixel-normal frame. The lethal hole is exactly `absorptionColors[4..7]`.

## The fix (main tree)

1. `OceanRenderable.cpp` binder: write ALL 8 `absorptionColors` slots every frame (unused
   tail = repeat of the last valid key).
2. Systemic guard in `RenderableObject::Render` AND `::RenderShadow`: memset the fresh
   per-object CB allocation to zero before the binder runs — every unwritten CB field in any
   binder becomes a deterministic 0.0 instead of a ring-layout lottery. (~1-2 KB memset per
   object per frame — noise.)

**Verification**: fixed main binary, 3x200 shots = 597 pairs, ZERO blinks, max diff 0.21,
and the steady-state shimmer median dropped 0.174-0.195 -> 0.160-0.164 — the clean-build
signature (the elevated shimmer was the same corruption at sub-blink scale).

Recommended follow-ups (not done):
- Debug-build NaN canary: memset the ring to 0xFF in ResetUpload under a debug flag — this
  class of bug then screams on the first frame instead of hiding for months.
- The skybox/gbuffer tail holes are benign today; the memset guard already covers them.

---

# Original investigation log (kept for the method)

**Status: root MECHANISM proven to the byte level; the guilty READER not yet identified.**
2026-08-21. Symptom reported on `wind_test`: with the ocean visible, the whole water surface
collapses to near-black for exactly one frame, then recovers. Static camera. User-bisected to
commit `f0a01c4` ("vsm and lod tuning"); parent `4036740` clean — both verified here with long
series (parent: 1200+ clean pairs; f0a01c4: reproduces at ~1-2 events per 200-shot run).

## Repro + detector

```
x64\Release\test_cube.exe --level=data/levels/wind_test.json --wind-freeze
  --set=exposure.autoExposure:0
  --cam-pos=-134.85,13.01,121.28 --cam-rot=-0.0185,0.9103,-0.4115,-0.0408
  --shot=<dir>\x.png --shot-delay=6 --shot-count=200 --shot-interval=0.13
```

Detector: meanabs diff of consecutive shots; blink = a single frame bracketed by two diffs of
50-74 against a noise floor of ~0.2. A blinking build also shows an elevated steady-state
shimmer median (~0.18 vs ~0.163 on clean builds) — a useful early tell from ONE run.
Scratchpad analyzer: `analyze.py` (keeps only anomalous frames ± neighbours, deletes the rest).

## The proven chain

1. All of f0a01c4's actual features are innocent. Each was disabled in an isolated worktree
   build (`D:\Programming\test_cube_bisect`) and the blink SURVIVED every time: per-chunk
   gbuffer draws off, RenderShadow chunk branch off, `vsm_page_setup_cs` override read
   neutralized, chunk LOD values forced (all-LOD0 / old palm bounds / both).
2. The trigger reduces to ONE LINE of `ShadowGpuData::RefreshChunkGroupLods`:
   `groupLodOverride_.fill(-1)` — executing JUST this (early return right after; no object
   walk, chunk tiers never computed) blinks. Replacing it with `fill(0)` — same write, same
   timing, same everything — is clean (verified 3x200). An equal-cost dummy spin at the same
   call site is also clean, so it is not CPU timing.
3. The GPU does not legitimately consume the values: with the setup shader FORCING
   `groupLod = 0` (the table read dead-code-eliminated; GPU LOD behavior identical to the
   clean config) the blink still fires as long as the CPU array holds -1s. The array's only
   legitimate journey is CPU array -> `SetupCB::groupLodOverride` tail -> per-frame upload
   ring (`FrameResource::AllocDynamic`) -> `vsm_page_setup_cs` b0. Nobody else references it
   (grepped: one writer, one CB copy, one shader read).

**Conclusion: the bytes `0xFFFFFFFF` sitting in the upload ring are poison in themselves.
Something OTHER than the setup CS occasionally reads that ring region through a stale or
mis-sized binding. `0xFFFFFFFF` as float = NaN -> black water for one frame; zeros (what the
parent's `gGroupLodBias` table carried for nearly every group) are a benign 0.0 — the same
hole existed BEFORE f0a01c4 and was chewing harmless bytes. The commit did not create the
bug; it changed the poison.** (The user predicted "что-то переезжает ресурсы, копай в
трипл-буферинг" — correct.)

## Supporting facts

- DLSS is NOT required: one native (--dlss=off) blink caught at 2560x1440; DLSS multiplies
  the rate ~5-10x (timing/overlap modulation). Reflections off (`render.reflectionSource:0`)
  still blinks. Ocean off = clean (the ocean is the VICTIM, not the source; it records last
  via the transparent-pass bundles, i.e. through TaskSystem worker threads).
- VSM page stats are flat through a blink (req=90, new=0, fail=0): page management uninvolved.
- Mesh-group count 56 of 64 — the VSM group cap (docs/vsm_group_cap_removal_plan.md) is NOT hit.
- Blink frame anatomy (kept evidence: scratchpad blink2/nochunk2_23/24/25.png): whole water
  near-black, foam/spec sparkles survive; neighbours pristine.
- Constant ring-layout shift (+256B dummy alloc at frame start from frame 600 on) does NOT
  make the corruption permanent -> the reader re-derives its address every frame; the stale
  window is intra-frame or one-frame-lagged, not load-time.

## Traps burned into this hunt (do not repeat)

- The worktree binary loads shaders from its CWD — but PROVE every shader edit reached the
  GPU (garbage-the-file test changes the frame; a "no visual change" judged by eye is NOT
  proof — the a0.x=0 sabotage DID apply at meanabs 2.588 while looking identical in a
  thumbnail). Metric over eyeballs, both directions.
- The live water shader is the LEGACY path (`ocean_surface_legacy.hlsli` via
  ocean_surface.hlsl); the modern PSMain there is dead code, and the water samples NO shadow
  itself (`shadowAttenuation = 1.0`) — the shadow band seen on water is the shadowed seabed
  refracted through it. `vsm_sample.hlsli` edits do not reach the water pass at all.
- Shot filenames without zero-padding string-sort wrong (`_13` after `_129`); the analyzer
  must numeric-sort or blink pairs get mislabeled.
- One 200-shot run proves "blinks"; it does NOT prove "clean" (~1-2 events/run Poisson).
  Clean verdicts need 3+ runs (600 pairs) minimum.

## Next steps

1. Alternating-parity ring shift (dummy alloc on odd frames only): a one-frame-lagged reader
   then misses EVERY frame -> water breaks constantly -> capture at leisure in
   RenderDoc/PIX and read off exactly which draw consumes the wrong region.
2. If not lag-1: intra-frame race over the concurrently-CAS-bumped ring (bundle workers +
   graph workers allocate in nondeterministic order). Amplify: fill the ring's unused tail
   with NaNs each frame — event rate should explode -> fast victim bisection by toggling
   passes.
3. When the victim is found: fix the real hole (stale CBV / CB declared larger than the CPU
   write / missed per-slot cache). Then `fill(-1)` stops being poison. Recommend a debug-mode
   NaN canary filling the ring's free space so this class of bug screams immediately.

## Worktree state

`D:\Programming\test_cube_bisect` (junctions: textures/, models/ -> main tree). Currently at
f0a01c4 + probes: gbuffer/shadow chunk branches off, SelectLod chunk fill off, Refresh body =
`fill(-1); return;`, Rebuild group-count telemetry print, BeginFrame constant +256B ring shift
after frame 600. Main tree untouched by this hunt. Delete the worktree when done
(`git worktree remove --force D:\Programming\test_cube_bisect`).
