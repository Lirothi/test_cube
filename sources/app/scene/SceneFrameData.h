#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "app/scene/SceneView.h"
#include "rendering/core/PhotographicSettings.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/shadows/VirtualShadowMap.h" // vsm::kNumClipmapLevels (Step 24d)

class Camera;
class DirectionalLight;
class OceanRenderable;
class RenderableObjectBase;
class Skybox;
class ShadowGpuData;
class VirtualShadowMap;
namespace vfx { struct WindState; } // W3: global wind, read when building the gbuffer per-view CB

// The screen-space search a reflection ray uses. (The Lettier tracer was the third option and was
// removed with P6C step 6 -- it was a fixed-step screen-space march that LogMarch strictly
// dominates, and keeping a third code path alive made every SSR A/B a three-way.)
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
    UeSsrQualityPreset preset = UeSsrQualityPreset::High;
    uint32_t numSteps = 8u;
    uint32_t numRays = 4u;
    bool glossyRays = true;
    bool useSurfaceRoughness = true;
    float roughnessOverride = 0.0f;

    // These are hard-coded in stock UE. They are exposed here because they control the tradeoff
    // visible in grazing mirror views: coarse coverage versus exact silhouette ownership.
    float startMipLevel = 0.0f;
    float slopeCompareToleranceScale = 4.0f;

    // Our full-depth guard layered over UE's coarse HZB result. Zero retries is the unmodified UE
    // acceptance rule. Nonzero values refine a candidate against full-res depth and keep marching
    // after a rejected coarse hit instead of turning it into a permanent hole.
    uint32_t confirmRetries = 0u;
    uint32_t refineSteps = 4u;
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

// Where surface reflections come from (S8). None disables both traced/screen
// reflections and the skybox fallback; SkyOnly keeps only the skybox fallback;
// SSR is screen-space; RT is hardware ray-traced (Tier-1). RT is only honored when
// Renderer::IsRaytracingSupported() — otherwise the renderer falls back to SSR.
enum class ReflectionSource : uint32_t
{
    None = 0,
    SkyOnly = 1,
    SSR = 2,
    RT = 3,
    Count
};

// Debug/render toggles owned by the app layer (AppController maps input actions
// to these) and snapshotted into SceneFrameData each frame.
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

struct SceneRenderSettings
{
    GtaoSettings gtao{};
    SsrTechnique ssrTechnique = SsrTechnique::LogMarch;
    UeSsrSettings ssrUe{};
    // SSR temporal resolve. A screen-space ray is violently sensitive to its own start, so under
    // DLSS's per-frame jitter the raw buffer boils even with a still camera; Unreal never show
    // theirs unfiltered either. Defaults ON -- see ssr_temporal_cs.hlsl.
    bool ssrTemporal = true;
    // UE's AA_LERP 8 for ETAAPassConfig::ScreenSpaceReflections: this frame is worth 1/8.
    float ssrTemporalBlendWeight = 0.125f;
    // How much the neighbourhood clamp box may widen when the camera is still (0 = never).
    float ssrTemporalClampExpand = 0.5f;
    bool doFxaa = false;
    bool debugTexMode = false;
    // Which target the fullscreen debug blit shows. It used to be hardwired to the cascade shadow
    // atlas, which meant a half-res intermediate could only ever be judged by opening the texture
    // viewer in the GUI -- no headless capture, so no A/B and no gate. The blit shows .rrr, which
    // is what every one of these is.
    //   0 = cascade shadow atlas (what it always showed)
    //   1 = GTAO raw   2 = GTAO denoised   3 = GTAO temporal   4 = GTAO upsampled (render res)
    //   5 = HZB furthest (mip selected by debugTexMip)   6 = scene depth
    //   7 = HZB closest (debug/P9; UE SSR reads target 5, the furthest chain)
    //   8 = the reflection buffer's ALPHA, i.e. which pixels' rays found a hit
    int debugTexTarget = 0;
    // Mip shown for a target that has a chain (P6C HZB). Ignored by single-level targets.
    int debugTexMip = 0;
    bool showProfiler = false;
    // S8: the reflection source. Default RT (today's behavior). RT runs the
    // Tier-1 ray-traced pass instead of SSR; SkyOnly clears the reflection buffer
    // but keeps skybox specular; None disables both. RT auto-falls back to SSR on
    // non-RT hardware.
    ReflectionSource reflectionSource = ReflectionSource::RT;
    // S6: RT hit/visibility debug viz (dev tool). Traces a reflection ray per
    // pixel and writes a hit-distance/miss image into the reflection target (view via
    // the texture inspector -> Reflection). Builds the AS regardless of source.
    bool rtDebugView = false;
    // S16: glossy reflections. The reflection blur radius scales with the reflector's
    // roughness by this factor (in reflection-res pixels at full roughness); 0 = sharp
    // mirror reflections. Applies to both RT and SSR. Tunable in the developer window.
    float reflectionGlossyScale = 1.0f;
    // Analytic sun specular boost on metals. The sun's specular lobe is scaled by
    // (1 + metal*sunMetalSpecInfluence) in the lighting pass so a smooth metal shows a
    // distinct sun highlight (which otherwise merges into the environment reflection,
    // since the sun disk is not painted into the skybox). 0 = pure physical. Dev-tunable.
    float sunMetalSpecInfluence = 0.0f;
    // Analytic sun angular size (added to the GGX alpha for the sun only). Floors the
    // specular lobe width so smooth surfaces show a finite, bright sun glint instead of a
    // sub-pixel spike that never lands on a pixel. ~0.01 ≈ a few-pixel disk; 0 = punctual
    // (vanishing highlight on mirrors). Dev-tunable.
    float sunAngularSize = 0.01f;
};

