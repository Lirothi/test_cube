#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <memory>

#include "CBManager.h"
#include "Material.h"
#include "Mesh.h"
#include "RenderContext.h"
#include "Math.h"

class Renderer;

class SceneObject {
public:
    // База создаёт CB по указанному layout (в б0 кладём "modelViewProj")
    // inputLayout/shaderFile — только для первичного графического материала (его можно заменить/править через GraphicsDesc)
    SceneObject(Renderer* renderer,
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
    virtual void RenderGBuffer(Renderer* renderer, ID3D12GraphicsCommandList* cl, const Math::mat4& view, const Math::mat4& proj);

    // Трансформ
    const Math::mat4& GetModelMatrix() const { return modelMatrix_; }
    void SetModelMatrix(const Math::mat4& m) { modelMatrix_ = m; }

    // Меш/материал
    Mesh* GetMesh() { return mesh_.get(); }
    const Mesh* GetMesh() const { return mesh_.get(); }

    Material* GetGraphicsMaterial() const { return graphicsMaterial_.get(); }
    void SetGraphicsMaterial(Material* m) { graphicsMaterial_.reset(m); } // если хочешь вручную

    // GraphicsDesc — правим пайплайн (топология/блендинг/растр/DS)
    Material::GraphicsDesc& GetGraphicsDesc() { return graphicsDesc_; }
    void SetGraphicsDesc(const Material::GraphicsDesc& gd) { graphicsDesc_ = gd; }

    // CB access
    ID3D12Resource* GetConstantBuffer() const { return constantBuffer_.Get(); }
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
        if (!cbLayout_ || !cbvDataBegin_) { return false; }
        return cbLayout_->SetField<T>(name, value, cbvDataBegin_);
    }

protected:
    // Данные рендера
    RenderContext graphicsCtx_;
    std::shared_ptr<Material> graphicsMaterial_;
    Material::GraphicsDesc graphicsDesc_;
    std::shared_ptr<Material> gbufferMaterial_;
    Material::GraphicsDesc gbufferDesc_;

    std::shared_ptr<Mesh> mesh_;
    Math::mat4 modelMatrix_;

    // CB (upload, пер-объектный)
    const ConstantBufferLayout* cbLayout_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> constantBuffer_;
    uint8_t* cbvDataBegin_ = nullptr;

    // Первичные настройки (из конструктора)
    std::wstring shaderFile_;
    std::string inputLayoutKey_;

private:
    SceneObject(const SceneObject&) = delete;
    SceneObject& operator=(const SceneObject&) = delete;
};