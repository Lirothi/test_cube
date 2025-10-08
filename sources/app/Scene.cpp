#include "app/Scene.h"

#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>

#include "input/InputManager.h"
#include "app/Camera.h"
#include "app/Systems.h"
#include "rendering/debug/DebugGrid.h"
#include "rendering/meshes/GpuInstancedModels.h"
#include "core/Helpers.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "ocean/OceanRenderable.h"
#include "rendering/meshes/StaticMesh.h"
#include "rendering/renderables/GlassCube.h"
#include "core/task/TaskSystem.h"
#include "text/TextManager.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"

void Scene::CBHandleCache::LightingHandles::Populate(Material* material)
{
    *this = {};
    if (!material) { return; }

    sunDir = material->ComputeCB0FieldHandle("sunDirWS");
    ambient = material->ComputeCB0FieldHandle("ambientIntensity");
    lightRgb = material->ComputeCB0FieldHandle("lightRgb");
    exposure = material->ComputeCB0FieldHandle("exposure");
    camPos = material->ComputeCB0FieldHandle("camPosWS");
    camDir = material->ComputeCB0FieldHandle("camDirWS");
    view = material->ComputeCB0FieldHandle("view");
    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    lightViewProj = material->ComputeCB0FieldHandle("lightViewProj");
    cascadeScaleBias = material->ComputeCB0FieldHandle("cascadeScaleBias");
    cascadeSplits = material->ComputeCB0FieldHandle("cascadeSplitsVS");
    shadowAtlasSize = material->ComputeCB0FieldHandle("shadowAtlasSize");
    shadowBiasNDC = material->ComputeCB0FieldHandle("shadowBiasNDC");
    normalBiasWS = material->ComputeCB0FieldHandle("normalBiasWS");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
}

void Scene::CBHandleCache::PointLightHandles::Populate(Material* material)
{
    *this = {};
    if (!material) { return; }

    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    camPos = material->ComputeCB0FieldHandle("camPosWS");
    lightCount = material->ComputeCB0FieldHandle("lightCount");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
}

void Scene::CBHandleCache::SpotLightHandles::Populate(Material* material)
{
    *this = {};
    if (!material) { return; }

    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    camPos = material->ComputeCB0FieldHandle("camPosWS");
    lightCount = material->ComputeCB0FieldHandle("lightCount");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
    shadowSize = material->ComputeCB0FieldHandle("shadowSize");
    invShadowSize = material->ComputeCB0FieldHandle("invShadowSize");
}

void Scene::CBHandleCache::SsrHandles::Populate(Material* material)
{
    *this = {};
    if (!material) { return; }

    view = material->ComputeCB0FieldHandle("view");
    proj = material->ComputeCB0FieldHandle("proj");
    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    depthA = material->ComputeCB0FieldHandle("depthA");
    depthB = material->ComputeCB0FieldHandle("depthB");
    zNear = material->ComputeCB0FieldHandle("zNear");
    zFar = material->ComputeCB0FieldHandle("zFar");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
}

void Scene::CBHandleCache::BlurHandles::Populate(Material* material)
{
    *this = {};
    if (!material) { return; }

    dir = material->ComputeCB0FieldHandle("dir");
    radius = material->ComputeCB0FieldHandle("radius");
}

void Scene::CBHandleCache::ComposeHandles::Populate(Material* material)
{
    *this = {};
    if (!material) { return; }

    invView = material->ComputeCB0FieldHandle("invView");
    invProj = material->ComputeCB0FieldHandle("invProj");
    skyboxIntensity = material->ComputeCB0FieldHandle("skyboxIntensity");
    camPos = material->ComputeCB0FieldHandle("camPosWS");
    screenSize = material->ComputeCB0FieldHandle("screenSize");
    invScreenSize = material->ComputeCB0FieldHandle("invScreenSize");
}

const mat4& Scene::GetCascadeView(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedLightView_[index];
}

const mat4& Scene::GetCascadeProj(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedLightProj_[index];
}

float2 Scene::GetCascadeScale(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedScale_[index];
}

float2 Scene::GetCascadeBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedBias_[index];
}

float Scene::GetCascadeNormalBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedNormalBiasWS_[index];
}

float Scene::GetCascadeDepthBias(size_t index) const
{
    assert(index < static_cast<size_t>(kCascades));
    return cachedDepthBiasNDC_[index];
}

void Scene::RefreshCachedHandles(Renderer* renderer)
{
    cbHandles_ = {};

    if (matLighting_)
    {
        cbHandles_.lighting.Populate(matLighting_.get());
    }
    if (matPointLightCS_)
    {
        cbHandles_.pointLights.Populate(matPointLightCS_.get());
    }
    if (matSpotLightCS_)
    {
        cbHandles_.spotLights.Populate(matSpotLightCS_.get());
    }
    if (matComposeCS_)
    {
        cbHandles_.compose.Populate(matComposeCS_.get());
    }
    if (matSSR_)
    {
        cbHandles_.ssr.Populate(matSSR_.get());
    }
    if (matBlur_)
    {
        cbHandles_.blur.Populate(matBlur_.get());
    }

    for (auto& obj : objects_)
    {
        if (obj)
        {
            obj->OnMaterialHotReload(renderer);
        }
    }

    if (skyBox_)
    {
        skyBox_->OnMaterialHotReload(renderer);
    }
}

static void BuildFrustumSliceCornersWS(const mat4& invView, const mat4& invProj,
    float zNearVS, float zFarVS, std::array<float3, 8>& outCornersWS)
{
    const float2 ndc[4] = { {-1,-1}, {+1,-1}, {+1,+1}, {-1,+1} };
    int idx = 0;
    for (int i = 0; i < 4; ++i)
    {
        // Take the ray from the camera toward the frustum corner (at z=1 in view space)
        float4 farVS = invProj.Transform(float4(ndc[i].x, ndc[i].y, 1.0f, 1.0f));
        float3 dirVS = farVS.xyz() / farVS.w; // direction on the far plane

        // Point at the desired depth z: scale the ray so its z matches the required depth
        float nz = std::max(1e-6f, dirVS.z);
        float3 nearVS = dirVS * (zNearVS / nz);
        float3 farV = dirVS * (zFarVS / nz);

        // Transform into world space
        float3 nearWS = (invView * float4(nearVS, 1)).xyz();
        float3 farWS = (invView * float4(farV, 1)).xyz();

        outCornersWS[idx++] = nearWS;
        outCornersWS[idx++] = farWS;
    }
}

class RotatingObject : public StaticMesh {
public:
    RotatingObject(
        const std::string& modelName,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader,
        float3 pos,
        float3 scale,
        float angSpeed = 10.0f * DEG2RAD)
        :StaticMesh(modelName, matPreset, inputLayout, graphicsShader), angularSpeed_(angSpeed)
    {
        SetPosition(pos);
        SetScale(scale);
    }

    void Tick(float deltaTime) override {
        rotationY_ += angularSpeed_ * deltaTime;
        if (rotationY_ > XM_2PI) {
            rotationY_ -= XM_2PI;
        }

        SetRotationEulerRad({ 0.0f, rotationY_, 0.0f });
    }

    float GetRotationY() const { return rotationY_; }
    void SetRotationY(float angle) { rotationY_ = angle; }

private:
    float rotationY_ = 0.0f;
    float angularSpeed_ = 10.0f * Math::DEG2RAD;
};

