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
#include "rendering/lighting/SkyAtmosphereSettings.h"
#include "rendering/lighting/TimeOfDaySettings.h" // N1: SkyMoonFrame
#include "rendering/lighting/VolumetricCloudSettings.h"
#include "rendering/shadows/VirtualShadowMap.h" // vsm::kNumClipmapLevels (Step 24d)

class Camera;
class DirectionalLight;
namespace render { class LocalShadowCache; }
class OceanRenderable;
class RenderableObjectBase;
class Skybox;
class ShadowGpuData;
class VirtualShadowMap;
class SdsmShadows;
namespace vfx { struct WindState; } // W3: global wind, read when building the gbuffer per-view CB
namespace vis { struct OcclusionQueryPlan; class OcclusionQueryHeap; class HzbOcclusionTester; } // occlusion plan S3a / S3b

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

// P7 -- global analytic aerial perspective. SHIPS DISABLED: it is a real image change, and this
// plan's own rule (the one GTAO and the P3B local exposure already follow) is that such a change
// earns its default with an explicit A/B rather than arriving switched on. With `enabled` false the
// density reaching the shader is 0 and compose skips the whole block, which is the interface
// contract's "screenshot-equivalent to M2".
struct HeightFogSettings
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
    // unchanged (their density/falloff do not -- see the units note in height_fog.hlsli).
    float sunScatterExponent = 4.0f;
    // UE keep the sun lobe out of the near field with a distance of its own
    // (DirectionalInscatteringStartDistance). Theirs is 10000 in a centimetre world.
    float sunScatterStartDistance = 100.0f;
    // How blurred the sky is when it is read as the FOG'S COLOUR, as a roughness fed to the same
    // IblSkyRadiance everything else uses. Only the LIGHTLY fogged end is blurred by it -- see
    // HeightFogSkyRoughness in height_fog.hlsli. 0 restores the original mip-0 read, which prints
    // the clouds onto whatever stands in front of them.
    float skyBlur = 0.5f;
    // The phase function, as "how bright the haze is with the sun BEHIND you" relative to looking
    // into it. Ships at 1.0 = flat = the pre-phase image, because it is a real look change and this
    // plan's rule is that those earn their default with an A/B. Below 1 the same air glows backlit
    // and stays dim front-lit, which is what real haze does -- and it is the RIGHT knob for that,
    // because a directional DENSITY would change extinction and make distant shapes fade in and out
    // as the camera pans.
    // WHERE the sky sample stops being directional. UE blend their fog cubemap from the top mip
    // (NonDirectionalColor, the sphere average) into mip 0 (DirectionalColor) across
    // [NonDirectionalInscatteringColorDistance, FullyDirectionalInscatteringColorDistance]
    // (HeightFogCommon.ush:148-171, FogRendering.cpp:273,284). Their defaults are 1000 and 100000
    // in a centimetre world = 10 m and 1000 m, which transfer unchanged because they are lengths.
    //
    // SHIPS OFF (both 0 => the fade is a no-op and the sample is always directional), and it has to,
    // because their structure is not ours. UE MULTIPLY the cubemap into an authored fog colour
    // (Inscattering = FogColor * lerp(NonDir, Dir, t)); we feed the sample straight into
    // HeightFogInscatter as the base. So their safe near-field average -- the whole sphere,
    // sun included -- becomes an injection of energy here, and with UE's own 10 m / 1000 m the open
    // water inside a kilometre went milk-white. Measured on the owner's HDRI camera, 2026-09-09.
    //
    // The knob stays because the reasoning behind it is sound: the fog's ray points at the SURFACE,
    // so up close it points steeply DOWN, where "the sky in that direction" means little. But the
    // near-field blur we already had (HeightFogSkyRoughness, bounded by HeightFogClampSkySample)
    // covers that, and turning this on is a look change that has to earn its default with an A/B --
    // the same rule every look change here ships under. It is NOT what removed the horizon band: a control
    // run with the fade off showed the band gone either way. That was the cube, not this.
    float nonDirectionalDistance = 0.0f;
    float fullyDirectionalDistance = 0.0f;

    // ---- Volumetric (froxel) fog, docs/volumetric_fog_sky_clouds_ssgi_plan.md part A. UE's
    // VolumetricFog on top of the analytic model above: inside `volumetricDistance` the air is a
    // lit, shadowed volume (light shafts through the palms); beyond it the analytic fog continues.
    // Off = the pre-plan image exactly (no pass, no volume). Defaults are UE's where dimensionless.
    bool volumetric = false;
    float volumetricDistance = 300.0f;  // m: the froxel volume's far plane (UE VolumetricFogDistance)
    float albedo = 1.0f;                // UE VolumetricFogAlbedo: scattering / extinction (white)
    float extinctionScale = 1.0f;       // UE VolumetricFogExtinctionScale
    float phaseG = 0.2f;                // UE VolumetricFogScatteringDistribution (Henyey-Greenstein g)
    float sunScatter = 1.0f;            // UE DirectionalLight VolumetricScatteringIntensity
    float skyScatter = 1.0f;            // UE SkyLight VolumetricScatteringIntensity
    float historyWeight = 0.9f;         // UE r.VolumetricFog.HistoryWeight
    bool temporal = true;               // UE r.VolumetricFog.TemporalReprojection (+ jitter)
    bool jitter = true;                 // UE r.VolumetricFog.Jitter (only with the history, else the cell centre)
    // Jittered samples per cell EVERY frame (1-4; UE take one and lean on TAA). 2 = the antithetic pair, which
    // cancels a linear gradient across the cell and stops a spot's tip flickering after the 0.9 history.
    int samplesPerCell = 2;
    bool conservativeDepth = true;      // UE r.VolumetricFog.ConservativeDepth: cells behind the tile's farthest surface are skipped
    bool localLights = true;            // spots and points light the volume (UE: every local light with VolumetricScatteringIntensity > 0)
    float localLightScatter = 1.0f;     // UE per-light VolumetricScatteringIntensity, one value for all local lights here
    // UE r.VolumetricFog.LightSoftFading: a spot cone edge fades over this many froxels (their default 0 = hard;
    // 1 is their own good starting point and the cure for a stepped, flickering cone edge -- ours ships at 1).
    float localSoftFading = 1.0f;
    // UE r.VolumetricFog.InverseSquaredLightDistanceBiasScale: the inverse-square falloff is biased by the cell
    // radius times this, so a cell that contains the source does not blow up (aliasing at the light).
    float localDistanceBias = 1.0f;
};

