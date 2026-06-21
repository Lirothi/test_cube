#include "rendering/renderables/InstancedDrawBatch.h"

#include <algorithm>
#include <utility>

#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/renderables/IInstanceable.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/meshes/LodSelect.h"
#include "app/camera/Camera.h"
#include "materials/Material.h"
#include "materials/MaterialData.h"

void InstancedDrawBatch::Configure(std::vector<RenderableObjectBase*> members,
                                   Material* gfx, Material* shadow, MaterialData* matData, Mesh* mesh,
                                   bool simple)
{
    members_ = std::move(members);
    gfxMat_ = gfx;
    shadowMat_ = shadow;
    matData_ = matData;
    mesh_ = mesh;
    simple_ = simple;

    // Step 6d: union of member world bounds -> the run picks ONE LOD per view (camera screen
    // size for gbuffer / cascade floor for shadows). Members are already visible (post-cull).
    bounds_ = AABB::Empty();
    for (RenderableObjectBase* m : members_)
    {
        if (m) { bounds_.Expand(m->GetWorldBounds()); }
    }
}

void InstancedDrawBatch::BuildLodBuckets()
{
    // Step 6d: PER-INSTANCE LOD — group members by the camera LOD chosen in PrepareViews (each
    // member used its OWN bounds, so a spatially spread run LODs each object correctly). Render
    // then emits one instanced draw per occupied tier. Called in PrepareViews (not recording).
    for (auto& bucket : lodBuckets_) { bucket.clear(); }
    if (!mesh_) { return; }
    const UINT maxTier = mesh_->GetLodCount() - 1u; // clamp so empty/duplicate tiers don't draw
    for (RenderableObjectBase* m : members_)
    {
        if (!m) { continue; }
        UINT tier = m->GetCameraLod();
        if (tier > maxTier) { tier = maxTier; }
        if (tier >= kMaxLodTiers) { tier = kMaxLodTiers - 1u; }
        lodBuckets_[tier].push_back(m);
    }
}

void InstancedDrawBatch::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& /*camera*/, D3D12_GPU_VIRTUAL_ADDRESS viewCB)
{
    if (!renderer || !cl || !gfxMat_ || !mesh_ || members_.empty()) { return; }
    for (UINT tier = 0; tier < kMaxLodTiers; ++tier)
    {
        if (!lodBuckets_[tier].empty())
        {
            RecordInstanced(renderer, cl, gfxMat_, viewCB, /*gbuffer=*/true, tier, lodBuckets_[tier]);
        }
    }
}

void InstancedDrawBatch::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& /*lightView*/, const mat4& /*lightProj*/, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod)
{
    if (!renderer || !cl || !shadowMat_ || !mesh_ || members_.empty()) { return; }
    // Shadows use the per-cascade LOD floor for the whole run (all casters in a cascade are at
    // ~the same depth slice), so no per-instance bucketing is needed here.
    RecordInstanced(renderer, cl, shadowMat_, viewCB, /*gbuffer=*/false, lod, members_);
}

void InstancedDrawBatch::RecordInstanced(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                         Material* material, D3D12_GPU_VIRTUAL_ADDRESS viewCB, bool gbuffer, UINT lod,
                                         const std::vector<RenderableObjectBase*>& members)
{
    const size_t total = members.size();
    const bool wireframe = gbuffer && renderer->GetWireframeMode();

    // Split runs larger than the shader's instance-array capacity into multiple draws.
    for (size_t base = 0; base < total; base += render::kMaxInstancesPerDraw)
    {
        const UINT count = static_cast<UINT>(std::min<size_t>(render::kMaxInstancesPerDraw, total - base));
        const UINT bytes = count * static_cast<UINT>(sizeof(render::InstancePerObject));

        auto alloc = renderer->GetFrameResource()->AllocDynamic(bytes, render::kConstantBufferAlignment);
        auto* dst = static_cast<render::InstancePerObject*>(alloc.cpu);
        for (UINT i = 0; i < count; ++i)
        {
            // Members are guaranteed instanceable by the detection in SceneRenderQueue.
            if (const IInstanceable* inst = members[base + i]->AsInstanceable())
            {
                inst->FillInstanceData(dst[i]);
            }
        }

        auto h = renderer->GetRenderContextPool()->Acquire();
        auto& ctx = h.ref();
        ctx.cbv[0] = alloc.gpu; // b0: per-instance array
        ctx.cbv[1] = viewCB;    // b1: shared per-pass view CB

        if (gbuffer && matData_)
        {
            // Shared material textures (t0..t2) + sampler (s0); instances live in b0, so no
            // t0 conflict with the GpuInstancedModels structured-buffer path.
            matData_->StageGBufferBindings(renderer, ctx, 0, 0);
        }

        material->Bind(cl, ctx, wireframe);
        mesh_->DrawInstanced(cl, count, lod);
    }
}
