#pragma once
#include "rendering/renderables/GBufferRenderable.h"
#include "rendering/meshes/InstanceBuffer.h"
#include "materials/Material.h"
#include "materials/Texture2D.h"
#include <wrl.h>
#include <d3d12.h>
#include <string>
#include <memory>
#include <vector>
#include <array>
#include <cstdint>

class GpuInstancedModels : public GBufferRenderable {
public:
    GpuInstancedModels(
        std::string modelName,
        UINT numInstances,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader,
        const std::wstring& computeShader);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    void Tick(float deltaTime) override;
    void SyncSceneState(SceneObjectSyncReason reason) override;
    // Step 6: override the camera draw to issue one instanced draw per LOD tier (per-instance
    // LOD) — the base path would draw the whole cloud at one LOD.
    void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB) override;
    // Step 6: build the per-instance LOD partition in PrepareViews; Render just reads it.
    void SelectLod(const Camera& camera) override;
    bool IsSimpleRender() const { return false; }
    bool CastsShadow() const override { return true; }
    // One object drives many GPU-side instances, so it can't be a single per-caster indirect
    // entry — the GPU-driven shadow path (Step 6) skips it and it draws via its own instanced
    // RenderShadow instead.
    bool IsGpuInstancedCaster() const override { return true; }
    // GI→VSM (Step 1): expose the per-instance transform buffer for the GI-scatter compute.
    D3D12_CPU_DESCRIPTOR_HANDLE GetInstanceCasterSrv() const override { return instanceBuffer_.GetSRVCPU(); }
    UINT GetInstanceCasterCount() const override { return instanceCount_; }
    ID3D12Resource* GetInstanceCasterResource() const override { return instanceBuffer_.GetResource(); }
    // Rung 1 (Step 10): per-instance rotation animates every frame -> always dynamic.
    bool IsDynamicCaster() const override { return true; }

    const AABB& GetWorldBounds() const override;
    // RT: a single instance is meaningless for the whole field — GetRtInstance stays
    // false; GetRtInstances (S14) emits one TLAS instance per GPU instance instead,
    // reconstructing each world matrix on the CPU (matches instance_anim.hlsl).
    bool GetRtInstance(RtInstanceDesc&) const override { return false; }
    size_t GetRtInstances(std::vector<RtInstanceDesc>& out) const override;
    // LOD from a single instance's size (not the whole-cloud bound), at the cloud's distance.
    // Vertex-enclosing sphere, not the AABB CORNER radius -- same correction as
    // RenderableObject::GetLodRadius (Unreal selects on Bounds.SphereRadius). Mesh-local on
    // purpose: an instanced cloud selects on ONE instance's size, not the aggregate bound.
    float GetLodRadius() const override
    {
        const Mesh* m = GetMesh();
        if (!m) { return 0.0f; }
        const float r = m->GetBoundingSphereRadius();
        return r > 1e-6f ? r : m->GetBoundingBox().GetRadius();
    }

    bool PrepareCompute(RenderGraphPassContext& ctx) override;
    void PrepareRender(RenderGraphPassContext& ctx) override;

protected:
    void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx) override;
    void DrawGeometry(ID3D12GraphicsCommandList* cl, UINT lod) override;

private:
    // Instancing data
    InstanceBuffer instanceBuffer_;
    UINT instanceCount_ = 0;
    float deltaTime_ = 0.0f;
    float angularSpeed_ = DirectX::XM_PIDIV2;

    // Compute pipeline
    std::shared_ptr<Material> computeMaterial_;
    std::wstring computeShader_;

    // Model / texture
    std::string modelName_;

    // CPU-side tracking of instance animation to compute bounds
    std::vector<float> instanceRotations_;
    mutable AABB instancedWorldBounds_;
    mutable AABB cachedWorstCaseLocalBounds_;
    mutable bool instanceBoundsDirty_ = true;
    mutable Math::mat4 lastModelMatrix_;
    mutable const Mesh* cachedMesh_ = nullptr;

    // Step 6 per-instance LOD: bucket instances by tier (from each instance's world position),
    // building instanceRemap_ (draw order, grouped by tier) + per-tier start/count.
    static constexpr UINT kLodTiers = 4;       // base + 3
    static constexpr UINT kMaxLodInstances = 256; // matches gRemap[64] in gbuffer_inst.hlsl
    std::array<uint32_t, kMaxLodInstances> instanceRemap_{};
    std::array<UINT, kLodTiers> tierBase_{};
    std::array<UINT, kLodTiers> tierCount_{};
    std::array<uint8_t, kMaxLodInstances> instanceLastTier_{}; // per-instance hysteresis state
    void BuildLodPartition(const Math::float3& camPos);

    void MarkInstanceBoundsDirty();
    Math::float3 ComputeInstanceOffset(UINT index) const;
    Math::mat4 BuildInstanceTransform(UINT index) const;
    AABB ComputeCombinedWorstCaseLocalBounds(const AABB& meshLocalBounds) const;
    void UpdateWorldBoundsCache() const;
};
