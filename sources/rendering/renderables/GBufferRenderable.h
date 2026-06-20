#pragma once

#include "rendering/renderables/RenderableObject.h"
#include "materials/MaterialData.h"

class GBufferRenderable : public RenderableObject
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

    // Draw identity includes the textures (MaterialData) — objects sharing a PSO but not
    // textures must NOT batch together (the single instanced draw binds one texture set).
    RenderBatchKey BatchKey() const override { return RenderBatchKey{ mesh_.get(), GetGraphicsMaterial(), matData_.get() }; }

    // Step 4 auto-instancing: opt in when an instanced shader variant was built (Init).
    bool SupportsInstancing() const override { return instancedGraphicsMaterial_ != nullptr; }
    Material* GetInstancedGraphicsMaterial() const override { return instancedGraphicsMaterial_.get(); }
    Material* GetInstancedShadowMaterial() const override { return instancedShadowMaterial_.get(); }
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
