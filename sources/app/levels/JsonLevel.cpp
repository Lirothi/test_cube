#include "app/levels/JsonLevel.h"

#include <cassert>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <DirectXMath.h>

// nlohmann/json - single header
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

#include "app/camera/Camera.h"
#include "rendering/lighting/DirectionalLight.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "core/math/Math.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/SpotLight.h"
#include "rendering/core/UploadBatch.h"
#include "app/Systems.h"
#include "ocean/OceanSimulation.h"
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

bool IsFreeCameraStart(const json& o)
{
    return o.value("type", std::string()) == "freeCameraStart";
}

void ApplyFreeCameraStart(const json& o, Camera& camera)
{
    camera.SetPosition(ToFloat3(o.value("position", json::array()), camera.GetPosition()));

    const float3 currentRotationDeg(
        camera.GetPitch() * RAD2DEG,
        camera.GetYaw() * RAD2DEG,
        0.0f);
    const float3 rotationDeg = ToFloat3(o.value("rotationDeg", json::array()), currentRotationDeg);
    camera.SetYawPitch(rotationDeg.y * DEG2RAD, rotationDeg.x * DEG2RAD);
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
            desc.shadowsEnabled = sl.value("shadowsEnabled", desc.shadowsEnabled);
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
            desc.shadowsEnabled = pl.value("shadowsEnabled", desc.shadowsEnabled);
            lightManager.PointLights().push_back({});
            lightManager.PointLights().back().SetDesc(desc);
        }
    }

    // Pre-grow the GPU light buffers now, at load time (the caller holds the GPU
    // idle via WaitForPreviousFrame before LoadLevel). The per-frame render path
    // (Pass_SpotLights / Pass_PointLights / TransparentStaticMesh) otherwise grows
    // a light buffer lazily by FREEING the old resource and allocating a bigger
    // one; doing that during parallel pass recording — or while a pipelined frame
    // still references the old buffer — frees a resource the GPU is using and
    // intermittently hangs the device (DXGI_ERROR_DEVICE_HUNG on the SpotLights/
    // PointLights compute dispatch). Growing here, before any pass runs, means the
    // render path always sees sufficient capacity and never reallocates in flight.
    // Mirrors EnvironmentRuntime::RebuildLights (the editor light-mutation path).
    lightManager.UpdateSpotLightCache();
    if (!lightManager.PointLights().empty())
    {
        lightManager.EnsurePointLightBuffer(&renderer, lightManager.PointLights().size());
    }
    if (lightManager.GetSpotLightCount() > 0)
    {
        lightManager.EnsureSpotLightBuffer(&renderer, lightManager.GetSpotLightCount());
    }

    if (j.contains("directionalLight"))
    {
        const json& dl = j["directionalLight"];
        const bool enabled = dl.value("enabled", true);
        DirectionalLight dirLight;
        dirLight.SetDirection(ToFloat3(dl.value("direction", json::array()), float3(0.0f, -1.0f, 0.0f)).Normalized());
        // A disabled sun contributes nothing (direct color + ambient zeroed).
        dirLight.SetColor(enabled ? ToFloat3(dl.value("color", json::array()), float3(1.0f, 1.0f, 1.0f)) : float3(0.0f, 0.0f, 0.0f));
        dirLight.SetExposure(dl.value("exposure", 1.0f));
        dirLight.SetAmbient(enabled ? dl.value("ambient", 0.05f) : 0.0f);
        scene.SetDirectionalLight(dirLight);
    }

    std::optional<json> freeCameraStart;
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
            if (IsFreeCameraStart(o))
            {
#if WITH_EDITOR
                const EditorObjectId editorObjectId =
                    ctx.editorDocument
                        ? ctx.editorDocument->ReadOrAllocateObjectId(o)
                        : EditorObjectId{ ReadOrAllocateEditorObjectId(o, nextEditorObjectId) };
                if (ctx.editorDocument)
                {
                    ctx.editorDocument->AddObjectFromJson(editorObjectId, o);
                }
#endif
                if (enabled)
                {
                    freeCameraStart = o;
                }
                continue;
            }

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
        const json& oceanJson = j["ocean"];
        const bool oceanEnabled = !oceanJson.is_object() || oceanJson.value("enabled", true);
        // Always create the ocean simulation when the level has an ocean preset.
        // The enabled flag only controls render visibility; the system stays
        // available for controls, config edits, and live re-enable without reload.
        AddAnonymousObjects(scene, objectRegistry.Create("ocean", creationCtx, oceanJson));
        scene.SetOceanVisible(oceanEnabled);

        // Apply the level's inline wind overrides (the "scene" block) on top of the
        // preset, so editor-saved wind settings survive reload. No-op if no sim.
        if (oceanJson.is_object() &&
            (oceanJson.contains("windForce") || oceanJson.contains("windDirectionDeg") || oceanJson.contains("swellDirectionDeg")))
        {
            if (OceanSimulation* ocean = Systems::GetOceanSimulation())
            {
                const float windDir = oceanJson.value("windDirectionDeg", ocean->GetLocalWindDirectionDegrees());
                const float swellDir = oceanJson.value("swellDirectionDeg", ocean->GetSwellDirectionDegrees());
                const float windForce = oceanJson.value("windForce", ocean->GetWindForce01());
                ocean->SetSceneVariables(&renderer, windDir, swellDir, windForce);
            }
        }
    }
    else
    {
        Systems::DestroyOceanSimulation();
    }

    AddAnonymousObjects(scene, objectRegistry.Create("debugGrid", creationCtx, json::object()));

    // The top-level camera section stores projection only. Baseline position to
    // origin, then let a FreeCameraStart object provide a level default transform.
    // EditorController may still override this from editor_state.json.
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
        // The top-level camera block only stores projection. A level may define
        // an explicit default editor/gameplay transform with FreeCameraStart.
        // LevelLoadOptions camera overrides still win after JsonLevel::Load.
        if (freeCameraStart)
        {
            ApplyFreeCameraStart(*freeCameraStart, camera);
        }
    }
}

void JsonLevel::Unload(const LevelLoadContext& ctx)
{
    Level::Unload(ctx);
    Systems::DestroyOceanSimulation();
}
