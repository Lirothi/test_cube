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

class Renderer;

class SceneObject {
public:
    // База создаёт CB по указанному layout (в б0 кладём "modelViewProj")
    // inputLayout/shaderFile — только для первичного графического материала (его можно заменить/править через GraphicsDesc)
    SceneObject(Renderer* renderer,
        const std::string& matPreset,
        const std::string& cbLayout,
        const std::string& inputLayout,
        const std::wstring& graphicsShader);
    virtual ~SceneObject();

    // Жизненный цикл
    virtual void Init(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
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

    bool IsTransparent() const {
        return graphicsDesc_.blend.RenderTarget[0].BlendEnable;
	}

	virtual bool IsSimpleRender() const = 0; // true, если не нужно делать Compute-проход

protected:
    // Хуки для наследников:
    // 1) Compute-проход перед графикой (по умолчанию — пусто)
    virtual void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) {}

	virtual void UpdateUniforms(Renderer* renderer, const mat4& view, const mat4& proj) {}

    // 2) Наполнить контекст перед Bind (cbv/table/samplerTable/constants и т.п.)
    virtual void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl) {}

    virtual void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl);

    // 3) Собственно отрисовка (по умолчанию — Draw меша)
    virtual void IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl);

    // Утилита записи в CB по имени из layout (b0)
    template<typename T> bool UpdateUniform(const std::string& name, const T& value) {
        if (!cbvDataBegin_) { return false; }
        if (cbLayout_)
        {
	        return cbLayout_->SetField<T>(name, value, cbvDataBegin_);
        }
        else
        {
            UINT off = 0, sz = 0, bytes = sizeof(T);
            if (graphicsMaterial_->GetCBFieldOffset(0, name, off, sz)) {
                std::memcpy(cbvDataBegin_ + off, &value, (bytes < sz ? bytes : sz));
                return true;
            }
        }

        return false;
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

private:
    SceneObject(const SceneObject&) = delete;
    SceneObject& operator=(const SceneObject&) = delete;
};