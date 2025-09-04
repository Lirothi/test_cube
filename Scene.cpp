#include "Scene.h"

#include <memory>
#include <algorithm>

#include "ActionMap.h"
#include "Camera.h"
#include "DebugGrid.h"
#include "GpuInstancedModels.h"
#include "Helpers.h"
#include "Renderer.h"
#include "RenderGraph.h"
#include "StaticMesh.h"
#include "TaskSystem.h"
#include "TextManager.h"
#include "Profiler.h"

static void BuildFrustumSliceCornersWS(const mat4& invView, const mat4& invProj,
    float zNearVS, float zFarVS, std::array<float3, 8>& outCornersWS)
{
    const float2 ndc[4] = { {-1,-1}, {+1,-1}, {+1,+1}, {-1,+1} };
    int idx = 0;
    for (int i = 0; i < 4; ++i)
    {
        // Берём луч из камеры в направлении угла фрустума (на z=1 во view)
        float4 farVS = invProj.Transform(float4(ndc[i].x, ndc[i].y, 1.0f, 1.0f));
        float3 dirVS = farVS.xyz() / farVS.w; // направление в плоскости far

        // Точка на заданной глубине z: масштабируем луч так, чтобы его z совпал с нужной глубиной
        float nz = std::max(1e-6f, dirVS.z);
        float3 nearVS = dirVS * (zNearVS / nz);
        float3 farV = dirVS * (zFarVS / nz);

        // Во world-space
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
    if (!matLighting_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/lighting_ps.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.inputLayoutKey = ""; // fullscreen
        gd.numRT = 1; gd.rtvFormats[0] = renderer->GetLightTargetFormat();
        gd.dsvFormat = DXGI_FORMAT_UNKNOWN;
        gd.depth.DepthEnable = FALSE;
        matLighting_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }

    if (!matCompose_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/compose_ps.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.inputLayoutKey = "";
        gd.numRT = 1; gd.rtvFormats[0] = renderer->GetSceneColorFormat();
        gd.dsvFormat = DXGI_FORMAT_UNKNOWN;
        gd.depth.DepthEnable = FALSE;
        matCompose_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }

    if (!matTonemap_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/tonemap_ps.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.inputLayoutKey = "";
        gd.numRT = 1; gd.rtvFormats[0] = renderer->GetBackbufferFormat();
        gd.dsvFormat = DXGI_FORMAT_UNKNOWN;
        gd.depth.DepthEnable = FALSE;
        matTonemap_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }

    if (!matSSR_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/ssr_ps.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.numRT = 1; gd.rtvFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;// renderer->GetSceneColorFormat();
        gd.depth.DepthEnable = FALSE;
        matSSR_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }

    if (!matBlur_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/blur_ps.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.numRT = 1; gd.rtvFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;// renderer->GetSceneColorFormat();
        gd.depth.DepthEnable = FALSE;
        matBlur_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
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

    pointLights_.emplace_back(); pointLights_.back().SetDesc({ {0,2,0}, 6.0f, {1,0.8f,0.6f}, 5.0f });
    pointLights_.emplace_back(); pointLights_.back().SetDesc({ {-4,1,-2}, 5.0f, {0.6f,0.7f,1.0f}, 8.0f });

    dirLight_ = { float3(-1.5f, -0.7f, -0.5f).Normalized() , {1,1,1}, 1.0f, 0.05f };
    //dirLight_.exposure *= 0.2f;
    //dirLight_.ambient *= 0.2f;

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

    AddObject(std::make_unique<DebugGrid>(100.0f));

    camera_.SetPosition({ 0.f, 1.f, -10.f });

    for (auto& obj : pointLights_)
    {
        obj.Init(renderer, uploadCmdList, uploadKeepAlive);
    }

    for (auto& obj : objects_)
    {
        obj->Init(renderer, uploadCmdList, uploadKeepAlive);
    }
}

void Scene::AddObject(std::unique_ptr<RenderableObjectBase> obj) {
    objects_.push_back(std::move(obj));
}

void Scene::Tick(float deltaTime) {
    CPU_SCOPE("Scene::Tick");

    if (input_ != nullptr && actions_ != nullptr)
    {
        camera_.UpdateFromActions(*input_, *actions_, deltaTime);

        if (actions_->WasActionPressed("DebugTex", *input_))
        {
            debugTexMode_ = !debugTexMode_;
        }
        if (actions_->WasActionPressed("ToggleProfiler", *input_))
        {
            showProfiler_ = !showProfiler_;
        }
    }
    auto& tasks = TaskSystem::Get();

    size_t batchSize = 16;
    tasks.Dispatch(objects_.size(),
        [this, deltaTime](size_t index) {
            if (index >= objects_.size()) {
                return;
			}
            objects_[index]->Tick(deltaTime);
		}, batchSize);

	tasks.WaitForAll();
}

void Scene::Render(Renderer* renderer) {
    if (!renderer) return;
    CPU_SCOPE("Scene::Render");

    if (actions_->WasActionPressed("Wireframe", *input_)) {
        renderer->SetWireframeMode(!renderer->GetWireframeMode());
    }

    auto* tb = renderer->GetTextManager();
    tb->Begin(renderer->GetWidth(), renderer->GetHeight(), 1.0f);
    int y = 8;
    tb->AddTextf(8, 8, float4(1, 1, 1, 0.5f), 32.0f, "FPS:%.0f MS:%0.2f", renderer->GetFPS(), 1000.0f / renderer->GetFPS());

    renderer->BeginFrame();
    renderer->BeginSubmitTimeline();

    // матрицы кадра и параметры камеры/света (как у тебя)
    const float aspect = float(renderer->GetWidth()) / float(renderer->GetHeight());
    const mat4 view = camera_.GetViewMatrix();
    constexpr float HFOV = XMConvertToRadians(90.f);
    const float VFOV = 2.f * atan(tan(HFOV * 0.5f) / aspect);
    const float zNear = 0.01f, zFar = 1000.0f;
    const mat4 proj = mat4::PerspectiveFovLH(VFOV, aspect, zNear, zFar);
    const mat4 invView = mat4::Inverse(view);
    const mat4 invProj = mat4::Inverse(proj);
    const float3 camDir = invView.TransformDirection(float3(0, 0, 1)).Normalized();

    // бакеты объектов (ровно как у тебя, просто в компактном виде)
    std::unordered_map<ObjectRenderType, std::vector<RenderableObjectBase*>> buckets;
    for (const auto& obj : objects_) {
        if (!obj) continue;
        bool tr = obj->IsTransparent();
        bool simple = obj->IsSimpleRender();
        auto key = tr ? (simple ? ObjectRenderType::TransparentSimple : ObjectRenderType::TransparentComplex) : (simple ? ObjectRenderType::OpaqueSimple : ObjectRenderType::OpaqueComplex);
        buckets[key].push_back(obj.get());
    }

    RenderGraph rg;
    auto pClear = rg.AddPass("PrologueClear", {},
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE("Pass_PrologueClear"); Pass_PrologueClear(renderer, ctx); });

    auto pShadow = rg.AddPass("CSM", { pClear },
        [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar, camDir, &buckets]
        (RenderGraph::PassContext ctx) {
            CPU_SCOPE("Pass_CSM");
            Pass_CSM(renderer, ctx, view, proj, invView, invProj, zNear, zFar, camDir, buckets);
        });

    auto pGbuf = rg.AddPass("GBuffer", { pShadow },
        [this, renderer, &view, &proj, &buckets](RenderGraph::PassContext ctx) {
            CPU_SCOPE("Pass_GBuffer");
            Pass_GBuffer(renderer, ctx, view, proj, buckets);
        });

    auto pLight = rg.AddPassMT("Lighting", { pGbuf }, { pShadow },
        [this, renderer, &view, &proj, &invView, &invProj, camDir](RenderGraph::PassContext ctx) {
            CPU_SCOPE("Pass_Lighting");
            Pass_Lighting(renderer, ctx, view, proj, invView, invProj, camDir);
        });

    auto pPointLights = rg.AddPass("PointLights", { pLight },
        [this, renderer, &view, &proj, &invView, &invProj](RenderGraph::PassContext ctx) {
            CPU_SCOPE("Pass_PointLights");
            Pass_PointLights(renderer, ctx, view, proj, invView, invProj);
        });

    auto pSky = rg.AddPass("Skybox", { pPointLights },
        [this, renderer, &view, &proj](RenderGraph::PassContext ctx) {
            CPU_SCOPE("Pass_Skybox");
            Pass_Skybox(renderer, ctx, view, proj);
        });

    auto pSSR = rg.AddPass("SSR", { pSky },
        [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar](RenderGraph::PassContext ctx) {
            CPU_SCOPE("Pass_SSR");
            Pass_SSR(renderer, ctx, view, proj, invView, invProj, zNear, zFar);
        });

    auto pBlur = rg.AddPass("SSR.Blur", { pSSR },
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE("Pass_SSR.Blur"); Pass_SSR_Blur(renderer, ctx); });

    auto pCompose = rg.AddPass("Compose", { pBlur },
        [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar](RenderGraph::PassContext ctx) {
            CPU_SCOPE("Pass_Compose");
            Pass_Compose(renderer, ctx, view, proj, invView, invProj, zNear, zFar);
        });

    auto pTransp = rg.AddPass("Transparent", { pCompose },
        [this, renderer, &view, &proj, &buckets](RenderGraph::PassContext ctx) {
            CPU_SCOPE("Pass_Transparent");
            Pass_Transparent(renderer, ctx, view, proj, buckets);
        });

    auto pTone = rg.AddPass("Tonemap", { pTransp },
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE("Pass_Tonemap"); Pass_Tonemap(renderer, ctx); });

    rg.AddPass("Debug", { pTone },
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE("Pass_Debug"); Pass_Debug(renderer, ctx); });

    rg.AddPass("Overlay", { pTone },
        [this, renderer](RenderGraph::PassContext ctx) { CPU_SCOPE("Pass_Overlay"); Pass_Overlay(renderer, ctx); });

    rg.ExecuteParallel(renderer, TaskSystem::Get());
    //rg.Execute(renderer);
    TaskSystem::Get().WaitForAll();
    renderer->EndFrame();
}

