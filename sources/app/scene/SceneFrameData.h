#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/Math.h"
#include "app/scene/SceneView.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/renderables/VirtualShadowMap.h" // vsm::kNumClipmapLevels (Step 24d)

class Camera;
class DirectionalLight;
class RenderableObjectBase;
class Skybox;
class ShadowGpuData;
class VirtualShadowMap;

enum class SsrTechnique : uint32_t
{
    Lettier = 0,
    LogMarch = 1,
    Count
};

// Where screen reflections come from (S8). Off = skybox specular only; SSR =
// screen-space; RT = hardware ray-traced (Tier-1). RT is only honored when
// Renderer::IsRaytracingSupported() — otherwise the renderer falls back to SSR.
enum class ReflectionSource : uint32_t
{
    Off = 0,
    SSR = 1,
    RT = 2,
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
    // S8: the reflection source. Default RT (today's behavior). RT runs the
    // Tier-1 ray-traced pass instead of SSR; Off clears the reflection buffer
    // (skybox specular only). RT auto-falls back to SSR on non-RT hardware.
    ReflectionSource reflectionSource = ReflectionSource::RT;
    // S6: RT hit/visibility debug viz (dev tool). Traces a reflection ray per
    // pixel and writes a hit-distance/miss image into the reflection target (view via
    // the texture inspector -> Reflection). Builds the AS regardless of source.
    bool rtDebugView = false;
    // S16: glossy reflections. The reflection blur radius scales with the reflector's
    // roughness by this factor (in reflection-res pixels at full roughness); 0 = sharp
    // mirror reflections. Applies to both RT and SSR. Tunable in the developer window.
    float reflectionGlossyScale = 1.0f;
    // Analytic sun specular boost on metals. The sun's specular lobe is scaled by
    // (1 + metal*sunMetalSpecInfluence) in the lighting pass so a smooth metal shows a
    // distinct sun highlight (which otherwise merges into the environment reflection,
    // since the sun disk is not painted into the skybox). 0 = pure physical. Dev-tunable.
    float sunMetalSpecInfluence = 0.0f;
    // Analytic sun angular size (added to the GGX alpha for the sun only). Floors the
    // specular lobe width so smooth surfaces show a finite, bright sun glint instead of a
    // sub-pixel spike that never lands on a pixel. ~0.01 ≈ a few-pixel disk; 0 = punctual
    // (vanishing highlight on mirrors). Dev-tunable.
    float sunAngularSize = 0.01f;
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
    std::array<SceneView, vsm::kNumClipmapLevels>* clipmapViews = nullptr; // Step 24d: directional clipmap (VSM)
    std::array<SceneView, LightManager::kMaxShadowedSpotLights>* spotShadowViews = nullptr;
    std::array<SceneView, LightManager::kMaxShadowedPointLights * 6>* pointShadowViews = nullptr;
    LightManager* lightManager = nullptr;
    Skybox* skybox = nullptr;
    const std::vector<std::unique_ptr<RenderableObjectBase>>* objects = nullptr;
    const DirectionalLight* dirLight = nullptr;
    ShadowGpuData* shadowGpu = nullptr; // Rung 0: GPU-driven shadow cull inputs/outputs
    VirtualShadowMap* vsm = nullptr;    // Rung 2: page pool + page table (Step 18; unused yet)

    CascadeData cascades{};
    std::uint64_t selectedEditorObjectId = 0;
    std::uint32_t selectionOutlineRadius = 1;

    SceneRenderSettings settings{};
};
