#pragma once
// R4 (docs/scene_renderer_refactor_plan.md): the scene's ray-tracing acceleration structures.
//
// Owns the per-mesh BLAS cache, the per-frame TLAS, the bindless geometry/material table the hit
// shading reads, and the two caches that keep the build incremental. SceneRenderer used to hold
// all of it plus the 178-line build body, which is work it has no other business with — the AS is
// consumed by the RT reflection and RT debug dispatches, not by the scene renderer itself.
//
// `Manager()` and `Bindless()` are deliberately exposed: those two objects ARE the interface the
// RT passes bind from (TLAS SRV, per-frame descriptor writes), and wrapping every one of their
// calls in a forwarder here would be a second name for the same thing.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>

#include "rendering/core/RenderGraph.h"
#include "rendering/renderables/RenderableObjectBase.h" // RtInstanceDesc
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/rt/BindlessTable.h"

class Renderer;
class RenderableObjectBase;
class GBufferRenderable;
class Material;
class Mesh;
struct SceneFrameData;

class RtSceneAs
{
public:
    ~RtSceneAs();
    // Level switch: the cached BLAS/TLAS key off Mesh*, which a reload invalidates.
    void Reset();
    // I2: a material's textures were rebuilt, so every mesh must re-register with its CURRENT
    // SRVs. MUST be called with the GPU idle.
    void Invalidate();

    // Lazy device init, from the frame gate that decides RT runs at all. Separate from Build()
    // because the RT passes' BUILDERS query the manager while the graph is being assembled,
    // i.e. before any body records.
    void EnsureInit(Renderer* renderer);

    // The Main_BuildAS pass body: gather this frame's instances, build/refit, publish the TLAS and
    // the bindless table. Records into the pass's own command list. windDeformMat is the
    // rt_wind_deform_cs PSO (null / null-pipeline => wind casters keep their rest-pose BLAS).
    void Build(Renderer* renderer, RenderGraphPassContext ctx, const SceneFrameData& frame,
               Material* windDeformMat);

    rt::AccelerationStructureManager& Manager() { return asManager_; }
    const rt::AccelerationStructureManager& Manager() const { return asManager_; }
    rt::BindlessTable& Bindless() { return bindless_; }
    const rt::BindlessTable& Bindless() const { return bindless_; }
    D3D12_CPU_DESCRIPTOR_HANDLE TlasSrvCpu(UINT frameIndex) const
    {
        return asManager_.TlasSrvCpu(frameIndex);
    }

private:
    // S5/S9: the BLAS cache and the per-frame TLAS, plus the bindless VB/IB + geometry-info table
    // the RT hit shading reads. Buffers the GPU may still read retire through the manager's
    // fence-guarded bin (stamped at the end of Build, released at its start).
    rt::AccelerationStructureManager asManager_;
    rt::BindlessTable bindless_;
    bool asManagerInited_ = false;
    bool memRegistered_ = false; // memory report providers (rt.as, rt.as.bin) registered once
    bool asVramLogged_ = false;  // S13: one-time AS VRAM accounting log
    std::vector<rt::InstanceEntry> rtInstances_; // reused scratch (only Build touches it)

    // The rest of Build()'s per-frame scratch, kept for the same reason: each is cleared at its
    // point of use and keeps last frame's capacity. As locals they grew from zero every frame --
    // the reuse-deny list alone is one entry per wind-capable caster (~1100 on wind_test), 19
    // reallocations and 124 KB a frame; with the candidates and the plan, 150 thousand
    // short-lived allocations in 50 s of orbit (memtrace churn, 80 % of the CPU churn bytes).
    // RW: the deform/refit recording plan for this frame -- (slot, entryIndex, gb).
    struct WindPlanEntry { UINT slot; size_t entryIndex; GBufferRenderable* gb; };
    struct WindCandidate { size_t entryIndex; GBufferRenderable* gb; float distSq; bool tight; };
    struct WindReuseEntry { uint32_t recordBase; uint32_t recordCount; size_t entryIndex; bool sway; };
    std::vector<WindPlanEntry> windPlan_;
    std::vector<WindCandidate> windCandidates_;
    std::vector<WindReuseEntry> windReuseEntries_;
    std::vector<uint8_t> windPlanOk_;
    std::vector<D3D12_RESOURCE_BARRIER> windStreamBarriers_;
    std::vector<RtInstanceDesc> instanceDescs_; // a GPU-instanced caster's instances
    std::vector<rt::BindlessTable::SlotMaterial> slotMats_;

    // RW: stable owner -> dynamic-BLAS slot binding. Stability is what makes the per-frame work a
    // REFIT instead of a rebuild -- a slot only rebuilds when its owner changes (or on cadence).
    std::array<const void*, rt::AccelerationStructureManager::kMaxWindBlasSlots> windSlotOwner_{};

    // Per-object bindless registration is stable across frames even though TLAS transforms are
    // rebuilt every frame; this is what makes the rebuild incremental instead of a full
    // re-registration. A material fingerprint change forces the entry to be re-made.
    struct RtBindlessObjectCache
    {
        const RenderableObjectBase* object = nullptr;
        const Mesh* mesh = nullptr;
        uint64_t materialFingerprint = 0;
        uint64_t nonOpaqueSlots = 0; // Part C: per-submesh masked bits for the BLAS build
        uint32_t instanceId = 0;
        bool valid = false;
    };
    std::vector<RtBindlessObjectCache> rtBindlessObjectCache_;
};
