# Ocean shore foam breakup plan

## Problem

The legacy contact foam (and any depth-driven shore foam) is a pure function of water depth:
`coverage = f(waterDepth, texture)`. Water depth on a flat beach is a monotonic function of the
distance to the waterline, so every threshold in that formula draws a **band of constant width
along the isobath**. At low wind, with no swash machinery running, the shoreline reads as a
static outlined rim ("обводная каёмка") — geometrically correct, visually dead. The dither
texture breaks the band *spatially* (torn edge) but not *temporally*: the same pixels stay lit
frame after frame.

Real reference: at dead calm a flat beach has almost NO surf foam — a thin, broken, slowly
shifting thread right at the waterline, not a uniform strip.

## Survey — how the industry breaks the band

1. **Counter-scrolled noise on the mask + contrast remap** (the standard stylized-water trick,
   80.lv / Roystan / Ghibli-style writeups). The foam mask is multiplied by two noise layers —
   a large slow one that tears the band into patches and a small fast one for detail — then
   remapped with contrast so edges are torn, not grey. Cheap, purely local.
2. **Swash phase + dissolve on retreat** (Cyanilux shoreline breakdown). A cosine over
   (shore distance, time) modulates the foam threshold, and the smoothstep edge is animated so
   the foam *dissolves* as it retreats instead of sliding. Gives the band a surf rhythm.
3. **Foam as a simulated quantity** (Crest). Shallow water and pinched crests *deposit* foam
   into an accumulation buffer; every tick it decays exponentially (fade rate). True dissipation
   with memory. NOTE: this does NOT fix the calm-shore band by itself — Crest's shallow-water
   deposit is depth-driven too, so its steady state at calm is the same rim, only with
   hysteresis. The sim pays off for wave-driven deposits (trails, whitecap memory).
4. **Time-evolving noise instead of scrolling** — the noise field changes in place (3rd
   coordinate = time). Without a 3D texture this is approximated by two layers drifting in
   OPPOSITE directions: the sum never reads as a current, it just breathes.

## Variants for our legacy surface

- **A. Dissipation layer (chosen first)** — techniques 1 + 4. A large-scale (10–40 m), slow
  spatio-temporal field `D(x, t) ∈ 0..1` multiplies the coverage **depth threshold**
  (`TailDepth · D`), so patches of the band thin out and geometrically vanish, then regrow in
  place. Threshold, not coverage: an alpha-fade of the whole band reads as transparency; a
  threshold squeeze reads as the foam actually dissipating. Two counter-drifting low-frequency
  samples of the existing ContactFoam texture (no new assets), remapped with contrast around
  mid-grey. All knobs authored, `amount = 0` is a true identity.
- **B. Depth-phase surf ritm** (technique 2, candidate next). `cos(waterDepth · k − t · speed)`
  modulates the local threshold with an amplitude that decays toward deep water — bands of foam
  crawling shoreward along the isobaths. Zero texture samples. Combines with A (noise tears the
  phase bands so they do not read as rings).
- **C. Wind thinning** — scale the band's reach/intensity by wind (the modern stack already does
  this: inland fade `lerp(0.02, 0.2, windAmount)`). Physically the right answer for dead calm
  (almost no foam). Cheap; candidate after A/B once the look is judged.
- **D. Foam accumulation sim (Crest-style)** — a real deposit/decay buffer, wave-energy-driven
  deposits. The only variant with memory (trails). Big: a new sim pass; only worth it for the
  MODERN surface someday. Out of scope here.

## Variant A design (implemented)

- **File**: `shaders/ocean_shore_foam_dissipation.hlsli`, self-contained: the caller passes the
  texture, sampler, world position, time and the parameter vector. No dependency on the legacy
  cbuffer layout → reusable from the modern surface later.
- **Field**: `n1` and `n2` are low-frequency samples of the breakup texture drifting along two
  FIXED world directions (not wind: the shape must not collapse at dead calm), at slightly
  different scales/speeds so the beat never repeats. `field = sat(((n1+n2)/2 − 0.5)·contrast + 0.5)`,
  `factor = lerp(1, field, amount)`.
- **Injection contract** (the only touches to `ocean_surface_legacy.hlsli`, each tagged
  `foam dissipation`): one cbuffer field `shoreLegacyDissipationParams`, one `#include` above
  `ContactFoam`, and `TailDepth → TailDepth · factor` inside `ContactFoam`. Delete those three
  and the file is byte-equivalent to the pre-A state; at runtime `Dissipation amount = 0` is the
  same identity without touching anything.
- **Knobs** (config `shoreLegacyDissipation*`, JSON, both UIs, section "Foam dissipation"):
  - `Dissipation scale` (m) — world size of the patches (default 25).
  - `Dissipation speed` (m/s) — drift/evolution rate (default 0.5).
  - `Dissipation amount` (0..1) — modulation depth; 0 = OFF (default 0).
  - `Dissipation contrast` — steepness of the patch remap; higher → patches saturate to full
    0/1 kill/keep (default 2, the averaged-samples distribution needs > 1 to reach the ends).

## Evaluation

Judge in a phase-series GIF (`--shot-count/--shot-step`), never in stills — the whole point is
temporal behaviour. Risks to watch: the "curtain over standing water" failure mode (threshold
animation over a world field) — expected to be acceptable here because dither patches already
read as foam, not water, and the field breathes in place instead of dragging a front; and patch
drift reading as a current if the two layer speeds are set too close.

## Variant C design (implemented) and the fate of variant B

Verdict on A alone: good at wind 0.6, but at wind 0.1 the band is still a standing heap. C
removes the excess:

- **C. Wind thinning** — the tail threshold is additionally scaled by
  `lerp(1, windAmount, thinning)`, where windAmount is the shared contact-foam wind remap
  (`ShoreFoamWindAmount`, a parameterized copy of the modern `ContactFoamWindAmount`; the legacy
  cbuffer declares `shoreFoamWindParams` under the modern name, so the existing C++ feed binds it
  with zero new plumbing). At dead calm the band starves toward nothing — the physically honest
  look. Knob: `Wind thinning` (0 = OFF, rides `shoreLegacyFoamParams2.y`).
- Both factors combine in `ShoreFoamBreakupThresholdFactor`; the legacy injection is a single
  call multiplying the tail depth threshold.

**Variant B was implemented twice and DELETED.** v1 (cosine over depth): the whole rim pulsed in
one synchronized beat and the modulation flashed the torn tail instead of running a solid front.
v2 (sawtooth phase, solid crest pulse + exp-decaying trail, noise-jittered phase): still rejected
on sight — the phase is a function of water depth, so the pattern stays CONCENTRIC (isobath
rings), it fires next to vertical walls (depth sweeps through every value within a pixel or two,
so the first band flashes at the base of a cliff), and the crest read as faint smearing rather
than a front. The structural conclusion: a believable travelling crest needs a direction and an
anchor to the waterline (shore SDF) plus material vertex travel — exactly what the MODERN stack
does; a pixel-threshold fake in the classic mode only reproduces the "curtain over standing
water" failure. The classic mode's honest ceiling is A + C: the band thins with wind and the
patches breathe. Anything more is the modern surface's job.

## Status

- A: implemented (shader include + injection + config/JSON/UI plumbing), default OFF.
- B: implemented twice (cosine, then crest+trail), rejected twice, DELETED.
- C: implemented, default OFF; A + C confirmed working by the user.
- D: rejected for legacy; future modern-surface topic.
