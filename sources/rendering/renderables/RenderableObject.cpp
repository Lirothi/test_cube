#include "rendering/renderables/RenderableObject.h"

#include <stdexcept>
#include <cstring>

#include "app/Systems.h"
#include "rendering/core/Renderer.h"
#include "core/Helpers.h"
#include "rendering/descriptors/InputLayoutManager.h"
#include "rendering/meshes/LodSelect.h"
#include "app/camera/Camera.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace
{
    bool NearlyEqualFloat3(const Math::float3& a, const Math::float3& b)
    {
        return Math::NearlyEqual(a.x, b.x) && Math::NearlyEqual(a.y, b.y) && Math::NearlyEqual(a.z, b.z);
    }
}

RenderableObject::RenderableObject(
    const std::string& inputLayout,
    const std::wstring& graphicsShader)
    : graphicsShader_(graphicsShader)
    , inputLayoutKey_(inputLayout)
{
    // No mesh by default (5b): concrete types Init a real one (StaticMesh/GpuInstancedModels
    // via MeshManager, Skybox/Ocean build their own). A null mesh draws nothing (DrawGeometry
    // skips), instead of allocating a 0-index Mesh that binds null buffers.
    SetModelMatrix(mat4::Identity());
}

void RenderableObject::SetPosition(const Math::float3& p)
{
    if (NearlyEqualFloat3(pos_, p))
    {
        return;
    }
    pos_ = p;
    MarkTransformDirty();
}

void RenderableObject::SetScale(const Math::float3& s)
{
    if (NearlyEqualFloat3(scale_, s))
    {
        return;
    }
    scale_ = s;
    MarkTransformDirty();
}

void RenderableObject::SetRotationEulerRad(const Math::float3& eulerXYZ)
{
    if (NearlyEqualFloat3(rotEuler_, eulerXYZ))
    {
        return;
    }
    rotEuler_ = eulerXYZ;
    MarkTransformDirty();
}

void RenderableObject::SetRotationEulerDeg(const Math::float3& eulerDegXYZ)
{
    const float k = DEG2RAD;
    SetRotationEulerRad(Math::float3(eulerDegXYZ.x * k, eulerDegXYZ.y * k, eulerDegXYZ.z * k));
}

RenderableObject::~RenderableObject() = default;

void RenderableObject::Init(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    (void)uploadCmdList;
    (void)uploadKeepAlive;

    if (!renderer)
    {
        return;
    }

    Material::GraphicsDesc graphicsDesc = BuildGraphicsDesc(renderer);
    graphicsMaterial_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, graphicsDesc);

    if (CastsShadow())
    {
        Material::GraphicsDesc shadowDesc = BuildShadowDesc(renderer, graphicsDesc);
        shadowMaterial_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, shadowDesc);
    }
    else
    {
        shadowMaterial_.reset();
    }

    if (uniformBinder_)
    {
        uniformBinder_->RebuildHandles(*this);
    }
}

void RenderableObject::ExecuteCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    RecordCompute(renderer, cl);
}

void RenderableObject::UpdateAndBindGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    if (uniformBinder_)
    {
        uniformBinder_->UpdateMainCB(*this, renderer, camera, cbData);
    }
    graphicsMaterial_->Bind(cl, ctx, renderer->GetWireframeMode() && allowWireframe_);
}

void RenderableObject::DrawGeometry(ID3D12GraphicsCommandList* cl, UINT lod)
{
    if (cl == nullptr) { return; }
    if (auto* mesh = GetMesh())
    {
        mesh->Draw(cl, lod);
    }
}

void RenderableObject::RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    if (!graphicsMaterial_) { return; }

    // Binds only — the draw is issued by Render()/RenderShadow() so the per-pass LOD index
    // can be passed to DrawGeometry without threading it through the Record* virtuals.
    UpdateAndBindGraphics(renderer, cl, ctx, camera, cbData);
}

void RenderableObject::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    if (!graphicsMaterial_) { return; }

    constexpr UINT kAlign = render::kConstantBufferAlignment;
    const UINT cbSizeBytes = graphicsMaterial_->GetCBSizeBytesAligned(0, kAlign);

    auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSizeBytes, kAlign);
    uint8_t* cbData = static_cast<uint8_t*>(alloc.cpu);
    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();
    ctx.cbv[0] = alloc.gpu;
    ctx.cbv[1] = viewCB; // shared per-pass view CB (b1); ignored by shaders without b1

    RecordGraphics(renderer, cl, ctx, camera, cbData);
    // Step 6: draw at the camera LOD chosen in PrepareViews (see SelectLod). Mesh::SelectLod
    // clamps to available LODs. No selection/mutation here — recording is side-effect-free.
    DrawGeometry(cl, cameraLod_);
}

