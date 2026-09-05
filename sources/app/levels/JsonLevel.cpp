#include "app/levels/JsonLevel.h"

#include "core/diagnostics/BootProfile.h"

#include <chrono>

#include <algorithm>
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
#include "app/scene/GtaoSettingsJson.h"
#include "app/scene/AtmosphereSettingsJson.h"
#include "app/scene/BloomSettingsJson.h"

// P8B: the level-wide look settings live under one "postProcess" section now. A level written
// before that carries them as top-level sections, and must keep loading unchanged -- so this
// prefers the folded section and falls back to the legacy key, per group. Both forms are read
// forever; only the WRITER changed.
namespace
{
    const nlohmann::json* PostProcessSection(const nlohmann::json& level, const char* key)
    {
        const auto folded = level.find("postProcess");
        if (folded != level.end() && folded->is_object())
        {
            const auto sub = folded->find(key);
            if (sub != folded->end() && sub->is_object())
            {
                return &(*sub);
            }
        }
        const auto legacy = level.find(key);
        return (legacy != level.end() && legacy->is_object()) ? &(*legacy) : nullptr;
    }
}
#pragma warning(pop)

#include "app/camera/Camera.h"
#include "rendering/core/PhotographicSettingsJson.h"
#include "rendering/lighting/DirectionalLight.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectRegistry.h"
#include "core/math/Math.h"
#include "rendering/lighting/LightManager.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/SpotLight.h"
#include "rendering/core/UploadBatch.h"
#include "app/Systems.h"
#include "ocean/OceanRenderConfigJson.h"
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

void ParsePointLightFlicker(const json& source, PointLightDesc& desc)
{
    const auto it = source.find("flicker");
    if (it == source.end() || !it->is_object())
    {
        return;
    }

    const json& flicker = *it;
    desc.flicker.amplitude = flicker.value("amplitude", desc.flicker.amplitude);
    desc.flicker.frequencyHz = flicker.value("frequencyHz", desc.flicker.frequencyHz);
    const auto seedIt = flicker.find("seed");
    if (seedIt != flicker.end() && seedIt->is_number_integer())
    {
        const std::int64_t seed = seedIt->get<std::int64_t>();
        desc.flicker.seed = seed > 0 ? static_cast<std::uint32_t>(seed) : 0u;
    }
}

bool IsFreeCameraStart(const json& o)
{
    return o.value("type", std::string()) == "freeCameraStart";
}

