#include "Scene.h"

#include <memory>
#include <algorithm>

#include "Camera.h"
#include "Renderer.h"
#include "RenderGraph.h"
#include "TaskSystem.h"
#include "TextManager.h"

void Scene::InitAll(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    for (auto& obj : objects_)
    {
        obj->Init(renderer, uploadCmdList, uploadKeepAlive);
    }

    if (!matLighting_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"lighting_ps.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.inputLayoutKey = ""; // fullscreen
        gd.numRT = 1; gd.rtvFormat = renderer->GetLightTargetFormat();
        gd.dsvFormat = DXGI_FORMAT_UNKNOWN;
        gd.depth.DepthEnable = FALSE;
        matLighting_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }

    if (!matCompose_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"compose_ps.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.inputLayoutKey = "";
        gd.numRT = 1; gd.rtvFormat = renderer->GetSceneColorFormat();
        gd.dsvFormat = DXGI_FORMAT_UNKNOWN;
        gd.depth.DepthEnable = FALSE;
        matCompose_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }

    if (!matTonemap_) {
        Material::GraphicsDesc gd{};
        gd.shaderFile = L"tonemap_ps.hlsl";
        gd.vsEntry = "VSMain"; gd.psEntry = "PSMain";
        gd.inputLayoutKey = "";
        gd.numRT = 1; gd.rtvFormat = renderer->GetBackbufferFormat();
        gd.dsvFormat = DXGI_FORMAT_UNKNOWN;
        gd.depth.DepthEnable = FALSE;
        matTonemap_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    }
}

void Scene::AddObject(std::unique_ptr<SceneObject> obj) {
    objects_.push_back(std::move(obj));
}

void Scene::Update(float deltaTime) {
    if (input_ != nullptr && actions_ != nullptr) {
        camera_.UpdateFromActions(*input_, *actions_, deltaTime);
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
    const mat4 proj = mat4::PerspectiveFovLH(VFOV, aspect, 0.01f, 500.f);

    enum class ObjectRenderType {
        OpaqueSimpleRender,
		OpaqueComplexRender,
		TransparentSimpleRender,
		TransparentComplexRender
	};

	std::unordered_map<ObjectRenderType, std::vector<SceneObject*>> objectsToRender;

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
            renderer->RecordBindAndClear(t.cl);
            renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    auto pGBuffer = rg.AddPass("GBuffer", { pClear },
        [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext ctx) {
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
        [this, renderer, &view, &proj](RenderGraph::PassContext ctx) {
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            const auto& D = renderer->GetDeferredForFrame();
            renderer->Transition(t.cl, D.gb0.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            renderer->Transition(t.cl, D.gb1.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            renderer->Transition(t.cl, D.gb2.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            renderer->Transition(t.cl, D.depth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->BindLightTarget(t.cl, Renderer::ClearMode::Color);

            float3 sunDirWS = Math::float3(-0.5f, -0.7f, -0.5f); // «лучи вниз»
            sunDirWS = sunDirWS.Normalized();
            mat4 invView = mat4::Inverse(view);
            mat4 invProj = mat4::Inverse(proj);

            // аллоцируем динамический CB в аплоад-ринге текущего кадра
            auto cb = renderer->GetFrameResource().AllocDynamic(matLighting_->GetCBSizeBytesAligned(0, 256), /*align*/256);

            matLighting_->UpdateCB0Field("sunDirWS", sunDirWS.xm(), (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("ambientIntensity", 0.01f, (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("lightRgb", float3(1, 1, 1).xm(), (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("exposure", 1.0f, (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("camPosWS", camera_.GetPosition().xm(), (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("invView", invView.xm(), (uint8_t*)cb.cpu);
            matLighting_->UpdateCB0Field("invProj", invProj.xm(), (uint8_t*)cb.cpu);

            RenderContext rc{};
            rc.cbv[0] = cb.gpu; // b0 — наш PerFrame
            rc.table[0] = renderer->StageGBufferSrvTable();
            rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::PointClamp() });

            matLighting_->Bind(t.cl, rc);
            t.cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            t.cl->DrawInstanced(3, 1, 0, 0);

            renderer->EndThreadCommandList(t, ctx.batchIndex);
        });

    // 3) COMPOSE — Light + Emissive → SceneColor
    auto pCompose = rg.AddPass("Compose", { pLighting },
        [this, renderer](RenderGraph::PassContext ctx) {
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
            const auto& D = renderer->GetDeferredForFrame();
            renderer->Transition(t.cl, D.light.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            renderer->Transition(t.cl, D.scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            renderer->BindSceneColor(t.cl, Renderer::ClearMode::Color, false);

            RenderContext rc{};
            rc.table[0] = renderer->StageComposeSrvTable(); // t0..t1
            rc.samplerTable[0] = renderer->GetSamplerManager()->GetTable(renderer, { SamplerManager::LinearClamp() });

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
    auto pTonemap = rg.AddPass("Tonemap", { pCompose },
        [this, renderer](RenderGraph::PassContext ctx) {
            auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
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

    // 6) OVERLAY — как было
    auto pOverlay = rg.AddPass("Overlay", { pTonemap },
        [this, renderer](RenderGraph::PassContext ctx) {
            if (auto* tm = renderer->GetTextManager()) {
                auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
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
    const std::vector<SceneObject*>& objects,
    size_t batchIndex,
    const mat4& view, const mat4& proj,
    bool useBundles,
    bool bindGbufOrScene)
{
    if (objects.empty()) return;

    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();
    const size_t chunkSize = 32;

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