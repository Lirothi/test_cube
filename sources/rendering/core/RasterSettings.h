#pragma once

#include <cstdint>
#include "core/threading/Knob.h" // game thread plan G5: knobs with a game twin

namespace render {

// Texture streaming plan A6: bindless material textures in the raster paths. When on (and the
// device has ResourceBindingTier 3 + SM 6.6, see BindlessHeap::DynamicResourcesSupported), every
// G-buffer material PSO is built with GBUFFER_BINDLESS=1: no t0..t2 descriptor table, the three
// texture indices travel in SurfaceParams (b2) and the shader reads ResourceDescriptorHeap[].
// The masked shadow permutation reads its albedo the same way, which removes the 16-group cap.
// OFF = the staged descriptor tables as before; both branches live in one binary (the A/B is
// `--set=raster.bindless:0|1`, and the toggle rebuilds the affected PSOs in place, keep-alive for
// the frames in flight).
inline bool g_rasterBindless = true;
inline bool g_rasterBindless_game = true;
inline const bool g_rasterBindless_knob = ::knob::Register(g_rasterBindless_game, g_rasterBindless, "render::g_rasterBindless");
// Bumped by every change of g_rasterBindless; the renderer's tick compares it with what it last
// applied and flags the G-buffer materials for a hot reload.
inline std::uint32_t g_rasterBindlessGen = 0;

inline void SetRasterBindless(bool on)
{
    if (g_rasterBindless != on) { g_rasterBindless = on; ++g_rasterBindlessGen; }
}

// The material samplers' anisotropy (docs/scalability_plan.md §4.4, S3; UE r.MaxAnisotropy; the group Textures; `--set
// textures.anisotropy:1|2|4|8|16`, user_settings.json graphics streaming.anisotropy, Developer Controls > Streaming):
// every surface sampler the G-buffer, the
// instanced models, glass and the ocean bind (SamplerManager::AnisoWrap). Live: the samplers are staged a frame by their
// description, so a change is the next frame's -- no pipeline is rebuilt. 16 = today.
inline std::uint32_t g_textureAnisotropy = 16u;
inline std::uint32_t g_textureAnisotropy_game = 16u;
inline const bool g_textureAnisotropy_knob = ::knob::Register(g_textureAnisotropy_game, g_textureAnisotropy, "render::g_textureAnisotropy");

inline std::uint32_t ClampAnisotropy(std::uint32_t a)
{
    return a <= 1u ? 1u : a <= 2u ? 2u : a <= 4u ? 4u : a <= 8u ? 8u : 16u;
}

} // namespace render
