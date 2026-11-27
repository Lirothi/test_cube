#include "app/scene/Scene.h"
#include "core/logging/Log.h"
#include "vfx/ParticleTypes.h" // the spray's runtime ceilings (vfx::g_sprayRateMax / g_sprayMistMax)

#include <memory>
#include <algorithm>
#include <array>
#include <cmath>
#include <cassert>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "rendering/view/Camera.h"
#include "app/Systems.h"
#include "core/Helpers.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderConstants.h" // g_cleanCapture
#include "rendering/core/UploadBatch.h"
#include "rendering/core/GpuIdlePoint.h" // R8.1: the editor's caster refresh waits once
#include "rendering/renderables/RuntimeObjects.h" // R13.1: the hand-over's road for objects made and taken out
#include "ocean/BuoyancyLayout.h" // the boats' hull footprints, for their wake
#include "ocean/OceanSimulation.h"
#include "ocean/OceanRenderable.h"
#include "core/task/TaskSystem.h"
#include "core/threading/ThreadRole.h" // game thread plan: who may touch what, and when
#include "core/threading/Knob.h"       // G5: the knobs cross at the hand-over
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"

void Scene::InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    render_.InitializeCommonResources(renderer, uploadCmdList, uploadKeepAlive);
}

void Scene::FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    render_.BeginLevelLoad(renderer, uploadCmdList, uploadKeepAlive); // the scene renderer's level resources
    SyncObjectsForRender(SceneObjectSyncReason::LevelLoad);
    // Local-light plan S9e: a fresh level starts from the initial shadow capacity -- the last level's lights do not keep
    // their view buffers resident; its own selection grows it again at the first ReconcileShadowMode. GPU idle here, and
    // the caster Rebuild below makes the cull's rows for it.
    lightManager_.ForgetWantedShadowed();
    lightManager_.SyncToRender(); // G3: the level's lights as loaded, before anything builds from them
    // The shadow capacity, the casters' buffers and the mega buffer, VSM's pool (RenderScene::FinishLevelLoad). R6.4a:
    // VSM's part now runs before the version bump below, which only drops the render half's queues.
    render_.FinishLevelLoad(renderer, uploadCmdList);
    BumpStaticSetVersion(); // Step 11: a fresh level = a new static caster set
}

void Scene::SyncObjectsForRender(SceneObjectSyncReason reason)
{
    for (const auto& obj : objects_)
    {
        if (obj)
        {
            obj->SyncSceneState(reason);
        }
    }
}

void Scene::StaticSetChanged()
{
    render_.ApplyStaticSet(staticSetVersion_); // R6.4b: World::BumpStaticSetVersion, the render half parked or at Sync
}

#if WITH_EDITOR
void Scene::RefreshShadowGpuForEditor(Renderer& renderer)
{
    // An editor spawn/delete changed the caster set, so ShadowGpuData's next UpdateForFrame will
    // Rebuild and drop megaReady_ — but nothing rebuilds the consolidated mega VB/IB mid-game (it is
    // only built at level load). Without it, VirtualShadowMap::RecordPageRender falls back to per-group
    // binding: 1024 pool pages × mesh-groups × (bind VB/IB + ExecuteIndirect) → ~10ms CPU, forever.
    // Game thread plan G4b': park the render half first, then hand it the edit this refresh is for (an object shown,
    // hidden or switched out of the shadows) -- the rebuild reads the render half's switches.
    // R8.1: one GPU-idle point for both (the park here and the rebuild's wait were two waits in a row).
    GpuIdlePoint idle(renderer);
    idle.Ensure();
    for (const std::unique_ptr<RenderableObjectBase>& obj : objects_)
    {
        if (obj) { obj->SyncSwitchesToRender(); }
    }
    if (render_.StaticSetVersion() != staticSetVersion_) { render_.ApplyStaticSet(staticSetVersion_); }
    render_.RebuildShadowCasters(renderer, idle);
}
#endif

