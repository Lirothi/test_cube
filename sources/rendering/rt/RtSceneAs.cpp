// R4 (docs/scene_renderer_refactor_plan.md): the RT acceleration structures.
//
// The body of what was SceneRenderer::Pass_BuildAS, moved VERBATIM, together with the six members
// only it and the RT dispatches ever read: the BLAS/TLAS manager, the bindless table, the
// one-time-scratch retire frame, the reused instance scratch and the per-object bindless cache.
//
// It is still driven by a render-graph pass — Main_BuildAS calls Build() — because the AS build
// has to be recorded into a command list like anything else. What moved is the ownership.

#include "rendering/rt/RtSceneAs.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "app/camera/Camera.h"
#include "app/scene/SceneFrameData.h"
#include "vfx/WindState.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"
#include "rendering/core/BarrierTranslation.h"
#include "rendering/core/Renderer.h"
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/renderables/RenderableObject.h"

namespace
{
    // Moved here from the SceneRenderer internal header: the AS build is its only caller, and a
    // helper that one subsystem uses belongs with that subsystem.
    uint64_t RtMaterialFingerprint(const GBufferRenderable& object)
    {
        // Slot 0 is the only material-parameter slot with a public mutable accessor; slots 1+
        // are immutable after Init and material/mesh hot reloads call Invalidate().
        const MaterialParams& p = object.MaterialParamsRef();
        uint64_t h = 1469598103934665603ull;
        const auto mix = [&h](uint64_t value) { h ^= value; h *= 1099511628211ull; };
        const auto mixFloat = [&mix](float value)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));
            mix(bits);
        };
        mix(reinterpret_cast<uint64_t>(object.GetMaterialData()));
        mix(static_cast<uint64_t>(object.SlotCount()));
        mixFloat(p.baseColor.x); mixFloat(p.baseColor.y);
        mixFloat(p.baseColor.z); mixFloat(p.baseColor.w);
        mixFloat(p.metalRough.x); mixFloat(p.metalRough.y);
        mixFloat(p.mrMultiply);
        mixFloat(p.texFlags.y); // useMR controls whether the MR descriptor participates
        mixFloat(p.alphaCutoff); // Part C: the cutoff travels in the bindless record
        return h;
    }
}

void RtSceneAs::Reset()
{
    windSlotOwner_.fill(nullptr);
    // Drop cached BLAS/TLAS — their Mesh* keys become dangling across a level reload. Re-inited
    // lazily on the next RT-enabled frame.
    asManager_.Reset();
    bindless_.Reset();
    asManagerInited_ = false;
    asScratchRetireFrame_ = 0;
    rtInstances_.clear();
    rtBindlessObjectCache_.clear();
}

void RtSceneAs::Invalidate()
{
    // I2: drop the cached structures + bindless geometry-info so the next RT frame re-registers
    // every mesh with its CURRENT material SRVs. MUST be called with the GPU idle.
    asManager_.Reset();
    bindless_.Reset();
    asManagerInited_ = false;
    asScratchRetireFrame_ = 0;
    rtInstances_.clear();
    rtBindlessObjectCache_.clear();
}

void RtSceneAs::EnsureInit(Renderer* renderer)
{
    if (asManagerInited_) { return; }
    asManager_.Init(renderer->GetDevice5());
    bindless_.Init(renderer->GetDevice());
    asManagerInited_ = true;
}

namespace
{
    // Mirrors the `Deform` cbuffer in rt_wind_deform_cs.hlsl (row-major matrices, 16B rows).
    struct RtWindDeformCB
    {
        Math::mat4 world;
        Math::mat4 invWorld;
        Math::float2 windDirXZ; float swayAmp = 0.0f; float swayFreq = 0.0f;
        float gustMul = 1.0f; float timeSec = 0.0f; float windStrength = 0.0f; float trunkStiff = 1.0f;
        float leafScale = 0.0f; float foliage = 0.0f; uint32_t vertexCount = 0; uint32_t vertexStride = 0;
    };

}

