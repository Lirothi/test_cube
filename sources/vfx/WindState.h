#pragma once

#include <cmath>

#include "core/math/Math.h"

namespace vfx
{
// W8 — DEBUG TIME FREEZE. Not cosmetic: verifying this system is the hard part, and the substitute
// everyone reaches for (authoring `swayFrequency: 0` to get diffable screenshots) is a TRAP, because
// a static lean and a FROZEN SHADOW are pixel-identical — which is exactly how a VSM shadow-freeze
// bug shipped past its own verification pass. Freezing the CLOCK instead keeps every wind parameter
// at its authored value, so the pose is the real one, and `g_windStep` advances it by an EXACT
// amount: shoot, step 1.0 s, shoot again, and the two frames are reproducible to the pixel while the
// tree has provably moved. A shadow that did not move is then a diff, not a judgement call.
// Frozen time also freezes the gust envelope (it is a function of `time`), so the whole wind state is
// deterministic — nothing is left riding on wall-clock timing or frame rate.
inline bool  g_windFreeze     = false; // stop taking time from the shared clock
inline float g_windFrozenTime = 0.0f;  // the held time; tracks the live clock while not frozen, so
                                       // toggling freeze on holds the CURRENT pose instead of jumping
inline float g_windStep       = 0.0f;  // one-shot: added to the held time on the next Tick, then cleared

// W1 — the single global wind source of truth. A `wind` level entity (W2) authors these params; the
// Scene owns one WindState and advances its derived per-frame values every Tick from the SAME
// elapsed-time clock the ocean uses (OceanRenderable::elapsedTime_). Sharing one clock is what keeps
// waves, foliage sway, and gusts phase-coherent (see docs/wind_system_plan.md). No rendering here:
// W3 plumbs the derived values into the gbuffer/shadow per-view constant buffers.
struct WindState
{
    // ---- Authored params (populated from the level's "wind" section in W2; defaults = calm) ----
    bool  active            = false; // true only when the level has a "wind" section. Gates the ocean
                                     // push: no wind section => ocean keeps its own preset (back-compat),
                                     // distinct from an authored strength of 0 (which flattens the water).
    float directionDeg      = 0.0f;  // horizontal wind heading (deg); shared with the ocean wave direction
    float strength          = 0.0f;  // 0..1 master force. 0 = wind disabled (rigid foliage; ocean keeps its preset)
    float swayFrequency     = 0.9f;  // base foliage sway rate fed into the sway sines
    float gustAmplitude     = 0.5f;  // W6: gust-envelope depth (extra sway layered on the steady bend)
    float gustFrequencyHz   = 0.15f; // W6: gust-envelope rate
    float gustSeed          = 3.0f;  // W6: decorrelates the gust noise
    float foliageSwayMeters = 0.25f; // max horizontal canopy displacement at full strength (metres)

    // ---- Derived per-frame values (filled by Tick; consumed by W3's CB build) ----
    float time     = 0.0f;           // shared clock seconds (== ocean elapsedTime_ when an ocean exists)
    float prevTime = 0.0f;           // last frame's `time`; feeds the prev-position for motion vectors (W4)
    Math::float2 windDirXZ = Math::float2(1.0f, 0.0f); // unit heading; same convention as OceanSimulation::GetLocalWindDirectionVector
    float swayAmplitude = 0.0f;      // metres actually used by the shader = foliageSwayMeters * strength
    float gustMul = 1.0f;            // current smooth gust-envelope multiplier
    float prevGustMul = 1.0f;        // previous frame's multiplier (motion-vector correctness)

    // Smooth, deterministic, coherent gust noise in [0, 1]. The three incommensurate sine layers
    // keep the envelope from reading as a metronome while remaining continuous frame-to-frame.
    // `phaseCycles` is time * gustFrequencyHz; `seed` only shifts the phases.
    static float GustNoise(float phaseCycles, float seed)
    {
        constexpr float kTau = 6.28318530718f;
        const float n =
            0.52f * std::sin(kTau * (phaseCycles + seed * 0.1031f)) +
            0.31f * std::sin(kTau * (phaseCycles * 0.47f + seed * 0.2317f) + 0.7f) +
            0.17f * std::sin(kTau * (phaseCycles * 1.91f + seed * 0.0713f) + 2.1f);
        const float u = Math::Saturate(0.5f + 0.5f * n);
        return u * u * (3.0f - 2.0f * u); // smoothstep: longer relaxed/strong-gust shoulders
    }

