#pragma once

#include <string>
#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "app/scene/SceneView.h"
#include "app/scene/SceneRenderConfig.h" // S8: CascadeShadowConfig, read by the lighting/glass CBs
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

// The clamped numbers the shaders actually receive. Two passes trace with this tracer now -- the
// deferred SSR dispatch and the ocean's planar reflection -- and a second copy of these clamps is
// a second thing to drift, so the resolve lives beside the settings it resolves.
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

// SSRTReflections.usf collapses the whole multi-ray budget into ONE mirror ray below roughness 0.1
// and caps that ray at 24 steps. A reflector that IS a plane is always that case, so a pass which
// traces a plane resolves the rule here instead of carrying a ray count it can never use into the
// shader. Same arithmetic as the roughness branch in ssr_cs.hlsl -- keep the two in step.
inline uint32_t UeSsrMirrorRaySteps(const ResolvedUeSsrSettings& r)
{
    return (r.glossyRays != 0u && r.numRays > 1u) ? std::min(r.numSteps * r.numRays, 24u)
                                                  : r.numSteps;
}

// P7 -- global analytic aerial perspective. SHIPS DISABLED: it is a real image change, and this
// plan's own rule (the one GTAO and the P3B local exposure already follow) is that such a change
// earns its default with an explicit A/B rather than arriving switched on. With `enabled` false the
// density reaching the shader is 0 and compose skips the whole block, which is the interface
// contract's "screenshot-equivalent to M2".
struct AtmosphereSettings
{
    bool enabled = false;
    // Extinction per world unit AT `referenceHeight`. The defaults below are a starting point for
    // tuning on the atoll, NOT a measured result -- they have never been looked at with fog on.
    float density = 0.004f;
    // e-folding rate with world height. 0 makes the medium height-invariant (uniform distance fog).
    float heightFalloff = 0.02f;
    float referenceHeight = 0.0f;   // sea level in this project's scenes
    float startDistance = 25.0f;    // fog-free air, so near beach contrast survives
    float maxOpacity = 0.9f;        // distance never fully flattens shape
    float sunScatterStrength = 0.35f;
    // UE's DirectionalInscatteringExponent default, which is dimensionless and so transfers
    // unchanged (their density/falloff do not -- see the units note in atmosphere.hlsli).
    float sunScatterExponent = 4.0f;
    // UE keep the sun lobe out of the near field with a distance of its own
    // (DirectionalInscatteringStartDistance). Theirs is 10000 in a centimetre world.
    float sunScatterStartDistance = 100.0f;
    // How blurred the sky is when it is read as the FOG'S COLOUR, as a roughness fed to the same
    // IblSkyRadiance everything else uses. Only the LIGHTLY fogged end is blurred by it -- see
    // AtmosphereSkyRoughness in atmosphere.hlsli. 0 restores the original mip-0 read, which prints
    // the clouds onto whatever stands in front of them.
    float skyBlur = 0.5f;
    // The phase function, as "how bright the haze is with the sun BEHIND you" relative to looking
    // into it. Ships at 1.0 = flat = the pre-phase image, because it is a real look change and this
    // plan's rule is that those earn their default with an A/B. Below 1 the same air glows backlit
    // and stays dim front-lit, which is what real haze does -- and it is the RIGHT knob for that,
    // because a directional DENSITY would change extinction and make distant shapes fade in and out
    // as the camera pans.
    float skyBackScatter = 1.0f;
};