void Scene::InitAll(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    cbHandles_ = {};

    if (!matLighting_) {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/lighting_cs.hlsl";
        cd.csEntry = "CSMain";
        matLighting_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    }

    if (!matPointLightCS_) {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/pointlight_cs.hlsl";
        cd.csEntry = "CSMain";
        matPointLightCS_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    }

    if (!matSpotLightCS_) {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/spotlight_cs.hlsl";
        cd.csEntry = "CSMain";
        matSpotLightCS_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    }

    if (!matComposeCS_) {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/compose_cs.hlsl";
        cd.csEntry = "CSMain";
        matComposeCS_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    }

    if (!matTonemapCS_) {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/tonemap_cs.hlsl";
        cd.csEntry = "CSMain";
        matTonemapCS_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    }

    if (!matSSR_) {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/ssr_cs.hlsl";
        cd.csEntry = "CSMain";
        matSSR_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    }

    if (!matBlur_) {
        Material::ComputeDesc cd{};
        cd.shaderFile = L"shaders/ssr_blur_cs.hlsl";
        cd.csEntry = "CSMain";
        matBlur_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, cd);
    }

    if (!matDebug_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/debug_texture.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.inputLayoutKey = "";
        gd.numRT = 1; gd.rtvFormats[0] = renderer->GetBackbufferFormat();
        gd.dsvFormat = DXGI_FORMAT_UNKNOWN;
        gd.depth.DepthEnable = FALSE;
        matDebug_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }

    skyBox_ = std::make_unique<Skybox>(L"textures/skybox.dds");
    skyBox_->Init(renderer, uploadCmdList, uploadKeepAlive);
    skyBox_->SetExposure(0.2f);

    auto& pointLights = lightManager_.PointLights();
    //pointLights.emplace_back(); pointLights.back().SetDesc({ {0,2,0}, 6.0f, {1,0.8f,0.6f}, 5.0f });
    //pointLights.emplace_back(); pointLights.back().SetDesc({ {-4,1,-2}, 5.0f, {0.6f,0.7f,1.0f}, 8.0f });

    SpotLightDesc warmSpot{};
    warmSpot.position = float3(4.0f, 4.0f, -3.0f);
    warmSpot.direction = float3(-1.0f, -1.0f, 0.0f).Normalized();
    warmSpot.range = 20.0f;
    warmSpot.innerAngle = XMConvertToRadians(18.0f);
    warmSpot.outerAngle = XMConvertToRadians(28.0f);
    warmSpot.color = float3(1.0f, 0.85f, 0.6f);
    warmSpot.intensity = 15.0f;
    warmSpot.shadowNormalBias = 0.05f;
    warmSpot.shadowDepthBias = 0.0001f;
    auto& spotLights = lightManager_.SpotLights();
    spotLights.push_back({});
    spotLights.back().SetDesc(warmSpot);

    SpotLightDesc coolSpot{};
    coolSpot.position = float3(-5.0f, 5.0f, -6.0f);
    coolSpot.direction = float3(0.5f, -1.0f, 0.25f).Normalized();
    coolSpot.range = 25.0f;
    coolSpot.innerAngle = XMConvertToRadians(20.0f);
    coolSpot.outerAngle = XMConvertToRadians(32.0f);
    coolSpot.color = float3(0.6f, 0.8f, 1.0f);
    coolSpot.intensity = 18.0f;
    coolSpot.shadowNormalBias = 0.05f;
    coolSpot.shadowDepthBias = 0.0001f;
    spotLights.push_back({});
    spotLights.back().SetDesc(coolSpot);

    dirLight_ = { float3(-1.5f, -0.7f, -0.5f).Normalized() , {1,1,1}, 1.0f, 0.05f };
    //dirLight_.exposure *= 0.02f;
    //dirLight_.ambient *= 0.02f;

    {
        auto box = std::make_unique<RotatingObject>("models/box.obj", "damaged_plaster", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(0.0f, 0.5f, -2.0f), float3(1, 1, 1));
        box->MaterialParamsRef().texFlags.w = 1;
        //box->MaterialParamsRef().SetUseMR(false);
        //box->MaterialParamsRef().metalRough = float2(0.0f, 0.8f);
        AddObject(std::move(box));

        box = std::make_unique<RotatingObject>("models/box.obj", "damaged_plaster", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(0.0f, 0.5f, -4.0f), float3(1, 1, 1), 0.0f);
        AddObject(std::move(box));
    }
    AddObject(std::make_unique<RotatingObject>("models/teapot.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(-1.0f, 0.5f, -1.0f), float3(1, 1, 1)));
    AddObject(std::make_unique<RotatingObject>("models/sphere.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(-3.0f, 0.5f, -1.0f), float3(1, 1, 1)));
    AddObject(std::make_unique<RotatingObject>("models/corgi.obj", "brick", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(3.0f, 0.5f, -1.0f), float3(1, 1, 1)));

    {
        auto glass = std::make_unique<GlassCube>(this, "models/box.obj", float3(-1.8f, 0.4f, -4.2f), float3(0.6f, 0.6f, 0.6f), 0.0f);
        glass->SetTint(float3(0.78f, 0.9f, 1.0f));
        glass->SetAbsorption(float3(0.16f, 0.07f, 0.03f));
        glass->SetThickness(0.65f);
        glass->SetReflectionStrength(1.25f);
        glass->SetRefractionDistortion(0.02f);
        glass->SetRoughness(0.05f);
        glass->SetIor(1.1f);
        glass->SetNormalMap(L"textures/damaged_plaster_normal.dds");
        AddObject(std::move(glass));

        glass = std::make_unique<GlassCube>(this, "models/sphere.obj", float3(-1.8f, 0.5f, -2.2f), float3(0.8f, 0.8f, 0.8f), 0.0f);
        glass->SetTint(float3(0.78f, 0.9f, 1.0f));
        glass->SetAbsorption(float3(0.16f, 0.07f, 0.03f));
        glass->SetThickness(0.65f);
        glass->SetReflectionStrength(4.25f);
        glass->SetRefractionDistortion(0.02f);
        glass->SetRoughness(0.05f);
        glass->SetIor(1.1f);
        glass->SetNormalMap(L"textures/damaged_plaster_normal.dds");
        AddObject(std::move(glass));
    }

    {
        auto floor = std::make_unique<StaticMesh>("models/box.obj", "sandstone_cracks", "PosNormTanUV", L"shaders/gbuffer.hlsl");
        floor->MaterialParamsRef().texOffsScale = float4(0.0f, 0.0f, 20.0f, 20.0f);
        floor->SetPosition(float3(0.0f, -0.5f, 0.0f));
        floor->SetScale(float3(40.0f, 1.0f, 40.0f));
        AddObject(std::move(floor));
            
        floor = std::make_unique<StaticMesh>("models/box.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl");
        floor->MaterialParamsRef().texOffsScale = float4(0.5f, 0.0f, 10.0f, 10.0f);
        floor->MaterialParamsRef().texFlags.w = 0.01f;
        floor->SetPosition(float3(-5.0f, -0.4f, 0.0f));
        floor->SetScale(float3(5.0f, 1.0f, 5.0f));
        AddObject(std::move(floor));
    }

    {
        int width = 10;
        int height = 5;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                auto sphere = std::make_unique<StaticMesh>("models/sphere.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl");
                sphere->MaterialParamsRef().SetUseMR(false);
                sphere->MaterialParamsRef().metalRough = float2((float)y / (height - 1), (float)x / (width - 1));
                sphere->SetPosition(float3((float)x + x * (0.2f), (float)y + y *(0.2f) + 1.0f, -(float)x + x * (0.2f) -12.0f));
                AddObject(std::move(sphere));
            }
        }
    }

    AddObject(std::make_unique<GpuInstancedModels>("models/teapot.obj", 100, "bronze", "PosNormTanUV", L"shaders/gbuffer_inst.hlsl", L"shaders/instance_anim.hlsl"));

    //AddObject(std::make_unique<OceanRenderable>(&camera_));

    AddObject(std::make_unique<DebugGrid>(100.0f));

    camera_.SetPosition({ 0.f, 1.f, -10.f });

    for (auto& obj : objects_)
    {
        obj->Init(renderer, uploadCmdList, uploadKeepAlive);
    }

    RefreshCachedHandles(renderer);
}

void Scene::AddObject(std::unique_ptr<RenderableObjectBase> obj) {
    objects_.push_back(std::move(obj));
}

void Scene::Tick(float deltaTime) {
    CPU_SCOPE(ProfilerScopes::kSceneTick);

    auto& input = Systems::GetInput();
    camera_.UpdateFromInput(deltaTime);

    if (input.WasActionPressed("DebugTex"))
    {
        debugTexMode_ = !debugTexMode_;
    }
    if (input.WasActionPressed("ToggleProfiler"))
    {
        showProfiler_ = !showProfiler_;
    }
#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    size_t batchSize = 32;
    TaskSystem::ParallelFor(objects_.size(),
        [this, deltaTime](size_t index) {
            if (index >= objects_.size()) {
                return;
            }
            objects_[index]->Tick(deltaTime);
        }, batchSize);

    TaskSystem::ParallelFor(objects_.size(),
        [this, deltaTime](size_t index) {
            if (index >= objects_.size()) {
                return;
            }
            objects_[index]->PostTick(deltaTime);
        }, batchSize);
#else
    for (auto& obj : objects_)
    {
        obj->Tick(deltaTime);
    }

    for (auto& obj : objects_)
    {
        obj->PostTick(deltaTime);
    }
#endif
}

void Scene::Render(Renderer* renderer) {
    if (!renderer) {
        return;
    }
    CPU_SCOPE(ProfilerScopes::kSceneRender);

    if (Systems::GetInput().WasActionPressed("Wireframe")) {
        renderer->SetWireframeMode(!renderer->GetWireframeMode());
    }

    if (renderer->ConsumeMaterialHotReloadFlag())
    {
        RefreshCachedHandles(renderer);
    }

    auto* tb = renderer->GetTextManager();
    tb->Begin(renderer->GetWidth(), renderer->GetHeight(), 1.0f);
    int y = 8;
    tb->AddTextfShadow(8, 8, 32.0f, float4(1, 1, 1, 0.6f), true, L"FPS:%.0f MS:%0.2f", renderer->GetFPS(), 1000.0f / renderer->GetFPS());
    //tb->AddText(8, 8 + 32, 10.0f, float4(1, 1, 1, 0.9f), L"The quick brown fox jumps over the lazy dog 0123456789", false);
    //tb->AddText(8, 8 + 32 + 32, 16.0f, float4(1, 1, 1, 0.9f), L"The quick brown fox jumps over the lazy dog 0123456789", true);
    //tb->AddText(8, 8 + 32 + 32 + 32, 64.0f, float4(1, 1, 1, 0.9f), L"The quick brown fox jumps over the lazy dog 0123456789", true);
    

    renderer->BeginSubmitTimeline();

    TaskSystem::Get().WaitForTrackedAsyncTasks();

    lightManager_.UpdateSpotLightCache();

    // Frame matrices and camera/light parameters (mirrors your setup)
    const float aspect = float(renderer->GetWidth()) / float(renderer->GetHeight());
    const mat4 view = camera_.GetViewMatrix();
    constexpr float HFOV = XMConvertToRadians(90.f);
    const float VFOV = 2.f * atan(tan(HFOV * 0.5f) / aspect);
    const float zNear = 0.01f, zFar = 1000.0f;
    const mat4 proj = mat4::PerspectiveFovLH(VFOV, aspect, zNear, zFar);
    const mat4 invView = mat4::Inverse(view);
    const mat4 invProj = mat4::Inverse(proj);
    const float3 camDir = invView.TransformDirection(float3(0, 0, 1)).Normalized();

    // bucketize renderables into the persistent scratch arrays to avoid per-frame allocations
    for (auto& bucket : renderBuckets_) {
        bucket.clear();
    }
    for (const auto& obj : objects_) {
        if (!obj) {
            continue;
        }
        const bool tr = obj->IsTransparent();
        const bool simple = obj->IsSimpleRender();
        const ObjectRenderType key = tr
            ? (simple ? ObjectRenderType::TransparentSimple : ObjectRenderType::TransparentComplex)
            : (simple ? ObjectRenderType::OpaqueSimple : ObjectRenderType::OpaqueComplex);
        renderBuckets_[ToIndex(key)].push_back(obj.get());
    }
    const auto& buckets = renderBuckets_;

    RenderGraph rg;
    auto pClear = rg.AddPass("PrologueClear", {},
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassPrologueClear); Pass_PrologueClear(renderer, ctx); });

    auto pShadow = rg.AddPass("CSM", { pClear },
        [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar, camDir, &buckets]
        (RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCSM);
            Pass_CSM(renderer, ctx, view, proj, invView, invProj, zNear, zFar, camDir, buckets);
        });

    auto pSpotShadow = rg.AddPass("SpotShadows", { pShadow },
        [this, renderer, &buckets](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotShadow);
            Pass_SpotShadows(renderer, ctx, buckets);
        });

    auto pGbuf = rg.AddPass("GBuffer", { pSpotShadow },
        [this, renderer, &view, &proj, &buckets](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassGBuffer);
            Pass_GBuffer(renderer, ctx, view, proj, buckets);
        });

    auto pLight = rg.AddPassMT("Lighting", { pGbuf }, { pShadow },
        [this, renderer, &view, &proj, &invView, &invProj, camDir](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassLighting);
            Pass_Lighting(renderer, ctx, view, proj, invView, invProj, camDir);
        });

    auto pSpotLights = rg.AddPass("SpotLights", { pLight },
        [this, renderer, &invView, &invProj](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSpotLights);
            Pass_SpotLights(renderer, ctx, invView, invProj);
        });

    auto pPointLights = rg.AddPass("PointLights", { pSpotLights },
        [this, renderer, &view, &proj, &invView, &invProj](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassPointLights);
            Pass_PointLights(renderer, ctx, invView, invProj);
        });

    auto pSky = rg.AddPass("Skybox", { pPointLights },
        [this, renderer, &view, &proj](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSkybox);
            Pass_Skybox(renderer, ctx, view, proj);
        });

    auto pSSR = rg.AddPass("SSR", { pSky },
        [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassSSR);
            Pass_SSR(renderer, ctx, view, proj, invView, invProj, zNear, zFar);
        });

    auto pBlur = rg.AddPass("SSR.Blur", { pSSR },
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassSSRBlur); Pass_SSR_Blur(renderer, ctx); });

    auto pCompose = rg.AddPass("Compose", { pBlur },
        [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassCompose);
            Pass_Compose(renderer, ctx, view, proj, invView, invProj, zNear, zFar);
        });

    auto pTransp = rg.AddPass("Transparent", { pCompose },
        [this, renderer, &view, &proj, &buckets](RenderGraph::PassContext ctx) {
            CPU_SCOPE(ProfilerScopes::kPassTransparent);
            Pass_Transparent(renderer, ctx, view, proj, buckets);
        });

    auto pTone = rg.AddPass("Tonemap", { pTransp },
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassTonemap); Pass_Tonemap(renderer, ctx); });

    rg.AddPass("Debug", { pTone },
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassDebug); Pass_Debug(renderer, ctx); });

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    rg.ExecuteParallel(renderer, TaskSystem::Get());
#else
    rg.Execute(renderer);
