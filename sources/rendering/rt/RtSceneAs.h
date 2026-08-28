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
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/rt/BindlessTable.h"

class Renderer;
class RenderableObjectBase;
class Material;
class Mesh;
struct SceneFrameData;

class RtSceneAs
{
public:
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
    // the RT hit shading reads. `asScratchRetireFrame_` defers releasing one-time BLAS scratch
    // until the frame that used it has surely completed.
    rt::AccelerationStructureManager asManager_;
    rt::BindlessTable bindless_;
    bool asManagerInited_ = false;
    uint64_t asScratchRetireFrame_ = 0;
    bool asVramLogged_ = false;  // S13: one-time AS VRAM accounting log
    std::vector<rt::InstanceEntry> rtInstances_; // reused scratch (only Build touches it)

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