// P8 -- exposure-aware HDR bloom. SHIPS DISABLED, same rule the rest of this plan follows: a real
// image change earns its default with an explicit A/B rather than arriving switched on. With
// `enabled` false no pass is scheduled at all and no pyramid is touched, which is the plan's
// "intensity = 0 schedules no unnecessary active work".
struct BloomSettings
{
    bool enabled = false;
    // UE's BloomIntensity, default 0.675. For the convolution it is their ScatterDispersionIntensity
    // (ScatterDispersion and ConvolutionIntensity at their default 1): how much of the kernel's
    // scatter the light is moved into. For the pyramid it scales UE's six Gaussian weights (I / 6).
    // It also scales the ghosts, as UE's LensFlareIntensity * BloomIntensity.
    float intensity = 0.675f;
    // THE PYRAMID'S threshold only -- UE's FFT bloom has none, and neither does the convolution here
    // (its FilterPixel is convPreFilter*). Luminance in the units the viewer sees, as UE's
    // BloomThreshold.
    //
    // NEGATIVE MEANS NO THRESHOLD AT ALL, and -1 is UE's own default
    // (`FPostProcessSettings::BloomThreshold`, Scene.cpp:457), described there as "all pixels affect
    // bloom equally (physically correct)". A lens scatters light from everything in front of it, so
    // thresholding is an artistic choice; it also makes bloom grow FASTER than exposure does,
    // because raising exposure both scales the pixels already over the line and pushes new ones
    // over it. At -1 the response is exactly linear in exposure. (This shipped positive while the
    // sun's disc was clamped to ~20x the sky; the disc is physical now, thousands of times it.)
    float threshold = -1.0f;
    // Slope of the ramp above the threshold. 0.5 is UE's hardwired value.
    float softKnee = 0.5f;
    // Tap spacing of the tent upsample, in destination texels. Wider = a broader, softer halo at the
    // same intensity; it does not change the total energy, only how far it is spread.
    float radius = 1.0f;
    // Karis average on the pyramid's first downsample: weight each tap by 1/(1+luma) so a single
    // blown-out texel cannot dominate its tile. OURS -- UE's downsample chain has none
    // (PostProcessDownsample.usf), so it defaults OFF; on for moving glints that pump the bloom.
    bool fireflyClamp = false;

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
    // OURS, UE have no such control: a per-channel ScatterDispersion -- how strongly each channel
    // scatters, so a bluish tint gives a bluish flare and what a channel does not scatter stays in
    // the scene (BloomRenderer FinalizeApply). It cannot live in the kernel image any more: the FFT
    // normalises each channel by its own sum, as UE's does, which would cancel it.
    float convKernelTint[3] = { 1.0f, 1.0f, 1.0f };
    // UE'S BRIGHT-PIXEL GAIN, WHICH IS WHAT THEIR FFT BLOOM HAS INSTEAD OF A THRESHOLD.
    //
    // `BloomConvolutionPreFilterMin/Max/Mult`, applied in GPUFastFourierTransform.usf's FilterPixel
    // on the forward transform only, in the PRE-EXPOSED units the scene colour is stored in:
    //
    //     if (Luma > Min) { Target = Mult * (Luma - Min) + Min; Target = min(Target, Max);
    //                       rgb *= Target / Luma; }
    //
    // Nothing is CUT. A pixel below Min passes through untouched and a pixel above it is
    // REWEIGHTED, with `Max` as a ceiling so a frame full of bright gaps -- or a physical sun disc --
    // cannot run away. Active when Min < Max (UE's gate) and Mult > 0 (under theirs Mult 0 clamps
    // everything above Min to Min; here, as this engine's levels have always meant it, it is off).
    // UE's defaults 7 / 15000 / 15 (Scene.cpp:480-482).
    float convPreFilterMin = 7.0f;
    float convPreFilterMax = 15000.0f;
    float convPreFilterMult = 15.0f;
    float convSize = 1.0f;
    // Resolution of the convolution as a percent of the DISPLAY resolution -- UE's
    // r.Bloom.ScreenPercentage (their default is 100; 50 is this engine's grid ceiling, and going
    // below buys the transform cost back). The 640x360 grid the first P8C ran on is 12.5 here,
    // and it is what made a 1-2 texel ray a dashed line of squares.
    float convPercent = 50.0f;
    // Feed the convolution the scene BEFORE the upscaler (2026-10-15). DLSS treats a glint of a few
    // pixels as aliasing and flattens it: a chrome sphere's sun glint kept ~10 % of its energy (the
    // MCP pixel probe, before vs after DLSS), so the kernel's star never formed, while UE's TSR keeps
    // it. The grid is a fraction of the display either way and samples by UV, so the render-resolution
    // image only changes what reaches it. No effect without DLSS.
    bool convBeforeUpscale = true;
    // What the scene before the upscaler costs: it is JITTERED, so a glint under a pixel lands in it or
    // not frame by frame and the star on a chrome glint flickered 40-50 % frame to frame. The bloom's
    // light (the FFT, the ghosts, the streak) is therefore accumulated over frames first, reprojected by
    // the G-buffer velocity (bloom_conv_cs.hlsl stage 12) -- the averaging UE gets from TSR, whose output
    // their bloom reads. This frame's weight: 1 = no accumulation.
    float convTemporalWeight = 0.1f;
    // ART CONTROLS, not physics (owner, 2026-10-15: "dim the sun's kernel while raising it for the rest").
    // The sun disc carries thousands of times a glint's light, so with one Intensity its star and ghosts
    // dominate while a glint's barely show. These scale the DISC's own share -- its tiles of the
    // convolution's input after FilterPixel, its tiles of the ghost scatter -- found by direction (the
    // disc and 1.25-2 radii round it), so Intensity / Ghost Intensity can be raised for everything and
    // the sun held back. 1 = the sun like any other source.
    float convSunKernelScale = 1.0f;
    float convSunGhostScale = 1.0f;
    // What "the sun" is to those two: the SKY within this many degrees of it (full to half of it). The
    // disc alone is not enough -- the bright sky round it makes as much of the halo and the ghosts --
    // and geometry never counts, so a glint next to the sun on screen keeps the common share. At least
    // the disc's own diameter.
    float convSunSplitRadiusDeg = 8.0f;
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
    float convAnamorphicThreshold = 1.5f; // pre-exposed units, as convGhostThreshold
    float convAnamorphicChroma = 0.5f;
    float convAnamorphicTint[3] = { 1.0f, 1.0f, 1.0f };
    // Lens ghosts, UE's mechanism (P8C-2 step 5): a bokeh SCATTER over the thresholded scene
    // builds the defocused image of every bright source, and the composite lays N copies of it
    // scaled about the screen centre. No sun position, no sprite atlas -- the sources' locations
    // are in the image. Default OFF until the visual sign-off.
    uint32_t convGhosts = 0u;
    // Sprite radius as a PERCENT of frame width -- UE's LensFlareBokehSize, default 3.
    float convGhostBokeh = 3.0f;
    // UE's LensFlareIntensity, default 1; the ghosts get it times `intensity` (BloomIntensity).
    float convGhostIntensity = 1.0f;
    // UE's LensFlareThreshold, default 8: a BINARY gate on dot(rgb, 1) of the pre-exposed scene
    // (LensFlareBlurVS). Ghosts are images of SOURCES; the physical sun disc clears it by thousands.
    float convGhostThreshold = 8.0f;
    // FAKE SUN GLARE -- the corona the convolution does not give (its kernel is the size of the
    // bloom image's percent and the photographed star has no wide veil). An analytic halo about
    // the sun's direction: an exponential core plus a wide veil, both in DEGREES, so its size does
    // not follow the resolution or the field of view. Its colour and strength are the frame's own
    // pixels over the sun disc, so whatever hides or tints the sun (a palm, a cloud, a sunset)
    // hides or tints the glare. Runs with either bloom method, into the same target.
    // The SHARE OF THE DISC'S LIGHT in the glare (BloomRenderer::SunGlareScale): the disc's radiance
    // times its solid angle, spread over the profile's integral. 0 = off. 0.005 matched the pre-
    // physical-disc look on wind_test (three views against the d1b6183 build, within 2/255).
    float sunGlareIntensity = 0.005f;
    float sunGlareRadiusDeg = 2.0f;      // the core's e-folding angle
    float sunGlareVeil = 0.15f;          // the wide veil's weight, relative to the core's peak
    float sunGlareVeilRadiusDeg = 12.0f; // the veil's half-strength angle
    // The glare on a LOW sun ("на низком угле солнца надо гасить sun glare"): full strength above
    // sunGlareLowSunDeg of elevation, easing (smoothstep) down to sunGlareLowSunFloor of itself at
    // the horizon. The exposure opens up as the sun sets, so a glare sized for noon washed a whole
    // sunset frame white (wind_test at 0.5-1.5 degrees). 12 degrees leaves every shipped level as it
    // was: the lowest sun among them is demo's 13.5.
    float sunGlareLowSunDeg = 12.0f;
    float sunGlareLowSunFloor = 0.2f;
    // The time constant (seconds) of the sun probe's temporal blend -- what the glare AND the corona read
    // of the disc (bloom_conv_cs.hlsl stage 10). Leaves crossing the disc made both dance (owner,
    // 2026-10-14); 0 = this frame's taps as they are. Measured on the owner's palm view (wind live, 24
    // frames 0.05 s apart, mean frame-to-frame change 40-120 px round the sun): 0 -> 3.5 %, 0.12 -> no
    // better, 0.5 -> 2.3 % against the swaying leaves' own 1.9 % -- but 0.5 was too slow ("the corona
    // fades on a disc already covered"); with the corona pinned to the disc's centre and its hole gone
    // (tonemap_cs.hlsl SunRays), only its strength follows the leaves.
    float sunGlareSmoothingSec = 0.1f;
    // THE SUN'S CORONA, drawn by the tone curve pass at output resolution (tonemap_cs.hlsl SunRays),
    // after Ritschel's "Temporal Glare": dense bright BUNDLES with dim gaps between them, fine
    // NEEDLES inside them, a power-law fall-off with warm tips, a faint lenticular HALO ring, and an
    // optional regular star. Same colour source as the glare: the frame's pixels over the sun disc.
    // Scaled by the disc's ENERGY (its solid angle over the corona's nominal area, 2 pi length^2 --
    // BloomRenderer::SunRays), so it follows the disc's light rather than its radiance. 0 = off.
    // 0.008 matched the pre-physical-disc look, as sunGlareIntensity's 0.005.
    float sunRaysIntensity = 0.008f;
    uint32_t sunRaysCount = 100u;        // needles around the full circle
    uint32_t sunRaysBundles = 12u;       // bright bundles around the full circle
    float sunRaysBundleWidthDeg = 5.0f;  // a bundle's FWHM as an ANGLE around the sun (at most ~its spacing)
    // How much the bundles rule: 0 = needles evenly round the circle and all alike; 1 = every needle
    // gathered into a bundle, dark between them. Brightens and gathers together, so the light stays
    // about the same.
    float sunRaysBundleStrength = 0.5f;
    float sunRaysLengthDeg = 2.5f;       // the power law's scale: where a needle is at half strength
    // A needle's width on the sky, full width at half maximum. Degrees, so the same at any
    // resolution: 0.15 is ~3.3 px at 1440p and a 90-degree FOV, ~5 px at 4K -- a hundred needles
    // that wide read as a full corona where pixel-thin ones needed hundreds. A needle is never
    // drawn thinner than a pixel: below that it gets FAINTER, as a thinner line would look.
    float sunRaysWidthDeg = 0.15f;
    float sunRaysHalo = 0.08f;           // the lenticular halo ring's weight (0 = none)
    float sunRaysHaloRadiusDeg = 3.5f;   // its radius
    // IMAGE MODE: a corona painted as a texture (grayscale on black, for additive use) laid about
    // the sun instead of the procedural one -- one fetch a pixel, the look of the picture. Empty =
    // procedural, the default: the owner preferred the procedural bundles over astra6's
    // sun_corona_v2.png (2026-09-25, "твои лучи прикольнее"), which stays one pick away.
    std::string sunRaysTexture;
    float sunRaysTextureSizeDeg = 15.0f; // the image's half-width on the sky
    float sunRaysSpikes = 0.0f;          // weight of the regular star (0 = none)
    uint32_t sunRaysSpikeCount = 6u;     // the star's spikes
    float sunRaysSpikeWidthDeg = 0.1f;   // a spike's width, as sunRaysWidthDeg (was a fixed 1.5 px)
    float sunRaysSpikeLengthDeg = 10.0f; // where a spike is at half (was 4x the corona length: 10 at its default)
    float sunRaysRotationDeg = 0.0f;     // turns the pattern on screen
};

