#pragma once

#include <memory>

#include "SceneObject.h"
#include "Camera.h"
#include "InputManager.h"

class Renderer;

class Scene {
public:
    void SetInput(InputManager* input) { input_ = input; }
    void SetActions(ActionMap* a) { actions_ = a; }
    Camera& CameraRef() { return camera_; }
    const Camera& CameraRef() const { return camera_; }

    void InitAll(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void AddObject(std::unique_ptr<SceneObject> obj);
    void Update(float deltaTime);
    void Render(Renderer* renderer);

private:
    void RenderObjectBatchGBuffer(Renderer* renderer, const std::vector<SceneObject*>& objects, size_t batchIndex, const Math::mat4& view, const Math::mat4& proj, bool useBundles);
    void RenderObjectBatch(Renderer* renderer, const std::vector<SceneObject*>& objects, size_t batchIndex, const mat4& view, const mat4& proj, bool useCommandBundle);
    
    std::shared_ptr<Material> matLighting_;
    std::shared_ptr<Material> matCompose_;
    std::shared_ptr<Material> matTonemap_;

    std::vector<std::unique_ptr<SceneObject>> objects_;
    InputManager* input_ = nullptr;
    ActionMap* actions_ = nullptr;
    Camera camera_;
};
