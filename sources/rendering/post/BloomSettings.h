#pragma once

#include <algorithm>
#include <cstdint>

#include "core/math/Math.h"

// Moved from app/scene/SceneFrameData.h (architecture plan R1.5).

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
    // UE's FFT bloom reads the upscaler's OUTPUT (PostProcessing.cpp:1131-1183, 1485-1515) and its glints survive because
    // TSR keeps them. Tried for UE's path on 2026-11-04 (owner: "one to one with theirs", his view of sparkles with UE's
    // little stars, UE cam (-3065, -9795, 450)): behind DLSS a sparkle's bloom is GONE -- the frame with the bloom and
    // without it the same, x10 intensity too, under preset M as under F (M kept 61 % of a low sun's broad glitter path,
    // F 30 %, but not the sparkles); without the upscaler, or with this scene before it, they glow. So this stays on:
    // it is what TSR does for UE's bloom. 0 = the upscaled image, UE's path, the A/B.
    bool convBeforeUpscale = true;
    // What the scene before the upscaler costs: it is JITTERED, so a glint under a pixel lands in it or
    // not frame by frame and the star on a chrome glint flickered 40-50 % frame to frame. The bloom's
    // light (the FFT, the ghosts, the streak) is therefore accumulated over frames first, reprojected by
    // the G-buffer velocity (bloom_conv_cs.hlsl stage 12) -- the averaging UE gets from TSR, whose output
    // their bloom reads. This frame's weight: 1 = no accumulation.
    float convTemporalWeight = 0.1f;
    // A sun moving in the SKY (the clock, a drag) has no motion vector: the history above kept its disc where it was
    // and the FFT drew a streak behind it (2026-11-04). While it moves, its disc now and its disc in the history take
    // this frame alone (BloomRenderer::RecordTemporalInput). Transient (`--set bloom.convTemporalSunReject:0` = before).
    bool convTemporalSunReject = true;
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
    // physical-disc look on wind_test (three views against the d1b6183 build, within 2/255), read off the
    // upscaled disc; x1.3 since the sun probe reads the disc before the upscaler (2026-10-28: DLSS gave a
    // clear disc 1.23-1.32x its energy), so a clear sun under DLSS keeps that look.
    float sunGlareIntensity = 0.0065f;
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
    // 0.008 matched the pre-physical-disc look, as sunGlareIntensity's 0.005, and took the same x1.3.
    float sunRaysIntensity = 0.0104f;
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
    // A PARTLY HIDDEN SUN (2026-10-27, owner: "a micro-pixel is covered and the corona is gone"): the
    // corona is the clear disc's x (visible share)^this. 1 = physical -- linear in the light that
    // shows, so a sun 80 % behind fronds keeps a fifth of its rays and they vanish; 0.5 keeps 45 %
    // there, as a sun through leaves throws a clear star in a photograph. A clear disc is untouched.
    // The share is measured on the scene BEFORE the upscaler (DLSS rings on the disc).
    float sunRaysVisibleExponent = 0.5f;
};
