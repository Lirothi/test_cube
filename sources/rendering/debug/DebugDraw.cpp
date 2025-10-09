#include "rendering/debug/DebugDraw.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "materials/Material.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/FrameResource.h"
#include "rendering/core/RenderContextPool.h"
#include "materials/MaterialManager.h"

namespace {
struct DebugVertex
{
    Math::float3 position;
    Math::float4 color;
};

struct DebugDrawCB
{
    Math::mat4 modelViewProj;
    Math::float4 color;
};

static void BuildSphereMesh(std::vector<DebugVertex>& outVerts, std::vector<uint16_t>& outIndices)
{
    constexpr int kStacks = 16;
    constexpr int kSlices = 24;

    outVerts.clear();
    outIndices.clear();
    outVerts.reserve((kStacks + 1) * (kSlices + 1));
    outIndices.reserve(kStacks * kSlices * 6);

    for (int stack = 0; stack <= kStacks; ++stack)
    {
        const float v = static_cast<float>(stack) / static_cast<float>(kStacks);
        const float phi = v * Math::PI;
        const float cosPhi = std::cos(phi);
        const float sinPhi = std::sin(phi);

        for (int slice = 0; slice <= kSlices; ++slice)
        {
            const float u = static_cast<float>(slice) / static_cast<float>(kSlices);
            const float theta = u * Math::TWO_PI;
            const float cosTheta = std::cos(theta);
            const float sinTheta = std::sin(theta);

            DebugVertex vert{};
            vert.position = Math::float3(
                0.5f * sinPhi * cosTheta,
                0.5f * cosPhi,
                0.5f * sinPhi * sinTheta);
            vert.color = Math::float4(1.f, 1.f, 1.f, 1.f);
            outVerts.push_back(vert);
        }
    }

    const int stride = kSlices + 1;
    for (int stack = 0; stack < kStacks; ++stack)
    {
        for (int slice = 0; slice < kSlices; ++slice)
        {
            const uint16_t a = static_cast<uint16_t>(stack * stride + slice);
            const uint16_t b = static_cast<uint16_t>((stack + 1) * stride + slice);
            const uint16_t c = static_cast<uint16_t>(stack * stride + slice + 1);
            const uint16_t d = static_cast<uint16_t>((stack + 1) * stride + slice + 1);

            outIndices.push_back(a);
            outIndices.push_back(b);
            outIndices.push_back(c);

            outIndices.push_back(c);
            outIndices.push_back(b);
            outIndices.push_back(d);
        }
    }
}

static void BuildBoxMesh(std::vector<DebugVertex>& outVerts, std::vector<uint16_t>& outIndices)
{
    outVerts.clear();
    outIndices.clear();

    static const Math::float3 kPositions[] = {
        {-0.5f, -0.5f, -0.5f},
        { 0.5f, -0.5f, -0.5f},
        { 0.5f,  0.5f, -0.5f},
        {-0.5f,  0.5f, -0.5f},
        {-0.5f, -0.5f,  0.5f},
        { 0.5f, -0.5f,  0.5f},
        { 0.5f,  0.5f,  0.5f},
        {-0.5f,  0.5f,  0.5f},
    };

    static const uint16_t kIndices[] = {
        0, 1, 2, 0, 2, 3, // back
        4, 6, 5, 4, 7, 6, // front
        4, 5, 1, 4, 1, 0, // bottom
        3, 2, 6, 3, 6, 7, // top
        4, 0, 3, 4, 3, 7, // left
        1, 5, 6, 1, 6, 2  // right
    };

    outVerts.resize(8);
    for (size_t i = 0; i < 8; ++i)
    {
        outVerts[i].position = kPositions[i];
        outVerts[i].color = Math::float4(1.f, 1.f, 1.f, 1.f);
    }

    outIndices.assign(std::begin(kIndices), std::end(kIndices));
}

static void BuildConeMesh(std::vector<DebugVertex>& outVerts, std::vector<uint16_t>& outIndices)
{
    constexpr int kSegments = 24;

    outVerts.clear();
    outIndices.clear();

    outVerts.reserve(kSegments + 2);
    outIndices.reserve(kSegments * 6);

    // Apex
    DebugVertex apex{};
    apex.position = Math::float3(0.f, 1.f, 0.f);
    apex.color = Math::float4(1.f, 1.f, 1.f, 1.f);
    outVerts.push_back(apex);

    // Base ring
    for (int i = 0; i < kSegments; ++i)
    {
        const float angle = (static_cast<float>(i) / static_cast<float>(kSegments)) * Math::TWO_PI;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        DebugVertex v{};
        v.position = Math::float3(0.5f * c, 0.f, 0.5f * s);
        v.color = Math::float4(1.f, 1.f, 1.f, 1.f);
        outVerts.push_back(v);
    }

    // Base center
    DebugVertex baseCenter{};
    baseCenter.position = Math::float3(0.f, 0.f, 0.f);
    baseCenter.color = Math::float4(1.f, 1.f, 1.f, 1.f);
    outVerts.push_back(baseCenter);

    const uint16_t apexIndex = 0;
    const uint16_t baseStart = 1;
    const uint16_t baseCenterIndex = static_cast<uint16_t>(outVerts.size() - 1);

    for (int i = 0; i < kSegments; ++i)
    {
        const uint16_t next = static_cast<uint16_t>(baseStart + ((i + 1) % kSegments));
        const uint16_t curr = static_cast<uint16_t>(baseStart + i);

        // Side
        outIndices.push_back(apexIndex);
        outIndices.push_back(curr);
        outIndices.push_back(next);

        // Base (ensure consistent winding)
        outIndices.push_back(baseCenterIndex);
        outIndices.push_back(next);
        outIndices.push_back(curr);
    }
}

} // namespace

