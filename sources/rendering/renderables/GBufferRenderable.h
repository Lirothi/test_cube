#pragma once

#include "rendering/renderables/RenderableObject.h"
#include "rendering/renderables/IInstanceable.h"
#include "materials/MaterialData.h"

class GBufferRenderable : public RenderableObject, public IInstanceable
{
public:
    GBufferRenderable(const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader);

    void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    MaterialParams& MaterialParamsRef() { return matParams_; }
    const MaterialParams& MaterialParamsRef() const { return matParams_; }

    MaterialData* GetMaterialData() const { return matData_.get(); }

    // RT (S10): augment the base instance with this object's albedo texture +
    // base-color tint so ray hits can be shaded with the real material color.
    bool GetRtInstance(RtInstanceDesc& out) const override
    {
        if (!RenderableObject::GetRtInstance(out))
        {
            return false;
        }
        if (matData_ && matData_->hasAlbedo)
        {
            out.albedoTex = matData_->albedo.GetResource();
            out.albedoSrv = matData_->albedo.GetSRVCPU();
        }
        out.baseColor = matParams_.baseColor;
        return true;
    }

    // Draw identity includes the textures (MaterialData) — objects sharing a PSO but not
    // textures must NOT batch together (the single instanced draw binds one texture set).
    RenderBatchKey BatchKey() const override { return RenderBatchKey{ mesh_.get(), GetGraphicsMaterial(), matData_.get() }; }

    // Step 5c: auto-instancing capability via IInstanceable. Non-null only when an instanced
    // shader variant was built at Init (i.e. the default gbuffer shader).
    const IInstanceable* AsInstanceable() const override { return instancedGraphicsMaterial_ ? this : nullptr; }
    Material* InstancedGraphicsMaterial() const override { return instancedGraphicsMaterial_.get(); }
    Material* InstancedShadowMaterial() const override { return instancedShadowMaterial_.get(); }
    void FillInstanceData(render::InstancePerObject& out) const override;

protected:
    void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx, const Camera& camera, uint8_t* cbData) override;
    void ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const override;

private:
    void BuildInstancedMaterials(Renderer* renderer);

    std::shared_ptr<MaterialData> matData_;
    MaterialParams matParams_;
    std::string matPreset_;

    // Instanced (cbuffer-array) variants of the gbuffer + shadow materials, built once at
    // Init when this object's graphics shader has an instanced counterpart. Shared/cached
    // across all objects of the same material by MaterialManager.
    std::shared_ptr<Material> instancedGraphicsMaterial_;
    std::shared_ptr<Material> instancedShadowMaterial_;
};