// Per-frame inputs for the render passes. Scene::PrepareViews fills this once per
// frame; pass bodies read from it instead of reaching back into Scene members.
// Scene keeps ownership of objects, lights, and views — this struct only caches
// derived per-frame data (cascade matrices) and points at the rest.
struct SceneFrameData
{
    static constexpr int kCascades = 4;
    static constexpr std::size_t kMaxEditorSelection = 64;

    struct CascadeData
    {
        mat4 lightView[kCascades];
        mat4 lightProj[kCascades];
        float2 atlasScale[kCascades];
        float2 atlasBias[kCascades];
        float splitsVS[kCascades + 1] = {}; // near..far in view space
        float normalBiasWS[kCascades] = {};
        float depthBiasNDC[kCascades] = {};

        // S0.1: per-cascade diagnostics for the developer window. Written by Scene::UpdateCascades
        // straight from the values the cascade was actually built with (NOT recomputed by the UI —
        // a recomputed estimate would hide exactly the fit bugs this readout exists to catch), and
        // read only by DeveloperWindow. Nothing here reaches the GPU.
        float sphereRadiusDbg[kCascades] = {};   // fitted bounding-sphere radius, BEFORE padding
        float radiusDbg[kCascades] = {};         // radius after padding — drives unitsPerTexel
        float unitsPerTexelDbg[kCascades] = {};  // world units per shadow texel (the density metric)
        float nearLsDbg[kCascades] = {};         // light-space ortho near plane
        float farLsDbg[kCascades] = {};          // light-space ortho far plane
        std::uint32_t tileSizeDbg[kCascades] = {}; // atlas tile edge in texels
    };

    const Camera* camera = nullptr;
    SceneView* mainView = nullptr;
    std::array<SceneView, kCascades>* cascadeViews = nullptr;
    std::array<SceneView, vsm::kNumClipmapLevels>* clipmapViews = nullptr; // Step 24d: directional clipmap (VSM)
    std::array<SceneView, LightManager::kMaxShadowedSpotLights>* spotShadowViews = nullptr;
    std::array<SceneView, LightManager::kMaxShadowedPointLights * 6>* pointShadowViews = nullptr;
    LightManager* lightManager = nullptr;
    Skybox* skybox = nullptr;
    const std::vector<std::unique_ptr<RenderableObjectBase>>* objects = nullptr;
    const DirectionalLight* dirLight = nullptr;
    ShadowGpuData* shadowGpu = nullptr; // Rung 0: GPU-driven shadow cull inputs/outputs
    VirtualShadowMap* vsm = nullptr;    // Rung 2: page pool + page table (Step 18; unused yet)
    const vfx::WindState* wind = nullptr; // W3: global wind, folded into the gbuffer per-view CB
    // Water in the level, or null. The deferred lighting pass reads its caustics settings, clock
    // and flipbook; no ocean simply means no caustics.
    OceanRenderable* ocean = nullptr;

    CascadeData cascades{};
    std::array<std::uint64_t, kMaxEditorSelection> selectedEditorObjectIds{};
    std::uint32_t selectedEditorObjectCount = 0;
    std::uint32_t selectionOutlineRadius = 1;

    SceneRenderSettings settings{};
    // P2: the level's photographic camera settings, snapshotted with the rest of the frame so the
    // metering pass reads a value that cannot change under it mid-frame.
    render::CameraExposureSettings cameraExposure{};
    render::ColorPipelineSettings colorPipeline{};
};