void DebugDrawSystem::Initialize(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    if (initialized_)
    {
        return;
    }
    if (!renderer || !uploadCmdList)
    {
        return;
    }

    Material::GraphicsDesc gd{};
    gd.shaderFile = L"shaders/debug_draw.hlsl";
    gd.vsEntry = "VSMain";
    gd.psEntry = "PSMain";
    gd.inputLayoutKey = "PosColor";
    gd.numRT = 1;
    gd.rtvFormats[0] = renderer->GetSceneColorFormat();
    gd.dsvFormat = Renderer::kDeferredDepthFormat;
    gd.topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.raster.CullMode = D3D12_CULL_MODE_NONE;
    gd.raster.FillMode = D3D12_FILL_MODE_SOLID;
    gd.depth.DepthEnable = TRUE;
    gd.depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    gd.depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    // Enable standard alpha blending
    D3D12_RENDER_TARGET_BLEND_DESC blend{};
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.blend.RenderTarget[0] = blend;

    material_ = renderer->GetMaterialManager()->GetOrCreateGraphics(renderer, gd);
    if (!material_)
    {
        return;
    }

    std::vector<DebugVertex> verts;
    std::vector<uint16_t> indices;

    BuildSphereMesh(verts, indices);
    sphereMesh_.CreateGPUFlexible(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts.data(), static_cast<UINT>(verts.size()), sizeof(DebugVertex),
        indices.data(), static_cast<UINT>(indices.size()), DXGI_FORMAT_R16_UINT);

    BuildBoxMesh(verts, indices);
    boxMesh_.CreateGPUFlexible(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts.data(), static_cast<UINT>(verts.size()), sizeof(DebugVertex),
        indices.data(), static_cast<UINT>(indices.size()), DXGI_FORMAT_R16_UINT);

    BuildConeMesh(verts, indices);
    coneMesh_.CreateGPUFlexible(renderer->GetDevice(), uploadCmdList, uploadKeepAlive,
        verts.data(), static_cast<UINT>(verts.size()), sizeof(DebugVertex),
        indices.data(), static_cast<UINT>(indices.size()), DXGI_FORMAT_R16_UINT);

    initialized_ = true;
}

void DebugDrawSystem::Shutdown()
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    solidCommands_.clear();
    wireframeCommands_.clear();
    material_.reset();
    sphereMesh_ = Mesh();
    boxMesh_ = Mesh();
    coneMesh_ = Mesh();
    initialized_ = false;
}

