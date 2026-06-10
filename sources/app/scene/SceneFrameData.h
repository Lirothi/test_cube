#pragma once

#include <array>
#include <cstdint>

#include "core/math/Math.h"
#include "app/scene/SceneView.h"
#include "rendering/lighting/LightManager.h"

class Camera;
class Skybox;
enum class SsrTechnique : uint32_t;

// Per-frame inputs for the render passes. Scene::PrepareViews fills this once per
// frame; pass bodies read from it instead of reaching back into Scene members.
// Scene keeps ownership of objects, lights, and views — this struct only caches
// derived per-frame data (cascade matrices) and points at the rest.
struct SceneFrameData
{
    static constexpr int kCascades = 4;

    struct CascadeData
    {
        mat4 lightView[kCascades];
        mat4 lightProj[kCascades];
        float2 atlasScale[kCascades];
        float2 atlasBias[kCascades];
        float splitsVS[kCascades + 1] = {}; // near..far in view space
        float normalBiasWS[kCascades] = {};
        float depthBiasNDC[kCascades] = {};
    };

    const Camera* camera = nullptr;
    SceneView* mainView = nullptr;
    std::array<SceneView, kCascades>* cascadeViews = nullptr;
    std::array<SceneView, LightManager::kMaxSpotLights>* spotShadowViews = nullptr;
    LightManager* lightManager = nullptr;
    Skybox* skybox = nullptr;

    CascadeData cascades{};

    SsrTechnique ssrTechnique{};
    bool doFxaa = false;
    bool debugTexMode = false;
    bool showProfiler = false;
};
