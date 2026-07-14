#pragma once
#include <DirectXMath.h>
#include <cstdint>

namespace render
{
// CPU mirror of HLSL `InstancePerObject` in shaders/gbuffer_common.hlsl. Field order and
// padding must match the cbuffer layout exactly (constant-buffer packing rules put
// metalRough at offset 144, texOffsScale at 160, and objectId at 192. Filled per
// visible instance and uploaded as a root-CBV array (b0) indexed by SV_InstanceID.
struct alignas(16) InstancePerObject
{
    DirectX::XMFLOAT4X4 world;        // 0
    DirectX::XMFLOAT4X4 prevWorld;    // 64
    DirectX::XMFLOAT4   baseColor;    // 128
    DirectX::XMFLOAT2   metalRough;   // 144
    float               _pad0[2];     // 152
    DirectX::XMFLOAT4   texOffsScale; // 160
    DirectX::XMFLOAT4   texFlags;     // 176
    uint32_t            objectId;     // 192
    uint32_t            _pad1[3];     // 196
};                                    // 208
static_assert(sizeof(InstancePerObject) == 208,
    "InstancePerObject must match the HLSL cbuffer layout (208 bytes)");

// B2b multi-slot instancing: CPU mirror of HLSL `SlotParams` (b2) in shaders/gbuffer_instcb.hlsl
// (INSTCB_SLOT_PARAMS variant). One upload per material slot per batch — the slot's
// MaterialParams, shared by every instance of the batch (member params are verified equal by
// IInstanceable::SameInstanceSlots before objects merge into a run).
struct alignas(16) InstanceSlotParams
{
    DirectX::XMFLOAT4 baseColor;    // 0
    DirectX::XMFLOAT2 metalRough;   // 16
    float             _pad0[2];     // 24
    DirectX::XMFLOAT4 texOffsScale; // 32
    DirectX::XMFLOAT4 texFlags;     // 48
};                                  // 64
static_assert(sizeof(InstanceSlotParams) == 64,
    "InstanceSlotParams must match the HLSL SlotParams cbuffer layout (64 bytes)");

// Per-caster world bounds for GPU shadow culling (Rung 0, Step 2). center.xyz = world-space
// AABB center (w = bounding radius, for a cheap sphere pre-test); halfExtents.xyz = world-space
// half-extents (w spare). Matches the positive-vertex AABB test in Frustum::Intersects that the
// Step 4 cull compute will port to HLSL. Maintained per caster id in lockstep with
// InstancePerObject. Unused until Step 4.
struct alignas(16) CasterBounds
{
    DirectX::XMFLOAT4 center;      // xyz world center, w bounding radius
    DirectX::XMFLOAT4 halfExtents; // xyz world half-extents, w unused
};
static_assert(sizeof(CasterBounds) == 32, "CasterBounds must be 32 bytes");

// One shadow view's 6 inward-facing frustum planes (unit normal, inside == n·p + d >= 0),
// the per-view input to the Step 4 GPU cull. Mirrors Frustum::planes_. Unused until Step 4.
struct alignas(16) ShadowViewFrustum
{
    DirectX::XMFLOAT4 planes[6];
};
static_assert(sizeof(ShadowViewFrustum) == 96, "ShadowViewFrustum must be 96 bytes");

// Must equal GBUFFER_MAX_INSTANCES in shaders/gbuffer_common.hlsl. Runs larger than this
// are split across multiple instanced draws.
inline constexpr uint32_t kMaxInstancesPerDraw = 256;

// Minimum visible run length worth instancing; shorter runs stay per-object.
inline constexpr uint32_t kInstancingThreshold = 8;

// Runtime kill-switch for auto-instancing (default on). Useful for A/B debugging and
// before/after measurement; when off, BuildInstancedBatches is a no-op (per-object path).
inline bool g_instancingEnabled = true;

// Rung 0 runtime toggle (default ON): the shadow passes draw via GPU cull + ExecuteIndirect
// (ShadowGpuData) instead of the per-object CPU RenderShadow loop — the CPU-submission win.
// Toggle OFF (Ctrl+I, "ToggleIndirectShadows") for the CPU-path A/B. If the cull PSOs fail to
// build, IndirectDrawReady() returns false and the passes fall back to the CPU path anyway.
inline bool g_indirectShadowsEnabled = true;

// GI→VSM runtime toggle (default ON): fold GPU-instanced casters' instances into the consolidated
// ShadowGpuData caster set (GPU scatter → cull → indirect), so they cast in VSM and via the indirect
// path in Legacy (dropping their per-view CPU RenderShadow tail). Toggle OFF (Ctrl+G,
// "ToggleGiIndirectShadows") for the A/B: GI reverts to the Legacy CPU tail only (nothing in VSM) —
// exactly today's behavior. Also the safety fallback: if the scatter PSO fails or an object is over
// the group cap, GI keeps drawing through the retained CPU tail. Requires g_indirectShadowsEnabled.
inline bool g_giIndirectShadowsEnabled = true;

// Rung 2 / Step 24a — active shadow method. Legacy = the CSM directional + spot/point/glass ATLAS
// path; VSM = the virtual page pool (spot/point/glass today; directional after Step 24). Drives both
// whether the VSM pipeline passes run AND which sampler the light/glass shaders use (VsmActive() →
// useVsm). Default Legacy (VSM opt-in). Toggle Legacy<->VSM with Ctrl+V ("ToggleVsmPageRequest").
// Step 24b makes the switch free the inactive mode's GPU resources (only one mode ever resident).
enum class ShadowMode : std::uint32_t { Legacy = 0, VSM = 1 };
inline ShadowMode g_shadowMode = ShadowMode::VSM;
inline bool VsmActive() { return g_shadowMode == ShadowMode::VSM; }
} // namespace render
