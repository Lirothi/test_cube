#include "app/levels/DemoLevel.h"

#include <memory>

#include <DirectXMath.h>

#include "app/Scene.h"
#include "app/Systems.h"
#include "core/math/Math.h"
#include "rendering/debug/DebugGrid.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/SpotLight.h"
#include "rendering/meshes/GpuInstancedModels.h"
#include "rendering/meshes/StaticMesh.h"
#include "rendering/renderables/TransparentStaticMesh.h"
#include "ocean/OceanRenderable.h"

using namespace Math;

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
}

void DemoLevel::Load(const LevelLoadContext& ctx)
{
    auto& renderer = Systems::GetRenderer();
    auto& scene = Systems::GetScene();
    auto& lightManager = scene.GetLightManager();

    lightManager.Reset();

    auto skybox = std::make_unique<Skybox>(L"textures/skybox.dds");
    skybox->Init(&renderer, ctx.uploadCmdList, ctx.uploadKeepAlive);
    skybox->SetExposure(0.2f);
    scene.SetSkybox(std::move(skybox));

    SpotLightDesc warmSpot{};
    warmSpot.position = float3(4.0f, 4.0f, -3.0f);
    warmSpot.direction = float3(-1.0f, -1.0f, 0.0f).Normalized();
    warmSpot.range = 20.0f;
    warmSpot.innerAngle = DirectX::XMConvertToRadians(18.0f);
    warmSpot.outerAngle = DirectX::XMConvertToRadians(28.0f);
    warmSpot.color = float3(1.0f, 0.85f, 0.6f);
    warmSpot.intensity = 15.0f;
    warmSpot.shadowNormalBias = 0.05f;
    warmSpot.shadowDepthBias = 0.0001f;
    lightManager.SpotLights().push_back({});
    lightManager.SpotLights().back().SetDesc(warmSpot);

    SpotLightDesc coolSpot{};
    coolSpot.position = float3(-5.0f, 5.0f, -6.0f);
    coolSpot.direction = float3(0.5f, -1.0f, 0.25f).Normalized();
    coolSpot.range = 25.0f;
    coolSpot.innerAngle = DirectX::XMConvertToRadians(20.0f);
    coolSpot.outerAngle = DirectX::XMConvertToRadians(32.0f);
    coolSpot.color = float3(0.6f, 0.8f, 1.0f);
    coolSpot.intensity = 18.0f;
    coolSpot.shadowNormalBias = 0.05f;
    coolSpot.shadowDepthBias = 0.0001f;
    lightManager.SpotLights().push_back({});
    lightManager.SpotLights().back().SetDesc(coolSpot);

    Scene::DirectionalLight dirLight{};
    dirLight.dir = float3(-1.5f, -0.7f, -0.5f).Normalized();
    dirLight.color = float3(1.0f, 1.0f, 1.0f);
    dirLight.exposure = 1.0f;
    dirLight.ambient = 0.05f;
    scene.SetDirectionalLight(dirLight);

    {
        auto box = std::make_unique<RotatingObject>("models/box.obj", "damaged_plaster", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(0.0f, 0.5f, -2.0f), float3(1, 1, 1));
        box->MaterialParamsRef().texFlags.w = 1;
        scene.AddObject(std::move(box));

        box = std::make_unique<RotatingObject>("models/box.obj", "damaged_plaster", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(0.0f, 0.5f, -4.0f), float3(1, 1, 1), 0.0f);
        scene.AddObject(std::move(box));
    }

    scene.AddObject(std::make_unique<RotatingObject>("models/teapot.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(-1.0f, 0.5f, -1.0f), float3(1, 1, 1)));
    scene.AddObject(std::make_unique<RotatingObject>("models/sphere.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(-3.0f, 0.5f, -1.0f), float3(1, 1, 1)));
    scene.AddObject(std::make_unique<RotatingObject>("models/corgi.obj", "brick", "PosNormTanUV", L"shaders/gbuffer.hlsl", float3(3.0f, 0.5f, -1.0f), float3(1, 1, 1)));

    {
        auto glass = std::make_unique<TransparentStaticMesh>(&scene, "models/box.obj", float3(-1.8f, 0.4f, -4.2f), float3(0.6f, 0.6f, 0.6f), 0.0f);
        glass->SetTint(float3(0.78f, 0.9f, 1.0f));
        glass->SetAbsorption(float3(0.16f, 0.07f, 0.03f));
        glass->SetThickness(0.65f);
        glass->SetReflectionStrength(1.25f);
        glass->SetRefractionDistortion(1.00f);
        glass->SetRoughness(0.05f);
        glass->SetIor(1.1f);
        glass->SetNormalMap(L"textures/damaged_plaster_normal.dds");
        scene.AddObject(std::move(glass));

        glass = std::make_unique<TransparentStaticMesh>(&scene, "models/sphere.obj", float3(-1.8f, 0.5f, -2.2f), float3(0.8f, 0.8f, 0.8f), 0.0f);
        glass->SetTint(float3(0.78f, 0.9f, 1.0f));
        glass->SetAbsorption(float3(0.16f, 0.07f, 0.03f));
        glass->SetThickness(0.65f);
        glass->SetReflectionStrength(4.25f);
        glass->SetRefractionDistortion(1.00f);
        glass->SetRoughness(0.05f);
        glass->SetIor(1.1f);
        glass->SetNormalMap(L"textures/damaged_plaster_normal.dds");
        scene.AddObject(std::move(glass));
    }

    {
        auto floor = std::make_unique<StaticMesh>("models/box.obj", "sandstone_cracks", "PosNormTanUV", L"shaders/gbuffer.hlsl");
        floor->MaterialParamsRef().texOffsScale = float4(0.0f, 0.0f, 20.0f, 20.0f);
        floor->SetPosition(float3(0.0f, -0.5f, 0.0f));
        floor->SetScale(float3(40.0f, 1.0f, 40.0f));
        scene.AddObject(std::move(floor));

        floor = std::make_unique<StaticMesh>("models/box.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl");
        floor->MaterialParamsRef().texOffsScale = float4(0.5f, 0.0f, 10.0f, 10.0f);
        floor->MaterialParamsRef().texFlags.w = 0.01f;
        floor->SetPosition(float3(-5.0f, -0.4f, 0.0f));
        floor->SetScale(float3(5.0f, 1.0f, 5.0f));
        scene.AddObject(std::move(floor));
    }

    {
        int width = 10;
        int height = 5;
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                auto sphere = std::make_unique<StaticMesh>("models/sphere.obj", "bronze", "PosNormTanUV", L"shaders/gbuffer.hlsl");
                sphere->MaterialParamsRef().SetUseMR(false);
                sphere->MaterialParamsRef().metalRough = float2(static_cast<float>(y) / (height - 1), static_cast<float>(x) / (width - 1));
                sphere->SetPosition(float3(static_cast<float>(x) + x * 0.2f, static_cast<float>(y) + y * 0.2f + 1.0f, -static_cast<float>(x) + x * 0.2f - 12.0f));
                scene.AddObject(std::move(sphere));
            }
        }
    }

    scene.AddObject(std::make_unique<GpuInstancedModels>("models/teapot.obj", 100, "bronze", "PosNormTanUV", L"shaders/gbuffer_inst.hlsl", L"shaders/instance_anim.hlsl"));
    scene.AddObject(std::make_unique<OceanRenderable>(&scene.CameraRef()));
    scene.AddObject(std::make_unique<DebugGrid>(100.0f));

    scene.CameraRef().SetPosition({ 0.f, 1.f, -10.f });
}

void DemoLevel::Unload(const LevelLoadContext& ctx)
{
    Level::Unload(ctx);
}