void Scene::RenderObjectBatch(Renderer* renderer,
    const std::vector<RenderableObjectBase*>& objects,
    size_t batchIndex,
    const mat4& view, const mat4& proj,
    bool useBundles,
    bool bindGbufOrScene)
{
    if (objects.empty()) return;

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();
    const size_t chunkSize = 64;

    tasks.Dispatch((N + chunkSize - 1) / chunkSize,
        [renderer, view, proj, &objects, useBundles, chunkSize, batchIndex, bindGbufOrScene](std::size_t jobIndex)
        {
            const size_t begin = jobIndex * chunkSize;
            const size_t end = std::min(begin + chunkSize, objects.size());

            if (useBundles) {
                auto b = renderer->BeginThreadCommandBundle(nullptr);
                for (size_t i = begin; i < end; ++i) {
                    if (auto* obj = objects[i]) obj->Render(renderer, b.cl, view, proj);
                }
                renderer->EndThreadCommandBundle(b, batchIndex);
            }
            else {
                auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
                if (bindGbufOrScene)
                {
                    renderer->BindGBuffer(t.cl, Renderer::ClearMode::None); // без очистки!
                }
                else
                {
                    renderer->BindSceneColor(t.cl, Renderer::ClearMode::None, true);
                }
                
                for (size_t i = begin; i < end; ++i) {
                    if (auto* obj = objects[i]) obj->Render(renderer, t.cl, view, proj);
                }
                renderer->EndThreadCommandList(t, batchIndex);
            }
        }, 1);
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
        chunkSize = 32;
    }

    tasks.Dispatch((N + chunkSize - 1) / chunkSize,
        [renderer, &objects, &lightView, &lightProj, cascadeIndex, chunkSize, batchIndex](std::size_t jobIndex)
        {
            const size_t begin = jobIndex * chunkSize;
            const size_t end = std::min(begin + chunkSize, objects.size());

            // каждый чанк — отдельный direct CL
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            t.cl->SetName(L"RenderShadowBatch");

            // важное: привязываем нужный тайл атласа каскада, без очистки
            renderer->BindShadowTarget(t.cl, cascadeIndex, /*clear=*/false);

            for (size_t i = begin; i < end; ++i) {
                if (auto* obj = objects[i]) {
                    obj->RenderShadow(renderer, t.cl, lightView, lightProj);
                }
            }
            renderer->EndThreadCommandList(t, batchIndex);
        }, 1);
}

