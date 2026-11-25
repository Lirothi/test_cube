#pragma once

#include <cstdint>

#include "core/math/Math.h"

// Moved from app/scene/SceneFrameData.h (architecture plan R1.5).

// P6B screen-space ambient occlusion. Disabled by default: it is a real image change and the plan
// requires an explicit A/B before it becomes the default.
struct GtaoSettings
{
    bool enabled = false;
    // WORLD units. A pixel radius would make the occlusion grow and shrink as the camera moves,
    // which reads as the whole scene breathing.
    float worldRadius = 0.75f;
    // 0 = every horizon is an infinitely thin wall, 1 = fully solid. Foliage needs this well below
    // 1 or each leaf casts a slab of shadow behind it.
    float thickness = 0.6f;
    float intensity = 1.0f;
    float fadeStart = 60.0f;
    float fadeEnd = 120.0f;
    uint32_t numAngles = 2u;
    uint32_t numSteps = 6u;
    // Where the surface normal comes from. FALSE = rebuilt from depth, which is UE's default
    // (r.GTAO.UseNormals = 0) and the only self-consistent choice: the horizon search walks the
    // DEPTH buffer, so feeding the integral a normal-mapped normal describes a surface the search
    // never saw, and every detail-mapped texel loses part of its hemisphere to "below the surface".
    bool useGBufferNormal = false;
    // P6C: walk the depth pyramid instead of flat depth. Default ON once measured; the switch stays
    // so the two can be compared in one binary.
    bool useHzb = true;
    // Added to every step's mip. UE tie this to their quality level: 2 at their lowest (4 taps),
    // 1 at 6 taps, 0 from 8 taps up. Higher = cheaper and blurrier.
    uint32_t hzbMipBias = 0u;

    // --- P16.4: the SECOND radius, the one that occludes the sky fill.
    //
    // `worldRadius` above is a CONTACT radius and has to stay one -- it is what makes a trunk meet
    // the sand. But the sky is a hemisphere-sized source, and whether a patch of ground is under a
    // canopy or inside a doorway is decided at TENS of metres. With one radius the two questions
    // share an answer and the second one is always "unoccluded": measured on the atoll, raising the
    // sky 6x brightened dense canopy 1.92 stops and open sand 1.88 -- the crown was worth 0.04
    // stops. This is the same split UE make as SSAO + DFAO and Godot as SSAO + SDFGI.
    //
    // <= worldRadius switches the second horizon walk off and copies the contact answer into both
    // channels, which is an exact no-op for both consumers -- so this doubles as the A/B switch.
    float skyRadius = 25.0f;
    // Mip bias for the sky walk only (the contact walk keeps `hzbMipBias`). Its taps are tens of
    // pixels apart, so a mip-0 fetch lands nowhere near the previous one; a coarse level both
    // caches better and AGGREGATES, which is the right answer at a scale where a single texel of
    // leaf is not what decides whether the ground is sheltered.
    uint32_t skyMipBias = 2u;
    // Mid-range intensity: scales the sky channel's darkening exponent independently of the
    // contact channel (1 = the shared-`intensity` behaviour). 0 switches the sky walk's compute
    // path off entirely in the kernel -- the same exact-no-op dead branch as skyRadius <=
    // worldRadius -- so it doubles as the dedicated off switch.
    float skyIntensity = 1.0f;

    // --- items 3-5: the filter chain. Each stage is separately switchable so the A/B the plan
    // asks for can isolate one at a time; the chain always ends in the render-resolution target.
    bool denoise = true;
    bool temporal = true;
    // Taps each side of the bilateral kernel; 2 = the 5x5 that is UE's default
    // (r.GTAO.FilterWidth = 5). 0 makes it a pass-through copy.
    uint32_t filterRadius = 2u;
    // WORLD metres a tap may sit off the plane fitted through the centre pixel's depth gradient
    // before it stops counting. Too small and a grazing floor stays noisy; too large and the
    // filter blurs over the contacts the pass exists to find.
    float filterPlaneTolerance = 0.05f;
    // Weight of the CURRENT frame in the temporal blend, i.e. roughly a 10-frame history.
    // UE's own default: `AmbientOcclusionTemporalBlendWeight = 0.1f` (Engine/Private/Scene.cpp),
    // clamped there to [0.01, 1] with a UI maximum of 0.5.
    float temporalBlendWeight = 0.1f;
    // How far the history may sit from this frame's estimate before it is clamped, with a still
    // camera (it closes to 0 as the camera moves). UE hardcode 0.1; 0.35 here is MEASURED, not
    // preferred: their AO does not sit behind a DLSS jitter that moves the depth buffer every
    // frame, so our per-frame spread exceeds their window and the history was being clamped back
    // onto each noisy frame instead of accumulating. Distant flicker, static camera, frozen wind:
    // 0.1 -> 4.600, 0.35 -> 2.935, 1.0 -> 2.501 (8-bit). 1.0 removes the clamp for no real gain.
    float temporalClampRange = 0.35f;
    // Depth tolerance for the edge-aware upsample, RELATIVE to the destination pixel's depth.
    float upsampleTolerance = 0.02f;

    // --- items 6-7: consumption. `strength` is UE's AmbientOcclusionStaticFraction (their default
    // is 1.0): the combined term is lerp(1, materialAO * dynamicAO, strength), so 0 is an exact
    // no-op and the knob is a clean A/B without touching the pass itself.
    float strength = 1.0f;
};
