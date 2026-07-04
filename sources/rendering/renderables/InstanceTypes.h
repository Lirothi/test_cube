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
} // namespace render