#endif

    {
        CPU_SCOPE(ProfilerScopes::kFrameAsyncWait);
        TaskSystem::Get().WaitForTrackedAsyncTasks();
    }

    RenderGraph epilogueRG;
    epilogueRG.AddPass("Overlay", {},
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE(ProfilerScopes::kPassOverlay); Pass_Overlay(renderer, ctx); });
    epilogueRG.Execute(renderer);
    
    {
        CPU_SCOPE(ProfilerScopes::kOverlayAsyncWait);
        TaskSystem::Get().WaitForTrackedAsyncTasks();
    }
    renderer->EndFrame();
}

void Scene::RenderObjectBatch(Renderer* renderer,
    const std::vector<RenderableObjectBase*>& objects,
    size_t batchIndex,
    const mat4& view, const mat4& proj,
    bool useBundles,
    bool bindGbufOrScene,
    size_t chunkSize)
{
    if (objects.empty()) {
        return;
    }

    //chunkSize = 16;

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();

    auto renderJob = [renderer, &view, &proj, &objects, useBundles, chunkSize, batchIndex, bindGbufOrScene](std::size_t jobIndex)
    {
        CPU_SCOPE(ProfilerScopes::kRenderObjectBatchAsync);
        const size_t begin = jobIndex * chunkSize;
        const size_t end = std::min(begin + chunkSize, objects.size());

        if (useBundles) {
            auto b = renderer->BeginThreadCommandBundle(nullptr);
            for (size_t i = begin; i < end; ++i) {
                if (auto* obj = objects[i]) {
                    obj->Render(renderer, b.cl, view, proj);
                }
            }
            renderer->EndThreadCommandBundle(b, batchIndex);
        }
        else {
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            {
                GPU_SCOPE(t.cl, ProfilerScopes::kRenderObjectBatchGpu);
                if (bindGbufOrScene)
                {
                    renderer->BindGBuffer(t.cl, Renderer::ClearMode::None); // no clear!
                }
                else
                {
                    renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);
                }

                for (size_t i = begin; i < end; ++i) {
                    if (auto* obj = objects[i]) {
                        obj->Render(renderer, t.cl, view, proj);
                    }
                }
            }
            renderer->EndThreadCommandList(t, batchIndex);
        }
    };

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    tasks.DispatchTrack((N + chunkSize - 1) / chunkSize, renderJob, 1);
#else
    (void)tasks;
    const size_t jobCount = (N + chunkSize - 1) / chunkSize;
    for (size_t jobIndex = 0; jobIndex < jobCount; ++jobIndex) {
        renderJob(jobIndex);
    }
