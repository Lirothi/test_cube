#include "app/game/PlayerStartJson.h"
#include "app/game/RaceCourseJson.h"
#include "app/levels/JsonLevel.h"
#include "app/levels/LevelEnvironment.h" // R9.4: the environment's one road

#include "core/diagnostics/BootProfile.h"
#include "core/JsonField.h" // R9.6: StrictScope
#include "core/logging/Log.h"
#include "core/vfs/Vfs.h"

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
#include "app/scene/HeightFogSettingsJson.h"
#include "app/scene/BloomSettingsJson.h"
#include "app/scene/SkyAtmosphereSettingsJson.h"
#include "app/scene/VolumetricCloudSettingsJson.h"
#include "app/scene/DirectionalLightJson.h"
#include "app/scene/TimeOfDaySettingsJson.h"

#pragma warning(pop)
#include "app/levels/LevelMigration.h" // R9.5: the older formats, read in one place

#include "rendering/view/Camera.h"
#include "rendering/core/PhotographicSettingsJson.h"
#include "rendering/lighting/DirectionalLight.h"
#include "app/scene/Scene.h"
#include "app/scene/SceneObjectFactory.h" // G10d: MeshAssetLoadScope
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
#include "app/levels/LevelDocumentSink.h"
#endif

using namespace Math;
using nlohmann::json;

