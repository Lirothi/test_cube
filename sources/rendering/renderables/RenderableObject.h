#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <memory>
#include <cstdint>

#include "materials/Material.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/core/RenderContext.h"
#include "core/math/Math.h"
#include "rendering/renderables/RenderableObjectBase.h"
#include "core/math/AABB.h"

class Renderer;
class Camera;

class RenderableObject: public RenderableObjectBase {
public:
    class UniformBinder
    {
    public:
        virtual ~UniformBinder() = default;

        virtual void RebuildHandles(RenderableObject& /*owner*/) {}
        virtual void UpdateMainCB(RenderableObject& /*owner*/, Renderer* /*renderer*/, const Camera& /*camera*/, uint8_t* /*cbData*/) {}
        virtual void UpdateShadowCB(RenderableObject& /*owner*/, Renderer* /*renderer*/, const mat4& /*lightView*/, const mat4& /*lightProj*/, uint8_t* /*cbData*/) {}

    protected:
        template<typename T>
        bool UpdateUniform(RenderableObject& owner, const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData) const
        {
            return owner.UpdateUniform(handle, material, value, cbData);
        }

        template<typename T>
        bool UpdateUniform(RenderableObject& owner, const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData, uint32_t arrayIndex) const
        {
            return owner.UpdateUniform(handle, material, value, cbData, arrayIndex);
        }
    };

    RenderableObject(
        const std::string& inputLayout,
        const std::wstring& graphicsShader);
    virtual ~RenderableObject();

    // Lifecycle
    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    virtual void Tick(float /*dt*/) {}
    virtual void PostTick(float /*dt*/) override;

    // Base renderer: Compute -> Graphics
    virtual void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Camera& camera, D3D12_GPU_VIRTUAL_ADDRESS viewCB);
    void ExecuteCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) override;
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, D3D12_GPU_VIRTUAL_ADDRESS viewCB, UINT lod) override;
    virtual void OnMaterialHotReload(Renderer* renderer);

    // Transform
    const Math::mat4& GetModelMatrix() const { return modelMatrix_; }
    const Math::mat4& GetPreviousModelMatrix() const { return prevModelMatrix_; }
    void ResetMotionHistory()
    {
        prevModelMatrix_ = modelMatrix_;
        prevModelMatrixValid_ = true;
        modelMatrixChangedThisTick_ = false;
    }
    void SetModelMatrix(const Math::mat4& m)
    {
        if (prevModelMatrixValid_)
        {
            prevModelMatrix_ = modelMatrix_;
        }
        else
        {
            prevModelMatrix_ = m;
            prevModelMatrixValid_ = true;
        }
        modelMatrix_ = m;
        transformDirty_ = false;
        modelMatrixChangedThisTick_ = true;
        MarkWorldBoundsDirty();
    }

    void SetPosition(const Math::float3& p);
    void SetScale(const Math::float3& s);
    void SetRotationEulerRad(const Math::float3& eulerXYZ);
    void SetRotationEulerDeg(const Math::float3& eulerDegXYZ);

    Math::float3 GetPosition() const { return pos_; }
    Math::float3 GetScale() const { return scale_; }
    Math::float3 GetRotationEulerRad() const { return rotEuler_; }
    Math::mat4 GetOrientationMatrix() const { return Math::mat4::RotationFromEulerXYZRad(rotEuler_); }

    // Mesh/material
    Mesh* GetMesh() { return mesh_.get(); }
    const Mesh* GetMesh() const { return mesh_.get(); }

    AABB GetLocalBounds() const;
    const AABB& GetWorldBounds() const override;

    // Step 6: radius of ONE drawn instance for LOD selection. Default = the object's world
    // radius (standalone). Cloud/instanced objects override to the single-mesh radius so they
    // don't stay at LOD 0 forever (their aggregate world bounds span the whole cloud).
    virtual float GetLodRadius() const { return GetWorldBounds().GetRadius(); }

    // Step 6: camera LOD chosen in PrepareViews (with hysteresis), read at draw time.
    void SelectLod(const Camera& camera) override;
    unsigned int GetCameraLod() const override { return cameraLod_; }

    Material* GetGraphicsMaterial() const { return graphicsMaterial_.get(); }
    void SetGraphicsMaterial(Material* m);
    Material* GetShadowMaterial() const { return shadowMaterial_.get(); }

    virtual bool IsTransparent() const;

    virtual bool CastsShadow() const { return true; }

    // Draw identity (no MaterialData at this tier; GBufferRenderable adds textures).
    RenderBatchKey BatchKey() const override { return RenderBatchKey{ mesh_.get(), graphicsMaterial_.get(), nullptr }; }

    // RT (S5): a standalone opaque mesh contributes one TLAS instance at its CPU
    // world matrix. No material textures at this tier (GBufferRenderable adds the
    // albedo); GpuInstancedModels overrides back to false (GPU-driven transforms).
    bool GetRtInstance(RtInstanceDesc& out) const override
    {
        if (!mesh_ || mesh_->GetIndexCount() == 0 || IsTransparent())
        {
            return false;
        }
        out.mesh = mesh_.get();
        out.world = modelMatrix_;
        return true;
    }

