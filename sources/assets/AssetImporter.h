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

} // namespace assets
