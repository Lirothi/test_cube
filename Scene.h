#pragma once

#include <memory>
#include <functional>

#include "RenderableObject.h"
#include "Camera.h"
#include "InputManager.h"
#include "Skybox.h"

class Renderer;

class Scene {
public:
    void SetInput(InputManager* input) { input_ = input; }
    void SetActions(ActionMap* a) { actions_ = a; }
    Camera& CameraRef() { return camera_; }
    const Camera& CameraRef() const { return camera_; }

    void InitAll(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void AddObject(std::unique_ptr<RenderableObjectBase> obj);
    void Tick(float deltaTime);
    void Render(Renderer* renderer);

    void Clear();

private:
    void RenderObjectBatch(Renderer* renderer, const std::vector<RenderableObjectBase*>& objects, size_t batchIndex,
        const mat4& view, const mat4& proj, bool useCommandBundle, bool bindGbufOrScene);
    
    std::shared_ptr<Material> matLighting_;
    std::shared_ptr<Material> matCompose_;
    std::shared_ptr<Material> matTonemap_;
    std::shared_ptr<Material> matSSR_;
    std::shared_ptr<Material> matBlur_;
    std::shared_ptr<Material> matShadowCSM_;   // depth-only
    std::shared_ptr<Material> matDebug_;
    static constexpr int kCascades = 4;

    // кэш для лайт-пасса
    mat4  cachedLightView_[kCascades];
    mat4  cachedLightProj_[kCascades];
    float2 cachedScale_[kCascades];  // atlas scale
    float2 cachedBias_[kCascades];   // atlas bias
    float  cachedSplitsVS_[kCascades + 1] = {}; // near..far в view-space
    float  cachedNormalBiasWS_[kCascades] = {};
    float  cachedDepthBiasNDC_[kCascades] = {};

    std::vector<std::unique_ptr<RenderableObjectBase>> objects_;
    InputManager* input_ = nullptr;
    ActionMap* actions_ = nullptr;
    Camera camera_;

    bool debugTexMode_ = false;

    std::unique_ptr<Skybox> skyBox_;
};