#endif
}

void Scene::RenderShadowBatch(Renderer* renderer,
    const std::vector<RenderableObjectBase*>& objects,
    size_t batchIndex,
    const mat4& lightView, const mat4& lightProj,
    UINT cascadeIndex, size_t chunkSize)
{
    if (objects.empty())
    {
        return;
    }

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();
    if (chunkSize == 0)
    {
        chunkSize = 16;
    }

    auto shadowJob = [renderer, &objects, &lightView, &lightProj, cascadeIndex, chunkSize, batchIndex](std::size_t jobIndex)
    {
        CPU_SCOPE(ProfilerScopes::kRenderShadowBatchAsync);
        const size_t begin = jobIndex * chunkSize;
        const size_t end = std::min(begin + chunkSize, objects.size());

        // Each chunk uses its own DIRECT command list
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        t.cl->SetName(L"RenderShadowBatch");
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kRenderShadowBatchGpu);

            // Important: bind the correct atlas tile for the cascade without clearing
            renderer->BindShadowTarget(t.cl, cascadeIndex, /*clear=*/false);

            for (size_t i = begin; i < end; ++i) {
                if (auto* obj = objects[i]) {
                    obj->RenderShadow(renderer, t.cl, lightView, lightProj);
                }
            }
        }
        renderer->EndThreadCommandList(t, batchIndex);
    };

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    tasks.DispatchTrack((N + chunkSize - 1) / chunkSize, shadowJob, 1);
#else
    (void)tasks;
    const size_t jobCount = (N + chunkSize - 1) / chunkSize;
    for (size_t jobIndex = 0; jobIndex < jobCount; ++jobIndex) {
        shadowJob(jobIndex);
    }
#endif
}

void Scene::Pass_PrologueClear(Renderer* r, RenderGraph::PassContext ctx)
{
    auto t = r->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPrologueClear);
        r->RecordBindAndClear(t.cl);
    }
    r->EndThreadCommandList(t, ctx.batchIndex);
}

