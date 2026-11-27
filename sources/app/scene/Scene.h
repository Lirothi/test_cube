#pragma once

#include <array>
#include <memory>
#include <optional>
#include <atomic>
#include <vector>
#include <wrl/client.h>
#include <cstdint>

#include <functional> // R6.2a: the game's hooks
#include "rendering/renderables/RenderableObject.h"
#include "app/camera/CameraController.h"
#include "rendering/core/PhotographicSettings.h"
#include "rendering/lighting/DirectionalLight.h"
#include "rendering/lighting/Skybox.h"
#include "rendering/lighting/LightManager.h"

#include "rendering/scene/SceneRenderConfig.h"
#include "rendering/scene/RenderScene.h" // R6.4a: the render half
#include "core/threading/ThreadRole.h" // game thread plan: the accessors say which half they belong to
#include "rendering/wind/WindState.h"
#include "vfx/VesselSpray.h"
#include "ocean/OceanBuoyancy.h"
#include "physics/PhysicsWorld.h"
#include "app/scene/TimeOfDay.h"
#include "rendering/lighting/NightLamps.h"

class Renderer;
class UploadBatch;
class OceanRenderable;

class Scene {
public:

    // THE GAME camera (game thread plan G2): input, the editor, MCP, the HUD move and read it. Its matrices are the
    // frame's about to render, refreshed at the hand-over (Sync). The render half has its own (RenderCamera).
    Camera& CameraRef() { TC_CHECK_GAME("Scene::CameraRef"); return camera_; }
    const Camera& CameraRef() const { TC_CHECK_GAME("Scene::CameraRef"); return camera_; }
    // What steers the game camera: the free fly, the look and its smoothing, the wheel's speed (game half only).
    CameraController& CameraControllerRef() { TC_CHECK_GAME("Scene::CameraControllerRef"); return cameraController_; }
    const CameraController& CameraControllerRef() const { TC_CHECK_GAME("Scene::CameraControllerRef"); return cameraController_; }
    // The render half's camera (RenderScene::RenderCamera).
    const Camera& RenderCamera() const { return render_.RenderCamera(); }

    const DirectionalLight& GetDirectionalLight() const { TC_CHECK_GAME("Scene::GetDirectionalLight"); return dirLight_; }
    // The render half's sun (RenderScene::RenderSun).
    const DirectionalLight& RenderSun() const { return render_.RenderSun(); }
    // P4 measurement hook: the sweep harness needs to drive the sun's own controls to measure how
    // much of the level's `exposure` the auto-exposure silently cancels. Runtime only -- like the
    // camera settings above, nothing here writes the level.
    DirectionalLight& DirectionalLightRef() { TC_CHECK_GAME("Scene::DirectionalLightRef"); return dirLight_; }
    // Time-of-day plan T2: the level's clock (TimeOfDay.h). With a `timeOfDay` section it owns the
    // directional light -- every Tick it hands it to the sun or the moon -- and the sky gets its own,
    // always-the-sun light (SceneFrameData::skyLight). Set at level load; the light is placed at once.
    void SetTimeOfDay(const TimeOfDaySettings& settings, const DirectionalLight& sun)
    {
        timeOfDay_.Reset(settings, sun);
        timeOfDay_.Tick(0.0f, dirLight_, skySun_);
    }
    // An edit of the section (T3: the editor applies the Time of Day object through this): the running
    // clock keeps its place unless the edit moved the authored time (TimeOfDay::Apply).
    void ApplyTimeOfDay(const TimeOfDaySettings& settings, const DirectionalLight& sun)
    {
        timeOfDay_.Apply(settings, sun);
        timeOfDay_.Tick(0.0f, dirLight_, skySun_);
    }
    TimeOfDay& TimeOfDayRef() { return timeOfDay_; }
    // THE SUN'S PROPERTIES as the runtime knobs (--set light.* / lightShafts.* / sun.*, the developer
    // window) should edit them: the time of day's sun template when the level has one -- the scene light
    // is rewritten from it every Tick, so an edit of dirLight_ would be undone a frame later -- else the
    // light itself.
    DirectionalLight& SunLightRef()
    {
        TC_CHECK_GAME("Scene::SunLightRef");
        return timeOfDay_.Present() ? timeOfDay_.SunTemplateRef() : dirLight_;
    }
    // Where the sun is (unit, world, TOWARD it), and putting it somewhere: with a time of day that is a
    // time and a path solved to stand the sun there exactly (astro::SolveSimpleTime -- simple positions,
    // the clock left as it was), without one the light's direction.
    Math::float3 GetSunToward() const;
    void SetSunToward(const Math::float3& toward);
    const TimeOfDay& GetTimeOfDay() const { return timeOfDay_; }
    // Plan L: the night lamps' switch (NightLamps.h) -- Night(), and an item's Level -- for the Inspector's read-out.
    const NightLamps& GetNightLamps() const { return nightLamps_; }
    // P1: the level's photographic camera settings. Dormant by default (enabled = false), so this
    // is pure state until P2 schedules the metering passes that read it.
    const render::CameraExposureSettings& GetCameraExposure() const { return cameraExposure_; }
    void SetCameraExposure(const render::CameraExposureSettings& settings) { cameraExposure_ = settings; }
    // Live tuning from the developer window. This is RUNTIME state only -- it does not write the
    // level, so a tuned value has to be copied into the level's cameraExposure section (or the
    // editor inspector) to survive a reload. Same deal as the ocean controls.
    render::CameraExposureSettings& CameraExposureRef() { return cameraExposure_; }