// P8 -- exposure-aware HDR bloom. SHIPS DISABLED, same rule the rest of this plan follows: a real
// image change earns its default with an explicit A/B rather than arriving switched on. With
// `enabled` false no pass is scheduled at all and no pyramid is touched, which is the plan's
// "intensity = 0 schedules no unnecessary active work".
struct BloomSettings
{
    bool enabled = false;
    // Weight of the bloom added back to the scene, in scene units. UE's BloomIntensity default is
    // 0.675 against their own extraction; ours is deliberately lower until it has been judged on the
    // canonical views, because the plan's own warning for this step is that ocean glints must not
    // turn the frame into a white fog bank.
    float intensity = 0.25f;
    // Luminance AFTER exposure at which bloom starts, in the units the viewer sees -- which is what
    // stops a darker scene from silently losing its bloom.
    //
    // NEGATIVE MEANS NO THRESHOLD AT ALL, and -1 is UE's own default
    // (`FPostProcessSettings::BloomThreshold`, Scene.cpp:423), described there as "all pixels affect
    // bloom equally (physically correct)". A lens scatters light from everything in front of it, so
    // thresholding is an artistic choice; it also makes bloom grow FASTER than exposure does,
    // because raising exposure both scales the pixels already over the line and pushes new ones
    // over it. At -1 the response is exactly linear in exposure.
    // SHIPPED POSITIVE, EVEN THOUGH -1 IS UE'S DEFAULT, and the reason is this project's CONTENT.
    // Measured on wind_test/sun_glint: the sun region is only about 20x the sky region, where a real
    // HDR sky puts the solar disc thousands of times above it. The sky fills half the frame, so at
    // -1 its TOTAL energy swamps the sun even though each of its pixels is dimmer -- the convolution
    // then reads as a uniform veil and the rays disappear (measured: veil on darks 8.3 vs lift near
    // the sun 6.6, i.e. the wrong way round). -1 is the physically correct setting and stays
    // available; it needs a sky with a real solar disc to look like anything.
    float threshold = 1.0f;
    // Slope of the ramp above the threshold. 0.5 is UE's hardwired value.
    float softKnee = 0.5f;
    // Tap spacing of the tent upsample, in destination texels. Wider = a broader, softer halo at the
    // same intensity; it does not change the total energy, only how far it is spread.
    float radius = 1.0f;
    // Karis average on the first downsample: weight each tap by 1/(1+luma) so a single blown-out
    // texel cannot dominate its tile. This is what keeps moving sun glints on water from pumping the
    // whole bloom, and it is why this defaults ON.
    bool fireflyClamp = true;