// P7 item 8. Deliberately NOT part of HeightFogSettings: that struct is serialized into the level,
// and a debug view saved into a level is a trap -- the same reasoning that keeps
// ocean::g_foamDebugView out of OceanRenderConfig. 0 = normal, 1 = transmittance, 2 = in-scattering.
inline uint32_t g_fogDebugView = 0u;

// The exact numbers the shaders receive. TWO passes apply aerial perspective -- compose, for opaque
// geometry, and the ocean's forward surface -- and the plan's own warning about this feature is that
// duplicated fog terms drift apart. So the packing lives here, once, and both callers use it.
// `hasSun` false zeroes the density: with no directional light there is nothing to colour the
// in-scattering with, and a fog that ignores that would tint the world with a stale sun.
struct HeightFogPacked
{
    float4 params0{}; // density, height falloff, reference height, start distance
    float4 params1{}; // max opacity, sun scatter strength, sun scatter exponent, sun scatter start
    float4 params2{}; // sky blur, sky back-scatter, directional-fade inverse range, its bias
};

inline HeightFogPacked PackHeightFog(const HeightFogSettings& a, bool hasSun)
{
    const bool on = a.enabled && hasSun && a.density > 0.0f;
    HeightFogPacked p{};
    p.params0 = float4(on ? std::max(a.density, 0.0f) : 0.0f, std::max(a.heightFalloff, 0.0f),
                       a.referenceHeight, std::max(a.startDistance, 0.0f));
    p.params1 = float4(std::clamp(a.maxOpacity, 0.0f, 1.0f), std::max(a.sunScatterStrength, 0.0f),
                       std::max(a.sunScatterExponent, 1.0f), std::max(a.sunScatterStartDistance, 0.0f));
    // UE FogRendering.cpp:273,284: InvRange over the gap, and the bias that makes
    // saturate(rayLength * zw.x + zw.y) reach 0 at nonDirectional and 1 at fullyDirectional.
    const float nonDir = std::max(a.nonDirectionalDistance, 0.0f);
    const float invRange = 1.0f / std::max(a.fullyDirectionalDistance - nonDir, 1.0e-5f);
    // params2.y is RESERVED: it carried `skyBackScatter` until the phase was found to be a dimmer
    // rather than a phase (height_fog.hlsli). Left as 0 rather than repacked, so every consumer's
    // swizzle keeps meaning what it means.
    p.params2 = float4(std::clamp(a.skyBlur, 0.0f, 1.0f), 0.0f, invRange, -nonDir * invRange);
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
    HeightFogSettings heightFog{};
    SkyAtmosphereSettings skyAtmosphere{};
    VolumetricCloudSettings volumetricCloud{}; // plan part C; level-authored like the sky
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
    // Interior lighting plan S: the RT sky visibility (UE DFAO's bent normal, traced on the TLAS at
    // half resolution, rt_sky_visibility_cs.hlsl) occluding the sky's diffuse and specular. Needs RT
    // hardware; off = the open sky everywhere, exactly the frame before it. `skyVisRays` per
    // half-resolution pixel per frame, accumulated with `skyVisHistory` (the weight of the
    // reprojected past); `skyVisMaxDistance` metres of ray -- an occluder farther than this does not
    // count (UE's SkyLight OcclusionMaxDistance, 10 m by default; ours reaches over a palm crown).
    // `skyVisDiffuse` / `skyVisSpecular` scale the two effects (0 off .. 1 UE).
    // Measured 2026-10-23 (wind_test grove, half of 2560x1440, GPU): 4 rays 0.28 / 0.31 / 0.34 ms at
    // 10 / 15 / 30 m, 8 rays 0.63 ms, 2 rays 0.18 ms -- the cost is the ray count, not the reach.
    // 8 rays and 0.98 since 2026-10-26 (owner: "what makes the noise here", a tunnel he built of boxes to watch the
    // light inside): a floor that sees the sky only through the tunnel's mouth gets it from a ray in a few, frame to
    // frame median 8.6 % (p90 12.3) at 4 rays / 0.95; 8 rays 5.6 %, 0.98 5.2 %, both 3.5 % (p90 5.1); SkyVis off
    // 0.2 % but the floor then takes the whole sky. 0.14 -> 0.25 ms there. The longer history costs the newly
    // uncovered surfaces of a moving camera a slower settle.
    bool skyVisEnabled = true;
    uint32_t skyVisRays = 8u;
    float skyVisMaxDistance = 15.0f;
    // With the spatial filter below (2026-10-24): wind_test's second room at 24:00, a wall lit by its window's
    // moonlit sky, per-pixel temporal noise over 12 shots median 9.4 % -> 3.2 % at 0.95 (p90 18 -> 5.7 %); 0.97
    // 2.8 %, 0.97 with 8 rays 2.5 % (+0.075 ms); SkyVis off 0.35 %. scratchpad noise_map.py.
    float skyVisHistory = 0.98f;
    float skyVisDiffuse = 1.0f;
    float skyVisSpecular = 1.0f;
    // The spatial half of the denoise (rt_sky_visibility_filter_cs.hlsl, UE DFAO's spatial filter): a 5x5
    // half-resolution gather of the accumulation, a tap counting while its relative depth step is under
    // `skyVisFilterDepth` and by its normal's agreement to the `skyVisFilterNormalPower`. Off = the accumulation.
    bool skyVisFilter = true;
    float skyVisFilterDepth = 0.05f;
    float skyVisFilterNormalPower = 8.0f;
    // 1 = paint the texels that had no history this frame as the open sky (bright wherever the sky is shut out):
    // what the temporal accumulation rejects -- a disocclusion, the screen's edge, a failed same-surface test.
    uint32_t skyVisDebug = 0u;
    // A texel without history is seeded from the neighbours that kept theirs (UE DFAO FilterHistoryPS). Off = it
    // starts from its own few rays, the behaviour before 2026-10-26 -- kept for the A/B (`--set skyVis.seed:0`).
    bool skyVisSeed = true;
    // Interior lighting plan D: the GI probe volume (DDGI over RayQuery, rendering/lighting/DdgiVolume.h):
    // 32x16x32 probes `giSpacing` m apart around the camera, 1 / `giUpdateDivisor` of them tracing 64
    // rays a frame and blending them in with `giHysteresis`. They hold the BOUNCE only (a miss brings
    // nothing; the sky is plan S's), which lighting_cs adds to the indirect diffuse x `giIntensity`
    // (1 = physical). `giRayDistance` m of ray; `giInfiniteBounce` feeds the volume back into its own
    // hits; `giDebug` 1 = the lit frame shows the bounce alone, 2 = the probes around each point (red moved by
    // their last update, green relocated off their grid point, blue inside geometry; ddgi_common.hlsli
    // DdgiProbeStates). Needs RT hardware; off = no bounce.
    bool giEnabled = true;
    float giSpacing = 0.5f;
    uint32_t giUpdateDivisor = 4u;
    // 0.98, not RTXGI's 0.97 at its 256 rays: 64 rays an update left the bathroom's walls and ceiling
    // breathing in metre-sized blotches (frame-to-frame std 0.68 sRGB levels at 0.9, 0.37 at 0.97, 0.29
    // at 0.98 -- 0.17 without GI; owner 2026-10-24 "dark spots dancing"). The divisor does not change the
    // noise (0.61 at 1/1), only how fast it decorrelates: 4 / (1 - 0.98) = 200 frames to settle.
    float giHysteresis = 0.98f;
    float giIntensity = 1.0f;
    float giRayDistance = 30.0f;
    bool giInfiniteBounce = true;
    // The FAR cascade: a second volume of the same 32x16x32 probes, `giFarSpacing` m apart, around the
    // near one, so the bounce reaches past +-8 m (64 x 32 x 64 m at 2 m). Lighting takes the near cascade
    // where it is whole and blends to the far one over the near's outer 2 m. It updates 1 / giFarDivisor
    // of its probes a frame, its own infinite bounce. Off = the near cascade alone, as before.
    bool giFarCascade = true;
    float giFarSpacing = 2.0f;
    uint32_t giFarDivisor = 4u;
    uint32_t giDebug = 0u;
    // Glass refraction (owner 2026-10-24: "the glasses pass the background straight through"): a
    // thinTranslucent slot bends what is behind it as a glass shell of the surface's own curvatures,
    // this thick under its surface (m), refracted in and out onto the opaque scene behind (glass.hlsl
    // GlassRefractedUv). 0 = straight through, the flat transmission before it; a few mm = a hollow
    // tumbler's wall (the_bathroom_free's are modelled 4.3 mm); at least the shape's radius = SOLID glass,
    // the default since the owner picked Sketchfab's look ("сделай чтобы как в скечфабе", 2026-10-24) --
    // its screen-space refraction bends every cup like a solid block. A pane is flat: no bend either way.
    float glassWallThickness = 0.5f;
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
    // The WATER reflection's own temporal resolve (Main_WaterReflection), separate from the one above.
    // Off by default (owner, 2026-10-10: "дико все мажет"): the resolve decides stillness from
    // gbVelocity, which under water is the SURFACE's motion -- ~0 with a still camera -- so the still
    // inertia relaxes the clamp and stretches the history ~4x while the waves change the reflection
    // every frame. Off = the trace's raw output; DLSS still accumulates the final image.
    bool waterReflectionTemporal = false;
    // Its own blend and still inertia (the deferred resolve's knobs above stay the deferred one's).
    // Inertia defaults to 0 for the reason above: the surface's own motion reads as "still", and the
    // inertia's relaxed clamp + longer history is exactly what smeared the waves.
    float waterReflectionTemporalBlend = 0.125f;
    float waterReflectionTemporalStillInertia = 0.0f;
    // EXPERIMENT (2026-10-12, UE's way): trace the water from its own reflection normal (GB1) and read
    // the result at the pixel itself, instead of a flat mirror broken up by the planar UV offset.
    // `--set=ssr.waterSurfaceNormal:1`, or `--sweep=ssr.waterExperiment:...` for the whole variant.
    bool waterReflectionSurfaceNormal = false;
    // How many FFT cascades (largest first) the water's reflection normal GB1 takes -- what the trace
    // above casts its rays about, and what the UV break-up reads when it is off. 4 = all of them, the
    // shading normal; 2 = the swell only (the owner's idea, 2026-10-12: the far ripples a pixel cannot
    // resolve sent neighbouring rays apart). The sky in the reflection keeps the full normal.
    // Handed to the ocean every frame (OceanRenderable::SetReflectionNormalCascades).
    std::uint32_t waterReflectionNormalCascades = 4u;
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
    // THE PIXEL PROBE (editor MCP get_readouts; `--set=debug.probeU/probeV/probeRadius`): the
    // brightest pre-exposed HDR pixel inside a window of `debugProbeRadiusPx` around (u, v) of the
    // frame the tonemap reads, the window's mean, a 4x4 box at the brightest pixel (what one texel of
    // the bloom's FFT grid at 25 % averages) and the brightest pixel after the local exposure the FFT
    // input gets -- read back with the exposure record (bloom_conv_cs.hlsl stage 11). It measures
    // what a screenshot cannot: the tone curve has already clipped a glint to white there.
    // Radius 0 = off, nothing is dispatched.
    float debugProbeU = 0.5f;
    float debugProbeV = 0.5f;
    float debugProbeRadiusPx = 0.0f;
    // Which frame the probe reads: 0 the one the tonemap and the bloom read (the upscaler's output
    // when DLSS runs), 1 the scene BEFORE the upscaler (render resolution; the same image without DLSS),
    // 2 the bloom target the tone curve adds (pre-exposed, its own resolution).
    std::uint32_t debugProbeSource = 0u;
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
    // mirror reflections. The SSR's only, since the RT reflection spreads over its GGX lobe
    // (reflectionLobeBlur). Tunable in the developer window.
    float reflectionGlossyScale = 1.0f;
    // The RT reflection's glossy filter (reflection_glossy_cs.hlsl): each pixel's sharp mirror reflection spread
    // over its GGX lobe as seen on the screen -- the hit distance against the reflector's, the roughness's lobe
    // width, the projection. Off = S16's roughness-scaled separable blur, as before 2026-10-26 (the A/B; --set
    // reflections.lobe:0).
    bool reflectionLobeBlur = true;
    // Analytic sun specular boost on metals. The sun's specular lobe is scaled by
    // (1 + metal*sunMetalSpecInfluence) in the lighting pass so a smooth metal shows a
    // distinct sun highlight (which otherwise merges into the environment reflection,
    // since the sun disk is not painted into the skybox). 0 = pure physical. Dev-tunable.
    float sunMetalSpecInfluence = 0.0f;
    // The sun's angular size is the SUN'S (DirectionalLight::GetSunHalfApexRadians, UE's
    // LightSourceAngle): it floors the analytic specular lobe AND draws the procedural disc, so it
    // lives on the level's light rather than here (part-B review, 2026-09-11).
};

