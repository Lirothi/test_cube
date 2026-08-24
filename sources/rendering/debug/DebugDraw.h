#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <vector>
#include <mutex>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "core/math/AABB.h"
#include "rendering/core/RenderConstants.h" // render::kFrameCount (per-slot instance buffers)
#include "core/math/OBB.h"
#include "core/math/Frustum.h"
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
    void AddBox(const AABB& bounds, const Math::float4& color, bool wireframe);
    void AddBox(const OBB& bounds, const Math::float4& color, bool wireframe);
    void AddBox(const Math::mat4& transform, const Math::float4& color, bool wireframe);
    void AddCone(const Math::float3& apex, const Math::float3& direction,
        float height, float radius, const Math::float4& color, bool wireframe);
    void AddCone(const Math::mat4& transform, const Math::float4& color, bool wireframe);
    void AddLine(const Math::float3& a, const Math::float3& b, const Math::float4& color);
    void AddFrustum(const Frustum& frustum, const Math::float4& color);

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
    struct LineCommand
    {
        Math::float3 a;
        Math::float3 b;
        Math::float4 color;
    };
    std::vector<LineCommand> lineCommands_;
    std::vector<LineCommand> lineCommandScratch_;
    struct LineVertex
    {
        Math::float3 position;
        Math::float4 color;
    };
    std::vector<LineVertex> lineVertexScratch_;
    mutable std::mutex commandMutex_;

    static constexpr size_t kShapeCount = static_cast<size_t>(ShapeType::Cone) + 1;
    // Indexed by FRAME SLOT first, not one global pair. These are UPLOAD buffers the CPU writes and
    // the GPU reads, so a single copy has two defects: frame N+1's memcpy lands in memory frame N is
    // still reading, and -- far worse -- growing it calls Reset() on a resource an in-flight command
    // list still holds a descriptor for, which is a GPU page fault, i.e. device removed. Both stayed
    // dormant while the only caller drew a couple of gizmo spheres and never passed the initial
    // 64-element capacity; the LOD debug view draws hundreds and made the growth path run for the
    // first time. Per slot, growth happens only when this slot's fence has already been waited on.
    using ShapeBuffers = std::array<InstanceBuffer, kShapeCount>;
    std::array<ShapeBuffers, render::kFrameCount> solidInstanceBuffers_{};
    std::array<ShapeBuffers, render::kFrameCount> wireframeInstanceBuffers_{};
    std::shared_ptr<Material> lineMaterial_;
};

