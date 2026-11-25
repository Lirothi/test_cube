#pragma once

#include <algorithm>
#include <cstdint>

// Moved from app/scene/SceneFrameData.h (architecture plan R1.5).

// The screen-space search a reflection ray uses. (The Lettier tracer was the third option and was
// removed with P6C step 6 -- it was a fixed-step screen-space march that LogMarch strictly
// dominates, and keeping a third code path alive made every SSR A/B a three-way.)
// DEFAULT IS LogMarch. The UeHzb path is a byte-for-byte port of SSRTReflections.usf (kept as
// the A/B reference), but its stock look lost the comparison: 8-16 coarse steps make the hit
// tolerance a whole fraction of the ray's depth span, which smears reflections into elongated
// streaks on oblique views -- the user checked the same scene in the UE editor and the artifact
// is authentically theirs. UE bury it under their main TAA + SSD denoiser and reach for Lumen/RT
// where it matters; this engine's LogMarch simply searches better.
enum class SsrTechnique : uint32_t
{
    LogMarch = 0,
    // Unreal's OWN SSR ray cast (SSRT/SSRTRayCast.ush): a fixed-step screen-space march reading
    // the FURTHEST pyramid at a fixed mip. Not the same thing as their TraceHZB, which is a Lumen
    // screen trace. Falls back to LogMarch on a frame where no pyramid was built.
    UeHzb = 1,
    Count
};

// The quality permutations in UE's SSRTReflections.usf. Custom is entered automatically when a
// developer edits the ray budget instead of selecting one of the source presets.
enum class UeSsrQualityPreset : uint32_t
{
    Custom = 0,
    Low = 1,     // 8 steps, 1 mirror ray
    Medium = 2,  // 16 steps, 1 mirror ray
    High = 3,    // 8 steps, 4 roughness-aware GGX rays
    Epic = 4,    // 12 steps, 12 roughness-aware GGX rays
    Count
};

struct UeSsrSettings
{
    // UE stock default: PostProcessSettings quality 50 + r.SSR.Quality 3 selects Medium.
    UeSsrQualityPreset preset = UeSsrQualityPreset::Medium;
    uint32_t numSteps = 8u;
    uint32_t numRays = 4u;
    bool glossyRays = true;
    bool useSurfaceRoughness = true;
    float roughnessOverride = 0.0f;

    // The two SSR knobs UE actually expose, from FPostProcessSettings with their stock defaults
    // (Scene.cpp: Intensity 100, MaxRoughness 0.6). StartMipLevel and the tolerance scale are
    // hardcoded 1.0/4.0 inside their RayCast() and are no longer knobs here; the full-depth
    // confirm/refine guard this engine used to layer on top is deleted -- byte-for-byte means
    // their acceptance rule and nothing else.
    float intensity = 1.0f;      // ScreenSpaceReflectionIntensity / 100
    float maxRoughness = 0.6f;   // ScreenSpaceReflectionMaxRoughness
};

inline void ApplyUeSsrQualityPreset(UeSsrSettings& settings, UeSsrQualityPreset preset)
{
    settings.preset = preset;
    switch (preset)
    {
    case UeSsrQualityPreset::Low:
        settings.numSteps = 8u;
        settings.numRays = 1u;
        settings.glossyRays = false;
        break;
    case UeSsrQualityPreset::Medium:
        settings.numSteps = 16u;
        settings.numRays = 1u;
        settings.glossyRays = false;
        break;
    case UeSsrQualityPreset::High:
        settings.numSteps = 8u;
        settings.numRays = 4u;
        settings.glossyRays = true;
        break;
    case UeSsrQualityPreset::Epic:
        settings.numSteps = 12u;
        settings.numRays = 12u;
        settings.glossyRays = true;
        break;
    case UeSsrQualityPreset::Custom:
    case UeSsrQualityPreset::Count:
        break;
    }
}

// The clamped numbers the shaders actually receive. Three dispatches trace with this tracer -- the
// deferred SSR, the glass and the water (Main_WaterReflection) -- and a second copy of these clamps
// is a second thing to drift, so the resolve lives beside the settings it resolves.
// Deliberately carries NO defaults: ResolveUeSsrSettings assigns every field, and a second set of
// initializers here would be a second place for a default to drift out of step with UeSsrSettings
// above -- which is the only place a default actually lives.
struct ResolvedUeSsrSettings
{
    uint32_t numSteps;
    uint32_t numRays;
    uint32_t glossyRays;
    float intensity;
    // SSRParams.g: ComputeSSRParams derives -2/MaxRoughness, doubled below the High tier.
    float roughnessMaskScale;
};

inline ResolvedUeSsrSettings ResolveUeSsrSettings(const UeSsrSettings& s)
{
    ResolvedUeSsrSettings r{};
    // The march consumes its samples in batches of four, so a budget that is not a multiple of
    // four would be rounded up inside the loop anyway. Round here, where it is visible.
    const uint32_t requestedSteps = std::clamp(s.numSteps, 4u, 64u);
    r.numSteps = std::min(64u, (requestedSteps + 3u) & ~3u);
    r.numRays = std::clamp(s.numRays, 1u, 12u);
    r.glossyRays = s.glossyRays ? 1u : 0u;
    r.intensity = std::clamp(s.intensity, 0.0f, 1.0f);
    // ComputeSSRParams: RoughnessMaskScale = -2/MaxRoughness, times 2 below ESSRQuality::High.
    // The engine's tier signal for "below High" is a preset without GGX rays.
    const float maxRoughness = std::clamp(s.maxRoughness, 0.01f, 1.0f);
    r.roughnessMaskScale = (-2.0f / maxRoughness) * (s.glossyRays ? 1.0f : 2.0f);
    return r;
}