namespace
{
std::uint64_t FrozenHashBytes(const void* data, std::size_t size, std::uint64_t h = 1469598103934665603ull)
{
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i)
    {
        h = (h ^ p[i]) * 1099511628211ull; // FNV-1a
    }
    return h;
}
} // namespace

void Scene::HashFrozenState(FrozenStateHash& out) const
{
    out.hashes.clear();
    out.objects = objects_.size();
    out.pointLights = lightManager_.PointLights().size();
    out.spotLights = lightManager_.SpotLights().size();
    out.hashes.reserve(out.objects * kFrozenObjectFields + out.pointLights + out.spotLights);
    for (const auto& obj : objects_)
    {
        // R7.1: the object's render state whole, field by field (kObjectRenderStateFields) -- the move flag too, which the
        // five hashes before it left out.
        const unsigned char* state = obj ? reinterpret_cast<const unsigned char*>(&obj->RenderState()) : nullptr;
        for (const ObjectRenderStateField& field : kObjectRenderStateFields)
        {
            out.hashes.push_back(state ? FrozenHashBytes(state + field.offset, field.size) : 0u);
        }
    }
    for (const PointLight& light : lightManager_.PointLights())
    {
        const PointLightDesc& d = light.GetRenderDesc();
        std::uint64_t h = FrozenHashBytes(&d.position, sizeof(d.position));
        h = FrozenHashBytes(&d.radius, sizeof(d.radius), h);
        h = FrozenHashBytes(&d.color, sizeof(d.color), h);
        out.hashes.push_back(FrozenHashBytes(&d.luminousFluxLm, sizeof(d.luminousFluxLm), h));
    }
    for (const SpotLight& light : lightManager_.SpotLights())
    {
        const SpotLightDesc& d = light.GetRenderDesc();
        const float flux = light.EffectiveFluxLm();
        std::uint64_t h = FrozenHashBytes(&d.position, sizeof(d.position));
        h = FrozenHashBytes(&d.range, sizeof(d.range), h);
        h = FrozenHashBytes(&d.color, sizeof(d.color), h);
        out.hashes.push_back(FrozenHashBytes(&flux, sizeof(flux), h));
    }
}

std::string Scene::FrozenFieldName(const FrozenStateHash& layout, std::size_t index) const
{
    const std::size_t objectHashes = layout.objects * kFrozenObjectFields;
    if (index < objectHashes)
    {
        const std::size_t object = index / kFrozenObjectFields;
        std::string name = "object #" + std::to_string(object);
#if WITH_EDITOR
        if (object < objectIds_.size() && objectIds_[object] != 0u)
        {
            name += " (editor id " + std::to_string(objectIds_[object]) + ")";
        }
#endif
        return name + " " + kObjectRenderStateFields[index % kFrozenObjectFields].name; // R7.1
    }
    index -= objectHashes;
    if (index < layout.pointLights)
    {
        return "point light #" + std::to_string(index) + " render desc";
    }
    return "spot light #" + std::to_string(index - layout.pointLights) + " render desc";
}

bool Scene::ScribbleFrozenStateForAudit()
{
    for (const auto& obj : objects_)
    {
        RenderableObject* r = obj ? obj->As<RenderableObject>() : nullptr;
        if (r)
        {
            // R7.1: every field of the frozen copy in turn (ObjectRenderState), not the model matrix alone -- the audit
            // must name each one it sees moved.
            r->ScribbleRenderStateForAudit(auditScribbleField_++);
            return true;
        }
    }
    return false;
}

namespace
{
// Architecture plan R13.1: the hand-over's road for objects made and taken out while the world runs (RuntimeObjects.h).
// Every object made in one hand-over goes into one upload batch, submitted and waited for once (Flush), before anything
// draws them -- the race's grid had a batch of its own and the spray one for every boat.
class HandOverObjects final : public RuntimeObjects
{
public:
    // R13.3: `journal` / `rollBack` -- the session's journal (Scene::OpenSessionJournal): a session object made while
    // `record` is written into the first, and whatever goes is crossed out of both.
    HandOverObjects(World& world, Renderer* renderer, std::vector<const RenderableObjectBase*>& journal,
                    std::vector<const RenderableObjectBase*>& rollBack, bool record)
        : world_(world), renderer_(renderer), journal_(journal), rollBack_(rollBack), record_(record) {}
    HandOverObjects(const HandOverObjects&) = delete;
    HandOverObjects& operator=(const HandOverObjects&) = delete;
    ~HandOverObjects() { Flush(); }