void DebugDrawSystem::BeginFrame()
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    solidCommands_.clear();
    wireframeCommands_.clear();
}

void DebugDrawSystem::AddSphere(const Math::float3& center, float radius, const Math::float4& color, bool wireframe)
{
    if (!initialized_ || radius <= 0.0f)
    {
        return;
    }

    Math::mat4 transform = Math::mat4::Scaling(radius * 2.0f, radius * 2.0f, radius * 2.0f)
        * Math::mat4::Translation(center);
    AddCommand(ShapeType::Sphere, transform, color, wireframe);
}

void DebugDrawSystem::AddBox(const Math::float3& center, const Math::float3& halfExtents,
    const Math::quat& orientation, const Math::float4& color, bool wireframe)
{
    if (!initialized_)
    {
        return;
    }
    if (halfExtents.x <= 0.0f || halfExtents.y <= 0.0f || halfExtents.z <= 0.0f)
    {
        return;
    }

    Math::mat4 transform = ComputeBoxTransform(center, halfExtents, orientation);
    AddCommand(ShapeType::Box, transform, color, wireframe);
}

void DebugDrawSystem::AddBox(const Math::float3& center, const Math::float3& halfExtents,
    const Math::float4& color, bool wireframe)
{
    AddBox(center, halfExtents, Math::quat::Identity(), color, wireframe);
}

void DebugDrawSystem::AddBox(const BoundingBox& bounds, const Math::float4& color, bool wireframe)
{
    if (!initialized_ || !bounds.IsValid())
    {
        return;
    }

    AddBox(bounds.GetCenter(), bounds.GetHalfExtents(), color, wireframe);
}

void DebugDrawSystem::AddBox(const Math::mat4& transform, const Math::float4& color, bool wireframe)
{
    if (!initialized_)
    {
        return;
    }
    AddCommand(ShapeType::Box, transform, color, wireframe);
}

void DebugDrawSystem::AddCone(const Math::float3& apex, const Math::float3& direction,
    float height, float radius, const Math::float4& color, bool wireframe)
{
    if (!initialized_)
    {
        return;
    }
    if (height <= 0.0f || radius <= 0.0f)
    {
        return;
    }

    Math::mat4 transform = ComputeConeTransform(apex, direction, height, radius);
    AddCommand(ShapeType::Cone, transform, color, wireframe);
}

void DebugDrawSystem::AddCone(const Math::mat4& transform, const Math::float4& color, bool wireframe)
{
    if (!initialized_)
    {
        return;
    }
    AddCommand(ShapeType::Cone, transform, color, wireframe);
}

bool DebugDrawSystem::HasCommands() const
{
    std::lock_guard<std::mutex> lock(commandMutex_);
    return !solidCommands_.empty() || !wireframeCommands_.empty();
}

void DebugDrawSystem::Render(Renderer* renderer, ID3D12GraphicsCommandList* cl,
    const Math::mat4& view, const Math::mat4& proj)
{
    if (!renderer || !cl || !material_ || !initialized_)
    {
        return;
    }
    const UINT cbSize = material_->GetCBSizeBytesAligned(0, 256);
    auto ctxHandle = renderer->GetRenderContextPool()->Acquire();
    auto& ctx = ctxHandle.ref();

    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        if (solidCommands_.empty() && wireframeCommands_.empty())
        {
            return;
        }
        solidCommandScratch_ = solidCommands_;
        wireframeCommandScratch_ = wireframeCommands_;
    }

    auto drawList = [&](const std::vector<Command>& commands, bool wireframe)
    {
        for (const Command& cmd : commands)
        {
            const Mesh* mesh = MeshForShape(cmd.shape);
            if (!mesh)
            {
                continue;
            }

            auto alloc = renderer->GetFrameResource()->AllocDynamic(cbSize, 256);
            std::memset(alloc.cpu, 0, cbSize);

            DebugDrawCB cb{};
            cb.modelViewProj = cmd.transform * view * proj;
            cb.color = cmd.color;
            std::memcpy(alloc.cpu, &cb, sizeof(cb));

            ctx.ClearFast();
            ctx.cbv[0] = alloc.gpu;

            material_->Bind(cl, ctx, wireframe);
            mesh->Draw(cl);
        }
    };

    drawList(solidCommandScratch_, false);
    drawList(wireframeCommandScratch_, true);
}

