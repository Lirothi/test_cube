#include "rendering/renderables/InstancedDrawBatch.h"
#include "core/logging/Log.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/RenderContext.h"
#include "rendering/renderables/IInstanceable.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/meshes/LodSelect.h"
#include "app/camera/Camera.h"
#include "materials/Material.h"
#include "materials/MaterialData.h"
#include "rendering/core/GBufferBindingGuard.h"
#include "rendering/renderables/RenderableObject.h" // plan S8: render::ShadowMaterialIsMasked
#include "rendering/descriptors/BindlessHeap.h" // A6: the null texture index

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
            // Once per process: diagnosable without the stats HUD (headless runs).
            LOG_INFO_ONCE(logging::LogCategory::Render, "multi-slot instancing batch active: {} instances x {} submeshes",
                          members_.size(), mesh_->GetSubmeshCount());
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
    for (auto& fades : lodBucketFades_) { fades.clear(); }
    if (!mesh_) { return; }
    const UINT maxTier = mesh_->GetLodCount() - 1u; // clamp so empty/duplicate tiers don't draw
    for (RenderableObjectBase* m : members_)
    {
        if (!m) { continue; }
        UINT tier = m->GetCameraLod();
        if (tier > maxTier) { tier = maxTier; }
        if (tier >= kMaxLodTiers) { tier = kMaxLodTiers - 1u; }
        // Dithered crossfade: a member in the fade band contributes to BOTH tiers — the
        // current one fading out (-f) and the next fading in (+f). The complementary PS
        // masks make the pair cover every pixel exactly once.
        const float fade = m->GetCameraLodFade();
        if (fade > 0.0f && tier < maxTier && (tier + 1u) < kMaxLodTiers)
        {
            lodBuckets_[tier].push_back(m);
            lodBucketFades_[tier].push_back(-fade);
            lodBuckets_[tier + 1u].push_back(m);
            lodBucketFades_[tier + 1u].push_back(fade);
            continue;
        }
        lodBuckets_[tier].push_back(m);
        lodBucketFades_[tier].push_back(0.0f);
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
            RecordInstanced(renderer, cl, gfxMat_, viewCB, /*gbuffer=*/true, tier,
                            lodBuckets_[tier], lodBucketFades_[tier].data());
        }
    }
}

void InstancedDrawBatch::PrepareSlotBindings(Renderer* renderer, bool shadow)
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
        const auto e = p.EmissiveLinear() * leadInst_->InstanceEmissiveScale(); // plan L (the key keeps the run's equal)
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

        {
            uint32_t idx[4] = { render::BindlessHeap::kNullTextureIndex, render::BindlessHeap::kNullTextureIndex,
                                render::BindlessHeap::kNullTextureIndex, 0u };
            if (slotData) { slotData->GatherGBufferIndices(idx); }
            sp->surface.texIndices = DirectX::XMUINT4(idx[0], idx[1], idx[2], idx[3]); // A6
        }

        binding.paramsCbv = cb.gpu;
        binding.material = shadow ? leadInst_->InstancedShadowMaterialForSlot(slot) // plan S8
                                  : leadInst_->InstancedGraphicsMaterialForSlot(slot);
        RenderContext ctx{};
        // Plan S8: the shadow pass needs a slot's textures only for a masked PSO, and always as a table.
        const bool wantTextures = !shadow || render::ShadowMaterialIsMasked(binding.material);
        if (MaterialData* md = wantTextures ? leadInst_->InstanceSlotData(slot) : nullptr)
        {
            md->StageGBufferBindings(renderer, ctx, 0, 0, !shadow && binding.material && binding.material->IsBindless());
        }
        binding.srvTable = ctx.srvTable[0];
        binding.samplerTable = ctx.samplerTable[0];
    }
}

void InstancedDrawBatch::RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& /*lightView*/, const mat4& /*lightProj*/, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod, bool /*chunkCameraLods*/, const Frustum* /*chunkCullFrustum*/, bool linearDepth)
{
    if (!renderer || !cl || !shadowMat_ || !mesh_ || members_.empty()) { return; }
    // Plan S4: a linear-depth local view draws the shadow material's twin (logged at its creation if it has none).
    Material* const mat = (linearDepth && shadowMat_->LinearDepthVariant()) ? shadowMat_->LinearDepthVariant() : shadowMat_;
    // Plan S8: a multi-slot run casts per submesh with each slot's shadow PSO, params and (masked) albedo.
    if (leadInst_) { PrepareSlotBindings(renderer, /*shadow=*/true); }
    // Shadows use the per-cascade LOD floor for the whole run (all casters in a cascade are at
    // ~the same depth slice), so no per-instance bucketing is needed here.
    RecordInstanced(renderer, cl, mat, viewCB, /*gbuffer=*/false, lod, members_, nullptr, linearDepth);
}

