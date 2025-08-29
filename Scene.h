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

    std::vector<std::unique_ptr<RenderableObjectBase>> objects_;
    InputManager* input_ = nullptr;
    ActionMap* actions_ = nullptr;
    Camera camera_;

    std::unique_ptr<Skybox> skyBox_;
};