    // Advance the derived values. `clockSeconds` is the shared elapsed time — the ocean's
    // elapsedTime_ when the level has an ocean, otherwise a standalone monotonic accumulator
    // (coherence is moot without an ocean). Monotonic as long as the caller's clock is.
    void Tick(float clockSeconds)
    {
        // W8: the freeze substitutes the clock and nothing else, so every authored parameter still
        // reads exactly as it does in-game. Tracking the live clock while unfrozen is what makes
        // toggling it hold the current pose rather than snap to some stale time.
        if (g_windFreeze)
        {
            g_windFrozenTime += g_windStep;
            g_windStep = 0.0f;
            clockSeconds = g_windFrozenTime;
        }
        else
        {
            g_windFrozenTime = clockSeconds;
        }

        prevTime = time;
        prevGustMul = gustMul;
        time = clockSeconds;

        // windDirXZ is the direction the wind BLOWS TOWARD, i.e. the direction the waves visibly
        // travel — which is the NEGATION of the ocean's (cos,sin) heading. Measured, not assumed:
        // the ocean spectrum peaks for wave vectors k along +(cos,sin), but the time evolution uses
        // Tessendorf's exp(+i*omega*t), so those waves propagate toward -k. Verified on screen at two
        // headings (dir 90 -> waves travel -Z; dir 0 -> waves travel -X). Without this negation the
        // foliage leans INTO the visible wind, 180 degrees off the water.
        // `directionDeg` itself is left untouched — it is shared verbatim with the ocean.
        const float rad = directionDeg * Math::DEG2RAD;
        windDirXZ = Math::float2(-std::cos(rad), -std::sin(rad));

        swayAmplitude = foliageSwayMeters * strength;
        const float gustAmp = gustAmplitude > 0.0f ? gustAmplitude : 0.0f;
        const float gustFreq = gustFrequencyHz > 0.0f ? gustFrequencyHz : 0.0f;
        gustMul = 1.0f + gustAmp * GustNoise(time * gustFreq, gustSeed);
    }

};

// (The W5 "max sway extent" caster-bounds pad used to live here. Removed 2026-08-20: shadow caster
// cull bounds now deliberately ignore the sway displacement — rationale + measured cost in
// ShadowGpuData::FillBounds and docs/bug_shadow_lod_bias_perf.md.)

// W8: the current wind push for anything that drifts but is not foliage — GPU particles today.
// windDirXZ * strength * gustMul, i.e. the visible travel direction (matching the water) scaled by
// the gust envelope, so smoke leans harder in a gust. Published by Scene::Tick alongside the sway
// extent; particle emitters have no Scene pointer, and this keeps them on the same wind and the same
// clock instead of sampling their own. 0 when the level has no wind.
inline Math::float2 g_windDriftXZ = Math::float2(0.0f, 0.0f);

// W8 distance fade. Sway is full-rate vertex ALU at any range, and far foliage swaying below a pixel
// is temporal noise for the upscaler to chase rather than motion anyone can see.
//
// DEFAULT OFF (end <= start), deliberately. Choosing a fade distance blind is a regression risk that
// outweighs a hazard nobody has actually observed here: an atoll vista legitimately shows palms
// hundreds of metres out, and freezing those is far more noticeable than the aliasing the fade is
// meant to prevent. Dial it in against a real vista — the Wind inspector has both sliders.
inline float g_windFadeStart = 0.0f;  // metres: full sway closer than this
inline float g_windFadeEnd   = 0.0f;  // metres: rigid beyond this (<= start disables the fade)
inline Math::float3 g_windFadeOriginWS = Math::float3(0.0f, 0.0f, 0.0f); // camera position

// THE single definition of the fade. Both the gbuffer and the shadow scale their windStrength by
// this, from the object's world origin — the same anchor the sway phase uses — so the two cannot
// drift apart and the shadow cannot detach. Never make this per-vertex or per-view: per-vertex tears
// the mesh, per-view (e.g. distance from the LIGHT) detaches the shadow.
inline float WindDistanceFade(const Math::float3& objectOriginWS)
{
    if (!(g_windFadeEnd > g_windFadeStart)) { return 1.0f; } // disabled
    const float dx = objectOriginWS.x - g_windFadeOriginWS.x;
    const float dy = objectOriginWS.y - g_windFadeOriginWS.y;
    const float dz = objectOriginWS.z - g_windFadeOriginWS.z;
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    return Math::Saturate((g_windFadeEnd - d) / (g_windFadeEnd - g_windFadeStart));
}

} // namespace vfx