namespace
{
std::wstring Widen(const std::string& s)
{
    return vfs::Widen(s);
}

// P8B: the level-wide look settings live under one "postProcess" section. An older level's top-level groups and the
// height fog's old name "atmosphere" were moved under it by levelmigrate::Migrate before anything here reads the file.
const json* PostProcessGroup(const json& level, const char* group)
{
    const auto folded = level.find("postProcess");
    if (folded == level.end() || !folded->is_object()) { return nullptr; }
    const auto sub = folded->find(group);
    return (sub != folded->end() && sub->is_object()) ? &*sub : nullptr;
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
    // A level is a JSON file edited by hand, by the editor and through MCP. Its keys are read with value() / get<>(),
    // which throw when a key holds another type than the reader expects -- and nothing above this caught it: the
    // process ended in std::terminate over one wrong value. Now the load stops at that key and says which: what was
    // made before it stays (the caller still finalizes and submits it), the editor's document already holds the whole
    // file (ResetFromLevelJson runs first), so nothing is lost by saving. Only nlohmann's own exceptions: a loader's
    // failure (a device, a file) is not a malformed level and goes on up as before.
    // R9.6: the readers this load shares with the editor (jsonfield::Value / As -- the key tables, the object factory,
    // the lights) give a key of another type its fallback in the editor's frame; inside this scope they throw, as
    // value() always did, so the load still stops at that key with the file named.
    jsonfield::StrictScope strict;
    try
    {
        LoadFromSource_(ctx);
    }
    catch (const nlohmann::json::exception& e)
    {
        LOG_ERROR(logging::LogCategory::Scene, "level '{}' is malformed -- {}. Loaded as far as that key; fix the file and "
                  "load it again", sourcePath_, e.what());
    }
}

void JsonLevel::LoadFromSource_(const LevelLoadContext& ctx)
{
    BOOT_SCOPE("JsonLevel::Load");
    SceneObjectFactory::MeshAssetLoadScope meshAssets; // game thread plan G10d: each mesh.json read once per load
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
        // Both say so in the session log first (a Release build has no assert: it used to show an empty world with
        // nothing written anywhere), then stop a Debug run as before.
        vfs::InStream f(sourcePath_, /*binary*/ false);
        if (!f)
        {
            LOG_ERROR(logging::LogCategory::Scene, "level '{}' cannot be opened: nothing is loaded, the world is empty",
                      sourcePath_);
            assert(false && "Level source file not found!");
            return;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        j = json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
        if (j.is_discarded())
        {
            LOG_ERROR(logging::LogCategory::Scene, "level '{}' is not valid JSON: nothing is loaded, the world is empty",
                      sourcePath_);
            assert(false && "Level source file is not valid JSON");
            return;
        }
    }
    // R9.5: an older level's keys become today's here, once; everything below (and the editor's document) reads only
    // today's names. `--tod-legacy-light` keeps the old sun section for the legacy branch below.
    levelmigrate::Report migration;
    j = levelmigrate::Migrate(std::move(j), g_timeOfDayConvertsDirectionalLight, &migration);
    if (!migration.moved.empty())
    {
        std::string moved;
        for (const std::string& m : migration.moved) { moved += (moved.empty() ? "" : "; ") + m; }
        LOG_INFO(logging::LogCategory::Scene, "level '{}' is in an older format, read as today's: {}", sourcePath_, moved);
    }

#if WITH_EDITOR
    if (ctx.editorDocument)
    {
        ctx.editorDocument->BeginLevel(sourcePath_, j);
    }
#endif

    // WHICH SKY, read before the cubemap and independently of it: a procedural level does not need a
    // texture at all, so this must not sit behind the `texture` gate below. The section's PRESENCE is
    // what makes a level procedural -- the same shape as adding a SkyAtmosphere actor in UE -- and a
    // level without it keeps its cubemap. It used to be a Developer-window combo that lived only for
    // the session, so a level could not state what it was and every reload came back HDRI.
    {
        const auto it = j.find("skyAtmosphere");
        levelenv::ApplySkyAtmosphere(scene, it != j.end() && it->is_object() ? &*it : nullptr); // R9.4
    }
    // Plan part C: the clouds, the same shape -- the section's presence gives the level clouds, its
    // absence leaves the struct default (off). They need the procedural sky; the renderer says so
    // once if a level pairs them with a cubemap.
    {
        const auto it = j.find("volumetricCloud");
        levelenv::ApplyVolumetricCloud(scene, it != j.end() && it->is_object() ? &*it : nullptr); // R9.4
    }

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
            levelenv::ReadSpotLight(sl, desc, levelenv::JsonRead::Strict); // R9.4: the editor's reader too
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
            levelenv::ReadPointLight(pl, desc, levelenv::JsonRead::Strict); // R9.4: the editor's reader too
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

    // Time-of-day plan: the level's light is its TIME OF DAY -- the entity hands the scene's one
    // directional light to the sun by day and to the moon by night (app/scene/TimeOfDay.h). A level
    // written before it (2026-10-13) carries its sun as `directionalLight`; levelmigrate::Migrate converted that
    // section above, the sun solved onto the simple path exactly where it was authored, and the file takes the
    // new section when it is next saved. `--tod-legacy-light` keeps the old section and reads it as before (rollback,
    // and the "before" of the conversion check). Reset on every load, so no level inherits the previous one's clock.
    {
        TimeOfDaySettings timeOfDay{};
        DirectionalLight sun = TimeOfDaySettingsJson::DefaultSun();
        const auto todIt = j.find("timeOfDay");
        const auto dlIt = j.find("directionalLight"); // only under --tod-legacy-light: Migrate erased it otherwise
        const bool haveTimeOfDay = todIt != j.end() && todIt->is_object();
        const bool haveSun = dlIt != j.end() && dlIt->is_object();
        if (haveTimeOfDay)
        {
            TimeOfDaySettingsJson::ApplyOverrides(*todIt, timeOfDay, sun);
            if (migration.sunConverted)
            {
                LOG_INFO(logging::LogCategory::Scene,
                         "level light: directionalLight -> timeOfDay (time {:.4f} h, path yaw {:.3f}, tilt {:.2f})",
                         timeOfDay.timeHours, timeOfDay.pathYawDeg, timeOfDay.pathTiltDeg);
            }
        }
        else if (haveSun)
        {
            DirectionalLight dirLight;
            dirLight.SetDirection(DirectionalLightJson::Direction(*dlIt, float3(0.0f, -1.0f, 0.0f)));
            DirectionalLightJson::ApplyOverrides(*dlIt, dirLight);
            scene.SetDirectionalLight(dirLight);
        }
        scene.SetTimeOfDay(timeOfDay, sun);
    }

    // P1: photographic camera settings. A level without the section keeps the struct defaults,
    // whose `enabled = false` reproduces the pre-plan image exactly -- that is the whole
    // compatibility story for old levels, so there is deliberately no fallback to the directional
    // light's legacy `exposure` here. P4 is where those two meet.
    {
        // R9.4: the camera over its defaults. R9.5: the P3B local-exposure keys an older level still carries in its
        // colour pipeline were moved onto the camera by levelmigrate::Migrate (one the camera owns wins).
        levelenv::ApplyCameraExposure(scene, PostProcessGroup(j, "cameraExposure"));
        // Plan section 6.4: a level load invalidates the adapted value. Without this the camera
        // would spend the first seconds of a new level adapting down from the previous one's
        // brightness, which is exactly the "spend seconds adapting from stale history" that
        // locked decision 5 rules out.
        renderer.Exposure().RequestReset();
    }

    // P6B: screen-space AO. A level without the section keeps the struct defaults, whose
    // `enabled = false` reproduces the pre-P6B image exactly.
    {
        levelenv::ApplyGtao(scene, PostProcessGroup(j, "gtao")); // R9.4

        levelenv::ApplyHeightFog(scene, PostProcessGroup(j, "heightFog")); // R9.5: "atmosphere" migrated above

        // P8: a level without the section gets the struct defaults, i.e. bloom OFF.
        levelenv::ApplyBloom(scene, PostProcessGroup(j, "bloom"));
    }

    // P3: display transform. A level without the section gets the struct defaults, i.e. AgX.
    {
        levelenv::ApplyColorPipeline(scene, PostProcessGroup(j, "colorPipeline")); // R9.4
    }

    ctx.scene.SetLevelPlayerStart(std::nullopt);
    game::RaceCourse raceCourse;
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
            // The race: a mark of a gate ("raceGate"), a boat a bot helms ("raceBot") -- whatever the object is.
            if (enabled) { game::ReadRaceKeys(o, ToFloat3(o.value("position", json::array()), float3(0.0f, 0.0f, 0.0f)), raceCourse); }
            if (IsFreeCameraStart(o))
            {
#if WITH_EDITOR
                const Scene::SceneObjectId editorObjectId =
                    ctx.editorDocument
                        ? ctx.editorDocument->LevelObjectId(o)
                        : ReadOrAllocateEditorObjectId(o, nextEditorObjectId);
                if (ctx.editorDocument)
                {
                    ctx.editorDocument->AddLevelObject(editorObjectId, o);
                }
#endif
                if (enabled)
                {
                    freeCameraStart = o;
                }
                continue;
            }

            // The Player Start: no runtime object either, a place the play session starts from. The editor's document holds it, and a build without the editor takes it from the Scene.
            if (type == game::kPlayerStartType)
            {
#if WITH_EDITOR
                if (ctx.editorDocument)
                {
                    const Scene::SceneObjectId startId = ctx.editorDocument->LevelObjectId(o);
                    ctx.editorDocument->AddLevelObject(startId, o);
                }
#endif
                if (enabled && !ctx.scene.LevelPlayerStart())
                {
                    const float3 rotationDeg = ToFloat3(o.value("rotationDeg", json::array()), float3(0.0f, 0.0f, 0.0f));
                    game::PlayerStart start;
                    start.feet = ToFloat3(o.value("position", json::array()), float3(0.0f, 0.0f, 0.0f));
                    start.pitchRad = rotationDeg.x * DEG2RAD;
                    start.yawRad = rotationDeg.y * DEG2RAD;
                    game::ReadPlayerKeys(o, start);
                    ctx.scene.SetLevelPlayerStart(std::move(start));
                }
                continue;
            }

            // A ZONE IS AN EDITOR OBJECT WITH NO RUNTIME SIDE -- a named region used to say
            // WHERE, exactly as `freeCameraStart` is a named pose. The runtime object
            // registry has never heard of it and is not supposed to, so it took the
            // "unknown type" branch: a level with a zone drawn into it fired "Unknown object
            // type in level JSON" on load in Debug and did not open.
            //
            // It is handed to the EDITOR DOCUMENT first and only then skipped. The plain
            // `continue` that this replaces looked right and was worse than the assert: the
            // document add happens further down, so the zone would have vanished from the
            // outliner and then out of the file itself at the next save.
            if (type == "zone")
            {
#if WITH_EDITOR
                if (ctx.editorDocument)
                {
                    const Scene::SceneObjectId zoneId = ctx.editorDocument->LevelObjectId(o);
                    ctx.editorDocument->AddLevelObject(zoneId, o);
                }
#endif
                continue;
            }

            const bool objectTypeRegistered = objectRegistry.Has(type);
            if (!objectTypeRegistered)
            {
                assert(false && "Unknown object type in level JSON");
                continue;
            }
#if WITH_EDITOR
            const Scene::SceneObjectId editorObjectId =
                ctx.editorDocument
                    ? ctx.editorDocument->LevelObjectId(o)
                    : ReadOrAllocateEditorObjectId(o, nextEditorObjectId);
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
                , editorObjectId, enabled
#endif
            );
#if WITH_EDITOR
            if (ctx.editorDocument)
            {
                ctx.editorDocument->AddLevelObject(editorObjectId, o);
            }
#endif
        }

    }

    raceCourse.level = sourcePath_;
    ctx.scene.SetLevelRaceCourse(std::move(raceCourse));

#if WITH_EDITOR
    if (ctx.editorDocument)
    {
        // Build environment entities after the object loop so their allocated ids
        // never collide with object ids (see EditorSceneDocument).
        ctx.editorDocument->EndLevel();
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
        camera.SetYawPitchRoll(0.0f, 0.0f, 0.0f); // and level: a level without one does not inherit the last level's turn
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

    // The bodies of everything this level built, in the load instead of its first frame.
    ctx.scene.Physics().LoadFinished();
}

void JsonLevel::Unload(const LevelLoadContext& ctx)
{
    Level::Unload(ctx);
    Systems::DestroyOceanSimulation();
}
