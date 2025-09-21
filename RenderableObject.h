#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <memory>

#include "Material.h"
#include "MaterialData.h"
#include "Mesh.h"
#include "RenderContext.h"
#include "Math.h"
#include "RenderableObjectBase.h"

class Renderer;

class RenderableObject: public RenderableObjectBase {
public:
    RenderableObject(
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader);
    virtual ~RenderableObject();

    // Жизненный цикл
    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    virtual void Tick(float /*dt*/) {}

    // Базовый отрисовщик: Compute -> Graphics (Bind/IssueDraw)
    virtual void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& view, const mat4& proj);
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj);

    // Трансформ
    const Math::mat4& GetModelMatrix() const { return modelMatrix_; }
    void SetModelMatrix(const Math::mat4& m) { modelMatrix_ = m; }

    // Меш/материал
    Mesh* GetMesh() { return mesh_.get(); }
    const Mesh* GetMesh() const { return mesh_.get(); }

    Material* GetGraphicsMaterial() const { return graphicsMaterial_.get(); }
    void SetGraphicsMaterial(Material* m) { graphicsMaterial_.reset(m); RebuildHandleCaches(); } // если хочешь вручную

    // пер-объектные параметры (b0)
    MaterialParams& MaterialParamsRef() { return matParams_; }
    const MaterialParams& MaterialParamsRef() const { return matParams_; }

    // GraphicsDesc — правим пайплайн (топология/блендинг/растр/DS)
    Material::GraphicsDesc& GetGraphicsDesc() { return graphicsDesc_; }
    void SetGraphicsDesc(const Material::GraphicsDesc& gd) { graphicsDesc_ = gd; }

    virtual bool IsTransparent() const {
        return graphicsDesc_.blend.RenderTarget[0].BlendEnable;
    }

    virtual bool CastsShadow() const { return true; }

protected:
    virtual void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) {}
    virtual void UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj, uint8_t* cbData);
    virtual void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx);
    virtual void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx);
    virtual void IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl);
    virtual void RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx, uint8_t* cbData);

    // Утилита записи в CB (b0)
    template<typename T>
    bool UpdateGraphicsUniform(const Material::CBFieldHandle& handle, const T& value, uint8_t* cbData)
    {
        if (!cbData) { return false; }
        if (handle.isValid && graphicsMaterial_) {
            return UpdateUniform(handle, graphicsMaterial_.get(), value, cbData);
        }
        return false;
    }

    template<typename T>
    bool UpdateShadowUniform(const Material::CBFieldHandle& handle, const T& value, uint8_t* cbData)
    {
        return UpdateUniform(handle, shadowMaterial_.get(), value, cbData);
    }

    void ApplyMaterialParamsToCB(uint8_t* cbData);

protected:
    // Данные рендера
    std::shared_ptr<MaterialData> matData_;          // ассет: текстуры+фичи (shared)
    MaterialParams                matParams_;        // пер-объект в b0
    std::shared_ptr<Material>     graphicsMaterial_; // вариант шейдера (PSO/RS)
    Material::GraphicsDesc        graphicsDesc_;
    std::string                   matPreset_;
    std::shared_ptr<Material>     shadowMaterial_;
    Material::GraphicsDesc        shadowDesc_;

    std::shared_ptr<Mesh> mesh_;
    Math::mat4 modelMatrix_;

    // CB (upload, пер-объектный)
    bool allowWireframe_ = true;

private:
    static std::wstring AppendSuffixBeforeExt(const std::wstring& file, const std::wstring& suffix);

    void RebuildHandleCaches();

    struct CBHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle view;
        Material::CBFieldHandle proj;
        Material::CBFieldHandle baseColor;
        Material::CBFieldHandle metalRough;
        Material::CBFieldHandle texOffsScale;
        Material::CBFieldHandle texFlags;
    } cb0Handles_{};

    struct ShadowCBHandles
    {
        Material::CBFieldHandle world;
        Material::CBFieldHandle view;
        Material::CBFieldHandle proj;
    } shadowHandles_{};

    template<typename T>
    bool UpdateUniform(const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData)
    {
        if (!cbData) { return false; }
        if (!material) { return false; }
        if (!handle.isValid) { return false; }
        return material->UpdateCBField(handle, value, cbData);
    }

    RenderableObject(const RenderableObject&) = delete;
    RenderableObject& operator=(const RenderableObject&) = delete;
};