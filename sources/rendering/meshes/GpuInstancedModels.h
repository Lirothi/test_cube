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
    // Step 6: override the camera draw to issue one instanced draw per LOD tier (per-instance
    // LOD) — the base path would draw the whole cloud at one LOD.
    void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB) override;
    // Step 6: build the per-instance LOD partition in PrepareViews; Render just reads it.
    void SelectLod(const Camera& camera) override;
    bool IsSimpleRender() const { return false; }
    bool CastsShadow() const override { return true; }

    const AABB& GetWorldBounds() const override;
    // RT (S5): instance transforms are GPU-computed (instance_anim.hlsl), so this
    // cloud is excluded from the CPU-built TLAS for now (revisit when RT needs it).
    bool GetRtInstance(Mesh*&, Math::mat4&) const override { return false; }
    // LOD from a single instance's size (not the whole-cloud bound), at the cloud's distance.
    float GetLodRadius() const override { return GetMesh() ? GetMesh()->GetBoundingBox().GetRadius() : 0.0f; }

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