void RtSceneAs::Build(Renderer* renderer, RenderGraphPassContext ctx,
                      const SceneFrameData& frame, Material* windDeformMat)
{
    if (!renderer)
    {
        return;
    }

    // Retire scratch from earlier (one-time) BLAS builds once their command
    // list's frame has surely completed — kFrameCount frames later, when that
    // frame slot is reused and BeginFrame has waited on its fence.
    const uint64_t frameNo = renderer->GetTotalFrameNumber();
    if (asManager_.HasPendingScratch() && frameNo >= asScratchRetireFrame_)
    {
        asManager_.ReleaseCompletedScratch();
    }

    // Gather opaque, single-mesh, CPU-placed instances. Ocean (GPU-displaced) is
    // excluded by design — kept on its planar-reflection path (S13). Instanced clouds
    // (S14) and transparent/glass (S15) also return false from GetRtInstance today;
    // bringing each into RT is its own step.
    rtInstances_.clear();
    // RW: the deform/refit recording plan for this frame -- (slot, entryIndex, gb).
    struct WindPlanEntry { UINT slot; size_t entryIndex; GBufferRenderable* gb; };
    std::vector<WindPlanEntry> windPlan;
    if (frame.objects)
    {
        const auto& objects = *frame.objects;
        if (rtBindlessObjectCache_.size() != objects.size())
        {
            rtBindlessObjectCache_.resize(objects.size());
        }
        uint32_t instanceId = 0;
        std::vector<RtInstanceDesc> descs; // reused across objects this frame
        // RW: near wind casters collected during the gather; the deform + refit recording below
        // patches their entries with a per-instance deformed BLAS.
        const bool windActive = frame.settings.rtWindBlas && frame.wind && frame.wind->active &&
                                frame.wind->swayAmplitude > 0.0f && windDeformMat != nullptr &&
                                windDeformMat->GetPipelineState() != nullptr;
        const Math::float3 camPos = frame.camera ? frame.camera->GetPosition() : Math::float3{};
        // Radius from the settings; the WIDE radius is the hysteresis band -- an owner keeps its
        // slot while inside it, newcomers are admitted only from the tight one. Without the band,
        // palms at near-equal distances churn the binding every frame (rebuild storms, and the
        // fence-retired buffer bin never drains because something new retires every frame).
        const float tightR = std::clamp(frame.settings.rtWindBlasRadius, 0.0f, 200.0f);
        const float tightR2 = tightR * tightR;
        const float wideR2 = tightR2 * (1.2f * 1.2f);
        struct WindCandidate { size_t entryIndex; GBufferRenderable* gb; float distSq; bool tight; };
        std::vector<WindCandidate> windCandidates;
        // wind_test has hundreds of identical multi-slot palms. Keep this scratch allocation
        // across objects instead of allocating/freeing a 4-5 element vector for every palm.
        std::vector<rt::BindlessTable::SlotMaterial> slotMats;
        for (size_t objectIndex = 0; objectIndex < objects.size(); ++objectIndex)
        {
            if (bindless_.BuildFailed()) { break; }
            const auto& obj = objects[objectIndex];
            // The editor's Enabled command maps to visibility. In no-editor
            // builds ordinary level objects retain their default visible state,
            // so this only removes explicitly disabled editor objects from RT.
            if (!obj || !obj->IsVisible()) { continue; }
            // Avoid the vector append for the common one-object/one-instance path. Only the GPU
            // instanced caster overrides GetRtInstances today and needs the reusable array.
            RtInstanceDesc singleDesc{};
            const RtInstanceDesc* descData = nullptr;
            size_t descCount = 0;
            if (obj->IsGpuInstancedCaster())
            {
                descs.clear();
                obj->GetRtInstances(descs);
                descData = descs.data();
                descCount = descs.size();
            }
            else if (obj->GetRtInstance(singleDesc))
            {
                descData = &singleDesc;
                descCount = 1;
            }
            // Per-slot RT materials (B3 follow-up): a multi-slot object registers one record per
            // submesh with THAT slot's albedo/MR/params, so palms reflect bark + green fronds
            // instead of slot-0 everywhere. Hit shaders already index per (InstanceID +
            // GeometryIndex). Single-slot objects and GI instance clouds keep the slot-0 path.
            GBufferRenderable* gb = obj->AsGBufferRenderable();
            const bool perSlot = bindless_.Ready() && gb && gb->MultiSlotDraw();
            // Cache unchanged multi-slot objects, but never share mutable material records between
            // owners: editing one palm must not change its neighbours. BindlessTable independently
            // shares the immutable mesh/texture descriptors, so this does not multiply heap usage.
            RtBindlessObjectCache& objectCache = rtBindlessObjectCache_[objectIndex];
            const uint64_t materialFingerprint = perSlot ? RtMaterialFingerprint(*gb) : 0;
            bool reuseObjectCache = perSlot && objectCache.valid &&
                objectCache.object == obj.get() && objectCache.mesh == gb->GetMesh() &&
                objectCache.materialFingerprint == materialFingerprint;
            if (perSlot && !reuseObjectCache)
            {
                // assign() value-initializes reused entries too, clearing descriptor handles left
                // by the preceding object when this one has no texture for a slot.
                slotMats.assign(gb->SlotCount(), {});
                for (size_t s = 0; s < slotMats.size(); ++s)
                {
                    MaterialData* md = gb->GetMaterialDataForSlot(s);
                    const MaterialParams* p = gb->InstanceSlotParams(s);
                    if (md && md->hasAlbedo) { slotMats[s].albedoSrv = md->albedo.GetSRVCPU(); }
                    if (md && md->hasMR && p && p->texFlags.y > 0.5f) { slotMats[s].mrSrv = md->mr.GetSRVCPU(); }
                    if (p)
                    {
                        slotMats[s].baseColor4 = &p->baseColor.x;
                        slotMats[s].roughness = p->metalRough.y;
                        slotMats[s].metalness = p->metalRough.x;
                        slotMats[s].mrMultiply = p->mrMultiply > 0.5f;
                        // Part C: same value the raster clip uses; >= 0 only on MASK slots.
                        slotMats[s].alphaCutoff = p->alphaCutoff;
                    }
                }
                // Part C: per-submesh non-opaque bits for the BLAS build. Submesh s reads bindless
                // record s, which BindlessTable fills from slots[min(s, slotCount-1)] — the mask
                // MUST use the same mapping, or a BLAS flag and the record it gates would disagree.
                // Masked needs both a cutoff and an albedo to test (the record fill nulls the
                // cutoff without one).
                uint64_t nonOpaque = 0;
                const size_t submeshCount = gb->GetMesh() ? gb->GetMesh()->GetSubmeshCount() : 0;
                for (size_t s = 0; s < submeshCount && s < 64u; ++s)
                {
                    const rt::BindlessTable::SlotMaterial& sm =
                        slotMats[s < slotMats.size() ? s : slotMats.size() - 1];
                    if (sm.alphaCutoff >= 0.0f && sm.albedoSrv.ptr != 0)
                    {
                        nonOpaque |= 1ull << s;
                    }
                }
                objectCache.nonOpaqueSlots = nonOpaque;
            }
            for (size_t descIndex = 0; descIndex < descCount; ++descIndex)
            {
                const RtInstanceDesc& desc = descData[descIndex];
                rt::InstanceEntry entry;
                entry.mesh = desc.mesh;
                entry.world = desc.world.m; // Math::mat4 wraps a row-major XMFLOAT4X4
                // TLAS InstanceID = the mesh's bindless geometry index (S9), so a hit
                // can index the geometry/material table directly. Same owner+mesh ->
                // same index (all instances of a cloud share one record run). Falls back to
                // a running index if the bindless table isn't up.
                if (perSlot && desc.mesh == gb->GetMesh())
                {
                    entry.nonOpaqueSlots = objectCache.nonOpaqueSlots;
                    if (reuseObjectCache)
                    {
                        entry.instanceId = objectCache.instanceId;
                    }
                    else
                    {
                        entry.instanceId = bindless_.GetOrUpdateMesh(
                            obj.get(), desc.mesh, slotMats.data(), slotMats.size());
                    }
                    if (!reuseObjectCache)
                    {
                        objectCache.object = obj.get();
                        objectCache.mesh = desc.mesh;
                        objectCache.materialFingerprint = materialFingerprint;
                        objectCache.instanceId = entry.instanceId;
                        objectCache.valid = entry.instanceId != rt::BindlessTable::kInvalidGeometry;
                        reuseObjectCache = objectCache.valid;
                    }
                }
                else
                {
                    // Single-material path: every submesh shares slot 0's record, so masked-ness
                    // is uniform across the BLAS geometries.
                    entry.nonOpaqueSlots =
                        (desc.alphaCutoff >= 0.0f && desc.albedoSrv.ptr != 0) ? ~0ull : 0ull;
                    entry.instanceId = bindless_.Ready()
                        ? bindless_.GetOrUpdateMesh(obj.get(), desc.mesh, desc.albedoSrv, desc.mrSrv, &desc.baseColor.x,
                                                      /*roughness*/ desc.metalRough.y, /*metalness*/ desc.metalRough.x,
                                                      desc.mrMultiply, desc.alphaCutoff)
                        : instanceId;
                }
                if (entry.instanceId == rt::BindlessTable::kInvalidGeometry) { break; }
                // RW: a CPU-placed single-instance wind caster near the camera is a candidate for
                // a deformed BLAS. GPU-instanced clouds (descCount > 1) are excluded -- their
                // per-instance transforms stream from the GPU side and stay rest-pose for now.
                if (windActive && descCount == 1 && gb && desc.mesh == gb->GetMesh() &&
                    gb->EffectiveWindStrength(gb->GetWindStrength()) > 0.0f)
                {
                    const Math::float3 d(entry.world._41 - camPos.x, entry.world._42 - camPos.y,
                                         entry.world._43 - camPos.z);
                    const float distSq = d.x * d.x + d.y * d.y + d.z * d.z;
                    if (distSq < wideR2)
                    {
                        windCandidates.push_back({ rtInstances_.size(), gb, distSq,
                                                   distSq < tightR2 });
                    }
                }
                rtInstances_.push_back(entry);
                ++instanceId;
            }
        }

        // RW slot binding with HYSTERESIS: existing owners keep their slot while inside the
        // wide radius (they refit, never rebuild); slots free only when the owner leaves the
        // band or disappears; newcomers are admitted nearest-first from the tight radius into
        // whatever slots remain.
        {
            constexpr UINT kSlots = rt::AccelerationStructureManager::kMaxWindBlasSlots;
            for (UINT sIdx = 0; sIdx < kSlots; ++sIdx)
            {
                const void* owner = windSlotOwner_[sIdx];
                if (!owner) { continue; }
                bool still = false;
                for (const WindCandidate& c : windCandidates)
                {
                    if (static_cast<const void*>(c.gb) == owner) { still = true; break; }
                }
                if (!still) { windSlotOwner_[sIdx] = nullptr; }
            }
            std::sort(windCandidates.begin(), windCandidates.end(),
                      [](const WindCandidate& a, const WindCandidate& b)
                      { return a.distSq < b.distSq; });
            for (const WindCandidate& c : windCandidates)
            {
                UINT slot = kSlots;
                for (UINT sIdx = 0; sIdx < kSlots; ++sIdx)
                {
                    if (windSlotOwner_[sIdx] == static_cast<const void*>(c.gb)) { slot = sIdx; break; }
                }
                if (slot == kSlots)
                {
                    if (!c.tight) { continue; } // newcomers only from the tight radius
                    for (UINT sIdx = 0; sIdx < kSlots; ++sIdx)
                    {
                        if (!windSlotOwner_[sIdx]) { windSlotOwner_[sIdx] = c.gb; slot = sIdx; break; }
                    }
                }
                if (slot < kSlots)
                {
                    windPlan.push_back({ slot, c.entryIndex, c.gb });
                }
            }
        }
    }
    if (!bindless_.UploadGeometryInfo(renderer->GetCurrentFrameIndex()))
    {
        // Never trace a partial/old material table. The next frame selects SSR on a sticky failure.
        rtInstances_.clear();
    }

    auto t = ctx.BeginCL();
    SetCommandListName(t.cl, ctx.pass);
    {
        GPU_SCOPE(t.cl, ProfilerScopes::kPassBuildAS);
        // AS buffers bypass the barrier compile: this pass declares no
        // resource states and never calls Transition on them, so the RenderGraph
        // never moves them out of RAYTRACING_ACCELERATION_STRUCTURE / UNORDERED_
        // ACCESS. Mesh VB/IB are read by first-frame BLAS builds via implicit
        // COMMON->NON_PIXEL_SHADER_RESOURCE promotion (this pass runs first, so
        // the buffers are fresh-decayed to COMMON).
        ID3D12GraphicsCommandList4* cl4 = renderer->AsCmdList4(t.cl);
        if (cl4)
        {
            // RW: deform the near wind casters and build/refit their per-instance BLASes
            // BEFORE the TLAS references them. Two loops on purpose: all deform dispatches
            // first (one PSO bind, and they can overlap on the GPU), then the builds.
            if (!windPlan.empty())
            {
                renderer->BindDescriptorHeaps(t.cl); // the staged SRV/UAV tables live there
                t.cl->SetComputeRootSignature(windDeformMat->GetRootSignature());
                t.cl->SetPipelineState(windDeformMat->GetPipelineState());
                std::vector<uint8_t> planOk(windPlan.size(), 0);
                for (size_t w = 0; w < windPlan.size(); ++w)
                {
                    const WindPlanEntry& pe = windPlan[w];
                    Mesh* mesh = rtInstances_[pe.entryIndex].mesh;
                    D3D12_CPU_DESCRIPTOR_HANDLE srcSrvCpu{};
                    D3D12_CPU_DESCRIPTOR_HANDLE dstUavCpu{};
                    UINT vertexCount = 0;
                    if (!asManager_.PrepareWindSlot(pe.slot, mesh, cl4, srcSrvCpu, dstUavCpu,
                                                    vertexCount))
                    {
                        continue;
                    }
                    planOk[w] = 1;

                    RtWindDeformCB cb{};
                    cb.world = Math::mat4(rtInstances_[pe.entryIndex].world);
                    cb.invWorld = Math::mat4::Inverse(cb.world);
                    cb.windDirXZ = frame.wind->windDirXZ;
                    cb.swayAmp = frame.wind->swayAmplitude;
                    cb.swayFreq = frame.wind->swayFrequency;
                    cb.gustMul = frame.wind->gustMul;
                    cb.timeSec = frame.wind->time;
                    cb.windStrength = pe.gb->EffectiveWindStrength(pe.gb->GetWindStrength());
                    cb.trunkStiff = pe.gb->GetWindTrunkStiffness();
                    cb.leafScale = pe.gb->GetWindLeafScaleWorld();
                    float foliage = 0.0f;
                    for (size_t slotIdx = 0; slotIdx < pe.gb->SlotCount(); ++slotIdx)
                    {
                        foliage = std::max(foliage, pe.gb->FoliageForSlot(slotIdx));
                    }
                    cb.foliage = foliage;
                    cb.vertexCount = vertexCount;
                    cb.vertexStride = mesh->GetVertexStride();

                    auto alloc = renderer->GetFrameResource()->AllocDynamic(
                        sizeof(RtWindDeformCB), render::kConstantBufferAlignment);
                    std::memcpy(alloc.cpu, &cb, sizeof(cb));
                    t.cl->SetComputeRootConstantBufferView(0, alloc.gpu);
                    t.cl->SetComputeRootDescriptorTable(
                        1, renderer->StageSrvUavTable({ srcSrvCpu }).gpu);
                    t.cl->SetComputeRootDescriptorTable(
                        2, renderer->StageSrvUavTable({ dstUavCpu }).gpu);
                    t.cl->Dispatch((vertexCount + 63u) / 64u, 1, 1);
                }
                // ONE batched transition for every deformed stream, then all builds
                // back-to-back, then the AS-read barriers together: interleaving build+barrier
                // per slot serialized the GPU at ~34 us/palm; batched, the builds pipeline.
                std::vector<D3D12_RESOURCE_BARRIER> streamBarriers;
                streamBarriers.reserve(windPlan.size());
                for (size_t w = 0; w < windPlan.size(); ++w)
                {
                    if (planOk[w] == 0) { continue; }
                    D3D12_RESOURCE_BARRIER b{};
                    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    b.Transition.pResource = asManager_.WindStreamResource(windPlan[w].slot);
                    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    streamBarriers.push_back(b);
                }
                if (!streamBarriers.empty())
                {
                    t.cl->ResourceBarrier(static_cast<UINT>(streamBarriers.size()),
                                          streamBarriers.data());
                }
                for (size_t w = 0; w < windPlan.size(); ++w)
                {
                    if (planOk[w] == 0) { continue; }
                    const WindPlanEntry& pe = windPlan[w];
                    const D3D12_GPU_VIRTUAL_ADDRESS blasVa = asManager_.BuildOrRefitWindSlot(
                        pe.slot, rtInstances_[pe.entryIndex].nonOpaqueSlots, frameNo, cl4);
                    rtInstances_[pe.entryIndex].blasOverride = blasVa;
                }
                for (size_t w = 0; w < windPlan.size(); ++w)
                {
                    if (planOk[w] == 0) { continue; }
                    barriers::EmitAccelerationStructureBuildBarrier(
                        cl4, asManager_.WindBlasResource(windPlan[w].slot));
                }
            }

            // BuildTlas records the zero count as well. That prevents a reused
            // frame slot from exposing a previous frame's TLAS after the last
            // visible RT instance is disabled.
            asManager_.BuildTlas(rtInstances_, cl4, renderer->GetCurrentFrameIndex());
            if (asManager_.HasPendingScratch())
            {
                asScratchRetireFrame_ = frameNo + render::kFrameCount;
            }
            // S13: one-time AS VRAM accounting for visibility/budgeting.
            if (!rtInstances_.empty() && !asVramLogged_ && !asManager_.BuildFailed())
            {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "[RT] Acceleration structures: %.2f MB VRAM, %zu instances.\n",
                              asManager_.GetAsMemoryBytes() / (1024.0 * 1024.0), rtInstances_.size());
                OutputDebugStringA(buf);
                asVramLogged_ = true;
            }
        }
    }
    ctx.EndCL(t);
}
