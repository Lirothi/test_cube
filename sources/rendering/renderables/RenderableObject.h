#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <string>
#include <memory>

#include "materials/Material.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/core/RenderContext.h"
#include "core/Math.h"
#include "rendering/renderables/RenderableObjectBase.h"

class Renderer;

class RenderableObject: public RenderableObjectBase {
public:
    class UniformBinder
    {
    public:
        virtual ~UniformBinder() = default;

        virtual void RebuildHandles(RenderableObject& /*owner*/) {}
        virtual void UpdateMainCB(RenderableObject& /*owner*/, Renderer* /*renderer*/, const mat4& /*view*/, const mat4& /*proj*/, uint8_t* /*cbData*/) {}
        virtual void UpdateShadowCB(RenderableObject& /*owner*/, Renderer* /*renderer*/, const mat4& /*lightView*/, const mat4& /*lightProj*/, uint8_t* /*cbData*/) {}

    protected:
        template<typename T>
        bool UpdateUniform(RenderableObject& owner, const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData) const
        {
            return owner.UpdateUniform(handle, material, value, cbData);
        }
    };

    RenderableObject(
        const std::string& inputLayout,
        const std::wstring& graphicsShader);
    virtual ~RenderableObject();

    // Lifecycle
    virtual void Init(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    virtual void Tick(float /*dt*/) {}
    virtual void PostTick(float /*dt*/) override {}

    // Base renderer: Compute -> Graphics (Bind/IssueDraw)
    virtual void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& view, const mat4& proj);
    virtual void RenderShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj);
    virtual void OnMaterialHotReload(Renderer* renderer);

    // Transform
    const Math::mat4& GetModelMatrix() const { return modelMatrix_; }
    void SetModelMatrix(const Math::mat4& m) { modelMatrix_ = m; }

    // Mesh/material
    Mesh* GetMesh() { return mesh_.get(); }
    const Mesh* GetMesh() const { return mesh_.get(); }

    Material* GetGraphicsMaterial() const { return graphicsMaterial_.get(); }
    void SetGraphicsMaterial(Material* m);
    Material* GetShadowMaterial() const { return shadowMaterial_.get(); }

    // GraphicsDesc—adjust the pipeline (topology/blending/raster/depth-stencil)
    Material::GraphicsDesc& GetGraphicsDesc() { return graphicsDesc_; }
    void SetGraphicsDesc(const Material::GraphicsDesc& gd) { graphicsDesc_ = gd; }

    virtual bool IsTransparent() const {
        return graphicsDesc_.blend.RenderTarget[0].BlendEnable;
    }

    virtual bool CastsShadow() const { return true; }

protected:
    virtual void RecordCompute(Renderer* renderer, ID3D12GraphicsCommandList* cl) {}
    virtual void PopulateContext(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx);
    virtual void RecordGraphics(Renderer* renderer, ID3D12GraphicsCommandList* cl, RenderContext& ctx);
    virtual void IssueDraw(Renderer* renderer, ID3D12GraphicsCommandList* cl);
    virtual void RecordShadow(Renderer* renderer, ID3D12GraphicsCommandList* cl, const mat4& lightView, const mat4& lightProj, RenderContext& ctx);

    template<typename T>
    bool UpdateUniform(const Material::CBFieldHandle& handle, Material* material, const T& value, uint8_t* cbData)
    {
        if (!cbData) { return false; }
        if (!material) { return false; }
        if (!handle.field) { return false; }
        return material->UpdateCBField(handle, value, cbData);
    }

protected:
    std::shared_ptr<Material>     graphicsMaterial_; // shader variant (PSO/RS)
    Material::GraphicsDesc        graphicsDesc_;
    std::shared_ptr<Material>     shadowMaterial_;
    Material::GraphicsDesc        shadowDesc_;

    std::shared_ptr<Mesh> mesh_;
    Math::mat4 modelMatrix_;

    // CB (upload, per-object)
    bool allowWireframe_ = true;

    void SetUniformBinder(std::unique_ptr<UniformBinder> binder);
    UniformBinder* GetUniformBinder() const { return uniformBinder_.get(); }

private:
    static std::wstring AppendSuffixBeforeExt(const std::wstring& file, const std::wstring& suffix);

    RenderableObject(const RenderableObject&) = delete;
    RenderableObject& operator=(const RenderableObject&) = delete;

    friend class UniformBinder;

    std::unique_ptr<UniformBinder> uniformBinder_;
};