#if WITH_EDITOR
void RenderableObject::RenderSelectionStencil(Renderer* renderer, ID3D12GraphicsCommandList* cl, Material* material, const Camera& camera)
{
    if (!renderer) { return; }
    if (cl == nullptr) { return; }
    if (!material) { return; }
    if (!GetMesh()) { return; }

    constexpr UINT kAlign = render::kConstantBufferAlignment;
    const UINT cbSizeBytes = material->GetCBSizeBytesAligned(0, kAlign);
    if (cbSizeBytes == 0)
    {
        return;
    }

    auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSizeBytes, kAlign);
    uint8_t* cbData = static_cast<uint8_t*>(alloc.cpu);
    std::memset(cbData, 0, cbSizeBytes);

    const Material::CBFieldHandle world = material->ComputeCBFieldHandle(0, "world");
    const Material::CBFieldHandle viewProj = material->ComputeCBFieldHandle(0, "viewProj");
    material->UpdateCBField(world, GetModelMatrix(), cbData);
    material->UpdateCBField(viewProj, camera.GetViewProjMatrix(), cbData);

    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();
    ctx.cbv[0] = alloc.gpu;

    material->Bind(cl, ctx, false);
    DrawGeometry(cl, cameraLod_);
}
#endif

void RenderableObject::SelectLod(const Camera& camera)
{
    // Hysteresis off the current tier; per-instance radius (GetLodRadius) so cloud/instanced
    // objects select on their single-mesh size, not their aggregate bound.
    cameraLod_ = render::SelectLodTier(GetWorldBounds().GetCenter(), GetLodRadius(), camera.GetPosition(), cameraLod_);
}

std::wstring RenderableObject::AppendSuffixBeforeExt(const std::wstring& file,
    const std::wstring& suffix)
{
    auto pos = file.find_last_of(L'.');
    if (pos == std::wstring::npos) {
        return file + suffix;
    }
    return file.substr(0, pos) + suffix + file.substr(pos);
}

void RenderableObject::RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj, RenderContext& ctx)
{
    (void)lightView;
    (void)lightProj;

    if (!renderer || !cl || !GetMesh() || !shadowMaterial_) { return; }
    shadowMaterial_->Bind(cl, ctx, false);
}

void RenderableObject::OnMaterialHotReload(Renderer* /*renderer*/)
{
    if (uniformBinder_)
    {
        uniformBinder_->RebuildHandles(*this);
    }
}

void RenderableObject::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const mat4& lightView, const mat4& lightProj, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod)
{
    if (!renderer || !cl || !shadowMaterial_)
    {
        return;
    }

    if (!CastsShadow())
    {
        return;
    }

    UINT cbSize = shadowMaterial_->GetCBSizeBytesAligned(0, render::kConstantBufferAlignment);
    auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSize, render::kConstantBufferAlignment);
    uint8_t* cbData = static_cast<uint8_t*>(alloc.cpu);
    auto h = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = h.ref();

    ctx.cbv[0] = alloc.gpu;
    ctx.cbv[1] = viewCB; // shared per-view light CB (b1: viewProj)

    if (uniformBinder_)
    {
        uniformBinder_->UpdateShadowCB(*this, renderer, lightView, lightProj, cbData);
    }

    RecordShadow(renderer, cl, lightView, lightProj, ctx);
    DrawGeometry(cl, lod); // Step 6c: caller passes the per-cascade LOD floor
}

void RenderableObject::SetGraphicsMaterial(Material* m)
{
    graphicsMaterial_.reset(m);
    if (uniformBinder_)
    {
        uniformBinder_->RebuildHandles(*this);
    }
}

void RenderableObject::SetUniformBinder(std::unique_ptr<UniformBinder> binder)
{
    uniformBinder_ = std::move(binder);
    if (uniformBinder_ && graphicsMaterial_)
    {
        uniformBinder_->RebuildHandles(*this);
    }
}

AABB RenderableObject::GetLocalBounds() const
{
    if (!mesh_)
    {
        return AABB::Empty();
    }
    return mesh_->GetBoundingBox();
}

const AABB& RenderableObject::GetWorldBounds() const
{
    if (worldBoundsDirty_)
    {
        UpdateWorldBoundsCache();
    }

    return worldBoundsCache_;
}

