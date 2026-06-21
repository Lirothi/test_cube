#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "app/scene/SceneView.h"
#include "rendering/lighting/LightManager.h"

class Camera;
class DirectionalLight;
class RenderableObjectBase;
class Skybox;

enum class SsrTechnique : uint32_t
{
    Lettier = 0,
    LogMarch = 1,
    Count
};

// Debug/render toggles owned by the app layer (AppController maps input actions
// to these) and snapshotted into SceneFrameData each frame.
struct SceneRenderSettings
{
    SsrTechnique ssrTechnique = SsrTechnique::LogMarch;
    bool doFxaa = false;
    bool debugTexMode = false;
    bool showProfiler = false;
    // S5: build the ray-tracing acceleration structures each frame (gated also on
    // Renderer::IsRaytracingSupported). Default off — when off, or on non-RT
    // hardware, the Main_BuildAS pass is never added and the frame is unchanged.
    // S8 will fold this into a proper reflection-source enum.
    bool rtBuildAccelStructures = false;
    // S6: RT hit/visibility debug viz. Traces a reflection ray per pixel against
    // the TLAS and writes a hit-distance/miss image into the SSR target (view it
    // via the render-target inspector -> Ssr). Implies the AS build. Default off.
    bool rtDebugView = false;
};

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
    const std::vector<std::unique_ptr<RenderableObjectBase>>* objects = nullptr;
    const DirectionalLight* dirLight = nullptr;

    CascadeData cascades{};

    SceneRenderSettings settings{};
};