protected:
    virtual void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) {}
    virtual void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData);
    virtual void RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx);
    void UpdateAndBindGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData);
    virtual void DrawGeometry(ID3D12GraphicsCommandList* cl, UINT lod = 0);

    void MarkTransformDirty();

    template<typename T>
    bool UpdateUniform(const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData)
    {
        if (!cbData) { return false; }
        if (!material) { return false; }
        if (!handle.field) { return false; }
        return material->UpdateCBField(handle, value, cbData);
    }

    template<typename T>
    bool UpdateUniform(const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData, uint32_t arrayIndex)
    {
        if (!cbData) { return false; }
        if (!material) { return false; }
        if (!handle.field) { return false; }
        return material->UpdateCBField(handle, value, cbData, arrayIndex);
    }

    const std::wstring& GetGraphicsShaderPath() const { return graphicsShader_; }

protected:
    Material::GraphicsDesc BuildGraphicsDesc(Renderer* renderer) const;
    Material::GraphicsDesc BuildShadowDesc(Renderer* renderer, const Material::GraphicsDesc& baseDesc) const;
    virtual void ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const;
    virtual void ConfigureShadowPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const;

protected:
    std::shared_ptr<Material>     graphicsMaterial_; // shader variant (PSO/RS)
    std::shared_ptr<Material>     shadowMaterial_;

    std::shared_ptr<Mesh> mesh_;
    Math::mat4 modelMatrix_;
    Math::mat4 prevModelMatrix_;

    // CB (upload, per-object)
    bool allowWireframe_ = true;

    void SetUniformBinder(std::unique_ptr<UniformBinder> binder);
    UniformBinder* GetUniformBinder() const { return uniformBinder_.get(); }

private:
    static std::wstring AppendSuffixBeforeExt(const std::wstring& file, const std::wstring& suffix);

    RenderableObject(const RenderableObject&) = delete;
    RenderableObject& operator=(const RenderableObject&) = delete;

    friend class UniformBinder;

    std::unique_ptr<UniformBinder> uniformBinder_;

    std::wstring graphicsShader_;
    std::string  inputLayoutKey_;

protected:
    void SetMesh(std::shared_ptr<Mesh> mesh);
    void MarkWorldBoundsDirty();

private:
    void RebuildModelMatrix();
    void UpdateWorldBoundsCache() const;

    mutable AABB worldBoundsCache_;
    mutable bool worldBoundsDirty_ = true;
    unsigned int cameraLod_ = 0u; // Step 6: camera LOD chosen in PrepareViews (persists for hysteresis)

    Math::float3 pos_{};
    Math::float3 scale_ = Math::float3(1.0f, 1.0f, 1.0f);
    Math::float3 rotEuler_{};
    bool transformDirty_ = true;
    bool prevModelMatrixValid_ = false;
    bool modelMatrixChangedThisTick_ = false;
};
