#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <vector>
#include <mutex>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/Math.h"
#include "rendering/meshes/BoundingBox.h"
#include "rendering/meshes/Mesh.h"

class Renderer;
class Material;

class DebugDrawSystem {
public:
    enum class ShapeType { Sphere, Box, Cone };

    struct InstanceBuffer
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> resource;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpuHeap;
        D3D12_CPU_DESCRIPTOR_HANDLE srvCPU = {};
        void* mapped = nullptr;
        UINT capacity = 0;
    };

    struct GPUInstanceData
    {
        Math::mat4 mvp;
        Math::float4 color;
    };

    DebugDrawSystem() = default;

    void Initialize(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void Shutdown();

    void BeginFrame();

    void AddSphere(const Math::float3& center, float radius, const Math::float4& color, bool wireframe);
    void AddBox(const Math::float3& center, const Math::float3& halfExtents, const Math::quat& orientation,
        const Math::float4& color, bool wireframe);
    void AddBox(const Math::float3& center, const Math::float3& halfExtents, const Math::float3& orientationEuler,
        const Math::float4& color, bool wireframe);
    void AddBox(const Math::float3& center, const Math::float3& halfExtents,
        const Math::float4& color, bool wireframe);
    void AddBox(const BoundingBox& bounds, const Math::float4& color, bool wireframe);
    void AddBox(const Math::mat4& transform, const Math::float4& color, bool wireframe);
    void AddCone(const Math::float3& apex, const Math::float3& direction,
        float height, float radius, const Math::float4& color, bool wireframe);
    void AddCone(const Math::mat4& transform, const Math::float4& color, bool wireframe);

    bool HasCommands() const;

    void Render(Renderer* renderer, ID3D12GraphicsCommandList* cl,
        const Math::mat4& view, const Math::mat4& proj);

    bool IsInitialized() const { return initialized_; }

private:
    struct Command {
        ShapeType shape = ShapeType::Sphere;
        Math::mat4 transform;
        Math::float4 color;
    };

    void AddCommand(ShapeType shape, const Math::mat4& transform,
        const Math::float4& color, bool wireframe);

    Math::mat4 ComputeBoxTransform(const Math::float3& center, const Math::float3& halfExtents,
        const Math::quat& orientation) const;
    Math::mat4 ComputeBoxTransform(const Math::float3& center, const Math::float3& halfExtents,
        const Math::float3& orientationEuler) const;
    Math::mat4 ComputeConeTransform(const Math::float3& apex, const Math::float3& direction,
        float height, float radius) const;

    static Math::quat RotationBetween(const Math::float3& from, const Math::float3& to);

    Mesh* MeshForShape(ShapeType shape);
    const Mesh* MeshForShape(ShapeType shape) const;

    std::shared_ptr<Material> material_;
    Mesh sphereMesh_;
    Mesh boxMesh_;
    Mesh coneMesh_;

    bool initialized_ = false;

    std::vector<Command> solidCommands_;
    std::vector<Command> wireframeCommands_;
    std::vector<Command> solidCommandScratch_;
    std::vector<Command> wireframeCommandScratch_;
    std::vector<GPUInstanceData> instanceDataScratch_;
    mutable std::mutex commandMutex_;

    std::array<InstanceBuffer, static_cast<size_t>(ShapeType::Cone) + 1> instanceBuffers_{};
};

