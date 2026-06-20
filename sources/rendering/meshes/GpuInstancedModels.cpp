#include "rendering/meshes/GpuInstancedModels.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "rendering/meshes/Mesh.h"
#include "rendering/core/Renderer.h"
#include "core/Helpers.h"
#include "rendering/descriptors/SamplerManager.h"
#include "core/math/Math.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr float kInstanceSpacing = 1.3f;
    constexpr UINT kInstanceGridSize = 10;
}

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

    instanceRotations_.assign(instanceCount_, 0.0f);
    MarkInstanceBoundsDirty();

    SetModelMatrix(mat4::RotationY(45.0f * DEG2RAD) * mat4::Translation({0.0f, 6.0f, 10.0f}));
    MarkInstanceBoundsDirty();
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
    ctx.uavTable[0] = uavTbl.gpu;

    // Dispatch the compute shader
    computeMaterial_->Bind(cl, ctx);
    constexpr UINT THREADS_PER_GROUP = 64;
    const UINT groups = (instanceCount_ + THREADS_PER_GROUP - 1u) / THREADS_PER_GROUP;
    cl->Dispatch(groups, 1, 1);

    renderer->UAVBarrier(cl, instanceBuffer_.GetResource());
}

void GpuInstancedModels::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    if (!renderer)
    {
        return;
    }

    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> srvs{};
    size_t count = 0;
    srvs[count++] = instanceBuffer_.GetSRVCPU(); // t0: instances
    if (auto* data = GetMaterialData()) {
        data->AppendGBufferSRVs(srvs.data(), count);
    }

    auto tbl = renderer->StageSrvUavTable(srvs, count);
    ctx.srvTable[0] = tbl.gpu;

    const D3D12_SAMPLER_DESC* aniso = SamplerManager::AnisoWrap(16);
    ctx.samplerTable[0] = renderer->GetSamplerManager()->Get(renderer, *aniso);

    const D3D12_RESOURCE_STATES kSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, instanceBuffer_.GetResource(), kSRV);

    RenderableObject::RecordGraphics(renderer, cl, ctx, camera, cbData);
}

void GpuInstancedModels::DrawGeometry(ID3D12GraphicsCommandList* cl, UINT lod)
{
    if (!cl || !mesh_) { return; } // 5b: skip cleanly if mesh-less
    mesh_->DrawInstanced(cl, instanceCount_, lod);
}

void GpuInstancedModels::RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx)
{
    const D3D12_RESOURCE_STATES kSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, instanceBuffer_.GetResource(), kSRV);
    ctx.srvTable[0] = instanceBuffer_.GetSRVForFrame(renderer);

    RenderableObject::RecordShadow(renderer, cl, lightView, lightProj, ctx);
}

void GpuInstancedModels::Tick(float deltaTime)
{
    deltaTime_ = deltaTime;
    bool resized = false;
    if (instanceRotations_.size() < instanceCount_)
    {
        instanceRotations_.resize(instanceCount_, 0.0f);
        resized = true;
    }

    if (instanceRotations_.empty())
    {
        return;
    }

    if (deltaTime <= 0.0f)
    {
        return;
    }

    constexpr float TWO_PI = Math::TWO_PI;
    for (UINT idx = 0; idx < instanceCount_; ++idx)
    {
        const float additionalSpeed = static_cast<float>(idx % 7) * 0.1f;
        float rotation = instanceRotations_[idx] + (angularSpeed_ + additionalSpeed) * deltaTime;
        if (rotation > TWO_PI)
        {
            rotation -= TWO_PI;
        }

        if (!Math::NearlyEqual(rotation, instanceRotations_[idx]))
        {
            instanceRotations_[idx] = rotation;
        }
    }

    if (resized)
    {
        MarkInstanceBoundsDirty();
    }
}

const AABB& GpuInstancedModels::GetWorldBounds() const
{
    const Mesh* currentMesh = GetMesh();
    const Math::mat4& currentModel = GetModelMatrix();

    bool needsUpdate = instanceBoundsDirty_;
    if (!needsUpdate && cachedMesh_ != currentMesh)
    {
        needsUpdate = true;
    }

    if (!needsUpdate)
    {
        const float* last = reinterpret_cast<const float*>(&lastModelMatrix_.m);
        const float* now = reinterpret_cast<const float*>(&currentModel.m);
        for (int i = 0; i < 16; ++i)
        {
            if (!Math::NearlyEqual(last[i], now[i]))
            {
                needsUpdate = true;
                break;
            }
        }
    }

    if (needsUpdate)
    {
        UpdateWorldBoundsCache();
    }

    return instancedWorldBounds_;
}