void RenderableObject::SyncSceneState(SceneObjectSyncReason reason)
{
    const bool resetMotionHistory = reason != SceneObjectSyncReason::Frame;

    if (!prevModelMatrixValid_)
    {
        prevModelMatrix_ = modelMatrix_;
        prevModelMatrixValid_ = true;
    }

    if (transformDirty_)
    {
        if (!resetMotionHistory)
        {
            prevModelMatrix_ = modelMatrix_;
        }
        RebuildModelMatrix();
        transformDirty_ = false;
        modelMatrixChangedThisTick_ = true;
    }

    if (resetMotionHistory)
    {
        ResetMotionHistory();
    }
    else if (!modelMatrixChangedThisTick_)
    {
        prevModelMatrix_ = modelMatrix_;
    }

    if (worldBoundsDirty_)
    {
        UpdateWorldBoundsCache();
    }

    modelMatrixChangedThisTick_ = false;
}

void RenderableObject::PostTick(float dt)
{
    SyncSceneState(SceneObjectSyncReason::Frame);

    RenderableObjectBase::PostTick(dt);

    //Systems::GetRenderer().GetDebugDrawSystem()->AddBox(GetWorldBounds(), { 1.0f, 0.0f, 0.0f, 0.7f }, true);
    //Systems::GetRenderer().GetDebugDrawSystem()->AddBox(pos_ + GetLocalBounds().GetCenter(), GetLocalBounds().GetHalfExtents() * scale_, rotEuler_, { 0.0f, 1.0f, 0.0f, 0.7f }, true);
}

void RenderableObject::SetMesh(std::shared_ptr<Mesh> mesh)
{
    mesh_ = std::move(mesh);
    MarkWorldBoundsDirty();
}

void RenderableObject::MarkWorldBoundsDirty()
{
    worldBoundsDirty_ = true;
}

void RenderableObject::MarkTransformDirty()
{
    transformDirty_ = true;
}

void RenderableObject::UpdateWorldBoundsCache() const
{
    Mesh* currentMesh = mesh_.get();
    if (!currentMesh)
    {
        worldBoundsCache_ = AABB::Empty();
    }
    else
    {
        const AABB& localBounds = currentMesh->GetBoundingBox();
        if (localBounds.IsValid())
        {
            worldBoundsCache_ = localBounds.Transform(modelMatrix_);
        }
        else
        {
            worldBoundsCache_ = AABB::Empty();
        }
    }

    worldBoundsDirty_ = false;
}

void RenderableObject::RebuildModelMatrix()
{
    Math::mat4 T = Math::mat4::Translation(pos_);
    Math::mat4 S = Math::mat4::Scaling(scale_);
    Math::mat4 R = Math::mat4::RotationFromEulerXYZRad(rotEuler_);
    Math::mat4 M = S * R * T;
    SetModelMatrix(M);
}

Material::GraphicsDesc RenderableObject::BuildGraphicsDesc(Renderer* renderer) const
{
    (void)renderer;
    Material::GraphicsDesc desc{};
    desc.shaderFile = graphicsShader_;
    desc.inputLayoutKey = inputLayoutKey_;
    desc.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.numRT = 0;
    desc.dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    ConfigureGraphicsPipeline(renderer, desc);
    return desc;
}

Material::GraphicsDesc RenderableObject::BuildShadowDesc(Renderer* renderer, const Material::GraphicsDesc& baseDesc) const
{
    Material::GraphicsDesc desc = baseDesc;
    ConfigureShadowPipeline(renderer, desc);
    return desc;
}

void RenderableObject::ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    (void)renderer;
    (void)desc;
}

void RenderableObject::ConfigureShadowPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const
{
    (void)renderer;
    const std::wstring& baseShader = desc.shaderFile.empty() ? graphicsShader_ : desc.shaderFile;
    desc.shaderFile = AppendSuffixBeforeExt(baseShader, L"_csm");
    if (desc.inputLayoutKey.empty())
    {
        desc.inputLayoutKey = inputLayoutKey_;
    }
    desc.numRT = 0;
    desc.dsvFormat = DXGI_FORMAT_D16_UNORM;
    desc.depth.DepthEnable = TRUE;
    desc.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    desc.raster.CullMode = D3D12_CULL_MODE_BACK;
    desc.blend.RenderTarget[0].BlendEnable = FALSE;
}

bool RenderableObject::IsTransparent() const
{
    if (graphicsMaterial_)
    {
        return graphicsMaterial_->GetCachedGraphicsDesc().blend.RenderTarget[0].BlendEnable != FALSE;
    }

    Material::GraphicsDesc desc = BuildGraphicsDesc(nullptr);
    return desc.blend.RenderTarget[0].BlendEnable != FALSE;
}
