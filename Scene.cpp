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

    //tb->AddText(8, textY, TextManager::RGBA(1,1,1), 32.0f, "FPS 144");
    textY += 32;
    tb->AddText(8, textY, TextManager::RGBA(1, 1, 1), 32.0f, "Some text with size 32!!!");
    textY += 32;
    tb->AddText(8, textY, TextManager::RGBA(1, 1, 1), 16.0f, "Some text with size 16!!!");
    textY += 32;
    tb->AddText(8, textY, TextManager::RGBA(1, 1, 1), 12.0f, "Some text with size 12!!!");
    textY += 32;
    tb->AddText(8, textY, TextManager::RGBA(1, 1, 1), 8.0f, "Some text with size 8!!!");

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

    auto pOpaque = rg.AddPass("Opaque", { pClear },
        [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext ctx) {
            RenderGraph rgOpaque(ctx.batchIndex);

            rgOpaque.AddPass("OpaqueSimpleRender", {},
                [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext ctx)
                {
                    RenderObjectBatch(renderer, objectsToRender[ObjectRenderType::OpaqueSimpleRender], ctx.batchIndex, view, proj, true);
                });

            rgOpaque.AddPass("OpaqueComplexRender", {},
                [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext ctx)
                {
                    RenderObjectBatch(renderer, objectsToRender[ObjectRenderType::OpaqueComplexRender], ctx.batchIndex, view, proj, false);
                });

            rgOpaque.Execute(renderer);
        });

    auto pTransp = rg.AddPass("Transparent", { pOpaque },
        [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext ctx) {
            RenderGraph rgTransparent(ctx.batchIndex);

            rgTransparent.AddPass("TransparentSimpleRender", {},
                [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext ctx)
                {
                    RenderObjectBatch(renderer, objectsToRender[ObjectRenderType::TransparentSimpleRender], ctx.batchIndex, view, proj, true);
                });

            rgTransparent.AddPass("TransparentComplexRender", {},
                [this, renderer, view, proj, &objectsToRender](RenderGraph::PassContext ctx)
                {
                    RenderObjectBatch(renderer, objectsToRender[ObjectRenderType::TransparentComplexRender], ctx.batchIndex, view, proj, false);
                });

            rgTransparent.Execute(renderer);
        });

    // 4) Оверлей
    auto pOverlay = rg.AddPass("Overlay", { pTransp },
        [this, renderer](RenderGraph::PassContext ctx) {
            // HUD-текст
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

void Scene::RenderObjectBatch(
    Renderer* renderer,
    const std::vector<SceneObject*>& objects,
    size_t batchIndex,
    mat4 view,
    mat4 proj,
    bool useCommandBundle)
{
    auto& tasks = TaskSystem::Get();
    const size_t N = objects.size();
    const size_t chunkSize = 32;

    if (N == 0) 
    {
        return;
    }

    if (useCommandBundle)
    {
        auto driver = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
        renderer->RegisterPassDriver(driver.cl, batchIndex);
        renderer->RecordBindDefaultsNoClear(driver.cl);
    }

    tasks.Dispatch((N + chunkSize - 1) / chunkSize,
        [renderer, view, proj, &objects, batchIndex, chunkSize, useCommandBundle](size_t gi) {
            const size_t begin = gi * chunkSize;
            const size_t end = std::min(begin + chunkSize, objects.size());
            if (begin >= end)
            {
                return;
            }

            if (useCommandBundle) {
                auto b = renderer->BeginThreadCommandBundle();
                for (size_t i = begin; i < end; ++i) {
                    auto* obj = objects[i];
                    if (obj) {
                        obj->Render(renderer, b.cl, view, proj);
                    }
                }
                renderer->EndThreadCommandBundle(b, batchIndex);
            }
            else {
                auto t = renderer->BeginThreadCommandList(D3D12_COMMAND_LIST_TYPE_DIRECT);
                renderer->RecordBindDefaultsNoClear(t.cl);
                for (size_t i = begin; i < end; ++i) {
                    auto* obj = objects[i];
                    if (obj) {
                        obj->Render(renderer, t.cl, view, proj);
                    }
                }
                renderer->EndThreadCommandList(t, batchIndex);
            }
        }, 1);
}