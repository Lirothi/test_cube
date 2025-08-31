#include "Scene.h"

#include <memory>
#include <algorithm>

#include "ActionMap.h"
#include "Camera.h"
#include "Renderer.h"
#include "RenderGraph.h"
#include "TaskSystem.h"
#include "TextManager.h"

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

// стабилизация проекции по размеру текселя (убирает shimmering)
static void StabilizeOrthoBounds(float3& minLS, float3& maxLS, float unitsPerTexel)
{
    // округляем min к сетке и расширяем max так, чтобы размер кратно шагу
    float2 sizeXY = float2(maxLS.x - minLS.x, maxLS.y - minLS.y);
    float2 minXY = float2(
        floor(minLS.x / unitsPerTexel) * unitsPerTexel,
        floor(minLS.y / unitsPerTexel) * unitsPerTexel);
    float2 snapsz = float2(
        ceil(sizeXY.x / unitsPerTexel) * unitsPerTexel,
        ceil(sizeXY.y / unitsPerTexel) * unitsPerTexel);

    minLS.x = minXY.x; minLS.y = minXY.y;
    maxLS.x = minXY.x + snapsz.x;
    maxLS.y = minXY.y + snapsz.y;
    // z оставляем как есть — паддинг добавим отдельно
}

void Scene::InitAll(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    for (auto& obj : objects_)
    {
        obj->Init(renderer, uploadCmdList, uploadKeepAlive);
    }

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

    if (!matShadowCSM_)
    {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"shaders/gbuffer_csm.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.inputLayoutKey = "PosNormTanUV";
        gd.numRT = 0;
        gd.dsvFormat = DXGI_FORMAT_D16_UNORM;
        gd.depth.DepthEnable = TRUE;
        gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        gd.raster.CullMode = D3D12_CULL_MODE_BACK; // при acne — попробуй FRONT
        matShadowCSM_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
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
}

void Scene::AddObject(std::unique_ptr<RenderableObjectBase> obj) {
    objects_.push_back(std::move(obj));
}

void Scene::Tick(float deltaTime) {
    if (input_ != nullptr && actions_ != nullptr)
    {
        camera_.UpdateFromActions(*input_, *actions_, deltaTime);

        if (actions_->WasActionPressed("DebugTex", *input_))
        {
            debugTexMode_ = !debugTexMode_;
        }
    }
    auto& tasks = TaskSystem::Get();

    tasks.Dispatch(objects_.size(),
        [this, deltaTime](size_t index) {
            if (index >= objects_.size()) {
                return;
			}
            objects_[index]->Tick(deltaTime);
		}, 1);

	tasks.WaitForAll();
}

void Scene::Render(Renderer* renderer) {
    if (renderer == nullptr) {
        return;
    }

    if (actions_->WasActionPressed("Wireframe", *input_))
    {
        renderer->SetWireframeMode(!renderer->GetWireframeMode()); //toggle
    }

    auto* tb = renderer->GetTextManager();
    tb->Begin(renderer->GetWidth(), renderer->GetHeight(), 1.0f);

	int textY = 8;
    tb->AddTextf(8, textY, TextManager::RGBA(1, 1, 1, 0.5), 32.0f, "FPS:%.0f", renderer->GetFPS());

    //textY += 32;
    //tb->AddText(8, textY, TextManager::RGBA(1, 1, 1), 32.0f, "Some text with size 32!!!");
    //textY += 32;
    //tb->AddText(8, textY, TextManager::RGBA(1, 1, 1), 16.0f, "Some text with size 16!!!");
    //textY += 32;
    //tb->AddText(8, textY, TextManager::RGBA(1, 1, 1), 12.0f, "Some text with size 12!!!");
    //textY += 32;
    //tb->AddText(8, textY, TextManager::RGBA(1, 1, 1), 8.0f, "Some text with size 8!!!");

    renderer->BeginFrame();
    renderer->BeginSubmitTimeline();

    // матрицы
    const float aspect = float(renderer->GetWidth()) / float(renderer->GetHeight());
    const mat4 view = camera_.GetViewMatrix();
    constexpr float HFOV = XMConvertToRadians(90.f);
    const float VFOV = 2.f * atan(tan(HFOV * 0.5f) / aspect);
    const float zNear = 0.01f;
    const float zFar = 500.0f;
    const mat4 proj = mat4::PerspectiveFovLH(VFOV, aspect, zNear, zFar);

    const mat4 invView = mat4::Inverse(view);
    const mat4 invProj = mat4::Inverse(proj);

    float3 camDir = invView.TransformDirection(float3(0, 0, 1)).Normalized();

    float3 sunDirWS = Math::float3(-0.5f, -0.7f, -0.5f);
    sunDirWS = sunDirWS.Normalized();

    enum class ObjectRenderType {
        OpaqueSimpleRender,
		OpaqueComplexRender,
		TransparentSimpleRender,
		TransparentComplexRender
	};

	std::unordered_map<ObjectRenderType, std::vector<RenderableObjectBase*>> objectsToRender;

    for (const auto& obj : objects_) {
        if (obj) {
            if (obj->IsTransparent())
            {
                if (obj->IsSimpleRender()) {
                    objectsToRender[ObjectRenderType::TransparentSimpleRender].push_back(obj.get());
                }
                else {
                    objectsToRender[ObjectRenderType::TransparentComplexRender].push_back(obj.get());
                }
            }
            else
            {
                if (obj->IsSimpleRender()) {
                    objectsToRender[ObjectRenderType::OpaqueSimpleRender].push_back(obj.get());
                }
                else {
                    objectsToRender[ObjectRenderType::OpaqueComplexRender].push_back(obj.get());
                }
            }
        }
	}

    RenderGraph rg;

    // 1) Пролог (clear)
    auto pClear = rg.AddPass("PrologueClear", {},
        [renderer](RenderGraph::PassContext ctx) {
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
            renderer->RecordBindAndClear(t.cl);
            renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    auto pShadow = rg.AddPass("CSM", { pClear },
        [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar, sunDirWS, camDir, &objectsToRender](RenderGraph::PassContext ctx)
        {
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
            const auto& D = renderer->GetDeferredForFrame();
            renderer->Transition(t.cl, D.shadow.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

            // параметры
            const float lambda = 0.7f;   // practical splits 0..1 (0=linear,1=log)
            const float shadowRangePadding = 1.0f; // добавить 1 юнит к min/max
            const UINT  tileRes = D.shadowRes / 2; // 2048

            const float shadowMaxDistance = 300.0f;
            const float zFarShadow = std::min(zFar, shadowMaxDistance);

            // practical splits (в view-space:  zNear..zFarShadow)
            //for (int i = 0; i <= kCascades; ++i)
            //{
            //    float si = float(i) / float(kCascades);
            //    float logZ = zNear * pow(zFarShadow / zNear, si);
            //    float linZ = zNear + (zFarShadow - zNear) * si;
            //    float zi = Lerp(linZ, logZ, lambda);
            //    cachedSplitsVS_[i] = zi;
            //}
            
            cachedSplitsVS_[0] = zNear;
            cachedSplitsVS_[1] = 10.0f;
            cachedSplitsVS_[2] = 30.0f;
            cachedSplitsVS_[3] = 100.0f;
            cachedSplitsVS_[4] = zFarShadow;

            // общий view света (смотрим из «центра каскада» назад по направлению света)
            for (int c = 0; c < kCascades; ++c)
            {
                float sliceNear = cachedSplitsVS_[c];
                float sliceFar = cachedSplitsVS_[c + 1];

                // углы слайса во world
                std::array<float3, 8> cornersWS;
                BuildFrustumSliceCornersWS(invView, invProj, sliceNear, sliceFar, cornersWS);

                const float tanH = 1.0f / proj.m._11;   // D3D: m00 = 1/tan(H/2)
                const float tanV = 1.0f / proj.m._22;   //       m11 = 1/tan(V/2)

                // 2) параметры среза
                const float zNearS = sliceNear;
                const float zFarS = sliceFar;
                const float halfSlice = 0.5f * (zFarS - zNearS);

                const float farCoef = (zFarS * tanH) * (zFarS * tanH) + (zFarS * tanV) * (zFarS * tanV);

                // 4) выбираем сдвиг вперёд (0..1 от толщины) — можно руками, можно оптимальный
                const float kForward = 0.5f;
                float delta = kForward * halfSlice;

                // 5) радиус, который реально нужен при этом сдвиге
                auto radiusFor = [&](float d) {
                    const float rf2 = farCoef + (halfSlice + d) * (halfSlice + d);
                    return std::sqrt(rf2);
                    };
                float radius = radiusFor(delta) + shadowRangePadding;
                radius -= halfSlice;

                // 6) центр каскада: середина по взгляду + сдвиг delta вперёд
                const float3 camPos = camera_.GetPosition();
                float3 center = camPos + camDir * (zNearS + halfSlice + delta);
                float spatialStep = radius * 0.1f;
                center = Floor(center / spatialStep) * spatialStep;

                // матрица вида света
                float3 up = float3(0, 1, 0);
                mat4 lightView = mat4::LookAtLH(center - sunDirWS * 300.0f, center, up);

                // центр в light-space
                float2 centerLS = (lightView * float4(center, 1)).xy();

                float minZ = +1e9f, maxZ = -1e9f;
                float rLS = 0.0f;
                for (int k = 0; k < 8; ++k)
                {
                    //r = std::max(r, (cornersWS[k] - center).Length());
                    float3 ls = (lightView * float4(cornersWS[k], 1)).xyz();
                    rLS = std::max(rLS, std::max(std::abs(ls.x - centerLS.x), std::abs(ls.y - centerLS.y)));
                    minZ = std::min(minZ, ls.z);
                    maxZ = std::max(maxZ, ls.z);
                }

                //radius = std::max(radius, rLS);

                float unitsPerTexel = (2.0f * radius) / tileRes;
                // СТАБИЛИЗАЦИЯ: снэп центра к сетке texel-grid в LS
                centerLS.x = floor(centerLS.x / unitsPerTexel) * unitsPerTexel;
                centerLS.y = floor(centerLS.y / unitsPerTexel) * unitsPerTexel;

                // квадратные границы в LS
                float minX = centerLS.x - radius, maxX = centerLS.x + radius;
                float minY = centerLS.y - radius, maxY = centerLS.y + radius;

                // Z-диапазон (фиксированный запас)
                const float intersectionDist = 5.0f;
                float nearLS = std::max(0.001f, minZ - intersectionDist);
                float farLS = maxZ + intersectionDist;

                mat4 lightProj = mat4::OrthoOffCenterLH(minX, maxX, minY, maxY, nearLS, farLS);

                const float normalBiasInTexels = 0.75f;  // на старте ~0.5..1.0
                const float depthBiasInTexels = 2.0f;  // на старте ~0.5..1.0

                // конвертируем: world-единицы/тексель → векторный bias и NDC-z bias
                float normalBiasWS_c = normalBiasInTexels * unitsPerTexel;
                float depthBiasNDC_c = (depthBiasInTexels * unitsPerTexel) / (farLS - nearLS);

                // сохраним, чтобы отдать в lighting
                cachedNormalBiasWS_[c] = normalBiasWS_c;
                cachedDepthBiasNDC_[c] = depthBiasNDC_c;

                // scale/bias для 2x2 атласа из тайлов tileRes
                const float2 scale = float2(float(tileRes) / float(D.shadowRes), float(tileRes) / float(D.shadowRes)); // 0.5
                const float2 bias = float2((c % 2) * scale.x, (c / 2) * scale.y);

                cachedScale_[c] = scale;
                cachedBias_[c] = bias;

                cachedLightView_[c] = lightView;
                cachedLightProj_[c] = lightProj;
            }

            // === рендерим 3 каскада ===
            // чистим атлас один раз перед проходом 0
            renderer->BindShadowTarget(t.cl, 0, /*clearDepth*/ true);

            const auto& objs = objectsToRender[ObjectRenderType::OpaqueSimpleRender];
            for (int c = 0; c < kCascades; ++c)
            {
                // переключаем viewport на нужный тайл
                renderer->BindShadowTarget(t.cl, c, /*clearDepth*/ false);

                for (auto* base : objs)
                {
                    if (!base) { continue; }
                    if (auto* obj = dynamic_cast<RenderableObject*>(base))
                    {
                        const UINT cbSize = matShadowCSM_->GetCBSizeBytesAligned(0, 256);
                        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSize, 256);

                        matShadowCSM_->UpdateCB0Field("world", obj->GetModelMatrix(), (uint8_t*)cb.cpu);
                        matShadowCSM_->UpdateCB0Field("view", cachedLightView_[c], (uint8_t*)cb.cpu);
                        matShadowCSM_->UpdateCB0Field("proj", cachedLightProj_[c], (uint8_t*)cb.cpu);

                        RenderContext rc{};
                        rc.cbv[0] = cb.gpu;

                        matShadowCSM_->Bind(t.cl, rc);
                        obj->GetMesh()->Draw(t.cl);
                    }
                }
            }

            renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    auto pGBuffer = rg.AddPass("GBuffer", { pShadow },
        [this, renderer, &view, &proj, &objectsToRender](RenderGraph::PassContext ctx) {
            RenderGraph rgGB(ctx.batchIndex);

            // 1.1 Driver: биндим и чистим один раз. НЕ закрываем driver тут.
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
            rgGB.AddPass("GBuffer.OpaqueSimple", {}, [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext sub) {
                RenderObjectBatch(renderer, objectsToRender[ObjectRenderType::OpaqueSimpleRender],
                    sub.batchIndex, view, proj, /*useBundles=*/true, true);
                });

            // 1.3 Opaque complex → direct CL, без очисток
            rgGB.AddPass("GBuffer.OpaqueComplex", {}, [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext sub) {
                RenderObjectBatch(renderer, objectsToRender[ObjectRenderType::OpaqueComplexRender],
                    sub.batchIndex, view, proj, /*useBundles=*/false, true);
                });

            rgGB.Execute(renderer);
        });

    // 2) LIGHTING — fullscreen → LightTarget (очистка один раз)
    auto pLighting = rg.AddPass("Lighting", { pGBuffer },
        [this, renderer, &view, &proj, &invView, &invProj, sunDirWS, camDir](RenderGraph::PassContext ctx) {
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

            matLighting_->UpdateCB0Field("sunDirWS", sunDirWS, (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("ambientIntensity", 0.05f, (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("lightRgb", float3(1, 1, 1), (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("exposure", 1.5f, (uint8_t*)cb.cpu);
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

            RenderContext rc{};
            rc.cbv[0] = cb.gpu; // b0 — наш PerFrame
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvs;
            srvs.push_back(D.gbSRV[0]);
            srvs.push_back(D.gbSRV[1]);
            srvs.push_back(D.gbSRV[2]);
            srvs.push_back(D.gbSRV[3]);
            srvs.push_back(D.shadowSRV); // NEW
            rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
            rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::PointClamp(), SamplerManager::ComparisonLinearClamp()});

            matLighting_->Bind(t.cl, rc);
            t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            t.cl->DrawInstanced(3, 1, 0, 0);

            renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    auto pSky = rg.AddPass("Skybox", { pLighting },
        [this, renderer, &view, &proj](RenderGraph::PassContext ctx) {
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
        });

    // --- SSR ---
    auto pSSR = rg.AddPass("SSR", { pSky }, [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar](RenderGraph::PassContext ctx) {
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

        RenderContext rc{};
        rc.cbv[0] = cb.gpu;
        rc.table[0] = renderer->StageSrvUavTable({ D.lightSRV, D.gbSRV[1], D.gbSRV[3] }).gpu; // t0 Light, t1 GB1, t2 Depth
        rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp(), SamplerManager::PointClamp() });

        matSSR_->Bind(t.cl, rc);
        t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        t.cl->DrawInstanced(3, 1, 0, 0);
        renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    // --- BLUR X ---
    auto pBlur = rg.AddPass("SSR.Blur", { pSSR }, [this, renderer](RenderGraph::PassContext ctx) {
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
        RenderContext rc{};
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
        });

    // 3) COMPOSE — Light + Emissive → SceneColor
    auto pCompose = rg.AddPass("Compose", { pBlur },
        [this, renderer, &view, &proj, &invView, &invProj, zNear, zFar](RenderGraph::PassContext ctx) {
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
            matCompose_->UpdateCB0Field("skyboxIntensity", 1.0f, (uint8_t*)cb.cpu);

            // === Собираем SRV-таблицу под root TABLE(SRV...) из compose_ps.hlsl
            std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvs;
            srvs.push_back(D.lightSRV);   // t0
            srvs.push_back(D.gbSRV[2]);   // t1 (GB2)
            srvs.push_back(D.gbSRV[0]);   // t2 (GB0)
            srvs.push_back(D.gbSRV[1]);   // t3 (GB1)
            srvs.push_back(D.gbSRV[3]);   // t4 (Depth)
            srvs.push_back(skyBox_->GetTex()->GetSRVCPU());
            srvs.push_back(D.ssrSRV);

            RenderContext rc{};
            rc.cbv[0] = cb.gpu; // b0
            rc.table[0] = renderer->StageSrvUavTable(srvs).gpu;
            rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp(), SamplerManager::PointClamp() });

            matCompose_->Bind(t.cl, rc);
            t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            t.cl->DrawInstanced(3, 1, 0, 0);

            renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    // 4) TRANSPARENT — forward поверх SceneColor, depth test по GBuffer DSV
    auto pTransp = rg.AddPass("Transparent", { pCompose },
        [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext ctx) {
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

            rgTr.AddPass("Transparent.Simple", {}, [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext sub) {
                RenderObjectBatch(renderer, objectsToRender[ObjectRenderType::TransparentSimpleRender],
                    sub.batchIndex, view, proj, /*useBundles=*/true, false);
                });

            rgTr.AddPass("Transparent.Complex", {}, [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext sub) {
                RenderObjectBatch(renderer, objectsToRender[ObjectRenderType::TransparentComplexRender],
                    sub.batchIndex, view, proj, /*useBundles=*/false, false);
                });

            rgTr.Execute(renderer);
        });

    // 5) TONEMAP — SceneColor → Backbuffer
    auto pTonemap = rg.AddPass("Tonemap", { pTransp },
        [this, renderer](RenderGraph::PassContext ctx) {
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
            const auto& D = renderer->GetDeferredForFrame();
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            renderer->RecordBindDefaultsNoClear(t.cl);

            RenderContext rc{};
            rc.table[0] = renderer->StageTonemapSrvTable(); // t0
            rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp() });

            matTonemap_->Bind(t.cl, rc);
            t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            t.cl->DrawInstanced(3, 1, 0, 0);

            renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    auto pDebug = rg.AddPass("Debug", { pTonemap },
        [this, renderer](RenderGraph::PassContext ctx) {
            if (!debugTexMode_)
            {
                return;
            }
            const auto& D = renderer->GetDeferredForFrame();
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
            renderer->RecordBindDefaultsNoClear(t.cl);

            RenderContext rc{};
            rc.table[0] = renderer->StageSrvUavTable({ D.shadowSRV }).gpu; // t0
            rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp() });

            matDebug_->Bind(t.cl, rc);
            t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            t.cl->DrawInstanced(3, 1, 0, 0);

            renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    // 6) OVERLAY — как было
    auto pOverlay = rg.AddPass("Overlay", { pTonemap },
        [this, renderer](RenderGraph::PassContext ctx) {
            if (auto* tm = renderer->GetTextManager()) {
                auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
                t.cl->SetName(std::wstring(ctx.passName.begin(), ctx.passName.end()).data());
                renderer->RecordBindDefaultsNoClear(t.cl);
                tm->Build(renderer, t.cl);
                tm->Draw(renderer, t.cl);
                renderer->EndThreadCommandList(t, ctx.batchIndex);
            }
        });

    // Граф только регистрирует бакеты и запускает задачи
    rg.Execute(renderer);

    // ОДИН общий вейт: ждём, пока воркеры допишут CL'ки в свои бакеты
    TaskSystem::Get().WaitForAll();

    // Сабмитим бакеты в порядке топологии и делаем Present
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
    const size_t chunkSize = 8;

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

void Scene::Clear()
{
    matLighting_.reset();
    matCompose_.reset();
    matTonemap_.reset();
    matBlur_.reset();
    matSSR_.reset();
    matShadowCSM_.reset();
    matDebug_.reset();
    objects_.clear();
    skyBox_.reset();
}
