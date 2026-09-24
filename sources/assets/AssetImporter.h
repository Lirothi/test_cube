#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "core/diagnostics/DiagPaths.h"

// Part H1 — offline asset conversion backend (no editor UI). Turns raw downloads staged in
// `import_staging/` into engine-ready content: PNG/JPG -> mipped BC7 DDS, texture-set MR
// synthesis, frame-sequence flipbook atlases, and equirect .hdr -> BC6H cubemap skyboxes.
// Runs headless via the `--import <dir> [--skybox <file.hdr>]` CLI so the executor and batch
// jobs never hand-run texconv. Backed by the vendored DirectXTex (third_party/DirectXTex).
namespace assets {

struct ImportOptions
{
    std::string stagingDir;         // directory to scan for importable assets
    std::string skyboxHdr;          // optional: a single equirect .hdr -> skybox cubemap
    int         maxTextureSize = 2048; // downscale longest edge to this (staged 4K rock -> 2K)
    bool        highQuality = false; // BC7 max (exhaustive, ~20x slower) vs fast mode-6 (default)
    bool        useGpu = true;      // H5: BC6H/BC7 on a D3D11 compute device (CPU fallback if absent)
    bool        flipGreen = false;  // invert normal-map green channel (OpenGL <-> DirectX Y)
    bool        bc5Normal = false;  // encode normal maps as BC5 (RG) instead of BC7 (RGB)
    bool        centerNormals = false; // re-center a normal map whose flat baseline strays from (128,128)
                                       // (a "purple cast"/DC lean that skews lighting); threshold-gated
    int         skyboxFaceSize = 1024; // cube face edge for --skybox equirect -> cubemap
    // Sky CALIBRATION. HDRI libraries are not calibrated to this engine's linear scale -- Poly
    // Haven skies land around a median luminance of 0.6-0.9, while the engine's manual exposure
    // multiplier is 0.18*8 = 1.44, so an un-normalised sky puts roughly half its pixels above 1.0
    // before the tone curve runs. Fixing that with the CAMERA (lowering EV) is wrong: it darkens
    // the whole scene to correct one asset. So the cube itself is scaled at import, which is the
    // only place that reaches EVERY consumer at once -- background, ocean reflection, compose and
    // the F7 IBL derivatives all read the same cube.
    //
    // The statistic is the MEDIAN, not the mean: the sun disc is a handful of enormous texels and
    // would drag a mean around by a factor of two depending on whether it happens to be in frame.
    // 0 disables normalisation and writes the source's own radiance.
    // 0.18 = middle grey. A sky should sit around the middle of the range, not below it: at 0.08
    // the calibration was measurably correct and visibly too dark, because the sky is also what
    // lights the water's reflection and the ambient fill, so darkening it darkens everything it
    // touches. Exposed in the import dialog ("Sky brightness") because the right value is a
    // per-sky judgement, not a constant.
    float       skyTargetMedianLuma = 0.18f;
    // P16.3 -- strip the sun disc from the LIGHTING derivatives (_spec/_diffuse). The display cube
    // keeps it; only what the engine LIGHTS with loses it.
    //
    // Measured on rustig_koppie_puresky_4k by integrating the upper hemisphere of the source .hdr:
    // the disc is 91.7% of the horizontal illuminance. An irradiance cube built from that is not a
    // sky, it is a second sun spread over the hemisphere -- shadowless, unoccluded, and impossible
    // to out-shine with the directional light that is supposed to BE the sun. The specular cube has
    // its own version of the problem: lighting_cs already adds an analytic sun specular, so a disc
    // in the prefiltered radiance double-counts on every glossy surface.
    //
    // Off reproduces the pre-P16.3 derivatives exactly, for a sky used with no directional light.
    bool        skyRemoveSunFromIbl = true;
    // Angular radius of the cut, in degrees. The sun is 0.53 wide; the rest of the default is the
    // aureole immediately around it. The integral says the exact value hardly matters -- 0.5 deg
    // already takes 91.0% and 10 deg only reaches 93.2%, so everything past the disc is real sky.
    float       skySunRadiusDeg = 1.5f;
    // Where the finished skybox goes. Empty = leave it beside the source, which is what the
    // headless path always did; set it to the textures root and the cube, its two IBL siblings and
    // the shared BRDF LUT are moved to exactly where the GUI import panel puts them:
    //     <root>/<name>/<name>.dds, _spec.dds, _diffuse.dds     and     <root>/brdf_lut.dds
    // The LUT is scene-independent and belongs to no single sky, which is why it lands in the root.
    std::string skyOutputRoot;
    // Default routed through diag::LogPath so a headless --import lands in logs/ like every
    // other engine diagnostic (the ImportPanel sets its own path the same way).
    std::string logPath = diag::LogPath("asset_import.log");

    // H3 import-dialog controls. registerPreset=false skips the texture-set pass (no MR synth / no
    // material preset — every selected image is converted as a loose DDS). includeRel, when
    // non-empty, whitelists which staging-relative files to convert (empty = convert everything).
    bool                     registerPreset = true;
    std::vector<std::string> includeRel;

    // Optional live-progress channel for a UI progress bar (both null in the headless CLI).
    // RunImport stores the convertible-texture count into *progressTotal after scanning, then
    // bumps *progressDone as each texture/set/flipbook completes, snapping to total at the end.
    std::atomic<int>* progressDone = nullptr;
    std::atomic<int>* progressTotal = nullptr;
};

// Headless entry point for the `--import` CLI. Returns a process exit code
// (0 = success, non-zero = number of failed conversions / fatal setup error).
int RunImport(const ImportOptions& opts);

// How steep a tangent-space normal map is where it is SEEN: the mean Z of its unit normals over
// the texels the albedo's alpha keeps (all texels when the albedo has no cutout), measured at
// <= 512 texels a side -- the mips a plant is mostly looked at through. A good leaf or bark map sits
// at 0.9-0.98; one derived from the albedo by a height-from-colour tool can sit near 0.5, i.e. its
// texels lean ~60 degrees on average, and a leaf lit through that is dark and grainy whatever the
// sun does. `strengthFor(target)` is the material's normalStrength (the shader's XY scale,
// gbuffer_common.hlsli) that brings the mean Z up to `target`.
struct NormalMapSteepness
{
    bool  valid = false;
    float coverage = 0.0f; // fraction of texels measured
    float meanZ = 1.0f;
    std::vector<float> xy2; // per measured texel: x^2 + y^2 of the unit normal
    std::vector<float> z;   // per measured texel: z of the unit normal (floored at 1e-4)
    float MeanZAt(float strength) const;
    float StrengthFor(float targetMeanZ) const; // 1 when the map already reaches it
};
NormalMapSteepness MeasureNormalMapSteepness(const std::string& normalPath, const std::string& albedoPath);

} // namespace assets
