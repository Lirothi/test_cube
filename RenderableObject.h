#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <memory>

#include "CBManager.h"
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

    // Трансформ
    const Math::mat4& GetModelMatrix() const { return modelMatrix_; }
    void SetModelMatrix(const Math::mat4& m) { modelMatrix_ = m; }

    // Меш/материал
    Mesh* GetMesh() { return mesh_.get(); }
    const Mesh* GetMesh() const { return mesh_.get(); }

    Material* GetGraphicsMaterial() const { return graphicsMaterial_.get(); }
    void SetGraphicsMaterial(Material* m) { graphicsMaterial_.reset(m); } // если хочешь вручную

    // пер-объектные параметры (b0)
    MaterialParams& MaterialParamsRef() { return matParams_; }
    const MaterialParams& MaterialParamsRef() const { return matParams_; }

    // GraphicsDesc — правим пайплайн (топология/блендинг/растр/DS)
    Material::GraphicsDesc& GetGraphicsDesc() { return graphicsDesc_; }
    void SetGraphicsDesc(const Material::GraphicsDesc& gd) { graphicsDesc_ = gd; }

    const ConstantBufferLayout* GetCBLayout() const { return cbLayout_; }

    virtual bool IsTransparent() const {
        return graphicsDesc_.blend.RenderTarget[0].BlendEnable;
	}

protected:
    virtual void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) {}
	virtual void UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj) {}
    virtual void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl) {}
    virtual void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl);
    virtual void IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl);

    // Утилита записи в CB по имени из layout (b0)
    template<typename T> bool UpdateUniform(const std::string& name, const T& value) {
        if (!cbvDataBegin_) { return false; }
        if (cbLayout_)
        {
	        return cbLayout_->SetField<T>(name, value, cbvDataBegin_);
        }
        return graphicsMaterial_->UpdateCB0Field(name, value, cbvDataBegin_);
    }

    void ApplyMaterialParamsToCB();

protected:
    // Данные рендера
    std::shared_ptr<MaterialData> matData_;          // ассет: текстуры+фичи (shared)
    MaterialParams                matParams_;        // пер-объект в b0
    std::shared_ptr<Material>     graphicsMaterial_; // вариант шейдера (PSO/RS)
    Material::GraphicsDesc        graphicsDesc_;
    RenderContext                 graphicsCtx_;
    std::string                   matPreset_;

    std::shared_ptr<Mesh> mesh_;
    Math::mat4 modelMatrix_;

    // CB (upload, пер-объектный)
    const ConstantBufferLayout* cbLayout_ = nullptr;
    uint8_t* cbvDataBegin_ = nullptr;

    bool allowWireframe_ = true;

private:
    RenderableObject(const RenderableObject&) = delete;
    RenderableObject& operator=(const RenderableObject&) = delete;
};