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

// Must equal GBUFFER_MAX_INSTANCES in shaders/gbuffer_common.hlsl. Runs larger than this
// are split across multiple instanced draws.
inline constexpr uint32_t kMaxInstancesPerDraw = 256;

// Minimum visible run length worth instancing; shorter runs stay per-object.
inline constexpr uint32_t kInstancingThreshold = 8;

// Runtime kill-switch for auto-instancing (default on). Useful for A/B debugging and
// before/after measurement; when off, BuildInstancedBatches is a no-op (per-object path).
inline bool g_instancingEnabled = true;
} // namespace render
