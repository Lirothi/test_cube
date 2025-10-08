#include "rendering/meshes/GpuInstancedModels.h"

#include <array>
#include <stdexcept>
#include <vector>

#include "rendering/meshes/Mesh.h"
#include "rendering/core/Renderer.h"
#include "core/Helpers.h"
#include "rendering/descriptors/SamplerManager.h"
#include "core/Math.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

GpuInstancedModels::GpuInstancedModels(
    std::string modelName,
    UINT numInstances,
    const std::string& matPreset,
    const std::string& inputLayout,
    const std::wstring& graphicsShader,
    const std::wstring& computeShader)
    : GBufferRenderable(matPreset, inputLayout, graphicsShader)
    , computeShader_(computeShader)
    , modelName_(std::move(modelName))
    , instanceCount_(numInstances)
{
}

void GpuInstancedModels::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    // Initialize RenderableObject (creates GraphicsMaterial and sets b0)
    GBufferRenderable::Init(renderer, uploadCmdList, uploadKeepAlive);

    // Compute material
    computeMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, computeShader_);

    // Model
    SetMesh(renderer->GetMeshManager()->Load(modelName_, renderer, uploadCmdList, uploadKeepAlive, { true, false, 0 }));
    {   // Resource states for VB/IB
        if (ID3D12Resource* vb = mesh_->GetVertexBufferResource()) {
            renderer->SetResourceState(vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        if (ID3D12Resource* ib = mesh_->GetIndexBufferResource()) {
            renderer->SetResourceState(ib, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        }
    }

    // Instance buffer (DEFAULT, UAV)
    instanceBuffer_.Create(renderer->GetDevice(), instanceCount_, uploadCmdList, uploadKeepAlive);

    // Register the current state (after Create — UAV)
    renderer->SetResourceState(instanceBuffer_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    SetModelMatrix(mat4::RotationY(45.0f * DEG2RAD) * mat4::Translation({0.0f, 6.0f, 10.0f}));
}

void GpuInstancedModels::RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    // Transition to UAV if required
    renderer->Transition(cl, instanceBuffer_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();

    // constants(b0) for the compute shader
    const uint32_t dtBits = Math::FloatToUint32(deltaTime_);
    const uint32_t angBits = Math::FloatToUint32(angularSpeed_);
    ctx.constants[0] = { dtBits, angBits, instanceCount_ };

    // UAV/SRV table for the compute shader: u0 = instanceBuffer UAV
    auto uavTbl = renderer->StageSrvUavTable({ instanceBuffer_.GetUAVCPU() });
    ctx.table[0] = uavTbl.gpu;

    // Dispatch the compute shader
    computeMaterial_->Bind(cl, ctx);
    constexpr UINT THREADS_PER_GROUP = 64;
    const UINT groups = (instanceCount_ + THREADS_PER_GROUP - 1u) / THREADS_PER_GROUP;
    cl->Dispatch(groups, 1, 1);

    renderer->UAVBarrier(cl, instanceBuffer_.GetResource());
}

void GpuInstancedModels::PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx)
{
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> srvs{};
    size_t count = 0;
    srvs[count++] = instanceBuffer_.GetSRVCPU(); // t0: instances
    if (auto* data = GetMaterialData()) {
        data->AppendGBufferSRVs(srvs.data(), count);
    }

    auto tbl = renderer->StageSrvUavTable(srvs, count);
    ctx.table[0] = tbl.gpu;

    const D3D12_SAMPLER_DESC* aniso = SamplerManager::AnisoWrap(16);
    ctx.samplerTable[0] = renderer->GetSamplerManager()->Get(renderer, *aniso);
}

void GpuInstancedModels::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx)
{
    const D3D12_RESOURCE_STATES kSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, instanceBuffer_.GetResource(), kSRV);

    RenderableObject::RecordGraphics(renderer, cl, ctx);
}

void GpuInstancedModels::IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (!cl) { return; }
    mesh_->DrawInstanced(cl, instanceCount_);
}

void GpuInstancedModels::RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx)
{
    const D3D12_RESOURCE_STATES kSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, instanceBuffer_.GetResource(), kSRV);
    ctx.table[0] = instanceBuffer_.GetSRVForFrame(renderer);

    RenderableObject::RecordShadow(renderer, cl, lightView, lightProj, ctx);
}

void GpuInstancedModels::Tick(float deltaTime)
{
    deltaTime_ = deltaTime;
}