    // P8C: which method FILLS the bloom texture. The tonemap is unaware of this -- it samples one
    // texture either way, and `intensity` scales both.
    //   0 = the P8 pyramid: cheap, symmetric, and structurally incapable of a streak.
    //   1 = FFT convolution with a generated aperture: streaks, starburst and halo out of one
    //       kernel, at the cost of two transforms per frame.
    // Pyramid stays the default: convolution is heavier and a scene with no bright highlights
    // cannot tell them apart.
    uint32_t method = 0u;
    // P8C-2: the kernel is an IMAGE -- UE's photographed DefaultBloomKernel -- and these are its
    // placement controls. The generated-aperture controls (kernelRadius, the spoke set, chroma,
    // the anamorphic squeeze) died with the aperture: a 2-wavelength |FT{aperture}|^2 is
    // physically dashed, and the photograph carries real full-spectrum dispersion instead.
    //
    // Kernel width as a fraction of the viewport's major axis -- UE's BloomConvolutionSize, same
    // units and same default of 1.0 (the kernel's faintest tails reach the whole screen).
    // P8C-2r: WHICH kernel image. Any square FP16 DDS in textures/ will do -- the placement,
    // the clamp and the centre/scatter survey are all derived from the pixels, so swapping the
    // photograph swaps the entire look of the glare with nothing else to retune. UE's own
    // DefaultBloomKernel.dds ships beside the derived star and is one pick away.
    std::string convKernel = "textures/BloomKernelStar.dds";
    // P8C-6: a colour multiplier on the kernel IMAGE, applied where it is resampled into the grid.
    // It survives the DC divide: that normalises by the LARGEST channel sum, precisely so a
    // kernel's own colour balance is not washed out, so tinting the kernel tints the glare. This
    // is the runtime equivalent of re-authoring the photograph, and it costs nothing -- the kernel
    // spectrum is rebuilt only when its key moves, and the tint is part of that key.
    float convKernelTint[3] = { 1.0f, 1.0f, 1.0f };
    // P8C-5 -- UE'S BRIGHT-PIXEL GAIN, WHICH IS WHAT THEIR FFT BLOOM HAS INSTEAD OF A THRESHOLD.
    //
    // `BloomConvolutionPreFilterMin/Max/Mult`, applied in GPUFastFourierTransform.usf's FilterPixel
    // on the forward transform only:
    //
    //     if (Luma > Min) { Target = Mult * (Luma - Min) + Min; Target = min(Target, Max);
    //                       rgb *= Target / Luma; }
    //
    // Nothing is CUT. A pixel below Min passes through untouched -- which is why an open sun keeps
    // its glow no matter how the auto-exposure moves -- and a pixel above Min is REWEIGHTED, with
    // `Max` as a ceiling so a frame full of bright gaps cannot run away. That ceiling is the whole
    // reason one setting can serve a beach and a palm grove; a threshold has no such thing, it can
    // only decide membership, and both frames then move together (measured: at 5 both bloomed, at 6
    // both died).
    //
    // Mult <= 0 deactivates it, which is UE's own default state.
    float convPreFilterMin = 1.5f;
    float convPreFilterMax = 6.0f;
    float convPreFilterMult = 0.0f;
    float convSize = 1.0f;
    // Resolution of the convolution as a percent of the DISPLAY resolution -- UE's
    // r.Bloom.ScreenPercentage (their default is 100; 50 is this engine's grid ceiling, and going
    // below buys the transform cost back). The 640x360 grid the first P8C ran on is 12.5 here,
    // and it is what made a 1-2 texel ray a dashed line of squares.
    float convPercent = 50.0f;
    // The iris. Only the GHOST BOKEH SPRITE is built from these now -- the sprite is baked at
    // load from the blade controls, which is where they survived the aperture kernel's removal.
    // P8C-2d: `convBladeRotation` was REMOVED, not hidden. Measured against a two-run noise floor
    // (max 1/255) it moved the frame by max 4/255 in the most favourable configuration buildable
    // -- a triangular bokeh at 8% of frame width turned by 60 degrees. Not a bug: the bake really
    // does rotate the sprite (verified in numpy, the two textures differ by max 1.0), but a ghost
    // is the SUPERPOSITION of thousands of splats over an extended source, so it converges to
    // (source shape) convolved with (sprite) and the orientation of a convex near-symmetric
    // sprite washes out. It would only read on a source smaller than one scatter tile.
    uint32_t convBlades = 6u;
    // P8C-2 step 3b: the anamorphic streak, COMPOSITED INTO the kernel at build time (the stock
    // EXR is a spherical-lens kernel and carries none). Intensity is the FRACTION OF TOTAL KERNEL
    // ENERGY the streak carries -- 0 is exactly the stock kernel, and the DC-divide keeps total
    // bloom energy unchanged as it is dialled (it redistributes, never brightens). Length is the
    // streak's 1/e extent as a fraction of the screen width.
    float convAnamorphicIntensity = 0.0f;
    float convAnamorphicLength = 0.28f;
    // The band's final soft width in DISPLAY pixels (vertical Gaussian sigma at composite).
    float convAnamorphicWidth = 3.0f;
    // P8C-2h: the streak is an anisotropic PYRAMID (KinoStreak's structure) -- prefilter with a
    // soft-knee threshold, horizontal-only downsample chain, weighted upsample. NARROWING IS THE
    // THRESHOLD'S JOB: measured on a soft source, raising it took a 149-row corona to 77 rows,
    // and being pointwise it has no window to hang off a frame edge. `convAnamorphicNarrow` (a
    // vertical min-filter) was DELETED with the cascade -- a min cannot taper at a border, which
    // is what put first a fat band and then a straight horizontal cut across the streak.
    float convAnamorphicThreshold = 1.5f; // absolute units too -- see convGhostThreshold
    float convAnamorphicChroma = 0.5f;
    float convAnamorphicTint[3] = { 1.0f, 1.0f, 1.0f };
    // Lens ghosts, UE's mechanism (P8C-2 step 5): a bokeh SCATTER over the thresholded scene
    // builds the defocused image of every bright source, and the composite lays N copies of it
    // scaled about the screen centre. No sun position, no sprite atlas -- the sources' locations
    // are in the image. Default OFF until the visual sign-off.
    uint32_t convGhosts = 0u;
    // Sprite radius as a PERCENT of frame width -- UE's LensFlareBokehSize, default 3.
    float convGhostBokeh = 3.0f;
    float convGhostIntensity = 0.6f;
    // The scatter's OWN threshold -- UE's LensFlareThreshold, in ABSOLUTE units: authored as
    // stored brightness at EV100 = 14, rescaled by the frame's pre-exposure at dispatch (P8C-2c),
    // so the same source crosses it from any viewpoint. Ghosts are images of SOURCES; too low and
    // sunlit foliage or bright clouds become ones (both observed). On wind_test's scale (sky ~1,
    // corona 4-6, sun core 10-12) the default takes the core only: one clean disc ghost.
    float convGhostThreshold = 10.0f;
};

