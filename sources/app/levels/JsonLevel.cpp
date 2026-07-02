#include "app/levels/JsonLevel.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

#include <DirectXMath.h>

// nlohmann/json - single header
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

#include "rendering/lighting/DirectionalLight.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "core/math/Math.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/SpotLight.h"
#include "rendering/core/UploadBatch.h"
#include "app/Systems.h"
#if WITH_EDITOR
#include "editor/scene/EditorSceneDocument.h"
#endif

using namespace Math;
using nlohmann::json;

namespace
{
std::wstring Widen(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

float3 ToFloat3(const json& j, const float3& def = float3(0.0f, 0.0f, 0.0f))
{
    if (!j.is_array() || j.size() < 3) { return def; }
    return float3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

#if WITH_EDITOR
Scene::SceneObjectId ReadOrAllocateEditorObjectId(const json& o, Scene::SceneObjectId& nextId)
{
    const auto idIt = o.find("id");
    if (idIt != o.end() && idIt->is_number_integer())
    {
        Scene::SceneObjectId id = 0;
        if (idIt->is_number_unsigned())
        {
            id = idIt->get<Scene::SceneObjectId>();
        }
        else
        {
            const int64_t signedId = idIt->get<int64_t>();
            if (signedId > 0)
            {
                id = static_cast<Scene::SceneObjectId>(signedId);
            }
        }

        if (id != 0)
        {
            if (id >= nextId)
            {
                nextId = id + 1;
            }
            return id;
        }
    }

    return nextId++;
}

void AddLoadedObjects(Scene& scene, SceneObjectRegistry::ObjectList objects, Scene::SceneObjectId editorId, bool enabled)
{
    for (std::unique_ptr<RenderableObjectBase>& obj : objects)
    {
        if (!obj)
        {
            continue;
        }

        obj->SetVisible(enabled);
        scene.AddObjectWithEditorId(std::move(obj), editorId);
    }
}
#else
void AddLoadedObjects(Scene& scene, SceneObjectRegistry::ObjectList objects)
{
    for (std::unique_ptr<RenderableObjectBase>& obj : objects)
    {
        if (!obj)
        {
            continue;
        }

        scene.AddObject(std::move(obj));
    }
}
#endif

void AddAnonymousObjects(Scene& scene, SceneObjectRegistry::ObjectList objects)
{
    for (std::unique_ptr<RenderableObjectBase>& obj : objects)
    {
        if (!obj)
        {
            continue;
        }
        scene.AddObject(std::move(obj));
    }
}
} // namespace

void JsonLevel::Load(const LevelLoadContext& ctx)
{
    auto& renderer = ctx.renderer;
    auto& scene = ctx.scene;
    auto& lightManager = scene.GetLightManager();
#if WITH_EDITOR
    Scene::SceneObjectId nextEditorObjectId = 1;
#endif

    lightManager.Reset();
    SceneObjectRegistry objectRegistry = SceneObjectRegistry::CreateWithBuiltins();
    SceneObjectRegistry::CreationContext creationCtx{ scene };

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
            assert(false && "Level source file is not valid JSON");
            return;
        }
    }

#if WITH_EDITOR
    if (ctx.editorDocument)
    {
        ctx.editorDocument->ResetFromLevelJson(sourcePath_, j);
    }
#endif

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
            if (!o.is_object())
            {
                continue;
            }

            const bool enabled = o.value("enabled", true);
#if !WITH_EDITOR
            if (!enabled)
            {
                continue;
            }
#endif
            const std::string type = o.value("type", std::string{});
            const bool objectTypeRegistered = objectRegistry.Has(type);
            if (!objectTypeRegistered)
            {
                assert(false && "Unknown object type in level JSON");
                continue;
            }
#if WITH_EDITOR
            const EditorObjectId editorObjectId =
                ctx.editorDocument
                    ? ctx.editorDocument->ReadOrAllocateObjectId(o)
                    : EditorObjectId{ ReadOrAllocateEditorObjectId(o, nextEditorObjectId) };
#endif
            SceneObjectRegistry::ObjectList objects = objectRegistry.Create(type, creationCtx, o);
            AddLoadedObjects(scene, std::move(objects)
#if WITH_EDITOR
                , editorObjectId.value, enabled
#endif
            );
#if WITH_EDITOR
            if (ctx.editorDocument)
            {
                ctx.editorDocument->AddObjectFromJson(editorObjectId, o);
            }
#endif
        }
    }

#if WITH_EDITOR
    if (ctx.editorDocument)
    {
        // Build environment entities after the object loop so their allocated ids
        // never collide with object ids (see EditorSceneDocument).
        ctx.editorDocument->RebuildEnvironmentEntities();
    }
#endif

    if (j.contains("ocean"))
    {
        AddAnonymousObjects(scene, objectRegistry.Create("ocean", creationCtx, j["ocean"]));
    }
    else
    {
        Systems::DestroyOceanSimulation();
    }

    AddAnonymousObjects(scene, objectRegistry.Create("debugGrid", creationCtx, json::object()));

    // Camera position is no longer stored in the level file. Baseline it to the
    // origin; in editor builds EditorController restores a per-level camera from
    // editor_state.json when a record exists, otherwise the camera stays at zero.
    // Only the projection (fov / near / far) comes from the level.
    {
        Camera& camera = scene.CameraRef();
        camera.SetPosition(float3(0.0f, 0.0f, 0.0f));
        if (j.contains("camera"))
        {
            const json& cam = j["camera"];
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
}

void JsonLevel::Unload(const LevelLoadContext& ctx)
{
    Level::Unload(ctx);
    Systems::DestroyOceanSimulation();
}
