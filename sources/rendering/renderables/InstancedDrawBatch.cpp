#include "rendering/renderables/InstancedDrawBatch.h"

#include <algorithm>
#include <cstdio>

#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderContext.h"
#include "rendering/renderables/IInstanceable.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/meshes/LodSelect.h"
#include "app/camera/Camera.h"
#include "materials/Material.h"
#include "materials/MaterialData.h"

void InstancedDrawBatch::Configure(std::vector<RenderableObjectBase*>::const_iterator first,
                                   std::vector<RenderableObjectBase*>::const_iterator last,
                                   Material* gfx, Material* shadow, MaterialData* matData, Mesh* mesh,
                                   bool simple)
{
    members_.assign(first, last);
    gfxMat_ = gfx;
    shadowMat_ = shadow;
    matData_ = matData;
    mesh_ = mesh;
    simple_ = simple;

    // B2b: a multi-slot lead switches the gbuffer path to a per-submesh loop (each range draws
    // with its slot's textures + params). Members are slot-identical to the lead — the queue's
    // run extension enforces SameInstanceSlots — so the lead speaks for the whole run.
    leadInst_ = nullptr;
    if (mesh_ && mesh_->GetSubmeshCount() > 1 && !members_.empty() && members_[0])
    {
        const IInstanceable* inst = members_[0]->AsInstanceable();
        if (inst && inst->InstanceSlotCount() > 1)
        {
            leadInst_ = inst;
            static bool loggedOnce = false; // diagnosable without stats HUD (headless runs)
            if (!loggedOnce)
            {
                loggedOnce = true;
                char buf[128];
                sprintf_s(buf, "[instancing] multi-slot batch active: %zu instances x %zu submeshes\n",
                          members_.size(), mesh_->GetSubmeshCount());
                OutputDebugStringA(buf);
            }
        }
    }

    // The batch is created only after this view's frustum cull and consumed directly by its
    // render pass. Recomputing the union of every member bound here is therefore redundant;
    // an empty sentinel also makes an accidental later cull conservative.
    bounds_ = AABB::Empty();
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
    if (leadInst_) { PrepareSlotBindings(renderer); }
    for (UINT tier = 0; tier < kMaxLodTiers; ++tier)
    {
        if (!lodBuckets_[tier].empty())
        {
            RecordInstanced(renderer, cl, gfxMat_, viewCB, /*gbuffer=*/true, tier, lodBuckets_[tier]);
        }
    }
}

void InstancedDrawBatch::PrepareSlotBindings(Renderer* renderer)
{
    const size_t slotCount = leadInst_ ? leadInst_->InstanceSlotCount() : 0;
    slotBindingsScratch_.resize(slotCount);
    const MaterialParams defaults{};
    const MaterialSurfaceParams surfaceDefaults{};

    for (size_t slot = 0; slot < slotCount; ++slot)
    {
        SlotBindingScratch& binding = slotBindingsScratch_[slot];
        auto cb = renderer->GetFrameResource()->AllocDynamic(
            sizeof(render::InstanceSlotParams), render::kConstantBufferAlignment);
        const MaterialParams* src = leadInst_->InstanceSlotParams(slot);
        const MaterialParams& p = src ? *src : defaults;
        auto* sp = static_cast<render::InstanceSlotParams*>(cb.cpu);
        sp->baseColor = DirectX::XMFLOAT4(p.baseColor.x, p.baseColor.y, p.baseColor.z, p.baseColor.w);
        sp->metalRough = DirectX::XMFLOAT2(p.metalRough.x, p.metalRough.y);
        sp->alphaCutoff = p.alphaCutoff;
        sp->mrMultiply = p.mrMultiply;
        sp->texOffsScale = DirectX::XMFLOAT4(p.texOffsScale.x, p.texOffsScale.y, p.texOffsScale.z, p.texOffsScale.w);
        sp->texFlags = DirectX::XMFLOAT4(p.texFlags.x, p.texFlags.y, p.texFlags.z, p.texFlags.w);
        const auto e = p.EmissiveLinear();
        sp->emissive = DirectX::XMFLOAT3(e.x, e.y, e.z);
        sp->windFoliage = p.windFoliage;
        const MaterialData* slotData = leadInst_->InstanceSlotData(slot);
        const MaterialSurfaceParams& surface = slotData ? slotData->surfaceParams : surfaceDefaults;
        sp->surface.subsurfaceColor = DirectX::XMFLOAT3(
            surface.subsurfaceColor.x, surface.subsurfaceColor.y, surface.subsurfaceColor.z);
        sp->surface.transmissionStrength = surface.transmissionStrength;
        sp->surface.ambientOcclusion = surface.ambientOcclusion;
        sp->surface.indirectSpecularScale = surface.indirectSpecularScale;
        sp->surface.transmissionAlbedoPower = surface.transmissionAlbedoPower;
        sp->surface.transmissionNormalWeight = surface.transmissionNormalWeight;
        sp->surface.terrainTiling = DirectX::XMFLOAT4(
            surface.terrainZoneSize,
            surface.terrainRotationDegrees * (DirectX::XM_PI / 180.0f),
            surface.terrainScaleVariation,
            surface.terrainBlend);
        sp->surface.terrainEdgeParams = DirectX::XMFLOAT4(
            surface.terrainEdgeBreakup,
            surface.terrainEdgeDetail,
            0.0f,
            0.0f);

        binding.paramsCbv = cb.gpu;
        binding.material = leadInst_->InstancedGraphicsMaterialForSlot(slot);
        RenderContext ctx{};
        if (MaterialData* md = leadInst_->InstanceSlotData(slot))
        {
            md->StageGBufferBindings(renderer, ctx, 0, 0);
        }
        binding.srvTable = ctx.srvTable[0];
        binding.samplerTable = ctx.samplerTable[0];
    }
}

