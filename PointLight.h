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

    // TWO SEPARATE FUNCTIONS:
    // 1) Z-FAIL stencil pre-pass (two passes: back then front)
    void RenderZFail(Renderer* r, ID3D12GraphicsCommandList* cl,
                     const Math::mat4& view, const Math::mat4& proj);

    // 2) Color pass (additive into the Light RT) with a stencil!=0 mask
    void RenderColor(Renderer* r, ID3D12GraphicsCommandList* cl,
                     const Math::mat4& view, const Math::mat4& proj,
                     const Math::mat4& invView, const Math::mat4& invProj,
                     const Math::float3& camPos);

    void OnMaterialHotReload();

private:
    Math::mat4 BuildModel() const;

    std::shared_ptr<Mesh> sphere_ = nullptr;

    // two PSOs for z-fail: one for back faces and one for front faces
    std::shared_ptr<Material> matZFail_;

    // color (additive) fullscreen pass
    std::shared_ptr<Material> matColorFS_;

    struct CBHandleCache {
        struct ZFailHandles {
            Material::CBFieldHandle world;
            Material::CBFieldHandle view;
            Material::CBFieldHandle proj;
        } zFail;

        struct ColorHandles {
            struct PerFrame {
                Material::CBFieldHandle view;
                Material::CBFieldHandle proj;
                Material::CBFieldHandle invView;
                Material::CBFieldHandle invProj;
                Material::CBFieldHandle camPos;
                Material::CBFieldHandle screenSize;
            } frame;

            struct PerLight {
                Material::CBFieldHandle position;
                Material::CBFieldHandle radius;
                Material::CBFieldHandle color;
                Material::CBFieldHandle intensity;
            } light;
        } color;
    } cbHandles_{};

    void RebuildHandleCache();

    PointLightDesc desc_{};
};