void DebugDrawSystem::AddCommand(ShapeType shape, const Math::mat4& transform,
    const Math::float4& color, bool wireframe)
{
    Command cmd{};
    cmd.shape = shape;
    cmd.transform = transform;
    cmd.color = color;
    std::lock_guard<std::mutex> lock(commandMutex_);
    if (wireframe)
    {
        wireframeCommands_.push_back(cmd);
    }
    else
    {
        solidCommands_.push_back(cmd);
    }
}

Math::mat4 DebugDrawSystem::ComputeBoxTransform(const Math::float3& center, const Math::float3& halfExtents,
    const Math::quat& orientation) const
{
    Math::mat4 scale = Math::mat4::Scaling(halfExtents.x * 2.0f, halfExtents.y * 2.0f, halfExtents.z * 2.0f);
    Math::mat4 rotation = Math::mat4::FromQuaternion(orientation);
    Math::mat4 translation = Math::mat4::Translation(center);
    return scale * rotation * translation;
}

Math::mat4 DebugDrawSystem::ComputeConeTransform(const Math::float3& apex, const Math::float3& direction,
    float height, float radius) const
{
    Math::float3 dir = direction;
    const float len = dir.Length();
    if (len < Math::EPS)
    {
        dir = Math::float3(0.f, 1.f, 0.f);
    }
    else
    {
        dir = dir * (1.0f / len);
    }

    Math::quat q = RotationBetween(Math::float3(0.f, 1.f, 0.f), dir);
    Math::mat4 scale = Math::mat4::Scaling(radius * 2.0f, height, radius * 2.0f);
    Math::mat4 rotation = Math::mat4::FromQuaternion(q);
    Math::mat4 sr = scale * rotation;

    Math::float3 apexLocal = Math::float3(0.f, 1.f, 0.f);
    Math::float3 apexOffset = sr.TransformPoint(apexLocal);
    Math::float3 translationVec = apex - apexOffset;
    Math::mat4 translation = Math::mat4::Translation(translationVec);
    return sr * translation;
}

Math::quat DebugDrawSystem::RotationBetween(const Math::float3& from, const Math::float3& to)
{
    Math::float3 f = from;
    Math::float3 t = to;
    const float lenF = f.Length();
    const float lenT = t.Length();
    if (lenF < Math::EPS || lenT < Math::EPS)
    {
        return Math::quat::Identity();
    }
    f = f * (1.0f / lenF);
    t = t * (1.0f / lenT);

    float cosTheta = Math::Clamp(f.Dot(t), -1.0f, 1.0f);
    Math::float3 axis = f.Cross(t);
    if (axis.Length() < Math::EPS)
    {
        if (cosTheta > 0.0f)
        {
            return Math::quat::Identity();
        }
        Math::float3 ortho = (std::fabs(f.x) > 0.9f) ? Math::float3(0.f, 1.f, 0.f) : Math::float3(1.f, 0.f, 0.f);
        axis = f.Cross(ortho);
    }
    axis = axis.Normalized();
    float angle = std::acos(Math::Clamp(cosTheta, -1.0f, 1.0f));
    return Math::quat::FromAxisAngle(axis, angle).Normalized();
}

Mesh* DebugDrawSystem::MeshForShape(ShapeType shape)
{
    switch (shape)
    {
    case ShapeType::Sphere: return &sphereMesh_;
    case ShapeType::Box:    return &boxMesh_;
    case ShapeType::Cone:   return &coneMesh_;
    }
    return nullptr;
}

const Mesh* DebugDrawSystem::MeshForShape(ShapeType shape) const
{
    switch (shape)
    {
    case ShapeType::Sphere: return &sphereMesh_;
    case ShapeType::Box:    return &boxMesh_;
    case ShapeType::Cone:   return &coneMesh_;
    }
    return nullptr;
}

