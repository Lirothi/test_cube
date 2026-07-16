#pragma once

#include <string>

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
    bool        flipGreen = false;  // invert normal-map green channel (OpenGL <-> DirectX Y)
    bool        bc5Normal = false;  // encode normal maps as BC5 (RG) instead of BC7 (RGB)
    std::string logPath = "asset_import.log";
};

// Headless entry point for the `--import` CLI. Returns a process exit code
// (0 = success, non-zero = number of failed conversions / fatal setup error).
int RunImport(const ImportOptions& opts);

} // namespace assets
