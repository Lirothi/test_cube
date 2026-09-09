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
    bool requiresDepthCheck = false;
    Type type = Type::Camera;
};

