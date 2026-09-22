#pragma once

#include <cstdint>

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
// Bumped by every change of g_rasterBindless; the renderer's tick compares it with what it last
// applied and flags the G-buffer materials for a hot reload.
inline std::uint32_t g_rasterBindlessGen = 0;

inline void SetRasterBindless(bool on)
{
    if (g_rasterBindless != on) { g_rasterBindless = on; ++g_rasterBindlessGen; }
}

} // namespace render
