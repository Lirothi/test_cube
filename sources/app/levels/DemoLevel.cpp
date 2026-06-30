#include "app/levels/DemoLevel.h"

#include <cassert>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <DirectXMath.h>

// nlohmann/json — single header
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

#include "rendering/lighting/DirectionalLight.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h"
#include "core/math/Math.h"
#include "rendering/debug/DebugGrid.h"
#include "rendering/RenderLayers.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/SpotLight.h"
#include "rendering/core/UploadBatch.h"
#include "rendering/meshes/GpuInstancedModels.h"
#include "rendering/meshes/StaticMesh.h"
#include "rendering/renderables/TransparentStaticMesh.h"
#include "app/Systems.h"
#include "ocean/OceanRenderable.h"

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

void SpawnStaticMesh(Scene& scene, const json& o)
{
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
        scene.AddObject(std::move(mesh));
    }
    else
    {
        scene.AddObject(SceneObjectFactory::CreateStaticMeshFromJson(o));
    }
}

void SpawnTransparentMesh(Scene& scene, const json& o)
{
    scene.AddObject(SceneObjectFactory::CreateTransparentMeshFromJson(scene, o));
}

// Generator: width x height grid of spheres sweeping metallic (rows) and
// roughness (columns), used as a PBR calibration wall.
void SpawnMetalRoughGrid(Scene& scene, const json& o)
{
    const std::string model = o.value("model", std::string("models/sphere.obj"));
    const std::string material = o.value("material", std::string("bronze"));
    const std::string layout = o.value("inputLayout", std::string("PosNormTanUV"));
    const std::wstring shader = Widen(o.value("shader", std::string("shaders/gbuffer.hlsl")));
    const int width = o.value("width", 10);
    const int height = o.value("height", 5);

    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            auto sphere = std::make_unique<StaticMesh>(model, material, layout, shader);
            sphere->MaterialParamsRef().SetUseMR(false);
            sphere->MaterialParamsRef().metalRough = float2(static_cast<float>(y) / (height - 1), static_cast<float>(x) / (width - 1));
            sphere->SetPosition(float3(static_cast<float>(x) + x * 0.2f, static_cast<float>(y) + y * 0.2f + 1.0f, -static_cast<float>(x) + x * 0.2f - 12.0f));
            scene.AddObject(std::move(sphere));
        }
    }
}

void SpawnInstancedModels(Scene& scene, const json& o)
{
    const std::string model = o.value("model", std::string{});
    const std::string material = o.value("material", std::string{});
    const std::string layout = o.value("inputLayout", std::string("PosNormTanUV"));
    const std::wstring shader = Widen(o.value("shader", std::string("shaders/gbuffer_inst.hlsl")));
    const std::wstring computeShader = Widen(o.value("computeShader", std::string("shaders/instance_anim.hlsl")));
    const UINT count = o.value("count", 1u);

    scene.AddObject(std::make_unique<GpuInstancedModels>(model, count, material, layout, shader, computeShader));
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

void LoadOceanFromLevel(Scene& scene, const json& j)
{
    if (!j.contains("ocean"))
    {
        Systems::DestroyOceanSimulation();
        return;
    }

    const json& ocean = j["ocean"];
    if (ocean.is_boolean())
    {
        if (!ocean.get<bool>())
        {
            Systems::DestroyOceanSimulation();
            return;
        }

        assert(false && "Level ocean requires a preset file path");
        Systems::DestroyOceanSimulation();
        return;
    }

    if (ocean.is_object() && !ocean.value("enabled", true))
    {
        Systems::DestroyOceanSimulation();
        return;
    }

    const std::string presetPath = ReadOceanPresetPath(ocean);
    if (presetPath.empty())
    {
        assert(false && "Level ocean requires preset/config path");
        Systems::DestroyOceanSimulation();
        return;
    }

    OceanSimulation* oceanSimulation = Systems::CreateOceanSimulation(Widen(presetPath));
    if (oceanSimulation)
    {
        scene.AddObject(std::make_unique<OceanRenderable>(&scene.CameraRef(), &scene, oceanSimulation));
    }
}
} // namespace