void InstancedDrawBatch::RecordInstanced(Renderer* renderer, ID3D12GraphicsCommandList* cl,
                                         Material* material, D3D12_GPU_VIRTUAL_ADDRESS viewCB, bool gbuffer, UINT lod,
                                         const std::vector<RenderableObjectBase*>& members,
                                         const float* fades, bool linearDepth)
{
    const size_t total = members.size();
    const bool wireframe = gbuffer && renderer->GetWireframeMode();

    // B2b: multi-slot gbuffer draws loop the LOD's submeshes; Render prepared each slot's
    // params/bindings once for all occupied LODs into a b2 slice read by INSTCB_SLOT_PARAMS.
    // B3: the shadow path loops the same ranges; local-light plan S8 binds each slot's instanced shadow PSO and b2
    // there (RenderShadow's PrepareSlotBindings), so a masked slot clips and a slot's foliage weight sways its shadow.
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

        // Dithered LOD crossfade weights (b3, gbuffer only — the shadow RS has no b3 and
        // shadows keep hard switches). Deliberately NOT part of InstancePerObject: that
        // 224-byte stride is shared with every shadow reader. Fixed-size slab (the shader
        // declares GBUFFER_MAX_INSTANCES/4 float4s); zeros = everything solid.
        D3D12_GPU_VIRTUAL_ADDRESS fadeCbv = 0;
        if (gbuffer)
        {
            constexpr UINT kFadeBytes = render::kMaxInstancesPerDraw * sizeof(float);
            auto fadeAlloc = renderer->GetFrameResource()->AllocDynamic(
                kFadeBytes, render::kConstantBufferAlignment);
            auto* fadeDst = static_cast<float*>(fadeAlloc.cpu);
            if (fades) { std::memcpy(fadeDst, fades + base, count * sizeof(float)); }
            else       { std::memset(fadeDst, 0, count * sizeof(float)); }
            if (count < render::kMaxInstancesPerDraw)
            {
                std::memset(fadeDst + count, 0,
                            (render::kMaxInstancesPerDraw - count) * sizeof(float));
            }
            fadeCbv = fadeAlloc.gpu;
        }

        if (!multiSlot)
        {
            RenderContext ctx{};
            ctx.cbv[0] = alloc.gpu; // b0: per-instance array
            ctx.cbv[1] = viewCB;    // b1: shared per-pass view CB
            if (gbuffer) { ctx.cbv[3] = fadeCbv; } // b3: crossfade weights

            if (gbuffer)
            {
                if (matData_)
                {
                    // Shared material textures (t0..t2) + sampler (s0); instances live in b0, so no
                    // t0 conflict with the GpuInstancedModels structured-buffer path.
                    matData_->StageGBufferBindings(renderer, ctx, 0, 0, material && material->IsBindless());
                    matData_->StageGBufferSurfaceParams(renderer, ctx, 2);
                }
                else
                {
                    MaterialData::StageNeutralGBufferSurfaceParams(renderer, ctx, 2);
                }
            }
            else if (matData_ && render::ShadowMaterialIsMasked(material))
            {
                // Plan S8: the masked shadow PSO clips by the albedo (the cutoff rides the instance array).
                matData_->StageGBufferBindings(renderer, ctx, 0, 0, /*bindless=*/false);
            }

            if (!material->Bind(cl, ctx, wireframe))
            {
                render::ReportMissingGBufferBindings(material, 0, "InstancedDrawBatch::Record");
                continue; // never draw with an unbound root parameter (GBufferBindingGuard.h)
            }
            mesh_->DrawInstanced(cl, count, lod);
            continue;
        }

        // One ranged instanced draw per submesh; the instance array (b0) is shared across the
        // loop, only the combined material/surface b2 + the SRV/sampler tables change
        // (redundant rebinds elided by the bind
        // cache). Null slot materials draw flat: texFlags gate all sampling in the PS. The
        // shadow path (gbuffer=false) draws the same ranges with each slot's shadow PSO and b2 (plan S8).
        const size_t slotCount = leadInst_->InstanceSlotCount();
        RenderContext ctx{};
        ctx.cbv[0] = alloc.gpu; // b0: per-instance array (shared by every submesh)
        ctx.cbv[1] = viewCB;    // b1: shared per-pass view CB
        if (gbuffer) { ctx.cbv[3] = fadeCbv; } // b3: crossfade weights (shared by every submesh)
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

                // (the check moved into Material::Bind -- only it knows the root signature)
                // A slot with NO material (or a material with no textures at all) leaves these
                // handles null. Material::Bind then skips the descriptor-table binds -- it has
                // nothing to bind -- and the draw would still go out, with gbuffer.hlsl's root
                // parameters 3 and 4 (SRV table, sampler table) UNBOUND. The pixel shader samples
                // them anyway, so the GPU reads whatever those root slots happen to contain.
                //
                // That is not a cosmetic glitch. Caught by GPU-based validation as
                // "Draw, Uninitialized root argument accessed ... gbuffer_common.hlsli(289)", and
                // its real-world symptom is the graphics QUEUE going quiet mid-batch: the device
                // stays healthy, TDR never fires (nothing faulted), our fences simply stop
                // advancing and every idle path waits forever. Dragging an un-materialed mesh into
                // the viewport hung the editor exactly this way.
                //
                // So: skip the DRAW, not just the bind. An invisible submesh is a bug worth seeing;
                // an unbound root argument is undefined behaviour.
            }
            else if (slot < slotBindingsScratch_.size())
            {
                // Plan S8: the shadow pass -- the slot's instanced shadow PSO (INSTCB_SLOT_PARAMS: its b2 carries the
                // foliage weight and the mask) or that PSO's linear-depth twin, and a masked slot's albedo table.
                const SlotBindingScratch& binding = slotBindingsScratch_[slot];
                ctx.cbv[2] = binding.paramsCbv;
                ctx.srvTable[0] = binding.srvTable;
                ctx.samplerTable[0] = binding.samplerTable;
                if (binding.material)
                {
                    Material* const linear = linearDepth ? binding.material->LinearDepthVariant() : nullptr;
                    slotMaterial = linear ? linear : binding.material;
                }
            }

            if (!slotMaterial->Bind(cl, ctx, wireframe))
            {
                render::ReportMissingGBufferBindings(slotMaterial, slot, "InstancedDrawBatch::Record");
                continue; // see GBufferBindingGuard.h -- never draw with an unbound root parameter
            }
            mesh_->DrawSubmeshInstanced(cl, static_cast<UINT>(s), count, lod);
        }
    }
}