void Scene::Pass_PrologueClear(Renderer* r, RenderGraph::PassContext ctx)
{
    auto t = r->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    r->RecordBindAndClear(t.cl);
    r->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_CSM(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj, const mat4& invView, const mat4& invProj,
    float zNear, float zFar, const float3& camDir,
    const std::unordered_map<ObjectRenderType, std::vector<RenderableObjectBase*>>& buckets)
{
    auto d = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    d.cl->SetName(L"CSM");
    const auto& D = renderer->GetDeferredForFrame();
    renderer->Transition(d.cl, D.shadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    renderer->BindShadowTarget(d.cl, 0, /*clear=*/true);
    renderer->EndThreadCommandList(d, ctx.batchIndex);

    const float shadowMaxDistance = 300.0f;
    const float zFarShadow = std::min(zFar, shadowMaxDistance);
    size_t batchIndex = ctx.batchIndex;

    // твои сплиты (жёстко прописанные)
    cachedSplitsVS_[0] = zNear;
    cachedSplitsVS_[1] = 10.0f;
    cachedSplitsVS_[2] = 30.0f;
    cachedSplitsVS_[3] = 100.0f;
    cachedSplitsVS_[4] = zFarShadow;

    TaskSystem::Get().Dispatch(kCascades, [this, renderer, &buckets, &invView, &invProj, &proj, camDir, sunDirWS = dirLight_.dir, batchIndex](std::size_t idx)
    {
        CPU_SCOPE("CSM.PerCascade");
        const auto& D = renderer->GetDeferredForFrame();

        float sliceNear = cachedSplitsVS_[idx], sliceFar = cachedSplitsVS_[idx + 1];
        const UINT  tileRes = D.shadowRes / 2;

        // 8 углов фрустума (твоя функция)
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

        // view света
        const float3 up(0, 1, 0);
        mat4 lightView = mat4::LookAtLH(center - sunDirWS * 300.0f, center, up);

        // AABB по Z + стабилизация XY
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

        auto it = buckets.find(ObjectRenderType::OpaqueSimple);
        if (it != buckets.end())
        {
            RenderShadowBatch(renderer, it->second, batchIndex, cachedLightView_[idx], cachedLightProj_[idx], (UINT)idx, /*chunk*/64);
        }
        it = buckets.find(ObjectRenderType::OpaqueComplex);
        if (it != buckets.end())
        {
            RenderShadowBatch(renderer, it->second, batchIndex, cachedLightView_[idx], cachedLightProj_[idx], (UINT)idx, /*chunk*/64);
        }
    }, 1, ctx.group);
}

void Scene::Pass_GBuffer(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const std::unordered_map<ObjectRenderType, std::vector<RenderableObjectBase*>>& buckets)
{
    RenderGraph rgGB(ctx.batchIndex);
    rgGB.AddPass("GBuffer.Driver", {}, [renderer](RenderGraph::PassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);

        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(driver.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(driver.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(driver.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(driver.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

        renderer->BindGBuffer(driver.cl, Renderer::ClearMode::ColorDepth);
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    // 1.2 Opaque simple → bundles
    rgGB.AddPass("GBuffer.OpaqueSimple", {}, [this, renderer, view, proj, &buckets](RenderGraph::PassContext sub) {
        auto found = buckets.find(ObjectRenderType::OpaqueSimple);
        if (found != buckets.end())
        {
            RenderObjectBatch(renderer, found->second, sub.batchIndex, view, proj, /*useBundles=*/true, true);
        }
        });

    // 1.3 Opaque complex → direct CL, без очисток
    rgGB.AddPass("GBuffer.OpaqueComplex", {}, [this, renderer, view, proj, &buckets](RenderGraph::PassContext sub) {
        auto found = buckets.find(ObjectRenderType::OpaqueComplex);
        if (found != buckets.end())
        {
            RenderObjectBatch(renderer, found->second, sub.batchIndex, view, proj, /*useBundles=*/false, true);
        }
        });

    rgGB.Execute(renderer);
}

void Scene::Pass_Lighting(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const mat4& invView, const mat4& invProj,
    const float3& camDir)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    const auto& D = renderer->GetDeferredForFrame();
    renderer->Transition(t.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.shadow.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    renderer->BindLightTarget(t.cl, Renderer::ClearMode::Color, false);

    // аллоцируем динамический CB в аплоад-ринге текущего кадра
    auto cb = renderer->GetFrameResource()->AllocDynamic(matLighting_->GetCBSizeBytesAligned(0, 256), /*align*/256);

    matLighting_->UpdateCB0Field("sunDirWS", dirLight_.dir, (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("ambientIntensity", dirLight_.ambient, (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("lightRgb", dirLight_.color, (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("exposure", dirLight_.exposure, (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("camPosWS", camera_.GetPosition(), (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("camDirWS", camDir, (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("view", view, (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("invView", invView, (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("invProj", invProj, (uint8_t*)cb.cpu);

    matLighting_->UpdateCB0Field("lightViewProj", (cachedLightView_[0] * cachedLightProj_[0]), (uint8_t*)cb.cpu, /*arrayIndex*/0);
    matLighting_->UpdateCB0Field("lightViewProj", (cachedLightView_[1] * cachedLightProj_[1]), (uint8_t*)cb.cpu, 1);
    matLighting_->UpdateCB0Field("lightViewProj", (cachedLightView_[2] * cachedLightProj_[2]), (uint8_t*)cb.cpu, 2);
    matLighting_->UpdateCB0Field("lightViewProj", (cachedLightView_[3] * cachedLightProj_[3]), (uint8_t*)cb.cpu, 3);

    matLighting_->UpdateCB0Field("cascadeScaleBias", float4(cachedScale_[0].x, cachedScale_[0].y, cachedBias_[0].x, cachedBias_[0].y), (uint8_t*)cb.cpu, 0);
    matLighting_->UpdateCB0Field("cascadeScaleBias", float4(cachedScale_[1].x, cachedScale_[1].y, cachedBias_[1].x, cachedBias_[1].y), (uint8_t*)cb.cpu, 1);
    matLighting_->UpdateCB0Field("cascadeScaleBias", float4(cachedScale_[2].x, cachedScale_[2].y, cachedBias_[2].x, cachedBias_[2].y), (uint8_t*)cb.cpu, 2);
    matLighting_->UpdateCB0Field("cascadeScaleBias", float4(cachedScale_[3].x, cachedScale_[3].y, cachedBias_[3].x, cachedBias_[3].y), (uint8_t*)cb.cpu, 3);

    matLighting_->UpdateCB0Field("cascadeSplitsVS", float4(cachedSplitsVS_[0], cachedSplitsVS_[1], cachedSplitsVS_[2], cachedSplitsVS_[3]), (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("shadowAtlasSize", float2((float)renderer->GetDeferredForFrame().shadowRes, (float)renderer->GetDeferredForFrame().shadowRes), (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("shadowBiasNDC", float4(cachedDepthBiasNDC_[0], cachedDepthBiasNDC_[1], cachedDepthBiasNDC_[2], cachedDepthBiasNDC_[3]), (uint8_t*)cb.cpu);
    matLighting_->UpdateCB0Field("normalBiasWS", float4(cachedNormalBiasWS_[0], cachedNormalBiasWS_[1], cachedNormalBiasWS_[2], cachedNormalBiasWS_[3]), (uint8_t*)cb.cpu);

    //matLighting_->UpdateCB0Field("shadowBias", 0.0015f, (uint8_t*)cb.cpu);
    //matLighting_->UpdateCB0Field("pcfRadius", 1.0f, (uint8_t*)cb.cpu);

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();

    rc.cbv[0] = cb.gpu; // b0 — наш PerFrame
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvs;
    srvs.push_back(D.gbSRV[0]);
    srvs.push_back(D.gbSRV[1]);
    srvs.push_back(D.gbSRV[2]);
    srvs.push_back(D.gbSRV[3]);
    srvs.push_back(D.shadowSRV); // NEW
    rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
    rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::PointClamp(), SamplerManager::ComparisonLinearClamp() });

    matLighting_->Bind(t.cl, rc);
    t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.cl->DrawInstanced(3, 1, 0, 0);

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_PointLights(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const mat4& invView, const mat4& invProj)
{
    if (pointLights_.empty()) { return; }

    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());

    const auto& D = renderer->GetDeferredForFrame();

    // Light RT и Depth (с подключённым DSV) — будем писать только стэнсил в Z-FAIL и аддитив в цвет в COLOR
    renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    renderer->Transition(t.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    renderer->BindLightTarget(t.cl, Renderer::ClearMode::None, /*withDepth*/true);

    // Обнуляем только STENCIL перед каждым светом
    for (auto& L : pointLights_)
    {
        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        // clear stencil=0
        t.cl->ClearDepthStencilView(D.dsv, D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

        // 1) Z-FAIL два прохода по объёму
        L.RenderZFail(renderer, t.cl, view, proj);

        renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ);
        // 2) Цвет: полноэкранный, stencil!=0, аддитив
        L.RenderColor(renderer, t.cl, view, proj, invView, invProj, camera_.GetPosition());
    }

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Skybox(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj)
{
    if (!skyBox_) { return; }
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());

    const auto& D = renderer->GetDeferredForFrame();
    renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_READ);

    // RTV = SceneColor, DSV = GBuffer Depth (read-only), без очисток
    renderer->BindLightTarget(t.cl, Renderer::ClearMode::None, true);

    skyBox_->Render(renderer, t.cl, view, proj);

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_SSR(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const mat4& invView, const mat4& invProj,
    float zNear, float zFar)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    const auto& D = renderer->GetDeferredForFrame();

    renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    renderer->BindSSRTarget(t.cl, Renderer::ClearMode::Color);

    auto cb = renderer->GetFrameResource()->AllocDynamic(matSSR_->GetCBSizeBytesAligned(0, 256), 256);
    matSSR_->UpdateCB0Field("view", view, (uint8_t*)cb.cpu);
    matSSR_->UpdateCB0Field("proj", proj, (uint8_t*)cb.cpu);
    matSSR_->UpdateCB0Field("invView", invView, (uint8_t*)cb.cpu);
    matSSR_->UpdateCB0Field("invProj", invProj, (uint8_t*)cb.cpu);
    matSSR_->UpdateCB0Field("depthA", zFar / (zFar - zNear), (uint8_t*)cb.cpu);
    matSSR_->UpdateCB0Field("depthB", (zNear * zFar) / (zNear - zFar), (uint8_t*)cb.cpu);
    matSSR_->UpdateCB0Field("zNear", zNear, (uint8_t*)cb.cpu);
    matSSR_->UpdateCB0Field("zFar", zFar, (uint8_t*)cb.cpu);
    matSSR_->UpdateCB0Field("screenSize", float2((float)renderer->GetWidth(), (float)renderer->GetHeight()), (uint8_t*)cb.cpu);

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();

    rc.cbv[0] = cb.gpu;
    rc.table[0] = renderer->StageSrvUavTable({ D.lightSRV, D.gbSRV[1], D.gbSRV[3] }).gpu; // t0 Light, t1 GB1, t2 Depth
    rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp(), SamplerManager::PointClamp() });

    matSSR_->Bind(t.cl, rc);
    t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.cl->DrawInstanced(3, 1, 0, 0);
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_SSR_Blur(Renderer* renderer, RenderGraph::PassContext ctx)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    const auto& D = renderer->GetDeferredForFrame();
    // X Pass---
    renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.ssrBlur.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

    renderer->BindSSRBlurTarget(t.cl, Renderer::ClearMode::Color);

    auto cb = renderer->GetFrameResource()->AllocDynamic(matBlur_->GetCBSizeBytesAligned(0, 256), 256);
    float2 dir = float2(1.0f / renderer->GetWidth(), 0.0f);
    matBlur_->UpdateCB0Field("dir", dir, (uint8_t*)cb.cpu);
    matBlur_->UpdateCB0Field("radius", 1.0f, (uint8_t*)cb.cpu);

	auto h = renderer->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();

	rc.cbv[0] = cb.gpu;
    rc.table[0] = renderer->StageSrvUavTable({ D.ssrSRV }).gpu;
    rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp() });

    matBlur_->Bind(t.cl, rc);
    t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.cl->DrawInstanced(3, 1, 0, 0);
    // ---
    // Y Pass---
    renderer->Transition(t.cl, D.ssrBlur.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

    renderer->BindSSRTarget(t.cl, Renderer::ClearMode::None); // RT=ssr

    cb = renderer->GetFrameResource()->AllocDynamic(matBlur_->GetCBSizeBytesAligned(0, 256), 256);
    dir = float2(0.0f, 1.0f / renderer->GetHeight());
    matBlur_->UpdateCB0Field("dir", dir, (uint8_t*)cb.cpu);
    matBlur_->UpdateCB0Field("radius", 1.0f, (uint8_t*)cb.cpu);

    rc.cbv[0] = cb.gpu;
    rc.table[0] = renderer->StageSrvUavTable({ D.ssrBlurSRV }).gpu;
    rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp() });

    matBlur_->Bind(t.cl, rc);
    t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.cl->DrawInstanced(3, 1, 0, 0);
    //---
    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Compose(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const mat4& invView, const mat4& invProj,
    float zNear, float zFar)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    const auto& D = renderer->GetDeferredForFrame();
    renderer->Transition(t.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.ssr.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    renderer->BindSceneColor(t.cl, Renderer::ClearMode::Color, false);

    // === CB для compose_ps ===

    auto cb = renderer->GetFrameResource()->AllocDynamic(matCompose_->GetCBSizeBytesAligned(0, 256), 256);
    matCompose_->UpdateCB0Field("view", view, (uint8_t*)cb.cpu);
    matCompose_->UpdateCB0Field("proj", proj, (uint8_t*)cb.cpu);
    matCompose_->UpdateCB0Field("invView", invView, (uint8_t*)cb.cpu);
    matCompose_->UpdateCB0Field("invProj", invProj, (uint8_t*)cb.cpu);
    matCompose_->UpdateCB0Field("skyboxIntensity", skyBox_->GetExposure(), (uint8_t*)cb.cpu);

    // === Собираем SRV-таблицу под root TABLE(SRV...) из compose_ps.hlsl
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvs;
    srvs.push_back(D.lightSRV);   // t0
    srvs.push_back(D.gbSRV[2]);   // t1 (GB2)
    srvs.push_back(D.gbSRV[0]);   // t2 (GB0)
    srvs.push_back(D.gbSRV[1]);   // t3 (GB1)
    srvs.push_back(D.gbSRV[3]);   // t4 (Depth)
    srvs.push_back(skyBox_->GetTex()->GetSRVCPU());
    srvs.push_back(D.ssrSRV);

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();

    rc.cbv[0] = cb.gpu; // b0
    rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
    rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp(), SamplerManager::PointClamp() });

    matCompose_->Bind(t.cl, rc);
    t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.cl->DrawInstanced(3, 1, 0, 0);

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Transparent(Renderer* renderer, RenderGraph::PassContext ctx,
    const mat4& view, const mat4& proj,
    const std::unordered_map<ObjectRenderType, std::vector<RenderableObjectBase*>>& buckets)
{
    RenderGraph rgTr(ctx.batchIndex);

    // Driver: RTV=SceneColor, DSV=GBuffer. Без очистки. НЕ закрываем.
    rgTr.AddPass("Transparent.Driver", {}, [renderer](RenderGraph::PassContext sub) {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        const auto& D = renderer->GetDeferredForFrame();
        renderer->Transition(driver.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        renderer->Transition(driver.cl, D.depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        renderer->BindSceneColor(driver.cl, Renderer::ClearMode::None, true);
        renderer->RegisterPassDriver(driver.cl, sub.batchIndex);
        });

    rgTr.AddPass("Transparent.Simple", {}, [this, renderer, view, proj, &buckets](RenderGraph::PassContext sub) {
        auto found = buckets.find(ObjectRenderType::TransparentSimple);
        if (found != buckets.end())
        {
            RenderObjectBatch(renderer, found->second, sub.batchIndex, view, proj, /*useBundles=*/true, false);
        }
        });

    rgTr.AddPass("Transparent.Complex", {}, [this, renderer, view, proj, &buckets](RenderGraph::PassContext sub) {
        auto found = buckets.find(ObjectRenderType::TransparentComplex);
        if (found != buckets.end())
        {
            RenderObjectBatch(renderer, found->second, sub.batchIndex, view, proj, /*useBundles=*/false, false);
        }
        });

    rgTr.Execute(renderer);
}

void Scene::Pass_Tonemap(Renderer* renderer, RenderGraph::PassContext ctx)
{
    auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
    t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
    const auto& D = renderer->GetDeferredForFrame();
    renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    renderer->RecordBindDefaultsNoClear(t.cl);

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();

    rc.table[0] = renderer->StageTonemapSrvTable(); // t0
    rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp() });

    matTonemap_->Bind(t.cl, rc);
    t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.cl->DrawInstanced(3, 1, 0, 0);

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
    renderer->RecordBindDefaultsNoClear(t.cl);

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& rc = h.ref();

    rc.table[0] = renderer->StageSrvUavTable({ D.shadowSRV }).gpu; // t0
    rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp() });

    matDebug_->Bind(t.cl, rc);
    t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    t.cl->DrawInstanced(3, 1, 0, 0);

    renderer->EndThreadCommandList(t, ctx.batchIndex);
}

void Scene::Pass_Overlay(Renderer* renderer, RenderGraph::PassContext ctx)
{
    if (auto* tm = renderer->GetTextManager()) {
        auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
        renderer->RecordBindDefaultsNoClear(t.cl);
        if (showProfiler_)
        {
            Profiler::Get().EmitOverlay(tm, /*x=*/8, /*y=*/48, /*maxLines=*/20);
        }
        tm->Build(renderer, t.cl);
        tm->Draw(renderer, t.cl);
        renderer->EndThreadCommandList(t, ctx.batchIndex);
    }
}

void Scene::Clear()
{
    matLighting_.reset();
    matCompose_.reset();
    matTonemap_.reset();
    matBlur_.reset();
    matSSR_.reset();
    matDebug_.reset();
    objects_.clear();
    skyBox_.reset();
}
