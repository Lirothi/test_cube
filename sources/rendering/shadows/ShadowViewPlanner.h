#pragma once
// Architecture plan R6.1 (docs/architecture_review_plan.md, section 11.2, wave 3): the planning of the shadow VIEWS, out
// of Scene -- the directional cascades (fitted, snapped, culled against last frame's boxes), VSM's camera-centred clipmap,
// the local lights' views and their cull, and the SDSM partition cache's frame parameters. It owns those views and the
// state the planning keeps between frames; the scene's render half hands it, per call, what it reads (ShadowPlanInputs):
// the render camera, sun and wind, the cascade knobs Sync took, the frame's data, the shadow GPU data and the SDSM object.
// The resources sized by the view capacity (VSM's table, SDSM's buffers, the casters' rows) stay the scene's: the planner
// only says how far the capacity grows and makes its own views for it.
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/math/Frustum.h"
#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h" // render::kShadowSpotsInitial, MaxShadowViews
#include "rendering/renderables/InstanceTypes.h" // render::LocalViewCull
#include "rendering/scene/SceneFrameData.h"
#include "rendering/scene/SceneRenderConfig.h" // CascadeShadowConfig
#include "rendering/scene/SceneView.h"
#include "rendering/shadows/ShadowGpuData.h"
#include "rendering/shadows/ShadowSettings.h"
#include "rendering/shadows/VirtualShadowMap.h"

class Camera;
class DirectionalLight;
class Renderer;
class RenderableObjectBase;
class SdsmShadows;
namespace vfx { struct WindState; }

// What one planning call reads (and the two it writes: the frame's cascade block / SDSM pointer, the SDSM object's
// parameters) -- the scene's render half, built per call (Scene::ShadowInputs).
struct ShadowPlanInputs
{
    SceneFrameData& frame;
    const Camera& camera;     // the render camera (renderCamera_)
    const DirectionalLight& sun; // the render sun
    const vfx::WindState& wind;  // the render wind
    const CascadeShadowConfig& cascadeConfig; // the cascade knobs Sync took
    ShadowGpuData& shadowGpu;
    SdsmShadows& sdsm;
    const std::vector<std::unique_ptr<RenderableObjectBase>>& objects;
    std::uint32_t staticSetVersion; // the render half's caster-set version
    // UpdateCascades' S14 readout may read LAST frame's cascade queues only while the object set and layer mask still
    // match them (the scene's cached queue sources are current).
    bool previousCascadeQueuesCurrent = false;
};

class ShadowViewPlanner
{
public:
    static constexpr int kCascades = SceneFrameData::kCascades;

    void UpdateCascades(const ShadowPlanInputs& in, const Camera& camera, Renderer* renderer);
    void UpdateClipmap(const ShadowPlanInputs& in, const Camera& camera); // Step 24d: camera-centered directional clipmap views (VSM)
    void UpdateSdsmFrameParams(const ShadowPlanInputs& in, Renderer* renderer); // S15: the analysis CB + this frame's SDSM gate

    // Local-light plan S9e: the capacity doubles from `have` until it holds `want`, up to `ceiling` (a memory guard).
    static unsigned GrowCapacity(unsigned have, unsigned want, unsigned ceiling);
    // The local views and their cull rows for this capacity (render::ShadowSpotCapacity / 6 x ShadowPointCapacity) --
    // made anew: a SceneView does not relocate (its render queue's batches are move-only, its move not noexcept); every
    // view is rebuilt each frame by PrepareViews anyway.
    void ResizeLocalViews(unsigned spots, unsigned points);