    RenderableObjectBase* Spawn(std::unique_ptr<RenderableObjectBase> object, Lifetime lifetime) override
    {
        if (!renderer_ || !object || flushed_) { return nullptr; }
        if (!begun_)
        {
            if (failed_ || !uploads_.Begin(renderer_)) { failed_ = true; return nullptr; }
            begun_ = true;
        }
        RenderableObjectBase* raw = object.get();
        if (!world_.AddInitializedObject(*renderer_, uploads_, std::move(object))) { return nullptr; }
        if (lifetime == Lifetime::Session && record_) { journal_.push_back(raw); }
        return raw;
    }

    std::size_t Despawn(const std::vector<const RenderableObjectBase*>& gone) override
    {
        // As the race's own road did: no renderer (a tool's scene), nothing goes.
        if (!renderer_ || gone.empty()) { return 0; }
        for (const RenderableObjectBase* object : gone)
        {
            std::erase(journal_, object); // R13.3: gone by the road -- nothing for the journal to take out
            std::erase(rollBack_, object);
        }
        return world_.RemoveRuntimeObjects(*renderer_, gone);
    }

    // The hand-over's objects to the GPU, waited for: once, after the last one that may come (the spray's).
    void Flush()
    {
        if (begun_) { uploads_.SubmitAndWait(renderer_); }
        begun_ = false;
        flushed_ = true;
    }

private:
    World& world_;
    Renderer* renderer_ = nullptr;
    std::vector<const RenderableObjectBase*>& journal_;
    std::vector<const RenderableObjectBase*>& rollBack_;
    bool record_ = false;
    UploadBatch uploads_;
    bool begun_ = false;
    bool failed_ = false;
    bool flushed_ = false;
};
} // namespace