void InstancedDrawBatch::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& /*lightView*/, const mat4& /*lightProj*/, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod, bool /*chunkCameraLods*/)
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

    // B2b: multi-slot gbuffer draws loop the LOD's submeshes; Render prepared each slot's
    // params/bindings once for all occupied LODs into a b2 slice read by INSTCB_SLOT_PARAMS.
    // B3: the shadow path loops the same ranges (no per-slot bindings — depth-only and the
    // ranges tile the buffer, so coverage is identical; Part C hooks masked slots here).
    const bool multiSlot = leadInst_ != nullptr;
    const std::vector<Mesh::Submesh>* subs = nullptr;
    if (multiSlot)
    {
        subs = &mesh_->SubmeshesForLod(lod);
    }
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

        if (!multiSlot)
        {
            RenderContext ctx{};
            ctx.cbv[0] = alloc.gpu; // b0: per-instance array
            ctx.cbv[1] = viewCB;    // b1: shared per-pass view CB

            if (gbuffer)
            {
                if (matData_)
                {
                    // Shared material textures (t0..t2) + sampler (s0); instances live in b0, so no
                    // t0 conflict with the GpuInstancedModels structured-buffer path.
                    matData_->StageGBufferBindings(renderer, ctx, 0, 0);
                    matData_->StageGBufferSurfaceParams(renderer, ctx, 2);
                }
                else
                {
                    MaterialData::StageNeutralGBufferSurfaceParams(renderer, ctx, 2);
                }
            }

            material->Bind(cl, ctx, wireframe);
            mesh_->DrawInstanced(cl, count, lod);
            continue;
        }

        // One ranged instanced draw per submesh; the instance array (b0) is shared across the
        // loop, only the combined material/surface b2 + the SRV/sampler tables change
        // (redundant rebinds elided by the bind
        // cache). Null slot materials draw flat: texFlags gate all sampling in the PS. The
        // shadow path (gbuffer=false) draws the same ranges with no per-slot bindings (B3).
        const size_t slotCount = leadInst_->InstanceSlotCount();
        RenderContext ctx{};
        ctx.cbv[0] = alloc.gpu; // b0: per-instance array (shared by every submesh)
        ctx.cbv[1] = viewCB;    // b1: shared per-pass view CB
        for (size_t s = 0; s < subs->size(); ++s)
        {
            size_t slot = (*subs)[s].materialSlot;
            if (slot >= slotCount) { slot = slotCount > 0 ? slotCount - 1 : 0; } // per-object clamp mirrored

            Material* slotMaterial = material;
            if (gbuffer)
            {
                if (slot < slotBindingsScratch_.size())
                {
                    const SlotBindingScratch& binding = slotBindingsScratch_[slot];
                    ctx.cbv[2] = binding.paramsCbv;
                    ctx.srvTable[0] = binding.srvTable;
                    ctx.samplerTable[0] = binding.samplerTable;
                    if (binding.material) { slotMaterial = binding.material; }
                }
            }

            slotMaterial->Bind(cl, ctx, wireframe);
            mesh_->DrawSubmeshInstanced(cl, static_cast<UINT>(s), count, lod);
        }
    }
}
