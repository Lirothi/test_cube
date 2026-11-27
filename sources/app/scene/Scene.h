#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "app/scene/World.h" // R6.4b: the game half
#include "rendering/scene/RenderScene.h" // R6.4a: the render half
#include "core/threading/ThreadRole.h"

class Renderer;

// Architecture plan R6: the scene is its game half (World: the objects, the lights, the camera, the clock, the wind, the
// physics -- Tick), its render half (RenderScene: the copies Sync hands it, the views, the shadow data, the scene
// renderer -- Render) and the hand-over between them (Sync), plus what touches both: the level load's GPU-idle end, the
// editor's caster-set refresh, Clear.
class Scene final : public World
{
public:
    // The render half's camera (RenderScene::RenderCamera).
    const Camera& RenderCamera() const { return render_.RenderCamera(); }
    // The render half's sun (RenderScene::RenderSun).
    const DirectionalLight& RenderSun() const { return render_.RenderSun(); }
    const SceneFrameData& FrameData() const { return render_.FrameData(); } // RenderScene::FrameData
    const SceneFrameData* FrameDataAddress() const { return render_.FrameDataAddress(); } // RenderScene::FrameDataAddress

    // R6.4a: the render half's readouts and services (RenderScene says what each is).
    const render::LocalShadowCache& GetLocalShadowCache() const { return render_.GetLocalShadowCache(); }
    const VirtualShadowMap& Vsm() const { return render_.Vsm(); }
    bool IsRtReflectActive() const { return render_.IsRtReflectActive(); }
    void InvalidateRaytracing() { render_.InvalidateRaytracing(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetRtTlasSrv(UINT frameIndex) const { return render_.GetRtTlasSrv(frameIndex); }
    const SceneFrameData::CascadeData& GetCascadeData() const { return render_.GetCascadeData(); }
    const ShadowGpuData& ShadowGpu() const { return render_.ShadowGpu(); }
    const SdsmShadows& Sdsm() const { return render_.Sdsm(); }

    void InitializeCommonResources(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
    void FinalizeLevelLoad(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);
#if WITH_EDITOR
    // Rebuild the shadow-caster GPU data + the consolidated mega VB/IB after an editor caster-set
    // change (spawn/delete). Runs on a fresh GPU-idle upload batch, so the VSM per-page draw keeps its
    // fast single-ExecuteIndirect-per-page path instead of the ~10ms per-group fallback until reload.
    void RefreshShadowGpuForEditor(Renderer& renderer);
#endif

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

    // Both halves: the render half's pointers into the objects first, then the world (World::Clear).
    void Clear();

    // R13.3: the play session's journal. Open (Play starts): every object the hand-over's road makes for the session
    // (RuntimeObjects::Lifetime::Session) is written down, and whatever goes by the road is crossed out. Close (Stop):
    // at the next hand-over, after the game's own part (its systems take theirs out first), what is still written down
    // goes -- the session leaves nothing it made in the world. A level that goes takes the journal's objects with it.
    void OpenSessionJournal()
    {
        TC_CHECK_GAME("Scene::OpenSessionJournal");
        sessionJournalOpen_ = true;
    }
    void CloseSessionJournal();

private:
    void StaticSetChanged() override; // R6.4b: World::BumpStaticSetVersion -> RenderScene::ApplyStaticSet
    void SyncObjectsForRender(SceneObjectSyncReason reason);

    std::vector<physics::VesselHull> vesselHulls_; // Sync's scratch: the boats as this frame draws them, for their wake
    // R13.3: the session's journal -- what the running session made (crossed out as it goes), what a closed one left to
    // take out at the next hand-over.
    std::vector<const RenderableObjectBase*> sessionSpawned_;
    std::vector<const RenderableObjectBase*> sessionRollBack_;
    bool sessionJournalOpen_ = false;

    // R6.4a: the render half -- what Sync hands it and what it keeps to draw (RenderScene.h). Declared after the world's
    // containers it holds references to (World, the base, is made first and goes last).
    RenderScene render_{ objects_,
#if WITH_EDITOR
                         objectIds_,
#endif
                         lightManager_, skyBox_ };
};