// Game thread plan G1: the frame's values that come from GAME state -- the level's settings, the time of day's
// tints and night camera, the sun's transmittance, the selection -- copied into frameData_ at the hand-over, where
// nothing else runs (docs/game_thread_plan.md section 3.3). PrepareViews publishes the render side's own pointers.
void Scene::Sync(Renderer* renderer)
{
    TC_CHECK_SYNC("Scene::Sync");
    CPU_SCOPE(ProfilerScopes::kSceneSync);
    threading::ClearRenderParked(); // the render half runs again after this
    render_.syncedThisFrame_ = true;
    render_.oceanSimulation_ = Systems::GetOceanSimulation(); // R6.4a: PrepareViews' (it read Systems there)
    knob::Exchange(); // G5: the knobs the game half set (and the ones the render half consumed) cross here
    if (renderer) { renderer->FlushHandOverSubmits(); } // G7: the game's GPU work (thumbnails, previews) to the queue

    // G2: the camera. The render half takes the game camera's pose (and a cut, by its revision); the game camera's own
    // matrices are refreshed for the pose about to render -- the gizmo, picking and the HUD read them next frame, as
    // they read the frame's CalcMatrices before.
    physics_.SyncPoses(); // what the step found while this thread waited for the frame before; the loop below takes them
    // The race's own boats (game::Race): made for a race where its grid is, gone when it is over -- here, the render half parked.
    HandOverObjects runtimeObjects(*this, renderer, sessionSpawned_, sessionRollBack_, sessionJournalOpen_); // R13.1, R13.3
    if (gameHandOver_) { gameHandOver_(renderer, runtimeObjects); } // R6.2a: the game's part (game::PlayWorld::HandOver)
    // R13.3: a session that stopped -- after its systems took out their own (just above), what it made and left goes.
    if (!sessionRollBack_.empty())
    {
        const std::vector<const RenderableObjectBase*> left = std::exchange(sessionRollBack_, {});
        const std::size_t removed = runtimeObjects.Despawn(left);
        LOG_INFO(logging::LogCategory::Scene, "session: {} object(s) made in Play and left in the world taken out at Stop", removed);
    }
    // The boats' wake on the water (ocean/OceanWake): every hull as this frame draws it, with its footprint (resolved from
    // its geometry once a shape, then a cache hit), to the ocean's render half -- parked here.
    if (OceanRenderable* ocean = FindOceanRenderable())
    {
        vessels_.CollectHulls(vesselHulls_);
        // Their spray (vfx/VesselSpray): emitters made on a boat's first frame, placed and driven here, before the
        // objects' SyncSceneState below takes their positions and rates; where each hull's bow wave breaks goes to the
        // wake below (its foam arms).
        vfx::SprayParams spray;
        if (const OceanSimulation* sim = ocean->GetSimulation())
        {
            const OceanRenderConfig& cfg = sim->GetRenderConfig();
            spray.enabled = cfg.sprayEnabled;
            spray.rate = std::min(cfg.sprayRate, vfx::g_sprayRateMax); // the Effects group's ceilings (docs/scalability_plan.md)
            spray.mist = std::min(cfg.sprayMist, vfx::g_sprayMistMax);
            spray.out = cfg.sprayOut;
            spray.up = cfg.sprayUp;
            spray.forward = cfg.sprayForward;
            spray.slam = cfg.spraySlam;
            // Over the water foam's tint: the drops are the same white water (the Inspector's Foam Tint moves both).
            spray.color = Math::float3(cfg.sprayColor.x * cfg.foamTint.x, cfg.sprayColor.y * cfg.foamTint.y,
                                       cfg.sprayColor.z * cfg.foamTint.z);
            spray.foamLight = cfg.sprayFoamLight;
            spray.forwardScatter = cfg.sprayForwardScatter;
            spray.opacity = cfg.sprayOpacity;
            spray.softFade = cfg.spraySoftFade;
        }
        vesselSpray_.Sync(runtimeObjects, vesselHulls_, spray);
        std::vector<ocean::WakeHull> hulls;
        hulls.reserve(vesselHulls_.size());
        for (const physics::VesselHull& vessel : vesselHulls_)
        {
            ocean::WakeHull hull;
            hull.position = Math::float2(vessel.position.x, vessel.position.z);
            hull.forward = Math::float2(vessel.forward.x, vessel.forward.z);
            hull.scale = 0.5f * (vessel.scale.x + vessel.scale.z);
            // The asset's waterline: VesselSettings::draft is where still water stands on the mesh, over its origin.
            hull.footprint = buoyancy::ResolveFootprint(vessel.object->GetGeometryPath(),
                Math::float2(vessel.settings->forward.x, vessel.settings->forward.z), vessel.settings->draft);
            // Its breaking bow wave (the wake's two foam arms): the spray's root on each side and its share.
            hull.id = vessel.object;
            hull.speed = vessel.speed;
            vesselSpray_.BowWaveOf(vessel.object, hull.bowRoot, hull.bowShare, hull.length);
            if (hull.footprint)
            {
                hulls.push_back(std::move(hull));
            }
        }
        ocean->SetWakeHulls(std::move(hulls));
        // The hulls the water stays out of (ocean/WaterMask): every marked mesh at the pose this frame draws it. Collected
        // whatever ocean.waterMask says (the surface's switch): the particles collide with the same columns.
        std::vector<ocean::WaterMaskInstance> masks;
        ocean::watermask::Collect(masks);
        ocean->SetWaterMasks(std::move(masks));
    }
    runtimeObjects.Flush(); // R13.1: what the game and the spray made this hand-over, to the GPU once, before anything draws it
    render_.renderCameraCut_ |= render_.renderCamera_.TakePose(camera_);
    if (renderer) { camera_.CalcMatrices(renderer); }
    // G4: every object's transform, bounds and move flag (SyncSceneState -- the step PostTick used to run), its
    // material params and the render-facing animation of emitters, spinners and the ocean, and its switches.
    {
        CPU_SCOPE(ProfilerScopes::kSceneSyncObjects);
        TaskSystem::ParallelFor(objects_.size(),
            [this](size_t index) {
                if (index >= objects_.size() || !objects_[index]) { return; }
                objects_[index]->SyncSceneState(SceneObjectSyncReason::Frame);
                objects_[index]->SyncSwitchesToRender();
                // The sky occlusion map's (SkyOcclusionMap): a static caster moved (an editor drag) -- not a mover
                // that animates or rides a render offset (physics, buoyancy), which would redraw it every frame.
                if (const RenderableObject* ro = objects_[index]->As<RenderableObject>();
                    ro && ro->MovedThisFrame() && !ro->HasRenderOffset() && !ro->IsDynamicCaster())
                {
                    render_.staticCasterMoved_.store(true, std::memory_order_relaxed);
                }
            }, 64);
    }
    if (render_.StaticSetVersion() != staticSetVersion_) { render_.ApplyStaticSet(staticSetVersion_); }
    if (renderer) { buoyancy_.SyncReadback(FindOceanRenderable(), *renderer); }
    // W8: the wind's fade origin is the CAMERA, shared by the G-buffer and every shadow view; and the drift push of the
    // GPU particle emitters. Read by the recording and by the emitters' tick (which ran before and reads last frame's,
    // as it always did).
    vfx::g_windFadeOriginWS = camera_.GetPosition();
    vfx::g_windDriftXZ = Math::float2(
        windState_.windDirXZ.x * windState_.strength * windState_.gustMul,
        windState_.windDirXZ.y * windState_.strength * windState_.gustMul);
    // G3: the lights' game descs (the editor's edits, the night lamps' levels, the candles' flicker) to the render half.
    lightManager_.SyncToRender();

    render_.frameData_.skyMoon = timeOfDay_.Present() ? timeOfDay_.GetSkyMoon() : SkyMoonFrame{};
    render_.frameData_.nightSky = timeOfDay_.Present() ? timeOfDay_.GetNightSky() : NightSkyFrame{};
    render_.frameData_.udsSky = timeOfDay_.Present() ? timeOfDay_.UdsSky() : UdsSkyFrameData{};
    // The stars twinkle on the scene's wind clock, so --wind-freeze stills them with everything else.
    render_.frameData_.nightSky.timeSeconds = windState_.time;
    render_.frameData_.settings = renderSettings_;
    // The atmosphere's authoritative copy is `heightFog_` (HeightFogRef: level load, Inspector,
    // --set). SetRenderSettings mirrors it into renderSettings_, but nothing guarantees a caller
    // between an HeightFogRef edit and this frame -- the scene-stress harness has none, and its
    // GBV gate ran with `volumetric` applied yet never rendered (volumetric fog plan A). The frame
    // takes the source directly.
    render_.frameData_.settings.heightFog = heightFog_;
    render_.frameData_.settings.skyAtmosphere = skyAtmosphere_;
    // Ultra Dynamic Sky's night Rayleigh and twilight ozone (TimeOfDaySettings, "twilight"), on the FRAME's
    // atmosphere: the Inspector, --set and the level file keep the scales as written.
    if (timeOfDay_.Present())
    {
        const float* rayleigh = timeOfDay_.RayleighTint();
        const float* ozone = timeOfDay_.OzoneTint();
        // ...and the night's aerosol (TimeOfDaySettings::mieNightScale): scattering and absorption, the anisotropy (.w)
        // kept.
        const float mie = timeOfDay_.MieScale();
        for (int c = 0; c < 3; ++c)
        {
            render_.frameData_.settings.skyAtmosphere.parameters.RayleighScattering[c] *= rayleigh[c];
            render_.frameData_.settings.skyAtmosphere.parameters.AbsorptionExtinction[c] *= ozone[c];
            render_.frameData_.settings.skyAtmosphere.parameters.MieScattering[c] *= mie;
            render_.frameData_.settings.skyAtmosphere.parameters.MieAbsorption[c] *= mie;
        }
        // ...and the day sky's brightness (TimeOfDaySettings::daySkyBrightness), the same way.
        render_.frameData_.settings.skyAtmosphere.luminanceScale *= timeOfDay_.SkyLuminanceScale();
        // ...and the height fog's density by the sun's height (TimeOfDaySettings::fogDayDensity ..), the frame's copy.
        render_.frameData_.settings.heightFog.density *= timeOfDay_.FogDensityScale();
    }
    // The sky's light in the height fog (UE's SkyAtmosphere HeightFogContribution) and Ultra Dynamic Sky's fog colour by
    // the sun's height (TimeOfDaySettings::fogUdsColor), on the frame's copy: HeightFogSettings' frame-only fields.
    {
        HeightFogSettings& fog = render_.frameData_.settings.heightFog;
        fog.frameSkyContribution = std::max(skyAtmosphere_.heightFogContribution, 0.0f);
        fog.frameSunScatterTint = float3(1.0f, 1.0f, 1.0f);
        fog.frameInscatterFloor = float3(0.0f, 0.0f, 0.0f);
        if (timeOfDay_.Present())
        {
            const float* tint = timeOfDay_.FogSunScatterTint();
            const float* lift = timeOfDay_.FogInscatterFloor();
            fog.frameSkyContribution *= timeOfDay_.FogSkyContribution();
            fog.frameSunScatterTint = float3(tint[0], tint[1], tint[2]);
            fog.frameInscatterFloor = float3(lift[0], lift[1], lift[2]);
        }
    }
    render_.frameData_.settings.volumetricCloud = volumetricCloud_;
    // B6.2: THE SUN'S TRANSMITTANCE, here because this is where the frame's light is still mutable
    // and because every consumer downstream reads it through GetEffectiveColor(). UE do the same
    // thing in PrepareSunLightProxy and hand it to the light proxy, which each lighting path then
    // multiplies in (LightGridInjection.cpp:1064, SceneRendering.cpp:1477); their fallback with no
    // atmosphere is white (SkyAtmosphereRendering.cpp:610), which is our HDRI mode, so those levels
    // do not move by a bit.
    //
    // Without it the sun keeps its full outer-space illuminance at any elevation and only N.L dims
    // it -- 4166 lux on horizontal ground at a 2.8 degree sun, and never reddening. The sky's own
    // LUTs must NOT get this: they integrate the extinction themselves, which is why the light
    // exposes GetOuterSpaceIlluminance() separately and MakeSkyView takes that one.
    {
        const auto d = -dirLight_.GetDirection();
        const float len = std::sqrt(std::max(1.e-12f, d.x * d.x + d.y * d.y + d.z * d.z));
        const float elevation = std::asin(std::clamp(d.y / len, -1.0f, 1.0f));
        // THE SUN SETS (time-of-day plan T0), in either sky mode: the share of its disc still above the
        // sea-level horizon, linear across the disc's angular diameter. Every shipped level's sun stands
        // 13-65 degrees up, so this is 1 for all of them; it matters once the sun moves -- before it, a
        // cubemap level kept a full white sun at any depth below the horizon and the procedural one
        // relied on the transmittance march's accident (now an explicit planet test).
        //
        // The atmosphere is crossed by the part of the disc that is still UP: once the centre is down,
        // the ray to it meets the planet and its transmittance is 0, though the disc's top still shines
        // until the whole disc is under (T2: that is where the time of day hands the light to the moon,
        // so there is no stretch in which neither lights). So it is taken at the middle of the visible
        // part -- between the horizon and the disc's top -- which is the centre itself once the disc is
        // wholly up.
        const float radius = std::max(dirLight_.GetSunHalfApexRadians(), 1.0e-4f);
        const float above = std::clamp(0.5f + 0.5f * elevation / radius, 0.0f, 1.0f);
        Math::float3 transmittance{1.0f, 1.0f, 1.0f};
        if (skyAtmosphere_.mode != 0u)
        {
            const float throughElevation = elevation >= radius ? elevation : std::max(elevation, 0.5f * (elevation + radius));
            float t[3] = {1.0f, 1.0f, 1.0f};
            SkyTransmittanceTowardSun(render_.frameData_.settings.skyAtmosphere.parameters, throughElevation, t);
            transmittance = Math::float3(t[0], t[1], t[2]);
        }
        transmittance = transmittance * above;
        dirLight_.SetAtmosphereTransmittance(transmittance);
        // The overcast deck takes the sun off the ground (the clouds still get it, GetAboveCloudsColor).
        // The clouds need the procedural sky: on a cubemap sky their pass does not run, so nor does this.
        dirLight_.SetCloudVisibility(skyAtmosphere_.mode != 0u ? CloudSunGroundVisibility(volumetricCloud_) : 1.0f);
    }
    render_.frameData_.cameraExposure = cameraExposure_;
    // The owner's night exposure (TimeOfDaySettings::nightExposureEv), on the FRAME's camera after the curve (the
    // curve stays keyed on the scene's own EV), by the frame's open-sky share like the steady camera below: the open
    // night darker, a closed room as by day (exposure_solve_cs.hlsl).
    if (timeOfDay_.Present())
    {
        render_.frameData_.cameraExposure.nightExposureEv = timeOfDay_.NightExposureEv();
        render_.frameData_.cameraExposure.nightOpenSkyFrom = timeOfDay_.Settings().nightCameraOpenFrom;
        render_.frameData_.cameraExposure.nightOpenSkyFull = timeOfDay_.Settings().nightCameraOpenFull;
        render_.frameData_.cameraExposure.nightIndoorLift = timeOfDay_.Settings().nightCameraIndoorLift;
    }
    // ...and the steady night camera (TimeOfDaySettings::nightCameraEv100), by the same night share. Its EV is the
    // FULL night's (the light at x nightBrightness); the night's light comes in by TimeOfDay::NightGain, so the
    // camera is held that many stops under it until then -- held at 13.13 while the moon was still x4310, the sun
    // 10.8 degrees down was 1.4 stops darker than both the dusk and the night (2026-10-31).
    if (timeOfDay_.Present() && timeOfDay_.Settings().nightCameraEv100 != 0.0f)
    {
        // nightCameraEv100 is the camera for the full moon 45 degrees up; another night's light moves it as the exposure
        // compensation CURVE says, read at that light's PHYSICAL EV (TimeOfDay::NightEv100: middle grey under the moon,
        // the twilight and the night sky, before Night Brightness -- -3.3 full moon, -6.9 quarter, -11 none): the camera
        // follows the light less the curve's change, (1 - slope) of it, as the auto exposure follows a scene. The
        // physical key keeps the night's section of the curve under the day's EVs: with Night Brightness in it a
        // quarter-moon beach (eye 7.56) and wind_test's bathroom by day (6.74) sat on the same point (owner, 2026-11-01:
        // "we have a special correction curve for the night, edit it"). Steady all the same: computed from the sky's
        // bodies, not the meter, so a lamp or a turn of the view does not move it. A curve flat there = the light in full.
        const float lightEv = timeOfDay_.NightLightEv();
        const float curveShift = render::EvaluateExposureCurve(render_.frameData_.cameraExposure, timeOfDay_.NightEv100()) -
                                 render::EvaluateExposureCurve(render_.frameData_.cameraExposure, timeOfDay_.NightRefEv100());
        render_.frameData_.cameraExposure.nightSteadyEv100 = timeOfDay_.Settings().nightCameraEv100 +
            std::log2(timeOfDay_.NightGain() / std::max(timeOfDay_.Settings().nightBrightness, 1.0e-6f)) +
            lightEv - curveShift;
        render_.frameData_.cameraExposure.nightSteadyFollow = timeOfDay_.Settings().nightCameraFollow;
        render_.frameData_.cameraExposure.nightSteadyWeight = timeOfDay_.NightShare();
    }
    render_.frameData_.colorPipeline = colorPipeline_;
#if WITH_EDITOR
    render_.renderSelection_ = selectedEditorObjectIds_; // the outline and the LOD view draw THIS frame's selection
    render_.frameData_.selectedEditorObjectIds = render_.renderSelection_.empty() ? nullptr : render_.renderSelection_.data();
    // A clean MCP capture draws no selection outline (render::g_cleanCapture).
    render_.frameData_.selectedEditorObjectCount = render::g_cleanCapture.load(std::memory_order_relaxed)
        ? 0u : static_cast<std::uint32_t>(render_.renderSelection_.size());
    render_.frameData_.selectionOutlineRadius = std::clamp<std::uint32_t>(selectionOutlineRadius_, 1u, 8u);
#else
    render_.frameData_.selectedEditorObjectIds = nullptr;
    render_.frameData_.selectedEditorObjectCount = 0;
    render_.frameData_.selectionOutlineRadius = 1;
#endif

    // G2: the render half's copies of the game's sun, sky sun, wind and cascade knobs -- after the transmittance above
    // went into the game light (the Mesh Editor's preview reads its effective colour), so the copy carries it too. With
    // no time of day the sky is lit by the scene light itself, as before: the same copy.
    render_.renderSun_ = dirLight_;
    if (timeOfDay_.Present()) { render_.renderSkySun_ = skySun_; }
    render_.frameData_.dirLight = &render_.renderSun_;
    render_.frameData_.skyLight = timeOfDay_.Present() ? &render_.renderSkySun_ : &render_.renderSun_;
    render_.frameData_.sunGlintScale = timeOfDay_.Present() ? timeOfDay_.GlintScale() : 1.0f;
    render_.renderWind_ = windState_;
    render_.frameData_.wind = &render_.renderWind_; // W3: gbuffer per-view CB reads this
    render_.renderCascadeConfig_ = cascadeConfig_;
    render_.frameData_.cascadeConfig = &render_.renderCascadeConfig_; // S8: a Developer Controls edit reaches the next frame
}

