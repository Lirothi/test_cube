#include "GpuInstancedModels.h"

#include <stdexcept>
#include <vector>

#include "Mesh.h"
#include "Renderer.h"
#include "Helpers.h"
#include "SamplerManager.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

GpuInstancedModels::GpuInstancedModels(
    std::string modelName,
    UINT numInstances,
    const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader,
    const std::wstring& computeShader)
    : RenderableObject(matPreset, inputLayout, graphicsShader)
    , computeShader_(computeShader)
    , modelName_(std::move(modelName))
    , instanceCount_(numInstances)
{
}

void GpuInstancedModels::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    // Инициализация RenderableObject (создаёт GraphicsMaterial, ставит b0)
    RenderableObject::Init(renderer, uploadCmdList, uploadKeepAlive);

    // Compute-материал
    computeMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, computeShader_);

    // Модель
    mesh_ = renderer->GetMeshManager()->Load(modelName_, renderer, uploadCmdList, uploadKeepAlive, { true, false, 0 });
    {   // ресурсные состояния VB/IB
        if (ID3D12Resource* vb = mesh_->GetVertexBufferResource()) {
            renderer->SetResourceState(vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        if (ID3D12Resource* ib = mesh_->GetIndexBufferResource()) {
            renderer->SetResourceState(ib, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        }
    }

    // Instance-buffer (DEFAULT, UAV)
    instanceBuffer_.Create(renderer->GetDevice(), instanceCount_, uploadCmdList, uploadKeepAlive);

    // Регистрируем текущее состояние (после Create — UAV)
    renderer->SetResourceState(instanceBuffer_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    modelMatrix_ = mat4::Translation({0.0f, 5.0f, 0.0f});
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
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srvs;
    srvs.reserve(4);
    srvs.push_back(instanceBuffer_.GetSRVCPU()); // t0: инстансы
    if (matData_) { matData_->AppendGBufferSRVs(srvs); } // t1..t3: albedo/mr/normal

    auto tbl = renderer->StageSrvUavTable(srvs);
    graphicsCtx_.table[0] = tbl.gpu;

    auto aniso = SamplerManager::AnisoWrap(16);
    graphicsCtx_.samplerTable[0] = renderer->GetSamplerManager()->Get(renderer, aniso);
}

void GpuInstancedModels::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    const D3D12_RESOURCE_STATES kSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, instanceBuffer_.GetResource(), kSRV);

	RenderableObject::RecordGraphics(renderer, cl);
}

void GpuInstancedModels::UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj)
{
    UpdateUniform("world", modelMatrix_.xm());
    UpdateUniform("view", view.xm());
    UpdateUniform("proj", proj.xm());

    ApplyMaterialParamsToCB();
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