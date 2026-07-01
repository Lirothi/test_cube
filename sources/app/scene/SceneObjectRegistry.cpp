#include "app/scene/SceneObjectRegistry.h"

#include <cassert>
#include <memory>
#include <string>
#include <utility>

#include <DirectXMath.h>

#include "app/Systems.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "core/math/Math.h"
#include "ocean/OceanRenderable.h"
#include "ocean/OceanSimulation.h"
#include "rendering/debug/DebugGrid.h"
#include "rendering/meshes/GpuInstancedModels.h"
#include "rendering/meshes/StaticMesh.h"

using namespace Math;
using nlohmann::json;

namespace
{
class RotatingObject : public StaticMesh
{
public:
    RotatingObject(const std::string& modelName,
        const std::string& matPreset,
        const std::string& inputLayout,
        const std::wstring& graphicsShader,
        float3 pos,
        float3 scale,
        float angSpeed = 10.0f * DEG2RAD)
        : StaticMesh(modelName, matPreset, inputLayout, graphicsShader)
        , angularSpeed_(angSpeed)
    {
        SetPosition(pos);
        SetScale(scale);
    }

    void Tick(float deltaTime) override
    {
        rotationY_ += angularSpeed_ * deltaTime;
        if (rotationY_ > XM_2PI)
        {
            rotationY_ -= XM_2PI;
        }

        SetRotationEulerRad({ 0.0f, rotationY_, 0.0f });
    }

private:
    float rotationY_ = 0.0f;
    float angularSpeed_ = 10.0f * Math::DEG2RAD;
};

std::wstring Widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

float3 ToFloat3(const json& j, const float3& def = float3(0.0f, 0.0f, 0.0f))
{
    if (!j.is_array() || j.size() < 3) { return def; }
    return float3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

bool TryFloat3(const json& j, float3& out)
{
    if (!j.is_array() || j.size() < 3) { return false; }
    out = float3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    return true;
}

SceneObjectRegistry::ObjectList CreateStaticMesh(SceneObjectRegistry::CreationContext& ctx, const json& o)
{
    (void)ctx;

    SceneObjectRegistry::ObjectList objects;

    // RotatingObject is demo-specific: build it here, then apply the shared
    // staticMesh JSON properties through the factory so behavior matches a plain
    // staticMesh. Non-rotating meshes come straight from the factory.
    if (o.contains("rotateSpeedDeg"))
    {
        const std::string model = o.value("model", std::string{});
        const std::string material = o.value("material", std::string{});
        const std::string layout = o.value("inputLayout", std::string("PosNormTanUV"));
        const std::wstring shader = Widen(o.value("shader", std::string("shaders/gbuffer.hlsl")));
        const float3 pos = ToFloat3(o.value("position", json::array()));
        const float3 scale = ToFloat3(o.value("scale", json::array()), float3(1.0f, 1.0f, 1.0f));

        auto mesh = std::make_unique<RotatingObject>(model, material, layout, shader, pos, scale,
            o["rotateSpeedDeg"].get<float>() * DEG2RAD);
        SceneObjectFactory::ApplyStaticMeshJsonProperties(*mesh, o);
        objects.push_back(std::move(mesh));
    }
    else
    {
        objects.push_back(SceneObjectFactory::CreateStaticMeshFromJson(o));
    }

    return objects;
}

SceneObjectRegistry::ObjectList CreateTransparentMesh(SceneObjectRegistry::CreationContext& ctx, const json& o)
{
    SceneObjectRegistry::ObjectList objects;
    objects.push_back(SceneObjectFactory::CreateTransparentMeshFromJson(ctx.scene, o));
    return objects;
}

SceneObjectRegistry::ObjectList CreateInstancedModels(SceneObjectRegistry::CreationContext& ctx, const json& o)
{
    (void)ctx;

    SceneObjectRegistry::ObjectList objects;
    const std::string model = o.value("model", std::string{});
    const std::string material = o.value("material", std::string{});
    const std::string layout = o.value("inputLayout", std::string("PosNormTanUV"));
    const std::wstring shader = Widen(o.value("shader", std::string("shaders/gbuffer_inst.hlsl")));
    const std::wstring computeShader = Widen(o.value("computeShader", std::string("shaders/instance_anim.hlsl")));
    const unsigned int count = o.value("count", 1u);

    auto instances = std::make_unique<GpuInstancedModels>(model, count, material, layout, shader, computeShader);

    float3 position = instances->GetPosition();
    float3 rotationRad = instances->GetRotationEulerRad();
    float3 scale = instances->GetScale();
    float3 value{};
    if (o.contains("position") && TryFloat3(o["position"], value))
    {
        position = value;
        instances->SetPosition(position);
    }
    if (o.contains("rotationDeg") && TryFloat3(o["rotationDeg"], value))
    {
        rotationRad = float3(value.x * DEG2RAD, value.y * DEG2RAD, value.z * DEG2RAD);
        instances->SetRotationEulerRad(rotationRad);
    }
    if (o.contains("scale") && TryFloat3(o["scale"], value))
    {
        scale = value;
        instances->SetScale(scale);
    }

    instances->SetModelMatrix(mat4::Scaling(scale) * mat4::RotationFromEulerXYZRad(rotationRad) * mat4::Translation(position));
    objects.push_back(std::move(instances));
    return objects;
}

std::string ReadOceanPresetPath(const json& ocean)
{
    if (ocean.is_string())
    {
        return ocean.get<std::string>();
    }
    if (!ocean.is_object())
    {
        return {};
    }

    constexpr const char* kPathKeys[] = { "preset", "presetFile", "config", "configFile" };
    for (const char* key : kPathKeys)
    {
        if (ocean.contains(key) && ocean[key].is_string())
        {
            return ocean[key].get<std::string>();
        }
    }
    return {};
}

SceneObjectRegistry::ObjectList CreateOcean(SceneObjectRegistry::CreationContext& ctx, const json& ocean)
{
    SceneObjectRegistry::ObjectList objects;

    if (ocean.is_boolean())
    {
        if (!ocean.get<bool>())
        {
            Systems::DestroyOceanSimulation();
            return objects;
        }

        assert(false && "Level ocean requires a preset file path");
        Systems::DestroyOceanSimulation();
        return objects;
    }

    if (ocean.is_object() && !ocean.value("enabled", true))
    {
        Systems::DestroyOceanSimulation();
        return objects;
    }

    const std::string presetPath = ReadOceanPresetPath(ocean);
    if (presetPath.empty())
    {
        assert(false && "Level ocean requires preset/config path");
        Systems::DestroyOceanSimulation();
        return objects;
    }

    OceanSimulation* oceanSimulation = Systems::CreateOceanSimulation(Widen(presetPath));
    if (oceanSimulation)
    {
        objects.push_back(std::make_unique<OceanRenderable>(&ctx.scene.CameraRef(), &ctx.scene, oceanSimulation));
    }
    return objects;
}

SceneObjectRegistry::ObjectList CreateDebugGrid(SceneObjectRegistry::CreationContext& ctx, const json& o)
{
    (void)ctx;

    SceneObjectRegistry::ObjectList objects;
    const float halfSize = o.value("halfSize", 100.0f);
    objects.push_back(std::make_unique<DebugGrid>(halfSize));
    return objects;
}
} // namespace

bool SceneObjectRegistry::Register(std::string type, Creator creator)
{
    if (type.empty() || !creator)
    {
        return false;
    }

    return creators_.try_emplace(std::move(type), std::move(creator)).second;
}

bool SceneObjectRegistry::Has(std::string_view type) const
{
    return creators_.find(std::string(type)) != creators_.end();
}

SceneObjectRegistry::ObjectList SceneObjectRegistry::Create(std::string_view type, CreationContext& ctx, const nlohmann::json& objectJson) const
{
    const auto it = creators_.find(std::string(type));
    if (it == creators_.end())
    {
        return {};
    }

    return it->second(ctx, objectJson);
}

SceneObjectRegistry SceneObjectRegistry::CreateWithBuiltins()
{
    SceneObjectRegistry registry;
    registry.Register("staticMesh", CreateStaticMesh);
    registry.Register("transparentMesh", CreateTransparentMesh);
    registry.Register("instancedModels", CreateInstancedModels);
    registry.Register("ocean", CreateOcean);
    registry.Register("debugGrid", CreateDebugGrid);
    return registry;
}