    std::array<SceneView, kCascades>& Cascades() { return cascadeViews_; }
    const std::array<SceneView, kCascades>& Cascades() const { return cascadeViews_; }
    std::array<SceneView, vsm::kNumClipmapLevels>& ClipmapViews() { return clipmapViews_; }
    const vsm::ClipmapSquares& ClipmapSquares() const { return clipmapSquares_; }
    std::vector<SceneView>& SpotViews() { return spotShadowViews_; }
    const std::vector<SceneView>& SpotViews() const { return spotShadowViews_; }
    std::vector<SceneView>& PointViews() { return pointShadowViews_; }
    const std::vector<SceneView>& PointViews() const { return pointShadowViews_; }
    std::vector<render::LocalViewCull>& LocalViewCull() { return localViewCull_; }
    std::vector<const render::LocalViewCull*>& LocalViewCullPtr() { return localViewCullPtr_; }
    std::vector<const Frustum*>& ViewFrustums() { return shadowViewFrustums_; }

private:
    std::array<SceneView, kCascades> cascadeViews_{};
    // S14 cross-check state: the ortho box and the accurate volume each cascade was culled with
    // LAST frame. UpdateCascades runs before this frame's queues are rebuilt, so the survivors it
    // can inspect were chosen by these planes and no others -- testing them against THIS frame's
    // box reads a camera turn as a cull leak (Debug assert on a fly-through, 2026-09-03).
    std::array<Frustum, kCascades> cascadeBoxPrev_{};
    std::array<Frustum, kCascades> cascadeAccPrev_{};
    std::array<SceneView, vsm::kNumClipmapLevels> clipmapViews_{}; // Step 24d: directional clipmap (VSM)
    // Built beside clipmapViews_ in UpdateClipmap; feeds the clipmap LOD fallback chain. Kept
    // separate from SceneView because it is expressed in the SHARED light frame, which no single
    // level's view matrix carries (each level's own view is centred on itself).
    vsm::ClipmapSquares clipmapSquares_{};
    // Each level's centre along the sun, kept while the camera stays inside its depth range, and the sun it was taken for (UpdateClipmap).
    std::array<float, vsm::kNumClipmapLevels> clipmapZ_{};
    Math::float3 clipmapZSun_{};
    bool clipmapZValid_ = false;
    // Local-light plan S9e: sized to the view capacity (render::ShadowSpotCapacity / 6 x ShadowPointCapacity), resized only
    // by the capacity growth at GPU idle (ResizeLocalViews).
    std::vector<SceneView> spotShadowViews_ = std::vector<SceneView>(render::kShadowSpotsInitial);
    std::vector<SceneView> pointShadowViews_ = std::vector<SceneView>(6u * render::kShadowPointsInitial); // 6 cube faces per light
    // Plan S9e: the local views' light bounds + UE hull handed to UpdateViewFrustums each frame (by slot; null = none),
    // MaxShadowViews() + 1 of each (resized with the capacity).
    std::vector<render::LocalViewCull> localViewCull_ = std::vector<render::LocalViewCull>(render::MaxShadowViews() + 1u);
    std::vector<const render::LocalViewCull*> localViewCullPtr_ = std::vector<const render::LocalViewCull*>(render::MaxShadowViews() + 1u);
    std::vector<const Frustum*> shadowViewFrustums_; // S9e: the per-frame frustum upload's slots (MaxShadowViews() + camera)
    // S20 partition cache (render::sdsm::g_cacheFrames): per partition, when it last rendered and
    // from where the camera looked then -- the camera guard re-renders it once the camera has
    // turned or moved further than its widened box covers. `valid` drops whenever anything the
    // kept tiles depend on changes (sun, camera cut, atlas, partition count, the mode itself).
    struct SdsmCacheSlot
    {
        bool valid = false;
        Math::float3 camPos{};
        Math::float3 camDir{};
    };
    std::array<SdsmCacheSlot, 4> sdsmCache_{};
    std::uint32_t sdsmCacheSunVersion_ = ~0u;
    std::uint64_t sdsmCacheCamRevision_ = ~0ull;
    UINT sdsmCacheAtlasRes_ = 0u;
    const void* sdsmCacheAtlas_ = nullptr; // the atlas resource: a re-created one holds nothing
    std::uint32_t sdsmCachePartitions_ = 0u;
    std::uint32_t sdsmCacheStaticSet_ = ~0u;
    std::uint64_t sdsmCacheFrame_ = ~0ull; // the frame the analyze last ran; a gap = the mode was off
};
