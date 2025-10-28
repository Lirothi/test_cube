#pragma once

#include <cstdint>

enum class RenderLayer : uint32_t
{
    Default     = 1u << 0,
    Transparent = 1u << 1,
    Terrain     = 1u << 2,
    Sky         = 1u << 3,
    Lights      = 1u << 4,
    Gizmo       = 1u << 5,
    Debug       = 1u << 6,
};

constexpr uint32_t RenderLayerMask(RenderLayer layer)
{
    return static_cast<uint32_t>(layer);
}

constexpr uint32_t kRenderLayerAll =
    RenderLayerMask(RenderLayer::Default) |
    RenderLayerMask(RenderLayer::Transparent) |
    RenderLayerMask(RenderLayer::Terrain) |
    RenderLayerMask(RenderLayer::Sky) |
    RenderLayerMask(RenderLayer::Lights) |
    RenderLayerMask(RenderLayer::Gizmo) |
    RenderLayerMask(RenderLayer::Debug);

inline bool IsLayerEnabled(uint32_t mask, RenderLayer layer)
{
    return (mask & RenderLayerMask(layer)) != 0u;
}

inline void EnableLayer(uint32_t& mask, RenderLayer layer)
{
    mask |= RenderLayerMask(layer);
}

inline void DisableLayer(uint32_t& mask, RenderLayer layer)
{
    mask &= ~RenderLayerMask(layer);
}
