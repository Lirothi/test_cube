#include "GpuInstancedModels.h"

#include <stdexcept>
#include <vector>

#include "Mesh.h"
#include "Renderer.h"
#include "Helpers.h"
#include "SamplerManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

GpuInstancedModels::GpuInstancedModels(Renderer* renderer,
    std::string modelName,
    UINT numInstances,
    const std::string& cbLayout,
    const std::string& inputLayout,
    const std::wstring& graphicsShader,
    const std::wstring& computeShader)
    : SceneObject(renderer, cbLayout, inputLayout, graphicsShader)
    , computeShader_(computeShader)
    , modelName_(std::move(modelName))
    , instanceCount_(numInstances)
{
}

void GpuInstancedModels::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    // Инициализация SceneObject (создаёт GraphicsMaterial, ставит b0)
    SceneObject::Init(renderer, uploadCmdList, uploadKeepAlive);

    // Compute-материал
    computeMaterial_ = renderer->GetMaterialManager().GetOrCreateCompute(renderer, computeShader_);

    // Модель
    mesh_ = renderer->GetMeshManager().Load(modelName_, renderer, uploadCmdList, uploadKeepAlive, { true, false, 0 });
    {   // ресурсные состояния VB/IB
        if (ID3D12Resource* vb = mesh_->GetVertexBufferResource()) {
            renderer->SetResourceState(vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        if (ID3D12Resource* ib = mesh_->GetIndexBufferResource()) {
            renderer->SetResourceState(ib, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        }
    }

    // Тестовая текстура (шахматка)
    std::vector<uint32_t> cpuRGBA(256 * 256);
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            bool c = ((x >> 5) ^ (y >> 5)) & 1;
            cpuRGBA[y * 256 + x] = c ? 0xFFFFFFFFu : 0xFF000000u;
        }
    }
    tex_.CreateFromRGBA8(renderer, uploadCmdList, cpuRGBA.data(), 256, 256, uploadKeepAlive);

    // Instance-buffer (DEFAULT, UAV)
    instanceBuffer_.Create(renderer->GetDevice(), instanceCount_, uploadCmdList, uploadKeepAlive);

    // Регистрируем текущее состояние (после Create — UAV)
    renderer->SetResourceState(instanceBuffer_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

void GpuInstancedModels::RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    // Переход в UAV (если нужно)
    renderer->Transition(cl, instanceBuffer_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // constants(b0) для CS
    uint32_t dtBits = 0, angBits = 0;
    memcpy(&dtBits, &deltaTime_, sizeof(float));
    memcpy(&angBits, &angularSpeed_, sizeof(float));
    computeCtx_.constants[0] = { dtBits, angBits, instanceCount_ };

    // таблица UAV/SRV для CS: u0 = instanceBuffer UAV
    auto uavTbl = renderer->StageSrvUavTable({ instanceBuffer_.GetUAVCPU() });
    computeCtx_.table[0] = uavTbl.gpu;

    // Запуск CS
    computeMaterial_->Bind(cl, computeCtx_);
    constexpr UINT THREADS_PER_GROUP = 64;
    const UINT groups = (instanceCount_ + THREADS_PER_GROUP - 1u) / THREADS_PER_GROUP;
    cl->Dispatch(groups, 1, 1);

    renderer->UAVBarrier(cl, instanceBuffer_.GetResource());
}

void GpuInstancedModels::PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    // Таблица SRV(t0..): t0 = instanceBuffer SRV, t1 = texture SRV
    auto tbl = renderer->StageSrvUavTable({ instanceBuffer_.GetSRVCPU(), tex_.GetSRVCPU() });
    graphicsCtx_.table[0] = tbl.gpu;

    // Самплер s0
    auto lin = SamplerManager::AnisoWrap(16);
    graphicsCtx_.samplerTable[0] = renderer->GetSamplerManager().Get(renderer, lin);
}

void GpuInstancedModels::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    const D3D12_RESOURCE_STATES kSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, instanceBuffer_.GetResource(), kSRV);

	SceneObject::RecordGraphics(renderer, cl);
}

void GpuInstancedModels::UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj)
{
    UpdateUniform("world", modelMatrix_.xm());
    UpdateUniform("view", view.xm());
    UpdateUniform("proj", proj.xm());

    UpdateUniform("baseColor", Math::float4(1, 1, 1, 1).xm());
    UpdateUniform("mr", Math::float2(0.95f, 0.4f).xm());

    // Флаги наличия текстур: Albedo есть (1), MR нет (0)
    UpdateUniform("texFlags", Math::float2(1.0f, 0.0f).xm());
}

void GpuInstancedModels::IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
	if (!cl) { return; }
    mesh_->DrawInstanced(cl, instanceCount_);
}

void GpuInstancedModels::Tick(float deltaTime)
{
    deltaTime_ = deltaTime;
}