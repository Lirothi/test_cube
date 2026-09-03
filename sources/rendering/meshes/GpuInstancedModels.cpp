#include "rendering/meshes/GpuInstancedModels.h"
#include "core/logging/Log.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

#include "rendering/meshes/Mesh.h"
#include "rendering/meshes/LodSelect.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/VisibilityStats.h" // S1: g_visChunkMask
#include "core/math/Frustum.h"
#include "core/Helpers.h"
#include "rendering/descriptors/SamplerManager.h"
#include "rendering/renderables/InstanceTypes.h"
#include "materials/MaterialData.h"
#include "core/math/Math.h"
#include "app/camera/Camera.h"
#include <cstdio>
#include <cstring>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    constexpr float kInstanceSpacing = 1.3f;
    constexpr UINT kInstanceGridSize = 10;

    struct alignas(16) GpuInstDrawParams
    {
        uint32_t instanceBase = 0;
        uint32_t _pad[3]{};
        render::MaterialSurfaceParamsGpu surface{};
    };
    static_assert(sizeof(GpuInstDrawParams) == 80,
        "GpuInstDrawParams must match the HLSL InstDraw cbuffer layout");
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
    const Math::float3 defaultPosition(0.0f, 6.0f, 10.0f);
    const Math::float3 defaultRotation(0.0f, 45.0f * DEG2RAD, 0.0f);
    SetPosition(defaultPosition);
    SetRotationEulerRad(defaultRotation);
    SetModelMatrix(Math::mat4::RotationFromEulerXYZRad(defaultRotation) * Math::mat4::Translation(defaultPosition));
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
    if (!mesh_) {
        // A level referencing geometry that no longer exists (e.g. a raw .obj after the asset was
        // migrated to .mesh.bin) must not take the process down — report and leave this object
        // inert. Everything below dereferences mesh_.
        LOG_ERROR(logging::LogCategory::Asset, "instanced model '{}' failed to load; object disabled", modelName_);
        return;
    }
    {   // Resource states for VB/IB
        // Step 6b: named before declaring. Mesh geometry buffers had no debug name at all, which
        // is why the canonical registry's growth in unnamed entries could not be attributed to
        // any owner. They rest where they are created — geometry is never transitioned.
        if (ID3D12Resource* vb = mesh_->GetVertexBufferResource()) {
            vb->SetName(L"Mesh.VertexBuffer");
            renderer->SetResourceState(vb, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        }
        if (ID3D12Resource* ib = mesh_->GetIndexBufferResource()) {
            ib->SetName(L"Mesh.IndexBuffer");
            renderer->SetResourceState(ib, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        }
    }

    // Instance buffer (DEFAULT, UAV)
    // Naming and declaring moved INTO InstanceBuffer::Create — they belong with the resource's
    // lifetime, not here. Leaving them out here is what stops the registry entry outliving the
    // buffer (this was the worst measured leak: 5 live entries for one buffer).
    instanceBuffer_.Create(renderer->GetDevice(), renderer->Declarations(), instanceCount_,
        uploadCmdList, uploadKeepAlive);

    instanceRotations_.assign(instanceCount_, 0.0f);
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

bool GpuInstancedModels::PrepareCompute(RenderGraphPassContext& ctx)
{
    // The rotation compute writes the instance buffer; the SRV flip back happens in Render,
    // which belongs to a later pass and registers there.
    ctx.Use(instanceBuffer_.GetResource(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

void GpuInstancedModels::PrepareRender(RenderGraphPassContext& ctx)
{
    ctx.Use(instanceBuffer_.GetResource(),
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void GpuInstancedModels::BuildLodPartition(const Math::float3& camPos, const Frustum& cameraFrustum)
{
    tierBase_.fill(0u);
    tierCount_.fill(0u);
    visibleInstanceCount_ = 0u;
    const UINT n = std::min(instanceCount_, kMaxLodInstances);
    if (n == 0u || !GetMesh()) { return; }

    const Math::mat4 model = GetModelMatrix();
    const AABB& local = GetMesh()->GetBoundingBox();
    const float radius = local.GetRadius();
    const UINT maxTier = std::min<UINT>(GetMesh()->GetLodCount() - 1u, kLodTiers - 1u);

    // S1: per-instance frustum test, so the camera draws the instances in front of it and not the
    // whole cloud. A sphere about the instance PIVOT (the grid offset the object matrix places),
    // radius = the farthest mesh corner from that pivot times the object's largest scale axis.
    // Rotation-invariant on purpose: the instance's Y rotation lives on the GPU (instance_anim.hlsl)
    // and the CPU copy of the angle must never decide visibility. Conservative like the object
    // cull -- an instance is dropped only when its whole sphere misses the frustum. Shadows are
    // untouched: DrawGeometry/GetRtInstances still walk instanceCount_, this only shapes the remap.
    const Math::float3 sc = GetScale();
    const float kScale = std::max(std::abs(sc.x), std::max(std::abs(sc.y), std::abs(sc.z)));
    const float cullRadius = (local.IsValid() ? local.GetCenter().Length() + radius : radius) * kScale;
    const bool cull = render::g_visChunkMask && cameraFrustum.IsValid();
    constexpr uint8_t kCulled = 0xFFu;

    // Per-instance tier from its world position (rotation/scale don't move the center).
    std::array<uint8_t, kMaxLodInstances> tierOf{};
    for (UINT i = 0; i < n; ++i)
    {
        const Math::float3 wp = model.TransformPoint(ComputeInstanceOffset(i));
        if (cull && !cameraFrustum.Intersects(wp, cullRadius)) { tierOf[i] = kCulled; continue; }
        UINT t = render::SelectLodTier(wp, radius, camPos, instanceLastTier_[i]); // per-instance hysteresis
        if (t > maxTier) { t = maxTier; }
        instanceLastTier_[i] = static_cast<uint8_t>(t);
        tierOf[i] = static_cast<uint8_t>(t);
        ++tierCount_[t];
        ++visibleInstanceCount_;
    }
    // Prefix-sum the per-tier counts into start offsets, then scatter instance indices.
    UINT acc = 0u;
    for (UINT t = 0; t <= maxTier; ++t) { tierBase_[t] = acc; acc += tierCount_[t]; }
    std::array<UINT, kLodTiers> cursor = tierBase_;
    for (UINT i = 0; i < n; ++i)
    {
        if (tierOf[i] == kCulled) { continue; }
        instanceRemap_[cursor[tierOf[i]]++] = i;
    }
}

void GpuInstancedModels::SelectLod(const Camera& camera, const Frustum& cameraFrustum)
{
    // Step 6: build the per-instance LOD partition once per frame in PrepareViews; Render reads it.
    BuildLodPartition(camera.GetPosition(), cameraFrustum);
}

void GpuInstancedModels::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    Material* mat = GetGraphicsMaterial();
    if (!renderer || !cl || !mat || !mesh_ || instanceCount_ == 0u) { return; }

    constexpr UINT kAlign = render::kConstantBufferAlignment;

    // Per-object CB (b0): the cloud's shared transform + material params.
    const UINT cbSize = mat->GetCBSizeBytesAligned(0, kAlign);
    auto b0 = renderer->GetFrameResource()->AllocDynamic(cbSize, kAlign);
    if (auto* binder = GetUniformBinder()) { binder->UpdateMainCB(*this, renderer, camera, static_cast<uint8_t*>(b0.cpu)); }

    // Remap CB (b3): instance indices grouped by LOD tier (matches gRemap[64] = 256 uints).
    auto remapCB = renderer->GetFrameResource()->AllocDynamic(
        static_cast<UINT>(instanceRemap_.size() * sizeof(uint32_t)), kAlign);
    std::memcpy(remapCB.cpu, instanceRemap_.data(), instanceRemap_.size() * sizeof(uint32_t));

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();
    ctx.cbv[0] = b0.gpu;
    ctx.cbv[1] = viewCB;
    ctx.cbv[3] = remapCB.gpu;

    // Instance SRV (t0) + material textures (t1..t3) + sampler (s0).
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> srvs{};
    size_t count = 0;
    srvs[count++] = instanceBuffer_.GetSRVCPU();
    if (auto* data = GetMaterialData()) { data->AppendGBufferSRVs(srvs.data(), count); }
    ctx.srvTable[0] = renderer->StageSrvUavTable(srvs, count).gpu;
    D3D12_SAMPLER_DESC aniso = *SamplerManager::AnisoWrap(16);
    aniso.MipLODBias = renderer->GetDlssMipBias(); // match the material path's DLSS mip bias
    ctx.samplerTable[0] = renderer->GetSamplerManager()->Get(renderer, aniso);

    const D3D12_RESOURCE_STATES kSRV =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    renderer->Transition(cl, instanceBuffer_.GetResource(), kSRV);

    // One instanced draw per occupied LOD tier (gInstanceBase = the tier's start in gRemap).
    const bool wireframe = renderer->GetWireframeMode() && allowWireframe_;
    for (UINT tier = 0; tier < kLodTiers; ++tier)
    {
        if (tierCount_[tier] == 0u) { continue; }
        auto baseCB = renderer->GetFrameResource()->AllocDynamic(sizeof(GpuInstDrawParams), kAlign);
        auto* drawParams = static_cast<GpuInstDrawParams*>(baseCB.cpu);
        *drawParams = {};
        drawParams->instanceBase = tierBase_[tier];
        const MaterialSurfaceParams surface = GetMaterialData()
            ? GetMaterialData()->surfaceParams
            : MaterialSurfaceParams{};
        drawParams->surface.subsurfaceColor = XMFLOAT3(
            surface.subsurfaceColor.x, surface.subsurfaceColor.y, surface.subsurfaceColor.z);
        drawParams->surface.transmissionStrength = surface.transmissionStrength;
        drawParams->surface.ambientOcclusion = surface.ambientOcclusion;
        drawParams->surface.indirectSpecularScale = surface.indirectSpecularScale;
        drawParams->surface.transmissionAlbedoPower = surface.transmissionAlbedoPower;
        drawParams->surface.transmissionNormalWeight = surface.transmissionNormalWeight;
        drawParams->surface.terrainTiling = XMFLOAT4(
            surface.terrainZoneSize,
            surface.terrainRotationDegrees * (XM_PI / 180.0f),
            surface.terrainScaleVariation,
            surface.terrainBlend);
        drawParams->surface.terrainEdgeParams = XMFLOAT4(
            surface.terrainEdgeBreakup,
            surface.terrainEdgeDetail,
            0.0f,
            0.0f);
        ctx.cbv[2] = baseCB.gpu;
        mat->Bind(cl, ctx, wireframe);
        mesh_->DrawInstanced(cl, tierCount_[tier], tier);
    }
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

void GpuInstancedModels::SyncSceneState(SceneObjectSyncReason reason)
{
    GBufferRenderable::SyncSceneState(reason);
    (void)GetWorldBounds();
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

size_t GpuInstancedModels::GetRtInstances(std::vector<RtInstanceDesc>& out) const
{
    if (instanceCount_ == 0)
    {
        return 0;
    }
    // Reuse the GBufferRenderable path for the shared mesh + material (albedo/MR
    // textures, base color, metal/rough); it also rejects a null/transparent mesh.
    // Its world (= modelMatrix_) is then overwritten per instance below.
    RtInstanceDesc base{};
    if (!GBufferRenderable::GetRtInstance(base))
    {
        return 0;
    }

    // Each instance's rendered world is BuildInstanceTransform(i) * objectWorld: the
    // instanced VS does mul(gInstances[i].world, world) (gbuffer_inst.hlsl), so the
    // object matrix must be folded in or the reflected instances drift from the
    // visible ones. BuildInstanceTransform mirrors instance_anim.hlsl's S*R*T exactly.
    const Math::mat4 objectWorld = GetModelMatrix();
    out.reserve(out.size() + instanceCount_);
    for (UINT i = 0; i < instanceCount_; ++i)
    {
        RtInstanceDesc d = base;
        d.world = BuildInstanceTransform(i) * objectWorld;
        out.push_back(d);
    }
    return instanceCount_;
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