//#define PARALLEL_SHADOW_BATCH 1
#define PARALLEL_SHADOW_CASCADES 1
void Scene::Pass_CSM(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj, const mat4& invView, const mat4& invProj,
    float zNear, float zFar, const float3& camDir,
    const ObjectBuckets& buckets)
{
    auto d = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    d.cl->SetName(L"CSM");
    {
#if PARALLEL_SHADOW_BATCH || PARALLEL_SHADOW_CASCADES
		{
#endif
        GPU_SCOPE(d.cl, ProfilerScopes::kPassCSM);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(d.cl, D.shadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        renderer->BindShadowTarget(d.cl, 0, /*clear=*/true);

#if PARALLEL_SHADOW_BATCH || PARALLEL_SHADOW_CASCADES
        }
        renderer->EndThreadCommandList(d, ctx.batchIndex);
#endif

        const float shadowMaxDistance = 300.0f;
        const float zFarShadow = std::min(zFar, shadowMaxDistance);
        size_t batchIndex = ctx.batchIndex;

        // Split distances (hard-coded to match your setup)
        cachedSplitsVS_[0] = zNear;
        cachedSplitsVS_[1] = 10.0f;
        cachedSplitsVS_[2] = 30.0f;
        cachedSplitsVS_[3] = 100.0f;
        cachedSplitsVS_[4] = zFarShadow;

        TaskSystem& tasks = TaskSystem::Get();
        auto csmJob = [this, &d, renderer, &buckets, &invView, &invProj, &proj, camDir, sunDirWS = dirLight_.dir, batchIndex](std::size_t idx)
            {
                CPU_SCOPE(ProfilerScopes::kCSMPerCascade);
                const auto& D = renderer->GetDeferredForFrame();

                float sliceNear = cachedSplitsVS_[idx], sliceFar = cachedSplitsVS_[idx + 1];
                const UINT  tileRes = D.shadowRes / 2;

                // Eight frustum corners (using your helper)
                std::array<float3, 8> cornersWS;
                BuildFrustumSliceCornersWS(invView, invProj, sliceNear, sliceFar, cornersWS);

                const float tanH = 1.0f / proj.m._11;
                const float tanV = 1.0f / proj.m._22;

                const float halfSlice = 0.5f * (sliceFar - sliceNear);
                const float farCoef = (sliceFar * tanH) * (sliceFar * tanH) + (sliceFar * tanV) * (sliceFar * tanV);

                const float kForward = 1.0f;
                float delta = kForward * halfSlice;

                auto radiusFor = [&](float d) {
                    const float rf2 = farCoef + (halfSlice - d) * (halfSlice - d);
                    return std::sqrt(rf2);
                    };
                const float overlap = 2.0f;
                float radius = radiusFor(delta) + overlap; // +padding
                //radius -= halfSlice;

                const float3 camPos = camera_.GetPosition();
                float3 center = camPos + camDir * (sliceNear + halfSlice + delta);
                float spatialStep = radius * 0.1f;
                center = Floor(center / spatialStep) * spatialStep;

                // Light view matrix
                const float3 up(0, 1, 0);
                mat4 lightView = mat4::LookAtLH(center - sunDirWS * 300.0f, center, up);

                // AABB along Z + stabilize XY
                float2 centerLS = (lightView * float4(center, 1)).xy();
                float minZ = +1e9f, maxZ = -1e9f, rLS = 0.0f;
                for (int k = 0; k < 8; ++k) {
                    float3 ls = (lightView * float4(cornersWS[k], 1)).xyz();
                    rLS = std::max(rLS, std::max(std::abs(ls.x - centerLS.x), std::abs(ls.y - centerLS.y)));
                    minZ = std::min(minZ, ls.z);
                    maxZ = std::max(maxZ, ls.z);
                }
                radius = std::min(radius, rLS);

                float unitsPerTexel = (2.0f * radius) / float(tileRes);
                centerLS.x = floor(centerLS.x / unitsPerTexel) * unitsPerTexel;
                centerLS.y = floor(centerLS.y / unitsPerTexel) * unitsPerTexel;

                float minX = centerLS.x - radius, maxX = centerLS.x + radius;
                float minY = centerLS.y - radius, maxY = centerLS.y + radius;

                const float zPad = 25.0f;
                float nearLS = std::max(0.001f, minZ - zPad);
                float farLS = maxZ + zPad;

                mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearLS, farLS);

                const float normalBiasInTexels = 0.75f;
                const float depthBiasInTexels = 2.0f;
                cachedNormalBiasWS_[idx] = normalBiasInTexels * unitsPerTexel;
                cachedDepthBiasNDC_[idx] = (depthBiasInTexels * unitsPerTexel) / (farLS - nearLS);

                const float2 scale = float2(float(tileRes) / float(D.shadowRes));
                const float2 bias = float2((idx % 2) * scale.x, (idx / 2) * scale.y);
                cachedScale_[idx] = scale; cachedBias_[idx] = bias;
                cachedLightView_[idx] = lightView; cachedLightProj_[idx] = lightProj;

#if PARALLEL_SHADOW_BATCH
                const auto& opaqueSimple = buckets[ToIndex(ObjectRenderType::OpaqueSimple)];
                if (!opaqueSimple.empty())
                {
                    RenderShadowBatch(renderer, opaqueSimple, batchIndex, cachedLightView_[idx], cachedLightProj_[idx], (UINT)idx, /*chunk*/64);
                }
                const auto& opaqueComplex = buckets[ToIndex(ObjectRenderType::OpaqueComplex)];
                if (!opaqueComplex.empty())
                {
                    RenderShadowBatch(renderer, opaqueComplex, batchIndex, cachedLightView_[idx], cachedLightProj_[idx], (UINT)idx, /*chunk*/64);
                }
#else
                const auto& opaqueSimple = buckets[ToIndex(ObjectRenderType::OpaqueSimple)];
                const auto& opaqueComplex = buckets[ToIndex(ObjectRenderType::OpaqueComplex)];

#if PARALLEL_SHADOW_CASCADES
                auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
				{
                    //GPU_SCOPE(t.cl, ProfilerScopes::kCSMPerCascade);
					renderer->BindShadowTarget(t.cl, (int)idx, /*clear=*/false);

                	for (auto obj : opaqueComplex)
                	{
                		obj->RenderShadow(renderer, t.cl, lightView, lightProj);
                	}

                	for (auto obj : opaqueSimple)
                	{
                		obj->RenderShadow(renderer, t.cl, lightView, lightProj);
                	}
				}
                renderer->EndThreadCommandList(t, batchIndex);
#else
                renderer->BindShadowTarget(d.cl, (int)idx, /*clear=*/false);

                for (auto obj : opaqueComplex)
                {
                    obj->RenderShadow(renderer, d.cl, lightView, lightProj);
                }

                for (auto obj : opaqueSimple)
                {
                    obj->RenderShadow(renderer, d.cl, lightView, lightProj);
                }
#endif

#endif
            };

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION && (PARALLEL_SHADOW_BATCH || PARALLEL_SHADOW_CASCADES)
        tasks.DispatchWait(kCascades, csmJob, 1);
#else
        (void)tasks;
        for (size_t idx = 0; idx < kCascades; ++idx) {
            csmJob(idx);
        }
        
#endif
    }
#if !PARALLEL_SHADOW_BATCH && !PARALLEL_SHADOW_CASCADES
    renderer->EndThreadCommandList(d, ctx.batchIndex);
#endif

}