void GpuInstancedModels::MarkInstanceBoundsDirty()
{
    instanceBoundsDirty_ = true;
    cachedWorstCaseLocalBounds_ = AABB::Empty();
    RenderableObject::MarkWorldBoundsDirty();
}

Math::float3 GpuInstancedModels::ComputeInstanceOffset(UINT index) const
{
    const UINT x = index % kInstanceGridSize;
    const UINT y = index / kInstanceGridSize;

    const float offsetX = (static_cast<float>(x) - (static_cast<float>(kInstanceGridSize) - 1.0f) * 0.5f) * kInstanceSpacing;
    const float offsetY = (static_cast<float>(y) - (static_cast<float>(kInstanceGridSize) - 1.0f) * 0.5f) * kInstanceSpacing;
    const float offsetZ = 0.0f;

    return Math::float3(offsetX, offsetY, offsetZ);
}

Math::mat4 GpuInstancedModels::BuildInstanceTransform(UINT index) const
{
    const Math::float3 offset = ComputeInstanceOffset(index);

    const float baseRotation = (index < instanceRotations_.size()) ? instanceRotations_[index] : 0.0f;
    const float angle = baseRotation + static_cast<float>(index);

    Math::mat4 rotation = Math::mat4::RotationY(angle);
    Math::mat4 translation = Math::mat4::Translation(offset);

    return rotation * translation;
}

AABB GpuInstancedModels::ComputeCombinedWorstCaseLocalBounds(const AABB& meshLocalBounds) const
{
    if (!meshLocalBounds.IsValid() || instanceCount_ == 0)
    {
        return AABB::Empty();
    }

    Math::float3 corners[8];
    meshLocalBounds.GetCorners(corners);

    float maxRadiusSq = 0.0f;
    for (const Math::float3& corner : corners)
    {
        const float radiusSq = corner.x * corner.x + corner.z * corner.z;
        maxRadiusSq = std::max(maxRadiusSq, radiusSq);
    }

    const float maxRadius = std::sqrt(maxRadiusSq);
    const Math::float3 meshMin = meshLocalBounds.GetMin();
    const Math::float3 meshMax = meshLocalBounds.GetMax();

    const Math::float3 rotatedMin(-maxRadius, meshMin.y, -maxRadius);
    const Math::float3 rotatedMax(+maxRadius, meshMax.y, +maxRadius);

    AABB combined = AABB::Empty();
    for (UINT idx = 0; idx < instanceCount_; ++idx)
    {
        const Math::float3 offset = ComputeInstanceOffset(idx);
        const Math::float3 instanceMin = rotatedMin + offset;
        const Math::float3 instanceMax = rotatedMax + offset;
        combined.Expand(AABB(instanceMin, instanceMax));
    }

    return combined;
}

void GpuInstancedModels::UpdateWorldBoundsCache() const
{
    const Mesh* mesh = GetMesh();
    const Math::mat4 model = GetModelMatrix();

    if (!mesh || instanceCount_ == 0)
    {
        instancedWorldBounds_ = AABB::Empty();
        cachedMesh_ = nullptr;
        cachedWorstCaseLocalBounds_ = AABB::Empty();
        lastModelMatrix_ = model;
        instanceBoundsDirty_ = false;
        return;
    }

    const AABB& localBounds = mesh->GetBoundingBox();
    if (!localBounds.IsValid())
    {
        instancedWorldBounds_ = AABB::Empty();
        cachedMesh_ = mesh;
        cachedWorstCaseLocalBounds_ = AABB::Empty();
        lastModelMatrix_ = model;
        instanceBoundsDirty_ = false;
        return;
    }

    if (cachedMesh_ != mesh || instanceBoundsDirty_)
    {
        cachedWorstCaseLocalBounds_ = ComputeCombinedWorstCaseLocalBounds(localBounds);
        cachedMesh_ = mesh;
        instanceBoundsDirty_ = false;
    }

    if (!cachedWorstCaseLocalBounds_.IsValid())
    {
        instancedWorldBounds_ = AABB::Empty();
    }
    else
    {
        instancedWorldBounds_ = cachedWorstCaseLocalBounds_.Transform(model);
    }

    lastModelMatrix_ = model;
}