void Scene::Render(Renderer* renderer)
{
    // R6.4a: the render half draws (RenderScene::Render); the hand-over runs here first if the frame loop skipped it.
    if (renderer && !render_.syncedThisFrame_)
    {
        TC_CHECK_RENDER("Scene::Render");
        // Every frame loop calls Sync before Render (App::Run, the scene-stress harness). One that does not would
        // render last frame's settings: say so, once, and hand over here.
        LOG_ERROR_ONCE(logging::LogCategory::Scene, "Scene::Render without Scene::Sync this frame -- synced late");
        threading::RoleScope syncRole(threading::ThreadRole::Sync);
        Sync(renderer);
    }
    render_.Render(renderer);
}

void Scene::Clear()
{
    TC_CHECK_GAME_PARKED_OR_SYNC("Scene::Clear");
    render_.Clear(); // R6.4a: the render half first -- every pointer it holds into the objects below goes
    World::Clear();  // R6.4b: the game half -- the objects, their ids, the lights, the sky
    sessionSpawned_.clear();  // R13.3: the journal's objects went with the level (a session goes on in the next one)
    sessionRollBack_.clear();
}

void Scene::CloseSessionJournal()
{
    // R13.3: Stop. What the session made and its makers have not taken out yet goes at the next hand-over, after the
    // game's own part -- the race queued its boats a moment ago (Race::End) and takes them out there first.
    TC_CHECK_GAME("Scene::CloseSessionJournal");
    sessionJournalOpen_ = false;
    sessionRollBack_.insert(sessionRollBack_.end(), sessionSpawned_.begin(), sessionSpawned_.end());
    sessionSpawned_.clear();
}
