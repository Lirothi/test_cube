# Ocean surf simulation plan (SDF-spawned wave sim + breaking foam)

## Goal

A REAL nearshore surf simulation, replacing every faked "travelling crest" we tried and rejected
(see docs/ocean_shore_foam_breakup_plan.md, variant B — deleted twice): discrete disturbance
segments spawned off the shoreline via the shore SDF launch waves into a 2D wave-equation sim
that runs ping-pong in compute; the wave fronts refract toward the beach on their own (speed
from water depth), BREAK where the surf criterion says real waves break, deposit foam there, and
the foam dissipates exponentially. Both ocean surfaces (legacy and modern) consume the foam as
one more coverage source; later the sim height can also displace vertices so the run-up becomes
a visible hump of water.

User verdict that motivated this: the shader-side A+C breakup (dissipation patches + wind
thinning) is good and STAYS; Crest's depth-driven shoreline deposit is "what we already have,
only worse"; the missing piece is real, event-driven surf.

## Invariants (violating any of these reproduces a rejected failure)

1. **No term may be a function of water depth alone.** Depth-only terms draw static isobath
   rims/concentric rings — rejected three times in this saga. Every foam deposit must trace back
   to a discrete EVENT (a spawned wave passing/breaking).
2. **The FFT open sea is untouched.** The sim is an additive nearshore layer; it must die out in
   deep water and never feed back into the FFT cascades.
3. **Dead calm = near-silence.** Spawn rate/amplitude ride the shared contact-foam wind remap
   (`ShoreFoamWindAmount`); at wind 0 the spawner (almost) stops, so no standing band.
4. **Feature default OFF** (`surfSimEnabled = false` in OceanRenderConfig); every step keeps
   both variants byte-identical when off.

## Detachability contract (hard requirement, same rank as the invariants)

The whole feature must be trivially disconnectable at THREE levels, and every step is built to
preserve all three:

1. **Runtime**: `surfSimEnabled` off = ZERO cost — no compute dispatches recorded (the
   `Ocean.SurfSim` scope disappears from the profiler, checked at S0), sim textures not
   created (lazy) or released, and the surface shaders' sim taps return identity through a
   uniform guard (no variant rebuild needed to toggle).
2. **Source**: every touch outside the new files is a TAGGED thin injection (`surf sim
   injection` tag), following the proven ocean_shore_foam_dissipation.hlsli pattern: one
   include + a couple of marked lines per surface shader, a couple of marked call sites in
   OceanRenderable. Deleting the tagged lines returns every existing file byte-identical to
   its pre-plan state; all real logic lives in the new OceanSurfSim.* / ocean_surf_sim_cs.hlsl
   files, which then simply drop out of the project.
3. **Data**: config/JSON fields are additive with safe defaults — old levels load unchanged,
   and a level saved with the feature never breaks a build that removed it (unknown JSON keys
   are ignored by the parser).

## Architecture

- **Domain**: a sliding world-space window centred on the camera (precedent: the shore depth
  map, 500 m / 512²; `Pass_ShoreDepth`, `ShoreDepthUV`). Start at 512², same 500 m; the window
  re-anchors with the camera (copy-shift on re-anchor — see how the shore depth / foam trail
  handle their windows before inventing anything).
