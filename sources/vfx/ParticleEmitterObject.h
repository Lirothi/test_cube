#pragma once
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "rendering/renderables/RenderableObject.h"
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

    const vfx::EmitterDesc& Desc() const { return desc_; }
    vfx::EmitterDesc& DescRef() { return desc_; } // live tweaks: consumed at the next CB fill

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

    Microsoft::WRL::ComPtr<ID3D12Resource> particles_; // GpuParticle[maxParticles], UAV+SRV
    Microsoft::WRL::ComPtr<ID3D12Resource> deadList_;  // uint[maxParticles], UAV
    Microsoft::WRL::ComPtr<ID3D12Resource> deadCount_; // uint[4] ([0] used), UAV
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;  // 4-slot ring of the dead counter
    const uint32_t* readbackPtr_ = nullptr;            // persistently mapped

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpuHeap_; // CPU-only staging descriptors
    D3D12_CPU_DESCRIPTOR_HANDLE particlesUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE deadListUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE deadCountUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE particlesSrv_{}; // consumed by the E2 billboard draw

    std::shared_ptr<Material> updateCs_;
    std::shared_ptr<Material> spawnCs_;
};
