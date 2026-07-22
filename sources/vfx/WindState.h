#pragma once

#include <cmath>

#include "core/math/Math.h"

namespace vfx
{
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
    float gustMul = 1.0f;            // W6: gust-envelope multiplier (stays 1.0 until gusts land)

    // Advance the derived values. `clockSeconds` is the shared elapsed time — the ocean's
    // elapsedTime_ when the level has an ocean, otherwise a standalone monotonic accumulator
    // (coherence is moot without an ocean). Monotonic as long as the caller's clock is.
    void Tick(float clockSeconds)
    {
        prevTime = time;
        time = clockSeconds;

        const float rad = directionDeg * Math::DEG2RAD;
        windDirXZ = Math::float2(std::cos(rad), std::sin(rad)); // matches the ocean's degrees->vector

        swayAmplitude = foliageSwayMeters * strength;
        gustMul = 1.0f; // W6 replaces this with the gust envelope
    }
};

} // namespace vfx
