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
    };

    mat4 view = mat4::Identity();
    mat4 proj = mat4::Identity();
    mat4 invView = mat4::Identity();
    mat4 invProj = mat4::Identity();
    Frustum frustum{};
    uint32_t renderLayerMask = kRenderLayerAll;
    SceneRenderQueue queue{};
    float3 position = float3(0.0f, 0.0f, 0.0f);
    float hfov = 0.0f;
    float zNear = 0.0f;
    float zFar = 0.0f;
    bool requiresDepthCheck = false;
    Type type = Type::Camera;
};