// P7 item 8. Deliberately NOT part of AtmosphereSettings: that struct is serialized into the level,
// and a debug view saved into a level is a trap -- the same reasoning that keeps
// ocean::g_foamDebugView out of OceanRenderConfig. 0 = normal, 1 = transmittance, 2 = in-scattering.
inline uint32_t g_atmosphereDebugView = 0u;

// The exact numbers the shaders receive. TWO passes apply aerial perspective -- compose, for opaque
// geometry, and the ocean's forward surface -- and the plan's own warning about this feature is that
// duplicated fog terms drift apart. So the packing lives here, once, and both callers use it.
// `hasSun` false zeroes the density: with no directional light there is nothing to colour the
// in-scattering with, and a fog that ignores that would tint the world with a stale sun.
struct AtmospherePacked
{
    float4 params0{}; // density, height falloff, reference height, start distance
    float4 params1{}; // max opacity, sun scatter strength, sun scatter exponent, sun scatter start
    float4 params2{}; // sky blur, sky back-scatter, zw reserved
};

inline AtmospherePacked PackAtmosphere(const AtmosphereSettings& a, bool hasSun)
{
    const bool on = a.enabled && hasSun && a.density > 0.0f;
    AtmospherePacked p{};
    p.params0 = float4(on ? std::max(a.density, 0.0f) : 0.0f, std::max(a.heightFalloff, 0.0f),
                       a.referenceHeight, std::max(a.startDistance, 0.0f));
    p.params1 = float4(std::clamp(a.maxOpacity, 0.0f, 1.0f), std::max(a.sunScatterStrength, 0.0f),
                       std::max(a.sunScatterExponent, 1.0f), std::max(a.sunScatterStartDistance, 0.0f));
    p.params2 = float4(std::clamp(a.skyBlur, 0.0f, 1.0f),
                       std::clamp(a.skyBackScatter, 0.0f, 1.0f), 0.0f, 0.0f);
    return p;
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

struct SceneRenderSettings
{
    GtaoSettings gtao{};
    AtmosphereSettings atmosphere{};
    BloomSettings bloom{};
    // STAYS LogMarch. The UE march is finished and correct after P13, and it is the cheaper search,
    // but on WATER the log march's dense mask is markedly the better picture -- and water is the
    // largest reflective surface in this project's scenes. The UE march is selectable everywhere
    // (`ssr.technique`), including the ocean plane; it is not the default. Do not flip this back on
    // the strength of the cost numbers alone: the comparison that decided it was the image.
    SsrTechnique ssrTechnique = SsrTechnique::LogMarch;
    UeSsrSettings ssrUe{};
    // RT foliage alpha test: on a FAILED alpha test the hit is still kept with this probability
    // (a FROZEN per-pixel dither -- static patterns measured strictly calmer than per-frame
    // re-rolls, see the frameSeed fill site). A 1-ray/px trace at reflection res undersamples
    // thin fronds and the reflected crown reads smaller than the real one; this inflates
    // coverage back. 0 = honest cutout, 1 = the old solid cards.
    float rtAlphaMissKeep = 0.15f;
    // RT foliage alpha mode. 0 = OFF: FORCE_OPAQUE traversal, foliage = solid cards, cheapest.
    // 1 = FIRST HIT: opaque traversal, then the committed hit is alpha-tested with the albedo
    //     sample shading fetches anyway -- transparent texels become misses (holes show the sky
    //     fallback, not what is truly behind); costs the same as OFF.
    // 2 = FULL: exact per-candidate alpha testing during traversal, the expensive one.
    uint32_t rtAlphaMode = 2u;
    // RW: wind-deformed per-instance BLASes for near casters, so foliage sway reaches RT
    // reflections (and later RT shadows). Off = every caster reflects its rest pose.
    bool rtWindBlas = true;
    // Casters inside this radius get a deformed BLAS (nearest-first, capped by the slot pool);
    // beyond it the sway is sub-pixel in a reflection and the shared static BLAS stands -- the
    // same near-only rule the VSM applies by keeping wind rigid beyond clipmap L2.
    float rtWindBlasRadius = 40.0f;
    // Reflection temporal resolve, BOTH sources. A screen-space ray is violently sensitive to its
    // own start, so under DLSS's per-frame jitter the raw buffer boils even with a still camera;
    // RT at half reflection res boils the same way once reflected foliage is subpixel. Unreal
    // never show theirs unfiltered either. Defaults ON -- see ssr_temporal_cs.hlsl.
    bool ssrTemporal = true;
    // UE's AA_LERP 8 for ETAAPassConfig::ScreenSpaceReflections: this frame is worth 1/8.
    float ssrTemporalBlendWeight = 0.125f;
    // How much the neighbourhood clamp box may widen when the camera is still (0 = never).
    float ssrTemporalClampExpand = 0.8f;
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
        float cascadeTexelWS[kCascades] = {}; // world size of one cascade texel (see Scene.cpp)
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

        // S11: the cascade's view-cone scissor in ATLAS texels (Scene::ComputeCascadeScissor).
        // Always computed, applied by Pass_CSM only while CascadeShadowConfig::scissorOptim is on,
        // so the readout can show what the optimisation WOULD cut before it is switched on.
        struct ScissorRect { std::int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0; };
        ScissorRect scissor[kCascades] = {};
        float scissorAreaDbg[kCascades] = {};    // rect area / content-rect area, 1 = nothing cut
    };

    const Camera* camera = nullptr;
    SceneView* mainView = nullptr;
    std::array<SceneView, kCascades>* cascadeViews = nullptr;
    std::array<SceneView, vsm::kNumClipmapLevels>* clipmapViews = nullptr; // Step 24d: directional clipmap (VSM)
    // The same levels expressed in the SHARED light frame, for the clipmap LOD fallback chain
    // (shaders/vsm_page_propagate_cs.hlsl). Null when the level has no valid sun.
    const vsm::ClipmapSquares* clipmapSquares = nullptr;
    // SMRT temporal dither phase, 1..64; 0 = temporal rotation off. Lives here so the lighting
    // pass and the glass pass rotate their sample sets IDENTICALLY -- two different phases would
    // make a window shade against a differently-sampled shadow than the ground beneath it.
    std::uint32_t smrtFrameIndex = 0;
    std::array<SceneView, LightManager::kMaxShadowedSpotLights>* spotShadowViews = nullptr;
    std::array<SceneView, LightManager::kMaxShadowedPointLights * 6>* pointShadowViews = nullptr;
    LightManager* lightManager = nullptr;
    Skybox* skybox = nullptr;
    const std::vector<std::unique_ptr<RenderableObjectBase>>* objects = nullptr;
    const DirectionalLight* dirLight = nullptr;
    // S8: the CSM knobs the lighting pass needs (receiver bias / sharpen / over-blur). Same object
    // the developer window edits, so a slider move reaches the shader the very next frame.
    const CascadeShadowConfig* cascadeConfig = nullptr;
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