// Per-frame inputs for the render passes. Scene::PrepareViews fills this once per
// frame; pass bodies read from it instead of reaching back into Scene members.
// Scene keeps ownership of objects, lights, and views — this struct only caches
// derived per-frame data (cascade matrices) and points at the rest.
struct SceneFrameData
{
    static constexpr int kCascades = 4;

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

        // S14: the cascade's caster-cull volume. Plane count (6 = the ortho box, 7..11 = the
        // accurate convex volume) and how many objects the CPU cull let through LAST frame
        // (the queue is filled after UpdateCascades runs), so a headless run can show the cut.
        std::uint32_t cullPlanesDbg[kCascades] = {};
        std::uint32_t cullCastersDbg[kCascades] = {};
        // Objects in LAST frame's queue that the OTHER volume OF THAT FRAME rejects: with the
        // accurate volume applied, how many of its survivors the plain box would reject (must be
        // 0 -- the volume carries every box plane); with the box applied, how many of its
        // survivors the accurate volume would have cut (the saving). A cross-check the image
        // cannot give. Same-frame pair on purpose (Scene::cascadeBoxPrev_/AccPrev_): against this
        // frame's box a camera turn counts as a leak. Dynamic/moved casters are not counted.
        std::uint32_t cullLeakDbg[kCascades] = {};
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
    // Local-light plan S9e: one view per shadow slot of the current capacity (render::ShadowSpotCapacity, 6 per
    // render::ShadowPointCapacity) -- the first GetShadowed*Count of them are this frame's.
    std::vector<SceneView>* spotShadowViews = nullptr;
    std::vector<SceneView>* pointShadowViews = nullptr;
    LightManager* lightManager = nullptr;
    Skybox* skybox = nullptr;
    const std::vector<std::unique_ptr<RenderableObjectBase>>* objects = nullptr;
    // Occlusion plan S3a: this frame's box queries (decided by the camera prepare) and the heap
    // that records them; null / empty batches = the pass declares nothing.
    const vis::OcclusionQueryPlan* occlusionPlan = nullptr;
    vis::OcclusionQueryHeap* occlusionQueries = nullptr;
    // S3b: the HZB tester that runs the plan's boxes when its method is Hzb (Main_VisTest).
    vis::HzbOcclusionTester* hzbTester = nullptr;
    // S3a.6: per light index, 1 = the light's influence volume was occluded last frame -- the
    // GPU light entry is zeroed (it can light no visible pixel) and its shadow views were built
    // with a reject-all frustum (their passes clear and draw nothing). Sized to the light lists.
    const std::vector<std::uint8_t>* spotLightOccluded = nullptr;
    const std::vector<std::uint8_t>* pointLightOccluded = nullptr;
    const DirectionalLight* dirLight = nullptr;
    // Time-of-day plan T2: the light the SKY is lit by -- its LUTs, the sky pixel and disc, the capture,
    // the distant light, the aerial perspective. The same object as dirLight unless the level's time of
    // day has handed dirLight to the moon; the sky keeps the sun, so the twilight outlives the hand-over.
    const DirectionalLight* skyLight = nullptr;
    // Time-of-day plan N1: the moon as the sky sees it -- the atmosphere's second light and the drawn
    // disc (SkyViewFrameData light 1). `present` false without a time of day: light 1 is skipped.
    SkyMoonFrame skyMoon{};
    NightSkyFrame nightSky{}; // time-of-day N2: stars, the Milky Way, the airglow
    // S8: the CSM knobs the lighting pass needs (receiver bias / sharpen / over-blur). Same object
    // the developer window edits, so a slider move reaches the shader the very next frame.
    const CascadeShadowConfig* cascadeConfig = nullptr;
    ShadowGpuData* shadowGpu = nullptr; // Rung 0: GPU-driven shadow cull inputs/outputs
    render::LocalShadowCache* localShadowCache = nullptr; // local-light plan S7: the atlas lights' static-caster cache
    // Occlusion plan S4: this frame draws the registry's eligible objects through the indirect
    // G-buffer pass (Scene decided before the camera prepare; the CPU buckets hold the rest).
    bool gbufferIndirect = false;
    VirtualShadowMap* vsm = nullptr;    // Rung 2: page pool + page table (Step 18; unused yet)
    // S15: the SDSM analysis. Non-null only while the mode is Sdsm AND its resources built --
    // which is exactly the gate every SDSM pass builder uses, so a failed PSO degrades to
    // "the passes are not in the graph" rather than to a black shadow atlas.
    SdsmShadows* sdsm = nullptr;
    const vfx::WindState* wind = nullptr; // W3: global wind, folded into the gbuffer per-view CB
    // Water in the level, or null. The deferred lighting pass reads its caustics settings, clock
    // and flipbook; no ocean simply means no caustics.
    OceanRenderable* ocean = nullptr;

    CascadeData cascades{};
    // The editor selection, SORTED ASCENDING, owned by Scene -- a pointer rather than a copied
    // fixed array. It used to be a std::array of 64 and everything past that was dropped without a
    // word, so selecting a few hundred meshes outlined the first 64 of them (reported 2026-09-15).
    // Sorted because the membership test runs per object per frame and has to be a binary search
    // once the selection is large; see IsSelectedEditorObject.
    const std::uint64_t* selectedEditorObjectIds = nullptr;
    std::uint32_t selectedEditorObjectCount = 0;
    std::uint32_t selectionOutlineRadius = 1;

    SceneRenderSettings settings{};
    // P2: the level's photographic camera settings, snapshotted with the rest of the frame so the
    // metering pass reads a value that cannot change under it mid-frame.
    render::CameraExposureSettings cameraExposure{};
    render::ColorPipelineSettings colorPipeline{};
};