void DemoLevel::Load(const LevelLoadContext& ctx)
{
    auto& renderer = ctx.renderer;
    auto& scene = ctx.scene;
    auto& lightManager = scene.GetLightManager();

    lightManager.Reset();

    json j;
    {
        std::ifstream f(sourcePath_);
        if (!f)
        {
            assert(false && "Level source file not found!");
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        j = json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
        if (j.is_discarded())
        {
            assert(false && "data/levels/demo.json is not valid JSON");
            return;
        }
    }

    if (j.contains("skybox") && j["skybox"].contains("texture"))
    {
        auto skybox = std::make_unique<Skybox>(Widen(j["skybox"]["texture"].get<std::string>()));
        skybox->Init(&renderer, ctx.uploads.CommandList(), ctx.uploads.KeepAlive());
        scene.SetSkybox(std::move(skybox));
    }

    if (j.contains("spotLights") && j["spotLights"].is_array())
    {
        for (const json& sl : j["spotLights"])
        {
            if (!sl.value("enabled", true))
            {
                continue;
            }
            SpotLightDesc desc{};
            desc.position = ToFloat3(sl.value("position", json::array()));
            desc.direction = ToFloat3(sl.value("direction", json::array()), float3(0.0f, -1.0f, 0.0f)).Normalized();
            desc.range = sl.value("range", desc.range);
            desc.innerAngle = DirectX::XMConvertToRadians(sl.value("innerAngleDeg", 15.0f));
            desc.outerAngle = DirectX::XMConvertToRadians(sl.value("outerAngleDeg", 25.0f));
            desc.color = ToFloat3(sl.value("color", json::array()), float3(1.0f, 1.0f, 1.0f));
            desc.intensity = sl.value("intensity", desc.intensity);
            desc.shadowNormalBias = sl.value("shadowNormalBias", desc.shadowNormalBias);
            desc.shadowDepthBias = sl.value("shadowDepthBias", desc.shadowDepthBias);
            lightManager.SpotLights().push_back({});
            lightManager.SpotLights().back().SetDesc(desc);
        }
    }

    if (j.contains("pointLights") && j["pointLights"].is_array())
    {
        for (const json& pl : j["pointLights"])
        {
            if (!pl.value("enabled", true))
            {
                continue;
            }

            PointLightDesc desc{};
            desc.position = ToFloat3(pl.value("position", json::array()), desc.position);
            desc.radius = pl.value("radius", desc.radius);
            desc.color = ToFloat3(pl.value("color", json::array()), desc.color);
            desc.intensity = pl.value("intensity", desc.intensity);
            lightManager.PointLights().push_back({});
            lightManager.PointLights().back().SetDesc(desc);
        }
    }

    if (j.contains("directionalLight"))
    {
        const json& dl = j["directionalLight"];
        DirectionalLight dirLight;
        dirLight.SetDirection(ToFloat3(dl.value("direction", json::array()), float3(0.0f, -1.0f, 0.0f)).Normalized());
        dirLight.SetColor(ToFloat3(dl.value("color", json::array()), float3(1.0f, 1.0f, 1.0f)));
        dirLight.SetExposure(dl.value("exposure", 1.0f));
        dirLight.SetAmbient(dl.value("ambient", 0.05f));
        scene.SetDirectionalLight(dirLight);
    }

    if (j.contains("objects") && j["objects"].is_array())
    {
        for (const json& o : j["objects"])
        {
            if (!o.value("enabled", true))
            {
                continue;
            }
            const std::string type = o.value("type", std::string{});
            if (type == "staticMesh")
            {
                SpawnStaticMesh(scene, o);
            }
            else if (type == "transparentMesh")
            {
                SpawnTransparentMesh(scene, o);
            }
            else if (type == "metalRoughGrid")
            {
                SpawnMetalRoughGrid(scene, o);
            }
            else if (type == "instancedModels")
            {
                SpawnInstancedModels(scene, o);
            }
            else
            {
                assert(false && "Unknown object type in level JSON");
            }
        }
    }

    LoadOceanFromLevel(scene, j);
    scene.AddObject(std::make_unique<DebugGrid>(100.0f));

    if (j.contains("camera"))
    {
        const json& cam = j["camera"];
        Camera& camera = scene.CameraRef();
        if (cam.contains("position"))
        {
            camera.SetPosition(ToFloat3(cam["position"], float3(0.0f, 1.0f, -10.0f)));
        }
        if (cam.contains("hfovDeg"))
        {
            camera.SetHFov(DirectX::XMConvertToRadians(cam["hfovDeg"].get<float>()));
        }
        if (cam.contains("zNear") || cam.contains("zFar"))
        {
            camera.SetZNearFar(cam.value("zNear", camera.GetZNear()), cam.value("zFar", camera.GetZFar()));
        }
    }
}

void DemoLevel::Unload(const LevelLoadContext& ctx)
{
    Level::Unload(ctx);
    Systems::DestroyOceanSimulation();
}