- **Textures** (all in the window's UV space):
  - WaveSim A/B: RG16F ping-pong — R = height h, G = vertical velocity v (Crest's
    `UpdateDynWaves.compute` layout; five-point Laplacian, verified scheme).
  - SurfFoam A/B: R16F ping-pong — deposited foam, decays; ping-pong so re-anchor and (later)
    advection are possible.
- **Update loop**: fixed-dt substeps (start 1/60 s) with a catch-up budget per frame (cap the
  substep count; Crest's `LodDataMgrPersistent` is the reference for the cadence/ping-pong
  pattern). Dispatched from the ocean's existing compute path
  (`OceanRenderable::PrepareCompute/RecordCompute`, where the FFT already lives). GPU scope
  `Ocean.SurfSim`, budget < 0.15 ms.
- **Wave speed from depth**: `c = sqrt(g · depth)` sampled from the shore depth map, clamped by
  the CFL condition `c·dt/dx <= ~0.7` (dx ≈ 1 m at 512²/500 m → c_max ≈ 40 m/s at 1/60 s; real
  c stays far below, but CLAMP ANYWAY — an unclamped deep-water texel explodes the sim).
  Depth-dependent speed buys REFRACTION for free: fronts slow on the shallows, bend parallel to
  the shoreline, bunch up — real surf behaviour, no faking. Waves attenuate/are damped in deep
  water (invariant 2) and at the window border (open boundary — damp, don't reflect).
- **SDF pipeline status (S0b — verify, don't assume)**: the SDF builds unconditionally once
  per level (`shoreSdfDirty_` starts true; `SetShoreArea` re-dirties) and its Terrain layer
  mask DOES include the authored islands — the atoll's island meshes carry
  `"renderLayer": "Terrain"` in the level, so they rasterize into both shore maps. Nothing was
  ever disabled. What the sim work actually needs restored/added: (1) a way to SEE the SDF in
  the legacy mode — the isoline/foam debug views are a modern-only variant (`g_foamDebug`),
  so fold an SDF/shore-depth display into the sim's own debug view at S0; (2) verify the
  first-frame bake against streamed mesh data (bounds exist from the JSON transform on frame
  one, but if vertex data arrives later the bake could rasterize nothing — if that reproduces,
  re-dirty the SDF when pending loads complete).
- **Spawner (the SDF part)**: a small fixed array of disturbance slots (~16, in the sim CB).
  On a wind-dependent cadence the CPU picks a random bearing/seed and a tiny compute kernel
  refines it against the shore SDF (`SampleShoreField` — plan distance + gradient): place the
  segment at `spawn distance` seaward of the waterline, oriented perpendicular to the SDF
  gradient (i.e. along the shore), `segment length` metres long. Each slot injects a capsule
  Gaussian hump into the height field over an attack/release lifetime, then frees. Discrete,
  local, randomized events — this is what kills concentric sync (invariant 1).
- **Breaking + foam deposit**: real surf criterion — a shallow-water wave breaks when
  `H / depth` exceeds gamma ≈ 0.78. Deposit
  `foam += k · dt · sat(h − gamma·depth) · frontWeight`, where frontWeight picks the steep
  forward face (e.g. `sat(-dh/dt)` or forward slope), broken up along the front by the existing
  ContactFoamTex noise so the line is torn, not drawn with a ruler. Foam then
  `*= 1 − fadeRate·dt` (Crest-style exponential dissipation).
- **Consumption**: the ocean surface samples SurfFoam (window UV, zero outside) as an ADDITIVE
  coverage source next to the contact foam, BEFORE the shore-albedo blend so the existing
  albedo/tail look (which the user tuned and likes) applies unchanged. Injection style: one
  function in a shared include + a couple of tagged lines per surface (the proven
  `ocean_shore_foam_dissipation.hlsli` pattern).
- **Debug view**: a runtime uniform (not a variant) that lets the LEGACY surface visualize the
  raw sim: height field / foam field / spawner footprints. Judged in phase-series GIFs
  (`--shot-count/--shot-step`), never stills.

## References

- Crest (MIT): local reference copies in **docs/ref/crest/** (kernels, managers, settings —
  see the README there). `UpdateDynWaves.compute` is the wave-equation kernel to copy the
  numerics from, `UpdateFoam.compute` the deposit/decay shape, `LodDataMgrPersistent.cs` the
  substep/ping-pong cadence. Upstream: https://github.com/wave-harmonic/crest
- Breaking criterion: shallow-water breaker index gamma = H/d ≈ 0.78 (McCowan) — any coastal
  engineering text; this is the "where surf actually breaks" rule.
- Ours: `shaders/ocean_surface.hlsl` (`SampleShoreField`, `ShoreDepthUV`), `Pass_ShoreDepth`
  (window + re-anchor precedent), `ocean_shore_foam_dissipation.hlsli` (injection pattern),
  `OceanRenderable::RecordCompute` (FFT compute precedent), memory: RecordComputeDispatch
  gotcha — check the numthreads(8,8,1) requirement before writing kernels.

## Steps (each one is a separable, verifiable increment; build Debug+Release 0/0 + dxc on every
touched shader at every step; feature stays default-OFF throughout)

- **S0 — Infrastructure, no math.** OceanSurfSim class (sources/ocean/OceanSurfSim.h/.cpp, both
  vcxproj AND filters), window transform + re-anchor copy-shift, WaveSim/SurfFoam ping-pong
  textures, empty update kernel (`shaders/ocean_surf_sim_cs.hlsl`), `Ocean.SurfSim` GPU scope,
  `surfSimEnabled` config/JSON/UI toggle, debug view plumbing (legacy PS shows the height
  channel as a tint). GATE: toggling on shows the (black) window tint moving with the camera,
  re-anchor shows no smearing/garbage; toggling OFF removes the `Ocean.SurfSim` scope from the
  profiler entirely (detachability level 1 proven here, once, and re-checked at every later
  step); `--scene-stress` CLEAN (buffers churn on level switch); GBV clean in Debug.
- **S0b — verify the SDF on the atoll + SDF debug display.** No mask work needed (island
  meshes are Terrain-layered — see the architecture note). Add the SDF/shore-depth channels to
  the sim debug view and confirm on the atoll level: distance rings hug every island, the bake
  survives streamed meshes (re-dirty on load completion if the first-frame bake comes up
  empty). GATE: debug view shows correct SDF around the mesh islands in LEGACY mode.
- **S1 — Wave equation.** Five-point Laplacian + velocity integration + damping, c from the
  shore depth map with the CFL clamp, deep-water and border attenuation. A "Poke" button in the
  ocean window injects a test hump under the camera. GATE (debug view GIF): the poke ring
  spreads, slows over the shallows, bends around the island, dies at the border; sim stays
  stable after minutes and after teleporting the camera.
- **S2 — SDF spawner.** Disturbance slots + CPU cadence + SDF refine kernel (position at spawn
  distance, orientation along shore), capsule hump with attack/release. Knobs: Spawn distance,
  Segment length, Wave amplitude, Spawn interval, Wind coupling. GATE (debug view GIF): waves
  are born in DIFFERENT stretches of coast at different times, travel shoreward, fronts refract
  parallel to the waterline; at wind ~0 the spawner goes quiet (invariant 3).
- **S3 — Breaking + foam.** Gamma criterion + front weight + ContactFoamTex breakup on deposit,
  exponential fade. Knobs: Deposit strength, Breaker gamma, Foam fade rate, Front breakup.
  GATE (debug view GIF): foam ignites only on the breaking front as a torn, finite-length
  stripe, trails behind the wave, dissolves; deep water stays black; NOTHING resembles an
  isobath band (invariant 1 — this is the step where the whole idea is judged).
- **S4 — Legacy consumption.** SurfFoam sample added to the legacy contact coverage (thin
  tagged injection, before the albedo blend). GATE: user judges phase-series GIFs at wind 0.1 /
  0.6 — sim foam coexists with the tuned tails; OFF stays byte-identical.
- **S5 (opt) — Modern consumption.** Same injection into the modern surface's contact foam.
- **S6 (opt) — Vertex hump.** Sim height added (clamped, depth-faded) to displacement.y in the
  surface VS so the run-up is a visible wave of water carrying its own foam. Separate gate: no
  cracks against the sink/edge logic, no poking through steep dunes (remember the
  displaced-ground saga).
- **S7 — Polish + perf.** Re-anchor under fast camera flight, `--profdump` before/after
  (budget < 0.15 ms GPU), `--scene-stress` + GBV, config/JSON/UI final pass, update this doc's
  status. Compatibility note for the async-compute plan: the sim joins the ocean's compute
  bucket (docs/async_compute_plan.md).

## Status

- **S0 DONE + S0b verified (2026-08-14, uncommitted).** OceanSurfSim class (512²/500 m window,
  WaveSim RG16F + SurfFoam R16F ping-pong pairs, texel-aligned snap + Relocate copy-shift),
  `ocean_surf_sim_cs.hlsl` (placeholder Update: world-anchored 4 m checkerboard; Relocate),
  `Ocean.SurfSim` GPU scope, `surfSimEnabled` config/JSON + both UIs, debug tint in the legacy
  PS (1 height / 2 velocity / 3 SDF isolines / 4 shore depth), boot flags `--ocean-surf-sim` +
  `--ocean-surf-debug=<view>` for headless capture. Verified: checkerboard stands still in the
  world inside the window and fades past 500 m; SDF isolines hug the MESH island on wind_test
  (S0b gate) in legacy mode; both builds 0/0, dxc clean, `--scene-stress=10` CLEAN. NOTE: the
  Relocate content-preservation check needs persistent content — done at S1 with Poke, as
  planned. NOTE: `--scene-stress` returns before the rest of the cmdline parses, so the stress
  ran with the sim OFF (the shipping default).
- A+C shader breakup shipped separately and stays regardless of this plan.
