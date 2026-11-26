#pragma once

#include <array>
#include <cstddef>
#include <d3d12.h>

#include "shaders/shared/glass_slots.h" // the glass pass's registers by name

class LightManager;
class Renderer;
class Skybox;

namespace render
{
// glass.hlsl's per-PASS table, t0..t14: the opaque scene copy, the sun / spot / point shadow maps, the
// sky, the light lists, the glass reflection, the VSM page table + pool, the prefiltered sky, the fog
// volume, the SDSM partitions and the local shadow pool. t6 (the normal map) is the DRAW's and is left
// empty here. Gathered once per Main_Translucent by SceneRenderer::Pass_Translucent and published on
// the renderer (Renderer::DirectInputs().glass) for every glass draw -- the TransparentStaticMesh panes and
// the glass material slots of opaque objects (GBufferRenderable::RenderTranslucent) -- which have no
// frame access of their own. Was built per pane in TransparentStaticMesh::RecordGraphics.
inline constexpr size_t kGlassPassSrvCount = GLASS_PASS_SRV_COUNT; // shaders/shared/glass_slots.h (R5.3)
inline constexpr size_t kGlassNormalMapSlot = GLASS_T_NORMAL_MAP;
using GlassPassSrvs = std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kGlassPassSrvCount>;

// False when a shadow map the table must hold is missing this frame (the glass draws then skip, as
// before) or the light lists could not be sized. Needs the VSM / SDSM / local-pool transports already
// set for the frame (Pass_Translucent sets them first).
bool GatherGlassPassSrvs(Renderer* renderer, LightManager& lights, Skybox* sky, GlassPassSrvs& out);
} // namespace render