void ApplyFreeCameraStart(const json& o, Camera& camera)
{
    camera.SetPosition(ToFloat3(o.value("position", json::array()), camera.GetPosition()));

    // rotationDeg is (pitch, yaw, roll) to match every other object's transform. The Z component
    // used to be parsed and then dropped on the floor; the camera can bank now, so it is applied.
    const float3 currentRotationDeg(
        camera.GetPitch() * RAD2DEG,
        camera.GetYaw() * RAD2DEG,
        camera.GetRoll() * RAD2DEG);
    const float3 rotationDeg = ToFloat3(o.value("rotationDeg", json::array()), currentRotationDeg);
    camera.SetYawPitchRoll(rotationDeg.y * DEG2RAD, rotationDeg.x * DEG2RAD, rotationDeg.z * DEG2RAD);
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
    BOOT_SCOPE("JsonLevel::Load");
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
        BOOT_SCOPE("Skybox + IBL");
        auto skybox = std::make_unique<Skybox>(Widen(j["skybox"]["texture"].get<std::string>()));
        // P16.3b: BEFORE Init(). Init loads `_diffuse.dds`, measures the sky's own illuminance and
        // logs the physical scale it implies -- with the authored lux set afterwards that line would
        // report 0 and be wrong about what the frame is doing.
        skybox->SetIlluminanceLux(j["skybox"].value("illuminanceLux", 0.0f));
        skybox->Init(&renderer, ctx.uploads.CommandList(), ctx.uploads.KeepAlive());
        // How bright this level's sky is, on the engine's linear scale. Default 1 = the cubemap's
        // own radiance, untouched, which is what every level got before this field existed.
        //
        // It has to be authorable because HDRI libraries are not calibrated to our scale: measured
        // on `citrus_orchard_puresky_4k`, the sky's median luminance is 0.657, and with the default
        // manual exposure multiplier of 1.44 that puts 48.6% of the sky ABOVE 1.0 before the tone
        // curve even runs -- a blowout no curve can undo. Auto-exposure used to hide this by
        // metering it away, which is precisely why it must not be the only thing holding the image
        // together. Same reasoning as P4's sunIntensity: the scene says how bright its lights are,
        // the camera decides how to photograph them.
        skybox->SetExposure(j["skybox"].value("intensity", 1.0f));
        // `illuminanceLux` is read above, before Init.
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
            // P16.5: `luminousFluxLm` is the field going forward; `intensity` is the pre-P16.5
            // number, converted by the r/2 rule (PhotographicSettings.h). NOT lossless -- the
            // falloff shape changed with the unit -- so an existing level keeps its lights roughly
            // where they were rather than exactly.
            desc.luminousFluxLm = sl.contains("luminousFluxLm")
                ? sl.value("luminousFluxLm", desc.luminousFluxLm)
                : render::LumensFromLegacyIntensity(sl.value("intensity", 5.0f), desc.range);
            desc.shadowNormalBias = sl.value("shadowNormalBias", desc.shadowNormalBias);
            desc.volumetricIntensity = sl.value("volumetricIntensity", desc.volumetricIntensity); // plan A4d
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
            desc.luminousFluxLm = pl.contains("luminousFluxLm") // P16.5, see the spot above
                ? pl.value("luminousFluxLm", desc.luminousFluxLm)
                : render::LumensFromLegacyIntensity(pl.value("intensity", 1.0f), desc.radius);
            desc.shadowsEnabled = pl.value("shadowsEnabled", desc.shadowsEnabled);
            desc.volumetricIntensity = pl.value("volumetricIntensity", desc.volumetricIntensity); // plan A4d
            ParsePointLightFlicker(pl, desc);
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
        dirLight.SetAmbient(enabled ? dl.value("ambient", 0.05f) : 0.0f);
        // P16.2: `sunIlluminanceLux` is the field going forward, then P4's `sunIntensity`, then
        // the original whole-scene `exposure` multiplier. Newest present wins, because a level that
        // has been converted should not keep paying for the older semantics, and every step of the
        // chain is LOSSLESS -- P4 folded the multiplier into the intensity for an identical product,
        // and P16.2 changed only what the number is called (see DirectionalLight).
        //
        // ONLY the sun intensity branches. The fill fields below are read UNCONDITIONALLY: they are
        // orthogonal to how the intensity was authored, and gating them on `sunIntensity` made them
        // dead on every existing level -- ticking the boxes in the inspector did nothing at all,
        // because every shipped level still carries the legacy field.
        if (dl.contains("sunIlluminanceLux"))
        {
            dirLight.SetSunIlluminanceLux(dl.value("sunIlluminanceLux", 100000.0f));
        }
        else if (dl.contains("sunIntensity"))
        {
            dirLight.MigrateLegacySunIntensity(dl.value("sunIntensity", 1.0f));
        }
        else
        {
            dirLight.MigrateLegacyExposure(dl.value("exposure", 1.0f));
        }
        dirLight.SetSkyFillIntensity(dl.value("skyFillIntensity", 1.0f));
        // P16.12: the ground's diffuse reflectance. Read unconditionally for the same reason the
        // fill fields above are -- it is orthogonal to how the sun intensity was authored. A
        // disabled sun still has a lit sky, so this is NOT gated on `enabled`; the term scales
        // itself down through the illuminance it reads.
        dirLight.SetGroundAlbedo(ToFloat3(dl.value("groundAlbedo", json::array()),
                                          dirLight.GetGroundAlbedo()));
        dirLight.SetUseSunTemperature(dl.value("useSunTemperature", false));
        dirLight.SetSunTemperatureK(dl.value("sunTemperatureK", 6500.0f));
        scene.SetDirectionalLight(dirLight);
    }

    // P1: photographic camera settings. A level without the section keeps the struct defaults,
    // whose `enabled = false` reproduces the pre-plan image exactly -- that is the whole
    // compatibility story for old levels, so there is deliberately no fallback to the directional
    // light's legacy `exposure` here. P4 is where those two meet.
    {
        render::CameraExposureSettings exposure{};
        const nlohmann::json* exposureSection = PostProcessSection(j, "cameraExposure");
        if (exposureSection)
        {
            render::PhotographicSettingsJson::ApplyOverrides(*exposureSection, exposure);
        }
        // The P3B local-exposure fields used to live in `colorPipeline`; lift them if this level
        // still writes them there. Done AFTER the camera parse so an explicit new-block value wins.
        if (const nlohmann::json* colorSection = PostProcessSection(j, "colorPipeline"))
        {
            render::PhotographicSettingsJson::MigrateLegacyLocalExposure(
                *colorSection,
                exposureSection ? *exposureSection : nlohmann::json::object(),
                exposure);
        }
        scene.SetCameraExposure(exposure);
        // Plan section 6.4: a level load invalidates the adapted value. Without this the camera
        // would spend the first seconds of a new level adapting down from the previous one's
        // brightness, which is exactly the "spend seconds adapting from stale history" that
        // locked decision 5 rules out.
        renderer.Exposure().RequestReset();
    }

    // P6B: screen-space AO. A level without the section keeps the struct defaults, whose
    // `enabled = false` reproduces the pre-P6B image exactly.
    {
        GtaoSettings gtao{};
        if (const nlohmann::json* section = PostProcessSection(j, "gtao"))
        {
            GtaoSettingsJson::ApplyOverrides(*section, gtao);
        }
        scene.SetGtao(gtao);

        AtmosphereSettings atmosphere{};
        if (const nlohmann::json* section = PostProcessSection(j, "atmosphere"))
        {
            AtmosphereSettingsJson::ApplyOverrides(*section, atmosphere);
        }
        scene.SetAtmosphere(atmosphere);

        // P8: a level without the section gets the struct defaults, i.e. bloom OFF.
        BloomSettings bloom{};
        if (const nlohmann::json* section = PostProcessSection(j, "bloom"))
        {
            BloomSettingsJson::ApplyOverrides(*section, bloom);
        }
        scene.SetBloom(bloom);
    }

    // P3: display transform. A level without the section gets the struct defaults, i.e. AgX.
    {
        render::ColorPipelineSettings color{};
        if (const nlohmann::json* section = PostProcessSection(j, "colorPipeline"))
        {
            render::PhotographicSettingsJson::ApplyOverrides(*section, color);
        }
        scene.SetColorPipeline(color);
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
            const auto objBegin = std::chrono::steady_clock::now();
            SceneObjectRegistry::ObjectList objects = objectRegistry.Create(type, creationCtx, o);
            // Bucketed by TYPE, not per object: a level has hundreds of objects and three or four
            // kinds, and the actionable answer is "terrain chunks cost X", not a list of names.
            boot::AddBucket("level object create",
                            std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - objBegin).count(),
                            type);
            boot::AddCount("level objects");
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

    // W2: the global wind entity (top-level "wind"). Populates the scene's WindState — the single
    // source of truth the foliage sway reads from W3 on, and (when present) the ocean's wind driver.
    // Reset to calm/inactive every load; absent section => wind disabled (rigid foliage; the ocean
    // keeps its own preset below — back-compat).
    {
        vfx::WindState& wind = scene.GetWindState();
        wind = vfx::WindState{};
        if (j.contains("wind") && j["wind"].is_object())
        {
            const json& w = j["wind"];
            wind.active = true;
            wind.directionDeg = w.value("directionDeg", wind.directionDeg);
            wind.strength = Saturate(w.value("strength", wind.strength));
            wind.swayFrequency = std::max(0.0f, w.value("swayFrequency", wind.swayFrequency));
            wind.foliageSwayMeters = std::max(0.0f, w.value("foliageSwayMeters", wind.foliageSwayMeters));
            if (w.contains("gust") && w["gust"].is_object())
            {
                const json& g = w["gust"];
                wind.gustAmplitude = std::max(0.0f, g.value("amplitude", wind.gustAmplitude));
                wind.gustFrequencyHz = std::max(0.0f, g.value("frequencyHz", wind.gustFrequencyHz));
                wind.gustSeed = g.value("seed", wind.gustSeed);
            }
        }
    }

    if (j.contains("ocean"))
    {
        const json& oceanJson = j["ocean"];
        const bool oceanEnabled = !oceanJson.is_object() || oceanJson.value("enabled", true);
        // Always create the ocean simulation when the level has an ocean preset.
        // The enabled flag only controls render visibility; the system stays
        // available for controls, config edits, and live re-enable without reload.
        AddAnonymousObjects(scene, objectRegistry.Create("ocean", creationCtx, oceanJson));
        scene.SetOceanVisible(oceanEnabled);

        // Render settings are level-local overrides layered over the selected ocean preset.
        // Older levels without this block keep the preset/default values unchanged.
        if (oceanJson.is_object() && oceanJson.contains("render") && oceanJson["render"].is_object())
        {
            if (OceanSimulation* ocean = Systems::GetOceanSimulation())
            {
                OceanRenderConfig render = ocean->GetRenderConfig();
                OceanRenderConfigJson::ApplyOverrides(oceanJson["render"], render);
                ocean->SetRenderConfig(render);
            }
        }

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

        // W2: when a global wind entity is authored, it is the ocean's wind source of truth — its
        // direction + strength override the preset applied above (the ocean's own swell is kept).
        // Done here at load, where the GPU is idle: SetSceneVariables rebuilds the FFT initial
        // spectrum (ResetInitialSpectrum), so it must NOT be called per frame. Live wind edits will
        // re-push under a GPU idle in W6.
        if (scene.GetWindState().active)
        {
            if (OceanSimulation* ocean = Systems::GetOceanSimulation())
            {
                const vfx::WindState& wind = scene.GetWindState();
                ocean->SetSceneVariables(&renderer, wind.directionDeg,
                    ocean->GetSwellDirectionDegrees(), wind.strength);
            }
        }

        // "--ocean-wind=<0..1>" wins over both sources above — it exists so a headless capture can
        // be forced into a sea state the level does not author.
        if (ocean::g_windForceOverride >= 0.0f)
        {
            if (OceanSimulation* ocean = Systems::GetOceanSimulation())
            {
                ocean->SetSceneVariables(&renderer, ocean->GetLocalWindDirectionDegrees(),
                    ocean->GetSwellDirectionDegrees(), ocean::g_windForceOverride);
            }
        }
    }
    else
    {
        Systems::DestroyOceanSimulation();
    }

    //AddAnonymousObjects(scene, objectRegistry.Create("debugGrid", creationCtx, json::object()));

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