    // P3: the display transform. Same ownership and live-tuning story as the exposure block above.
    const render::ColorPipelineSettings& GetColorPipeline() const { return colorPipeline_; }
    void SetColorPipeline(const render::ColorPipelineSettings& s) { colorPipeline_ = s; }
    render::ColorPipelineSettings& ColorPipelineRef() { return colorPipeline_; }
    LightManager& GetLightManager() { return lightManager_; }
    const LightManager& GetLightManager() const { return lightManager_; }
    Skybox* GetSkybox() const { return skyBox_.get(); }

    // R6.4a: the render half's readouts and services (RenderScene says what each is).
    const render::LocalShadowCache& GetLocalShadowCache() const { return render_.GetLocalShadowCache(); }
    const VirtualShadowMap& Vsm() const { return render_.Vsm(); }
    bool IsRtReflectActive() const { return render_.IsRtReflectActive(); }
    void InvalidateRaytracing() { render_.InvalidateRaytracing(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtTlasSrv(UINT frameIndex) const { return render_.GetRtTlasSrv(frameIndex); }
    const SceneFrameData::CascadeData& GetCascadeData() const { return render_.GetCascadeData(); }
    const ShadowGpuData& ShadowGpu() const { return render_.ShadowGpu(); }
    const SdsmShadows& Sdsm() const { return render_.Sdsm(); }

    // The game's copy (Developer Controls, --set); the render half reads the one Sync took (game thread plan G2).
    CascadeShadowConfig& CascadeConfig() { TC_CHECK_GAME("Scene::CascadeConfig"); return cascadeConfig_; }
    const CascadeShadowConfig& CascadeConfig() const { TC_CHECK_GAME("Scene::CascadeConfig"); return cascadeConfig_; }

    void InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void AddObject(std::unique_ptr<RenderableObjectBase> obj);
    // Occlusion plan S0: union of every renderable's world box except the ocean. For the
    // `scene.replicate` stress grid's step; not a per-frame call.
    AABB ComputeStaticBounds() const;
    bool AddInitializedObject(Renderer& renderer, UploadBatch& uploads, std::unique_ptr<RenderableObjectBase> obj);
    // Runtime objects (no editor identity) out of the scene, at the hand-over or parked (it parks). Their GPU resources go
    // once the frames in flight are done (RetireRemovedObject). How many went.
    std::size_t RemoveRuntimeObjects(Renderer& renderer, const std::vector<const RenderableObjectBase*>& gone);
    // R6.2a: the game's part of the hand-over (game::PlayWorld::HandOver: the race's own boats, the helm's and the
    // spectator's camera) -- Sync calls it right after the physics poses are synced, the render half parked -- and what
    // Clear tells the game (its boats were among the objects). Set by the play session; unset, the scene has no game.
    void SetGameHooks(std::function<void(Renderer*)> handOver, std::function<void()> forget)
    {
        gameHandOver_ = std::move(handOver);
        gameForget_ = std::move(forget);
    }
    bool RemoveOceanObjects();
    void SetOceanVisible(bool visible);
    // W1: locate the ocean's shared clock (null if no ocean). PUBLIC since P16.6 -- the capture
    // harness turns the shore contact foam off through it, which beats editing tuned content.
    OceanRenderable* FindOceanRenderable();
    // What floats and where its pontoons are this frame (the editor's overlay).
    const OceanBuoyancy& Buoyancy() const { return buoyancy_; }
    // The collision world and the rigid bodies (docs/physics_plan.md).
    physics::World& Physics() { return physics_; }
    const physics::World& Physics() const { return physics_; }
    const vfx::VesselSpray& VesselSprayRef() const { return vesselSpray_; } // readouts (written at Sync)

#if WITH_EDITOR
    // Stable identity for editor-spawned objects. SceneObjectId 0 = a runtime
    // object with no editor identity (what AddObject assigns). Editor code uses
    // the same uint64 value for EditorObjectId and SceneObjectId. objectIds_ is
    // kept in lockstep with objects_ on every add/remove/clear.
    using SceneObjectId = std::uint64_t;
    std::size_t ObjectCount() const { return objects_.size(); } // the renderables, editor and level objects alike
    SceneObjectId AddEditorObject(std::unique_ptr<RenderableObjectBase> obj);
    void AddObjectWithEditorId(std::unique_ptr<RenderableObjectBase> obj, SceneObjectId id);
    // `visible` is the editor document's `enabled` and is not optional: a factory builds every
    // object visible, and a caller that respawned a HIDDEN object (a material, mesh or preset
    // change, a Material/Mesh Editor save) showed it again while the document still said hidden.
    bool AddInitializedEditorObject(Renderer& renderer, UploadBatch& uploads, SceneObjectId id,
        std::unique_ptr<RenderableObjectBase> obj, bool visible);
    // Out of the scene now (parked or at the hand-over); its GPU resources once the frames in flight are done
    // (RetireRemovedObject) -- no GPU idle needed before it.
    bool RemoveEditorObject(SceneObjectId id);
    // Rebuild the shadow-caster GPU data + the consolidated mega VB/IB after an editor caster-set
    // change (spawn/delete). Runs on a fresh GPU-idle upload batch, so the VSM per-page draw keeps its
    // fast single-ExecuteIndirect-per-page path instead of the ~10ms per-group fallback until reload.
    void RefreshShadowGpuForEditor(Renderer& renderer);
    // Marks cached render-queue sources stale after an editor visibility/layer change. The command
    // stack coalesces visibility with other caster-set changes into one shadow GPU refresh.
    void NotifyEditorShadowCasterVisibilityChanged() { BumpStaticSetVersion(); }
    RenderableObjectBase* FindEditorObject(SceneObjectId id);
    const RenderableObjectBase* FindEditorObject(SceneObjectId id) const;
    void SetSelectedEditorObjectIds(const std::vector<SceneObjectId>& ids);
    void SetEditorSelectionOutlineRadius(std::uint32_t radius) { selectionOutlineRadius_ = radius; }

    // Nearest editor-owned, visible object hit by the ray. World AABBs provide the
    // broad phase; standalone CPU meshes use exact base-LOD triangles as the narrow
    // phase. Runtime generators and hidden objects are intentionally not editor
    // picking or placement surfaces.
    // `onlyObjectIds`, when given, restricts the cast to those objects. It is not an
    // optimisation dressed as an option: a query about ONE thing should not be answered by
    // whatever else happens to stand in the way. Tracing the island's shoreline was casting
    // 9216 rays at the whole scene, and every one of them walked the triangles of any palm
    // whose bounding box it crossed -- 71.6 seconds on the frame thread, which from outside
    // the editor is indistinguishable from a hang, and a contour that followed the tops of
    // the palms rather than the ground they stand on.
    SceneObjectId RaycastEditorObject(const Math::float3& origin,
        const Math::float3& dir,
        float* outDistance = nullptr,
        SceneObjectId ignoredObjectId = 0,
        const std::vector<SceneObjectId>* ignoredObjectIds = nullptr,
        const std::vector<SceneObjectId>* onlyObjectIds = nullptr) const;
#endif

    void Tick(float deltaTime);
    // Game thread plan (docs/game_thread_plan.md): the hand-over between the game half of the frame and the render
    // half -- the frame's values that come from game state go into frameData_ and the render half's copies here, where
    // nothing else runs. Every frame calls Sync() (role Sync) between the game half and Render() (role Render).
    void Sync(Renderer* renderer);
    void Render(Renderer* renderer);

    // Plan G9, --rt-audit (app/diagnostics/RtAudit.h): what the hand-over froze for the render half, hashed -- per object
    // its model matrix, last frame's, position, visibility and shadow switch (kFrozenObjectFields), then one hash a
    // point light and a spot light (their render descs). Render half (or the hand-over).
    static constexpr std::size_t kFrozenObjectFields = 5;
    struct FrozenStateHash
    {
        std::vector<std::uint64_t> hashes;
        std::size_t objects = 0;
        std::size_t pointLights = 0;
        std::size_t spotLights = 0;
    };
    void HashFrozenState(FrozenStateHash& out) const;
    std::string FrozenFieldName(const FrozenStateHash& layout, std::size_t index) const;
    // --rt-audit-selftest: writes one object's model matrix from the game half while the render thread draws -- what
    // the audit must report. False when the scene has no object to write.
    bool ScribbleFrozenStateForAudit();

    void Clear();

    void SetDirectionalLight(DirectionalLight light);
    void SetSkybox(std::unique_ptr<Skybox> skybox);

    // Render/debug toggles are owned by the app layer (AppController) and pushed
    // here each frame; PrepareViews snapshots them into SceneFrameData.
    // P6B: the AO settings belong to the LEVEL, like cameraExposure and colorPipeline, not to the
    // app's runtime toggles — they describe how this scene should look, and a level has to be able
    // to save them. AppController still owns the rest of SceneRenderSettings and re-pushes it every
    // Tick, so `gtao` is deliberately overwritten from here on the way in: the app's copy is a
    // transport, this is the source of truth.
    const GtaoSettings& GetGtao() const { return gtao_; }
    void SetGtao(const GtaoSettings& s) { gtao_ = s; }
    GtaoSettings& GtaoRef() { return gtao_; }

    // P7: aerial perspective is level-scoped for exactly the reasons above -- it describes how this
    // scene should look, and a level has to be able to save it.
    const HeightFogSettings& GetHeightFog() const { return heightFog_; }
    void SetHeightFog(const HeightFogSettings& s) { heightFog_ = s; }
    HeightFogSettings& HeightFogRef() { return heightFog_; }
    SkyAtmosphereSettings& SkyAtmosphereRef() { return skyAtmosphere_; }
    // B6.2: the level-authored half, replaced wholesale so a field the level does not carry lands on
    // the struct default rather than on whatever the previous level left behind -- the same rule the
    // other look sections load under. The debug views are session state and are preserved.
    void SetSkyAtmosphere(const SkyAtmosphereSettings& s)
    {
        const unsigned aerialView = skyAtmosphere_.aerialDebugView;
        const unsigned lutView = skyAtmosphere_.lutDebugView;
        const bool lutEnabled = skyAtmosphere_.lutEnabled;
        const bool lutValidate = skyAtmosphere_.lutValidate;
        skyAtmosphere_ = s;
        skyAtmosphere_.aerialDebugView = aerialView;
        skyAtmosphere_.lutDebugView = lutView;
        skyAtmosphere_.lutEnabled = lutEnabled;
        skyAtmosphere_.lutValidate = lutValidate;
    }
    // Plan part C: the clouds, the same ownership rule as the sky -- the level-authored half is
    // replaced wholesale, the session's debug view is preserved.
    VolumetricCloudSettings& VolumetricCloudRef() { return volumetricCloud_; }
    void SetVolumetricCloud(const VolumetricCloudSettings& s)
    {
        const unsigned debugView = volumetricCloud_.debugView;
        volumetricCloud_ = s;
        volumetricCloud_.debugView = debugView;
    }

    // P8 bloom. Same ownership rule as the two above: the SCENE copy is the source of truth and
    // SceneRenderSettings is only the per-frame transport, so the dev window, `--set` and the
    // editor all edit this one.
    const BloomSettings& GetBloom() const { return bloom_; }
    void SetBloom(const BloomSettings& s) { bloom_ = s; }
    BloomSettings& BloomRef() { return bloom_; }

    void SetRenderSettings(const SceneRenderSettings& settings)
    {
        renderSettings_ = settings;
        renderSettings_.gtao = gtao_;
        renderSettings_.heightFog = heightFog_;
        renderSettings_.skyAtmosphere = skyAtmosphere_;
        renderSettings_.volumetricCloud = volumetricCloud_;
        renderSettings_.bloom = bloom_;
    }
    const SceneRenderSettings& GetRenderSettings() const { return renderSettings_; }

    const SceneFrameData& FrameData() const { return render_.FrameData(); } // RenderScene::FrameData

    // W1: the global wind source of truth (drives the ocean's force/direction and, from W3 on,
    // foliage sway). Advanced each Tick from the ocean's shared clock; authored by the "wind"
    // level entity (W2).
    vfx::WindState& GetWindState() { TC_CHECK_GAME("Scene::GetWindState"); return windState_; }
    const vfx::WindState& GetWindState() const { TC_CHECK_GAME("Scene::GetWindState"); return windState_; }

    // Rung 1 (Step 11) foundation: monotonic version of the shadow-caster set — bumped when its
    // membership or editor visibility changes. A shadow cache compares it to know its data is
    // stale. Dynamic movers do NOT bump it (their motion is tracked per-caster via
    // RenderableObject::MovedThisFrame).
    std::uint32_t GetStaticSetVersion() const { return staticSetVersion_; }

private:
    // Every queue stores non-owning object pointers. Changing scene membership must invalidate
    // them immediately; waiting for PrepareViews is too late because UpdateCascades reads the
    // previous frame's cascade queues first.
    void BumpStaticSetVersion();
    // A removed object (R16.10): out of the world now (RenderableObjectBase::DetachFromWorld, so the physics world's
    // ForgetRemoved after the removal sees it), destroyed through render::deferred once the GPU has finished the frames
    // that may read its buffers, its mesh's last reference, its textures -- an editor delete or respawn idled the GPU
    // for that until then.
    static void RetireRemovedObject(std::unique_ptr<RenderableObjectBase> object);
    void SyncObjectsForRender(SceneObjectSyncReason reason);

    CascadeShadowConfig cascadeConfig_{};

    std::vector<std::unique_ptr<RenderableObjectBase>> objects_;
#if WITH_EDITOR
    // Lockstep with objects_: objectIds_[i] is the editor id of objects_[i], or
    // 0 for a runtime object with no editor identity.
    std::vector<SceneObjectId> objectIds_;
    SceneObjectId nextEditorId_ = 1;
    // Sorted + deduplicated; no cap. SceneFrameData points straight at it.
    std::vector<SceneObjectId> selectedEditorObjectIds_;
    std::uint32_t selectionOutlineRadius_ = 1;
#endif
    LightManager lightManager_{};
    Camera camera_;       // the GAME camera (CameraRef)
    CameraController cameraController_{camera_}; // its input (CameraControllerRef)

    SceneRenderSettings renderSettings_{};
    std::uint32_t staticSetVersion_ = 0; // Step 11: bumped on shadow-caster membership/visibility changes (the game's)

    vfx::WindState windState_{}; // W1: global wind, advanced each Tick from the ocean's shared clock
    OceanBuoyancy buoyancy_; // floats "buoyant" objects on the ocean (Tick, before PostTick)
    std::function<void(Renderer*)> gameHandOver_; // R6.2a: SetGameHooks
    std::function<void()> gameForget_;
    physics::World physics_; // the bodies (Tick, after the buoyancy: a boat's pose is this frame's)
    std::vector<physics::VesselHull> vesselHulls_; // Sync's scratch: the boats as this frame draws them, for their wake
    vfx::VesselSpray vesselSpray_; // the boats' spray: runtime emitters in objects_, driven at Sync

    DirectionalLight dirLight_;
    TimeOfDay timeOfDay_{};    // T2: owns dirLight_ when the level has a `timeOfDay` section
    DirectionalLight skySun_;  // T2: the sky's sun while the time of day runs (frameData_.skyLight)
    NightLamps nightLamps_{};  // plan L: lights / glowing meshes with "onAt" (TickNightLamps, after the time of day)
    void TickNightLamps(float deltaTime);
    void TickPhysics(float deltaTime);
    render::CameraExposureSettings cameraExposure_{}; // P1, dormant; see PhotographicSettings.h
    render::ColorPipelineSettings colorPipeline_{};   // P3; see PhotographicSettings.h
    GtaoSettings gtao_{};                             // P6B; level-scoped, see GetGtao
    SkyAtmosphereSettings skyAtmosphere_{}; // B1 authoritative settings, including headless harnesses
    VolumetricCloudSettings volumetricCloud_{}; // plan part C; same rule
    HeightFogSettings heightFog_{};                 // P7; level-scoped, see GetHeightFog
    BloomSettings bloom_{};                           // P8; level-scoped, see GetBloom

    std::unique_ptr<Skybox> skyBox_;

    // R6.4a: the render half -- what Sync hands it and what it keeps to draw (RenderScene.h). Declared after the world's
    // containers it holds references to: made after them, gone before them.
    RenderScene render_{ objects_,
#if WITH_EDITOR
                         objectIds_,
#endif
                         lightManager_, skyBox_ };
};
