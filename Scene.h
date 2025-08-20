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
    void RenderObjectBatch(Renderer* renderer, const std::vector<SceneObject*>& objects, size_t batchIndex, mat4 view, mat4 proj, bool useCommandBundle);

    std::vector<std::unique_ptr<SceneObject>> objects_;
    InputManager* input_ = nullptr;
    ActionMap* actions_ = nullptr;
    Camera camera_;
};