void Scene::Pass_SpotShadows(Renderer* renderer, RenderGraph::PassContext ctx,
    const ObjectBuckets& buckets)
{
    const size_t spotLightCount = lightManager_.GetSpotLightCount();
    if (spotLightCount == 0)
    {
        return;
    }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotShadow);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(t.cl, D.spotShadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

        const auto& opaqueSimple = buckets[ToIndex(ObjectRenderType::OpaqueSimple)];
        const auto& opaqueComplex = buckets[ToIndex(ObjectRenderType::OpaqueComplex)];

        for (size_t lightIndex = 0; lightIndex < spotLightCount; ++lightIndex)
        {
            renderer->BindSpotShadowTarget(t.cl, static_cast<UINT>(lightIndex), /*clearDepth=*/true);

            const mat4& lightView = lightManager_.GetSpotView(lightIndex);
            const mat4& lightProj = lightManager_.GetSpotProj(lightIndex);

            for (auto* obj : opaqueSimple)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, lightView, lightProj);
                }
            }

            for (auto* obj : opaqueComplex)
            {
                if (obj)
                {
                    obj->RenderShadow(renderer, t.cl, lightView, lightProj);
                }
            }
        }
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_GBuffer(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const ObjectBuckets& buckets)
{
    RenderGraph rgGB(ctx.batchIndex);
    rgGB.AddPass("GBuffer.Driver", {}, [renderer](RenderGraph::PassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        {
            GPU_SCOPE(driver.cl, ProfilerScopes::kGBufferDriver);

            const auto& D = renderer->GetDeferredForFrame();
            renderer->Transition(driver.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

            renderer->BindGBuffer(driver.cl, Renderer::ClearMode::ColorDepth);
        }
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    // 1.2 Opaque simple → bundles
    rgGB.AddPass("GBuffer.OpaqueSimple", {}, [this, renderer, &view, &proj, &buckets](RenderGraph::PassContext sub) {
        const auto& opaqueSimple = buckets[ToIndex(ObjectRenderType::OpaqueSimple)];
        if (!opaqueSimple.empty())
        {
            RenderObjectBatch(renderer, opaqueSimple, sub.batchIndex, view, proj, /*useBundles=*/true, true, 32);
        }
        });

    // 1.3 Opaque complex → direct command list, no clears
    rgGB.AddPass("GBuffer.OpaqueComplex", {}, [this, renderer, &view, &proj, &buckets](RenderGraph::PassContext sub) {
        const auto& opaqueComplex = buckets[ToIndex(ObjectRenderType::OpaqueComplex)];
        if (!opaqueComplex.empty())
        {
            RenderObjectBatch(renderer, opaqueComplex, sub.batchIndex, view, proj, /*useBundles=*/false, true, 32);
        }
        });

    rgGB.Execute(renderer);
}

void Scene::Pass_Lighting(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const mat4& invView, const mat4& invProj,
    const float3& camDir)
{
    (void)proj;
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassLighting);
        const auto& D = renderer->GetDeferredForFrame();
        const D3D12_RESOURCE_STATES srvState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        renderer->Transition(t.cl, D.gb0.Get(), srvState);
        renderer->Transition(t.cl, D.gb1.Get(), srvState);
        renderer->Transition(t.cl, D.gb2.Get(), srvState);
        renderer->Transition(t.cl, D.depth.Get(), srvState);
        renderer->Transition(t.cl, D.shadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto cb = renderer->GetFrameResource()->AllocDynamic(matLighting_->GetCBSizeBytesAligned(0, 256), 256);
        const auto& handles = cbHandles_.lighting;

        matLighting_->UpdateCBField(handles.sunDir, dirLight_.dir, (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.ambient, dirLight_.ambient, (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.lightRgb, dirLight_.color, (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.exposure, dirLight_.exposure, (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.camPos, camera_.GetPosition(), (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.camDir, camDir, (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.view, view, (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.invView, invView, (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.invProj, invProj, (uint8_t*)cb.cpu);

        matLighting_->UpdateCBField(handles.lightViewProj, (cachedLightView_[0] * cachedLightProj_[0]), (uint8_t*)cb.cpu, 0);
        matLighting_->UpdateCBField(handles.lightViewProj, (cachedLightView_[1] * cachedLightProj_[1]), (uint8_t*)cb.cpu, 1);
        matLighting_->UpdateCBField(handles.lightViewProj, (cachedLightView_[2] * cachedLightProj_[2]), (uint8_t*)cb.cpu, 2);
        matLighting_->UpdateCBField(handles.lightViewProj, (cachedLightView_[3] * cachedLightProj_[3]), (uint8_t*)cb.cpu, 3);

        matLighting_->UpdateCBField(handles.cascadeScaleBias, float4(cachedScale_[0].x, cachedScale_[0].y, cachedBias_[0].x, cachedBias_[0].y), (uint8_t*)cb.cpu, 0);
        matLighting_->UpdateCBField(handles.cascadeScaleBias, float4(cachedScale_[1].x, cachedScale_[1].y, cachedBias_[1].x, cachedBias_[1].y), (uint8_t*)cb.cpu, 1);
        matLighting_->UpdateCBField(handles.cascadeScaleBias, float4(cachedScale_[2].x, cachedScale_[2].y, cachedBias_[2].x, cachedBias_[2].y), (uint8_t*)cb.cpu, 2);
        matLighting_->UpdateCBField(handles.cascadeScaleBias, float4(cachedScale_[3].x, cachedScale_[3].y, cachedBias_[3].x, cachedBias_[3].y), (uint8_t*)cb.cpu, 3);

        matLighting_->UpdateCBField(handles.cascadeSplits, float4(cachedSplitsVS_[0], cachedSplitsVS_[1], cachedSplitsVS_[2], cachedSplitsVS_[3]), (uint8_t*)cb.cpu);
        const float shadowRes = static_cast<float>(renderer->GetDeferredForFrame().shadowRes);
        matLighting_->UpdateCBField(handles.shadowAtlasSize, float2(shadowRes, shadowRes), (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.shadowBiasNDC, float4(cachedDepthBiasNDC_[0], cachedDepthBiasNDC_[1], cachedDepthBiasNDC_[2], cachedDepthBiasNDC_[3]), (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.normalBiasWS, float4(cachedNormalBiasWS_[0], cachedNormalBiasWS_[1], cachedNormalBiasWS_[2], cachedNormalBiasWS_[3]), (uint8_t*)cb.cpu);

        const float width = static_cast<float>(std::max(renderer->GetWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetHeight(), 1u));
        const float2 screenSize = float2(width, height);
        const float2 invScreen = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);
        matLighting_->UpdateCBField(handles.screenSize, screenSize, (uint8_t*)cb.cpu);
        matLighting_->UpdateCBField(handles.invScreenSize, invScreen, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();
        rc.ClearFast();

        rc.cbv[0] = cb.gpu;
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> srvs = {
            D.gbSRV[0],
            D.gbSRV[1],
            D.gbSRV[2],
            D.gbSRV[3],
            D.shadowSRV
        };
        rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.lightUAV }).gpu;
        const auto samplerDescs = std::array{ *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        matLighting_->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }
        renderer->UAVBarrier(t.cl, D.light.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_SpotLights(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& invView, const mat4& invProj)
{
    const size_t spotLightCount = lightManager_.GetSpotLightCount();
    if (spotLightCount == 0)
    {
        return;
    }

    lightManager_.EnsureSpotLightBuffer(renderer, spotLightCount);
    auto* spotLightBufferCPU = lightManager_.GetSpotLightBufferCPU();
    const D3D12_CPU_DESCRIPTOR_HANDLE spotLightSrvHandle = lightManager_.GetSpotLightSrv();
    if (!spotLightBufferCPU || spotLightSrvHandle.ptr == 0)
    {
        return;
    }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSpotLights);
        const auto& D = renderer->GetDeferredForFrame();
        const D3D12_RESOURCE_STATES srvState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(t.cl, D.gb0.Get(), srvState);
        renderer->Transition(t.cl, D.gb1.Get(), srvState);
        renderer->Transition(t.cl, D.gb2.Get(), srvState);
        renderer->Transition(t.cl, D.depth.Get(), srvState);
        renderer->Transition(t.cl, D.spotShadow.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        const auto& spotLights = lightManager_.SpotLights();
        for (size_t i = 0; i < spotLightCount; ++i)
        {
            const auto& desc = spotLights[i].GetDesc();
            const mat4 viewProj = lightManager_.GetSpotView(i) * lightManager_.GetSpotProj(i);
            const float3 dir = lightManager_.GetSpotDirection(i);

            spotLightBufferCPU[i].positionRange = float4(desc.position, desc.range);
            spotLightBufferCPU[i].directionCosOuter = float4(dir, lightManager_.GetSpotCosOuter(i));
            spotLightBufferCPU[i].colorIntensity = float4(desc.color, desc.intensity);
            spotLightBufferCPU[i].shadowParams = float4(lightManager_.GetSpotCosInner(i), static_cast<float>(i), lightManager_.GetSpotInvAngleRange(i), lightManager_.GetSpotDepthBias(i));
            spotLightBufferCPU[i].shadowParams2 = float4(lightManager_.GetSpotNormalBias(i), 0.0f, 0.0f, 0.0f);
            spotLightBufferCPU[i].viewProj = viewProj;
        }

        auto cb = renderer->GetFrameResource()->AllocDynamic(matSpotLightCS_->GetCBSizeBytesAligned(0, 256), 256);
        const auto& handles = cbHandles_.spotLights;

        matSpotLightCS_->UpdateCBField(handles.invView, invView, (uint8_t*)cb.cpu);
        matSpotLightCS_->UpdateCBField(handles.invProj, invProj, (uint8_t*)cb.cpu);
        matSpotLightCS_->UpdateCBField(handles.camPos, camera_.GetPosition(), (uint8_t*)cb.cpu);

        const float width = static_cast<float>(std::max(renderer->GetWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetHeight(), 1u));
        const float2 screenSize = float2(width, height);
        const float2 invScreen = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);
        matSpotLightCS_->UpdateCBField(handles.screenSize, screenSize, (uint8_t*)cb.cpu);
        matSpotLightCS_->UpdateCBField(handles.invScreenSize, invScreen, (uint8_t*)cb.cpu);

        const float shadowRes = static_cast<float>(renderer->GetDeferredForFrame().spotShadowRes);
        const float2 shadowSize = float2(shadowRes, shadowRes);
        const float invRes = shadowRes > 0.0f ? 1.0f / shadowRes : 0.0f;
        const float2 invShadowSize = float2(invRes, invRes);
        matSpotLightCS_->UpdateCBField(handles.shadowSize, shadowSize, (uint8_t*)cb.cpu);
        matSpotLightCS_->UpdateCBField(handles.invShadowSize, invShadowSize, (uint8_t*)cb.cpu);

        const uint32_t lightCount = static_cast<uint32_t>(spotLightCount);
        matSpotLightCS_->UpdateCBField(handles.lightCount, lightCount, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();
        rc.ClearFast();

        rc.cbv[0] = cb.gpu;
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6> srvs = {
            D.gbSRV[0],
            D.gbSRV[1],
            D.gbSRV[2],
            D.gbSRV[3],
            D.spotShadowSRV,
            spotLightSrvHandle
        };
        rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.lightUAV }).gpu;
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp(), *SamplerManager::ComparisonLinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        matSpotLightCS_->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }
        renderer->UAVBarrier(t.cl, D.light.Get());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_PointLights(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& invView, const mat4& invProj)
{
    auto& pointLights = lightManager_.PointLights();
    if (pointLights.empty()) { return; }

    lightManager_.EnsurePointLightBuffer(renderer, pointLights.size());
    auto* pointLightBufferCPU = lightManager_.GetPointLightBufferCPU();
    const D3D12_CPU_DESCRIPTOR_HANDLE pointLightSrvHandle = lightManager_.GetPointLightSrv();
    if (!pointLightBufferCPU || pointLightSrvHandle.ptr == 0) { return; }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassPointLights);

        const auto& D = renderer->GetDeferredForFrame();
        const D3D12_RESOURCE_STATES srvState =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        renderer->Transition(t.cl, D.gb0.Get(), srvState);
        renderer->Transition(t.cl, D.gb1.Get(), srvState);
        renderer->Transition(t.cl, D.gb2.Get(), srvState);
        renderer->Transition(t.cl, D.depth.Get(), srvState);

        for (size_t i = 0; i < pointLights.size(); ++i)
        {
            const auto& desc = pointLights[i].GetDesc();
            pointLightBufferCPU[i].position = desc.position;
            pointLightBufferCPU[i].radius = desc.radius;
            pointLightBufferCPU[i].color = desc.color;
            pointLightBufferCPU[i].intensity = desc.intensity;
        }

        auto cb = renderer->GetFrameResource()->AllocDynamic(matPointLightCS_->GetCBSizeBytesAligned(0, 256), 256);
        const auto& handles = cbHandles_.pointLights;

        matPointLightCS_->UpdateCBField(handles.invView, invView, (uint8_t*)cb.cpu);
        matPointLightCS_->UpdateCBField(handles.invProj, invProj, (uint8_t*)cb.cpu);
        matPointLightCS_->UpdateCBField(handles.camPos, camera_.GetPosition(), (uint8_t*)cb.cpu);

        const float width = static_cast<float>(std::max(renderer->GetWidth(), 1u));
        const float height = static_cast<float>(std::max(renderer->GetHeight(), 1u));
        const float2 screenSize = float2(width, height);
        const float2 invScreen = float2(width > 0.f ? (1.0f / width) : 0.0f, height > 0.f ? (1.0f / height) : 0.0f);
        matPointLightCS_->UpdateCBField(handles.screenSize, screenSize, (uint8_t*)cb.cpu);
        matPointLightCS_->UpdateCBField(handles.invScreenSize, invScreen, (uint8_t*)cb.cpu);

        const uint32_t lightCount = static_cast<uint32_t>(pointLights.size());
        matPointLightCS_->UpdateCBField(handles.lightCount, lightCount, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();
        rc.ClearFast();

        rc.cbv[0] = cb.gpu;
        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 5> srvs = {
            D.gbSRV[0],
            D.gbSRV[1],
            D.gbSRV[2],
            D.gbSRV[3],
            pointLightSrvHandle
        };
        rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.lightUAV }).gpu;
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        matPointLightCS_->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }
        renderer->UAVBarrier(t.cl, D.light.Get());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Skybox(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj)
{
    if (!skyBox_) { return; }
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSkybox);

        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ);

        // RTV = SceneColor, DSV = GBuffer Depth (read-only), no clears
        renderer->BindLightTarget(t.cl, Renderer::ClearMode::None, true);

        skyBox_->Render(renderer, t.cl, view, proj);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_SSR(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const mat4& invView, const mat4& invProj,
    float zNear, float zFar)
{
    //return;
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSSR);
        const auto& D = renderer->GetDeferredForFrame();

        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto cb = renderer->GetFrameResource()->AllocDynamic(matSSR_->GetCBSizeBytesAligned(0, 256), 256);
        const auto& handlesSSR = cbHandles_.ssr;
        matSSR_->UpdateCBField(handlesSSR.view, view, (uint8_t*)cb.cpu);
        matSSR_->UpdateCBField(handlesSSR.proj, proj, (uint8_t*)cb.cpu);
        matSSR_->UpdateCBField(handlesSSR.invView, invView, (uint8_t*)cb.cpu);
        matSSR_->UpdateCBField(handlesSSR.invProj, invProj, (uint8_t*)cb.cpu);
        matSSR_->UpdateCBField(handlesSSR.depthA, zFar / (zFar - zNear), (uint8_t*)cb.cpu);
        matSSR_->UpdateCBField(handlesSSR.depthB, (zNear * zFar) / (zNear - zFar), (uint8_t*)cb.cpu);
        matSSR_->UpdateCBField(handlesSSR.zNear, zNear, (uint8_t*)cb.cpu);
        matSSR_->UpdateCBField(handlesSSR.zFar, zFar, (uint8_t*)cb.cpu);
        matSSR_->UpdateCBField(handlesSSR.screenSize, float2((float)renderer->GetWidth(), (float)renderer->GetHeight()), (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable({ D.lightSRV, D.gbSRV[1], D.gbSRV[3] }).gpu; // t0 Light, t1 GB1, t2 Depth
        rc.table[1] = renderer->StageSrvUavTable({ D.ssrUAV }).gpu; // u0 output
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        matSSR_->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT ssrWidth = renderer->GetSsrTextureWidth();
        const UINT ssrHeight = renderer->GetSsrTextureHeight();
        const UINT groupsX = (ssrWidth + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (ssrHeight + kGroupSize - 1u) / kGroupSize;
        t.cl->Dispatch(groupsX, groupsY, 1);
        renderer->UAVBarrier(t.cl, D.ssr.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_SSR_Blur(Renderer* renderer, RenderGraph::PassContext ctx)
{
    //return;
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassSSRBlur);
        const auto& D = renderer->GetDeferredForFrame();
        constexpr UINT kGroupSize = 8;
        const UINT ssrWidth = renderer->GetSsrTextureWidth();
        const UINT ssrHeight = renderer->GetSsrTextureHeight();
        const UINT groupsX = (ssrWidth + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (ssrHeight + kGroupSize - 1u) / kGroupSize;

        // Horizontal pass
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssrBlur.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto cb = renderer->GetFrameResource()->AllocDynamic(matBlur_->GetCBSizeBytesAligned(0, 256), 256);
        const float invSsrWidth = ssrWidth > 0 ? (1.0f / static_cast<float>(ssrWidth)) : 0.0f;
        float2 dir = float2(invSsrWidth, 0.0f);
        matBlur_->UpdateCBField(cbHandles_.blur.dir, dir, (uint8_t*)cb.cpu);
        matBlur_->UpdateCBField(cbHandles_.blur.radius, 0.5f, (uint8_t*)cb.cpu);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable({ D.ssrSRV }).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.ssrBlurUAV }).gpu;
        const auto samplerDescsX = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescsX);

        matBlur_->Bind(t.cl, rc);
        t.cl->Dispatch(groupsX, groupsY, 1);
        renderer->UAVBarrier(t.cl, D.ssrBlur.Get());

        // Vertical pass
        renderer->Transition(t.cl, D.ssrBlur.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        cb = renderer->GetFrameResource()->AllocDynamic(matBlur_->GetCBSizeBytesAligned(0, 256), 256);
        const float invSsrHeight = ssrHeight > 0 ? (1.0f / static_cast<float>(ssrHeight)) : 0.0f;
        dir = float2(0.0f, invSsrHeight);
        matBlur_->UpdateCBField(cbHandles_.blur.dir, dir, (uint8_t*)cb.cpu);
        matBlur_->UpdateCBField(cbHandles_.blur.radius, 0.5f, (uint8_t*)cb.cpu);

        rc.ClearFast();
        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable({ D.ssrBlurSRV }).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.ssrUAV }).gpu;
        const auto samplerDescsY = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescsY);

        matBlur_->Bind(t.cl, rc);
        t.cl->Dispatch(groupsX, groupsY, 1);
        renderer->UAVBarrier(t.cl, D.ssr.Get());
    }
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Compose(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const mat4& invView, const mat4& invProj,
    float zNear, float zFar)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassCompose);
        const auto& D = renderer->GetDeferredForFrame();

        renderer->Transition(t.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const float width = static_cast<float>(renderer->GetWidth());
        const float height = static_cast<float>(renderer->GetHeight());
        if (width <= 0.0f || height <= 0.0f)
        {
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->EndThreadCommandList(t, ctx.batchIndex);
            return;
        }

        auto cb = renderer->GetFrameResource()->AllocDynamic(matComposeCS_->GetCBSizeBytesAligned(0, 256), 256);
        const auto& composeHandles = cbHandles_.compose;
        matComposeCS_->UpdateCBField(composeHandles.invView, invView, (uint8_t*)cb.cpu);
        matComposeCS_->UpdateCBField(composeHandles.invProj, invProj, (uint8_t*)cb.cpu);
        matComposeCS_->UpdateCBField(composeHandles.skyboxIntensity, skyBox_->GetExposure(), (uint8_t*)cb.cpu);
        matComposeCS_->UpdateCBField(composeHandles.camPos, camera_.GetPosition(), (uint8_t*)cb.cpu);
        const float2 screenSize = float2(width, height);
        const float2 invScreenSize = float2(1.0f / width, 1.0f / height);
        matComposeCS_->UpdateCBField(composeHandles.screenSize, screenSize, (uint8_t*)cb.cpu);
        matComposeCS_->UpdateCBField(composeHandles.invScreenSize, invScreenSize, (uint8_t*)cb.cpu);

        const std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 7> srvs = {
            D.lightSRV,
            D.gbSRV[2],
            D.gbSRV[0],
            D.gbSRV[1],
            D.gbSRV[3],
            skyBox_->GetTex()->GetSRVCPU(),
            D.ssrSRV
        };

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();
        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.sceneUAV }).gpu;
        const auto samplerDescs = std::array{ *SamplerManager::LinearClamp(), *SamplerManager::PointClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, samplerDescs);

        matComposeCS_->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }

        renderer->UAVBarrier(t.cl, D.scene.Get());
        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Transparent(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const ObjectBuckets& buckets)
{
    RenderGraph rgTr(ctx.batchIndex);

    // Driver: RTV=SceneColor, DSV=GBuffer. No clear. Do NOT close the driver list.
    rgTr.AddPass("Transparent.Driver", {}, [renderer, &ctx](RenderGraph::PassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        driver.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
        {
            GPU_SCOPE(driver.cl, ProfilerScopes::kTransparentDriver);
            const auto& D = renderer->GetDeferredForFrame();
            if (D.sceneOpaque.Get())
            {
                renderer->Transition(driver.cl, D.scene.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                renderer->Transition(driver.cl, D.sceneOpaque.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
                driver.cl->CopyResource(D.sceneOpaque.Get(), D.scene.Get());
                renderer->Transition(driver.cl, D.sceneOpaque.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            renderer->Transition(driver.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->Transition(driver.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
            renderer->BindSceneColor(driver.cl, Renderer::ClearMode::None, true);
        }
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    rgTr.AddPass("Transparent.Simple", {}, [this, renderer, &view, &proj, &buckets](RenderGraph::PassContext sub) {
        const auto& transparentSimple = buckets[ToIndex(ObjectRenderType::TransparentSimple)];
        if (!transparentSimple.empty())
        {
            RenderObjectBatch(renderer, transparentSimple, sub.batchIndex, view, proj, /*useBundles=*/true, false, 32);
        }
        });

    rgTr.AddPass("Transparent.Complex", {}, [this, renderer, &view, &proj, &buckets](RenderGraph::PassContext sub) {
        const auto& transparentComplex = buckets[ToIndex(ObjectRenderType::TransparentComplex)];
        if (!transparentComplex.empty())
        {
            RenderObjectBatch(renderer, transparentComplex, sub.batchIndex, view, proj, /*useBundles=*/false, false, 32);
        }
        });

    rgTr.Execute(renderer);
}

void Scene::Pass_Tonemap(Renderer* renderer, RenderGraph::PassContext ctx)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassTonemap);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.table[0] = renderer->StageSrvUavTable({ D.sceneSRV }).gpu;
        rc.table[1] = renderer->StageSrvUavTable({ D.tonemapUAV }).gpu;
        const auto tonemapSamplers = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, tonemapSamplers);

        matTonemapCS_->Bind(t.cl, rc);
        constexpr UINT kGroupSize = 8;
        const UINT groupsX = (renderer->GetWidth() + kGroupSize - 1u) / kGroupSize;
        const UINT groupsY = (renderer->GetHeight() + kGroupSize - 1u) / kGroupSize;
        if (groupsX > 0 && groupsY > 0)
        {
            t.cl->Dispatch(groupsX, groupsY, 1);
        }

        renderer->UAVBarrier(t.cl, D.tonemap.Get());

        ID3D12Resource* const backbuffer = renderer->GetCurrentBackbuffer();
        if (backbuffer)
        {
            renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            renderer->Transition(t.cl, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST);
            t.cl->CopyResource(backbuffer, D.tonemap.Get());
            renderer->Transition(t.cl, D.tonemap.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            renderer->Transition(t.cl, backbuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Debug(Renderer* renderer, RenderGraph::PassContext ctx)
{
    if (!debugTexMode_)
    {
        return;
    }
    const auto& D = renderer->GetDeferredForFrame();
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassDebug);
        renderer->RecordBindDefaultsNoClear(t.cl);

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& rc = h.ref();

        rc.table[0] = renderer->StageSrvUavTable({ D.shadowSRV }).gpu; // t0
        const auto debugSamplers = std::array{ *SamplerManager::LinearClamp() };
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, debugSamplers);

        matDebug_->Bind(t.cl, rc);
        t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.cl->DrawInstanced(3, 1, 0, 0);
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Overlay(Renderer* renderer, RenderGraph::PassContext ctx)
{
    if (auto* tm = renderer->GetTextManager()) {
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
        {
            GPU_SCOPE(t.cl, ProfilerScopes::kPassOverlay);
            renderer->RecordBindDefaultsNoClear(t.cl);
            if (showProfiler_)
            {
                Profiler::Get().EmitOverlay(tm, /*x=*/8, /*y=*/48, /*maxLines=*/20);
            }
            tm->Build(renderer, t.cl);
            tm->Draw(renderer, t.cl);
        }
        renderer->EndThreadCommandList(t, ctx.batchIndex);
    }
}

void Scene::Clear()
{
    matLighting_.reset();
    matPointLightCS_.reset();
    matSpotLightCS_.reset();
    matComposeCS_.reset();
    matTonemapCS_.reset();
    matBlur_.reset();
    matSSR_.reset();
    matDebug_.reset();
    lightManager_.Reset();
    cbHandles_ = {};
    objects_.clear();
    for (auto& bucket : renderBuckets_) {
        bucket.clear();
    }
    skyBox_.reset();
}
