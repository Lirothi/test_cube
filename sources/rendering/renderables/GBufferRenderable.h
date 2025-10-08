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

protected:
    void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx) override;
    void ConfigureGraphicsPipeline(Renderer* renderer, Material::GraphicsDesc& desc) const override;

private:
    std::shared_ptr<MaterialData> matData_;
    MaterialParams matParams_;
    std::string matPreset_;
};
