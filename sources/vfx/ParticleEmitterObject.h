#pragma once
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "core/math/AABB.h"
#include "materials/Texture2D.h"
#include "rendering/renderables/RenderableObject.h"
#include "rendering/core/ResourceDeclarations.h"
#include "vfx/ParticleTypes.h"

// Part E: a GPU-simulated particle emitter. E1 = the sim core: per-emitter DEFAULT-heap
// buffers (slot array + dead-list stack + atomic counter), spawn/update compute dispatches
// driven from the object-compute pass, CPU fractional-rate spawn accumulator, and a debug
// alive-count readback. E2 adds the billboard draw (until then Render() no-ops via the null
// graphics material) and E3 adds presets/editor authoring.
//
// Lifetime: the buffers die with the object; scene mutations already gate on GPU idle
// (level switches / editor WaitForPreviousFrame), matching how Mesh buffers are handled.
class ParticleEmitterObject : public RenderableObject
{
public:
    explicit ParticleEmitterObject(const vfx::EmitterDesc& desc);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;
    void Tick(float dt) override;

    bool CastsShadow() const override { return false; }
    bool IsSimpleRender() const override { return true; }
    // E2: draws in the sorted TransparentSimple bucket of Pass_Transparent.
    bool IsTransparent() const override { return true; }
    // The swept culling AABB is huge and has no solid surface — keep it out of viewport picking
    // so it never hijacks drag-drop placement (bug: mesh spawned at the camera). Select via the
    // outliner. Same rationale as GetWorldBounds' conservative swept box.
    bool IsRaycastPickable() const override { return false; }
    // Conservative swept bounds (position ± max travel + sprite size), updated in Tick — the
    // base implementation needs a mesh, which an emitter doesn't have.
    const AABB& GetWorldBounds() const override { return worldBounds_; }

    // E2: billboard draw — 6*maxParticles vertices straight from the sim buffer (t0); dead
    // slots collapse to zero-w degenerate quads (no alive-list / indirect args).
    void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB) override;

    ParticleEmitterObject* AsParticleEmitter() override { return this; } // E3: editor inspector

    const vfx::EmitterDesc& Desc() const { return desc_; }
    vfx::EmitterDesc& DescRef() { return desc_; } // live tweaks: consumed at the next CB fill

    bool PrepareCompute(RenderGraphPassContext& ctx) override;
    void PrepareRender(RenderGraphPassContext& ctx) override;

protected:
    void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;

private:
    void CreateBuffers(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void CreateDescriptors(ID3D12Device* device);

    vfx::EmitterDesc desc_;

    float dt_ = 0.0f;
    float spawnAccum_ = 0.0f;
    float logAccum_ = 0.0f;
    uint32_t frameCounter_ = 0;

    // Step 6b part 2: on the wrapper. These four had NO unregister path at all — an emitter
    // destroyed on a level switch left four dangling registry entries behind.
    GpuResource particles_; // GpuParticle[maxParticles], UAV+SRV
    GpuResource deadList_;  // uint[maxParticles], UAV
    GpuResource deadCount_; // uint[4] ([0] used), UAV
    GpuResource sorted_;    // E2c: uint[maxParticles] back-to-front order
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;  // 4-slot ring of the dead counter
    const uint32_t* readbackPtr_ = nullptr;            // persistently mapped

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpuHeap_; // CPU-only staging descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE particlesUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE deadListUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE deadCountUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE particlesSrv_{}; // consumed by the E2 billboard draw
    D3D12_CPU_DESCRIPTOR_HANDLE sortedUav_{};    // E2c sort output (compute)
    D3D12_CPU_DESCRIPTOR_HANDLE sortedSrv_{};    // E2c: read by the billboard VS

    std::shared_ptr<Material> updateCs_;
    std::shared_ptr<Material> spawnCs_;
    std::shared_ptr<Material> sortCs_; // E2c (only for sorted emitters)
    bool sortEnabled_ = false;         // desc_.sortParticles && maxParticles <= SORT_N
    Math::float3 lastCamPos_{0.0f, 0.0f, 0.0f}; // cached in Render for the next compute-pass sort

    // --- E2 rendering ---
    std::shared_ptr<Material> drawMaterial_; // particles.hlsl PSO (blend per desc_.additive)
    Texture2D sprite_;                       // optional atlas; procedural disc when absent
    bool hasSprite_ = false;
    AABB worldBounds_ = AABB::Empty();
};
