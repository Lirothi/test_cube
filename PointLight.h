#pragma once
#include "Math.h"
#include "Mesh.h"
#include "Material.h"

class Renderer;

struct PointLightDesc {
    Math::float3 position = Math::float3(0, 0, 0);
    float        radius   = 1.0f;
    Math::float3 color    = Math::float3(1, 1, 1);
    float        intensity= 1.0f;
};

class PointLight {
public:
    void Init(Renderer* r, ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive);

    void SetDesc(const PointLightDesc& d) { desc_ = d; }
    const PointLightDesc& GetDesc() const { return desc_; }

    // ДВА ОТДЕЛЬНЫЕ ФУНКЦИИ:
    // 1) Z-FAIL стенсил-препасс (два прохода: back затем front)
    void RenderZFail(Renderer* r, ID3D12GraphicsCommandList* cl,
                     const Math::mat4& view, const Math::mat4& proj);

    // 2) Цветовой проход (аддитив в Light RT) с маской stencil!=0
    void RenderColor(Renderer* r, ID3D12GraphicsCommandList* cl,
                     const Math::mat4& view, const Math::mat4& proj,
                     const Math::mat4& invView, const Math::mat4& invProj,
                     const Math::float3& camPos);

private:
    Math::mat4 BuildModel() const;

    std::shared_ptr<Mesh> sphere_ = nullptr;

    // два PSO для z-fail: отдельные на back/front
    std::shared_ptr<Material> matZFail_;

    // цветовой (аддитивный) полноэкранный
    std::shared_ptr<Material> matColorFS_;

    PointLightDesc desc_{};
};