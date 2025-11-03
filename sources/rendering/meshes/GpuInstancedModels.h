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
    bool IsSimpleRender() const { return false; }
    bool CastsShadow() const override { return true; }

    const AABB& GetWorldBounds() const override;

protected:
    void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData) override;
    void RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx) override;
    void DrawGeometry(ID3D12GraphicsCommandList* cl) override;

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

    void MarkInstanceBoundsDirty();
    Math::float3 ComputeInstanceOffset(UINT index) const;
    Math::mat4 BuildInstanceTransform(UINT index) const;
    AABB ComputeCombinedWorstCaseLocalBounds(const AABB& meshLocalBounds) const;
    void UpdateWorldBoundsCache() const;
};
