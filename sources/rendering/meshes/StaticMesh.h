#pragma once
#include "rendering/renderables/GBufferRenderable.h"
#include "core/math/Math.h"

class StaticMesh : public GBufferRenderable
{
public:
    StaticMesh(const std::string& modelName,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader);

    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive) override;

    bool IsSimpleRender() const override { return true; }
    bool CastsShadow() const override { return true; }

private:
    std::string modelName_;
};
