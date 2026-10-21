#pragma once

#include <cstdint>

#include "core/math/Math.h"
#include "core/math/Frustum.h"
#include "rendering/RenderLayers.h"
#include "app/scene/SceneRenderQueue.h"

struct SceneView
{
    enum class Type
    {
        Camera,
        Shadow,
        ShoreDepth,
    };

    mat4 view = mat4::Identity();
    mat4 proj = mat4::Identity();
    mat4 invView = mat4::Identity();
    mat4 invProj = mat4::Identity();
    Frustum frustum{};
    uint32_t renderLayerMask = kRenderLayerAll;
    SceneRenderQueue queue{};
    // B6.1: the visible TransparentComplex bucket, split by whether the object WRITES DEPTH.
    // `transparentWater` draws in Main_Transparent, before the screen-space fog that fogs it;
    // `translucentComplex` draws in Main_Translucent, after it, and fogs itself. Filled once the
    // transparent sort has fixed the blend order, so both halves keep that order. Camera view only.
    std::vector<RenderableObjectBase*> transparentWater{};
    std::vector<RenderableObjectBase*> translucentComplex{};
    float3 position = float3(0.0f, 0.0f, 0.0f);
    float hfov = 0.0f;
    float zNear = 0.0f;
    float zFar = 0.0f;
    // Local-light shadow plan S1: a SPOT shadow view's UE biases (shadowBias, shadowSlopeBias) -- Pass_SpotShadows
    // turns them into its depth pass's world-metre bias at the atlas's resolution. Zero on every other view.
    float2 shadowBias = float2(0.0f, 0.0f);
    // Plan S4: a spot / point-face shadow view's own resolution (LightManager: UE's size for its light) -- the square it
    // draws into, and the resolution its S1 bias is for. 0 on every other view. Plan S9: that square's place -- the
    // atlas page (array slice) and its corner in texels.
    std::uint32_t shadowRes = 0;
    std::uint32_t shadowPage = 0, shadowX = 0, shadowY = 0;
    // Plan S7 (render::LocalShadowCache): 0 = uncached, 1 = hit (copy the cached statics in, draw the movables), 2 = miss
    // (the cache pass renders the statics into the pool first); the view's square in the pool. A frame the cull did not
    // split (ShadowGpuData::LocalSplitFrame) treats every view as uncached.
    std::uint8_t cacheMode = 0;
    std::int32_t cachePage = -1;
    std::uint32_t cacheX = 0, cacheY = 0;
    // Plan S9e: a spot / point-face view's light, for the GPU caster cull (UE AffectsBounds): its reach sphere (xyz
    // origin, w radius; w < 0 = none) and a spot's cone (xyz axis, w cos of the outer angle; > 1 = none). Not part of
    // `frustum` -- the CPU path culls with the planes alone.
    float4 lightSphere = float4(0.0f, 0.0f, 0.0f, -1.0f);
    float4 lightCone = float4(0.0f, 0.0f, 0.0f, 2.0f);
    // Plan S10e: a local light's VSM mip bias on top of the frame's (vsm::LocalMovingMipBias of its mobility).
    float vsmMipBias = 0.0f;
    bool requiresDepthCheck = false;
    Type type = Type::Camera;
};

