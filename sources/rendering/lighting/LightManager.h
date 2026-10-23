#pragma once

#include <vector>
#include <algorithm>
#include <array>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/RenderConstants.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/lighting/PointLight.h"
#include "rendering/lighting/SpotLight.h"
#include "rendering/shadows/ShadowAtlasPacker.h" // ShadowPackScratch

class Renderer;
class Frustum;

class LightManager {
public:
    // The total spot-light count is unbounded (the structured buffer grows on demand), and since local-light plan S9e so
    // are the shadow CASTERS -- UE's: every lit light whose reach touches the view and whose fade is above 1/256 wants a
    // shadow. The view slots they take are a CAPACITY (render::ShadowSpotCapacity / ShadowPointCapacity) the caller
    // passes as `maxCount`; a frame that wants more takes the best-ranked that fit and reports the rest through
    // WantedShadowedSpots / Points, and Scene::ReconcileShadowMode grows the capacity at its next GPU-idle point. The
    // atlas itself is a page budget (PackShadowAtlases), VSM's a page pool. kVsmShadowed* is only the size of the
    // layout's PREFIX -- the spots and faces before the clipmap slots.
    static constexpr std::uint32_t kVsmShadowedSpotLights = render::kShadowViewVsmSpots;
    static constexpr std::uint32_t kVsmShadowedPointLights = render::kShadowViewVsmPoints;

    struct alignas(16) PointLightGpu
    {
        Math::float3 position;
        float radius;
        Math::float3 color;
        float intensity;
        Math::float4 shadowParams; // x = shadow slot (-1 = none), y = bias, z = nearPlane, w = farPlane (radius)
        Math::float4 volumetric;   // x = volumetric scattering intensity (plan A4d); local-light shadows: y = UE's slope
                                   // scale (S1), z = UE's sharpen 7s+1 (S2), w = 1: UE's 29-tap disc, 0: the old 3x3
        Math::float4 shadowFade;   // local-light shadows S3: x = UE's shadow fade (lerp(1, shadow, x)); S9: y = the atlas
                                   // page; S4: z = 1 / the light's face resolution; w = the TEXEL bias's normal texels
                                   // (> 0 marks that mode, render::LocalShadowNormalTexels).
        // Plan S9: the light's 3f x 2f block in its atlas page, in page UV -- xy = the block's corner, zw = one face's
        // size (face k at xy + (k % 3, k / 3) * zw). Mirrored by pointlight_cs.hlsl, glass.hlsl and rt_lights.hlsli.
        Math::float4 shadowAtlas;
        // Plan S10e: x = the light's VSM mip bias on top of the frame's (vsm::LocalMovingMipBias: UE's
        // ResolutionLodBiasLocalMoving by the light's mobility). Plan S4: y = 1 / (f - n) when its faces hold the
        // LINEAR depth (render::PointShadowLinearDepth; n = shadowParams.z), 0 = perspective. Plan S9e: z = the VSM view
        // of its +X face (vsm::PointFaceView of slot * 6; the other five follow). w reserved. Same mirrors.
        Math::float4 shadowParams4;
    };

    struct alignas(16) SpotLightGpu
    {
        Math::float4 positionRange;      // xyz = position, w = range
        Math::float4 directionCosOuter;  // xyz = direction (normalized), w = cos(outer)
        Math::float4 colorIntensity;     // xyz = color, w = intensity
        Math::float4 shadowParams;       // x = cos(inner), y = shadow index, z = invAngleRange, w = depth bias
        Math::float4 shadowParams2;      // x = normal bias (WS), y = volumetric scattering intensity (plan A4d),
                                         // z = UE's shadow fade (S3: lerp(1, shadow, z)), w = the TEXEL bias's normal
                                         // texels (> 0 marks that mode; x is then its push texels)
        Math::mat4   viewProj;
        // Local-light shadow plan S2: x = the projection's near plane, y = UE's soft-transition scale (1/m), z = UE's
        // sharpen (7 x ShadowSharpen + 1), w = 1: UE's 6x6 tent filter, 0: the old 3x3 box. Mirrored by
        // spotlight_cs.hlsl, glass.hlsl and rt_lights.hlsli.
        Math::float4 shadowParams3;
        // Plan S9: the light's square in its atlas page -- xy = its corner in page UV, z = its size in page UV, w = the
        // page. Same mirrors.
        Math::float4 shadowAtlas;
        // Plan S10e: x = the light's VSM mip bias on top of the frame's (UE's moving bias, see PointLightGpu).
        // Plan S4: y = 1 / (f - n) when its atlas square holds the LINEAR depth (w - n) / (f - n)
        // (render::SpotShadowLinearDepth; n = shadowParams3.x, both the projection's own), 0 = the perspective NDC
        // depth. Plan S9e: z = its VSM view (vsm::SpotView of its slot). w reserved. Same mirrors.
        Math::float4 shadowParams4;
    };

    LightManager() = default;
    ~LightManager();

    std::vector<PointLight>& PointLights() { return pointLights_; }
    const std::vector<PointLight>& PointLights() const { return pointLights_; }

    std::vector<SpotLight>& SpotLights() { return spotLights_; }
    const std::vector<SpotLight>& SpotLights() const { return spotLights_; }

    void UpdateSpotLightCache();

    // Per-frame: among lit spots whose influence intersects the camera frustum,
    // choose the highest-priority projected/bright spots up to
    // `maxCount` (the view capacity, plan S9e) to cast shadows, and assign each a shadow slot in
    // [0, GetShadowedSpotCount()) in descending-priority order. Frustum-culling
    // first means a nearby spot that can't affect any visible pixel (e.g. behind
    // the camera, out of reach) never consumes a scarce shadow slot. Must run
    // after UpdateSpotLightCache and before the spot shadow views are built /
    // the spot-light buffer is filled.
    //
    // Local-light shadow plan S3: ranked, as UE sizes a light's shadow, by the resolution it WANTS -- its effective
    // screen radius (UE GetEffectiveScreenRadius: a spot's inscribed sphere at the cone's end, SpotLightSceneProxy.cpp
    // :117-135; a point's radius) x r.Shadow.TexelsPerPixelSpotlight 2.546 / ...Pointlight 1.273 (ShadowSetup.cpp
    // :276-292) -- and faded by it as UE fades (CalculateShadowFadeAlpha, :534-566: full above r.Shadow.FadeResolution
    // 64 texels, none at MinResolution 32, FadeExponent 0.25); a light whose fade is 0 takes no slot. A light shadowed
    // last frame ranks x kShadowSlotHysteresis, so two lights of about one size do not swap the slot back and forth.
    // `screenScale` = max(W/2 * P11, H/2 * P22) of the camera's projection at the render resolution.
    //
    // Plan S4: the same wanted resolution sizes the light's shadow as UE sizes it -- a spot's rounded down to a power of
    // two up to `maxRes` (the spot atlas page), a point's cube face to the first of maxRes/2, /4, /8, /16, 32
    // (GetSpotShadowResolution). Plan S9: up to `maxCount` lights (render::ShadowSpotCapacity -- the atlas modes and,
    // since plan S10b, VSM; S9e: WantedShadowedSpots grows it); PackShadowAtlases then places them on the atlases.
    void SelectShadowedSpots(const Math::float3& cameraPos, const Frustum& cameraFrustum, float screenScale,
                             std::uint32_t maxRes, std::uint32_t maxCount);
    // S3: the light's shadow fade this frame, 0..1 (1 for an index without one).
    float GetSpotShadowFade(size_t lightIndex) const
    {
        return lightIndex < spotShadowFade_.size() ? spotShadowFade_[lightIndex] : 1.0f;
    }
    float GetPointShadowFade(size_t lightIndex) const
    {
        return lightIndex < pointShadowFade_.size() ? pointShadowFade_[lightIndex] : 1.0f;
    }
    // S4: the light's shadow resolution this frame (a spot's square, a point's cube face), 0 for an index without one.
    std::uint32_t GetSpotShadowResolution(size_t lightIndex) const
    {
        return lightIndex < spotShadowRes_.size() ? spotShadowRes_[lightIndex] : 0u;
    }
    std::uint32_t GetPointShadowResolution(size_t lightIndex) const
    {
        return lightIndex < pointShadowRes_.size() ? pointShadowRes_[lightIndex] : 0u;
    }
    static constexpr float kShadowSlotHysteresis = 1.25f;

    size_t GetSpotLightCount() const { return cachedSpotLightCount_; }

    // Number of spots casting a shadow this frame (<= the caller's maxCount, the view capacity).
    size_t GetShadowedSpotCount() const { return shadowedSpotLightIndices_.size(); }
    // Plan S9e: how many spots / points the last selection WANTED to shadow (every candidate, before the capacity cut) --
    // what Scene::ReconcileShadowMode grows the view capacity to.
    std::uint32_t WantedShadowedSpots() const { return wantedShadowedSpots_; }
    std::uint32_t WantedShadowedPoints() const { return wantedShadowedPoints_; }
    // A fresh level asks again from nothing (Scene::FinalizeLevelLoad resets the capacity with it).
    void ForgetWantedShadowed() { wantedShadowedSpots_ = 0u; wantedShadowedPoints_ = 0u; }
    // Shadow-atlas slot for a spot-light index, or -1 if it is not shadowed.
    int GetSpotShadowSlot(size_t lightIndex) const
    {
        return lightIndex < spotShadowSlot_.size() ? spotShadowSlot_[lightIndex] : -1;
    }
    // Inverse of the slot map: which spot-light index owns a given shadow slot.
    size_t GetShadowedSpotLightIndex(size_t slot) const
    {
        return slot < shadowedSpotLightIndices_.size() ? shadowedSpotLightIndices_[slot] : 0;
    }

    // Per-frame point-light equivalent of SelectShadowedSpots. Among point lights
    // whose influence sphere (position, radius) intersects the camera frustum and
    // which have shadows enabled, choose the highest projected-size up to
    // `maxCount` (the view capacity) to cast (cube) shadows, assigning each a cube slot in
    // [0, GetShadowedPointCount()). Pass the SAME non-jittered frustum used for
    // spots. Must run before the point cube views are built / the point-light buffer
    // is filled.
    void SelectShadowedPoints(const Math::float3& cameraPos, const Frustum& cameraFrustum, float screenScale,
                              std::uint32_t maxRes, std::uint32_t maxCount); // S3 / S4 / S9 as spots

    // Local-light shadow plan S9: place every shadowed light in the atlas pages, as UE packs its whole-scene shadows
    // into atlases (ShadowSetup.cpp AllocateAtlasedShadowDepthTargets :5971-6090): a spot's res x res square in a
    // spot page (spotPageSize square), a point's six faces as ONE 3f x 2f block in a point page (3 * pointFaceMax by
    // 2 * pointFaceMax). UE adds an atlas when one is full; ours are resident pages, so a frame that does not fit
    // halves every light one step at a time, as UE's VSM raises one global LOD bias on a short pool (and, past the
    // floor, drops the lowest-ranked) -- and asks for more pages, up to `maxPages`, through WantedSpotShadowPages /
    // WantedPointShadowPages (Scene::ReconcileShadowMode grows them).
    // After both Select*; may lower GetSpot/PointShadowResolution and drop slots.
    struct ShadowAtlasPages
    {
        std::uint32_t spotPages = 1, pointPages = 1, maxPages = 1;
        std::uint32_t spotPageSize = 0, pointFaceMax = 0;
    };
    void PackShadowAtlases(const ShadowAtlasPages& pages);
    struct ShadowAtlasPlacement { std::int32_t page = -1; std::uint32_t x = 0, y = 0; }; // texels in the page
    ShadowAtlasPlacement GetSpotShadowPlacement(size_t lightIndex) const
    {
        return lightIndex < spotShadowPlace_.size() ? spotShadowPlace_[lightIndex] : ShadowAtlasPlacement{};
    }
    ShadowAtlasPlacement GetPointShadowPlacement(size_t lightIndex) const // the 3f x 2f block's corner
    {
        return lightIndex < pointShadowPlace_.size() ? pointShadowPlace_[lightIndex] : ShadowAtlasPlacement{};
    }
    // Local-light plan S7, the direct read: this frame the light's whole shadow is its picture in the cache pool
    // (render::LocalShadowCache) -- the pool page and the corner of its square / 3f x 2f block; page -1 = read its atlas
    // place as usual. Scene decides each frame (after the cache requests), the light buffers read it.
    void ClearShadowDirect();
    void SetSpotShadowDirect(size_t lightIndex, ShadowAtlasPlacement place);
    void SetPointShadowDirect(size_t lightIndex, ShadowAtlasPlacement place);
    ShadowAtlasPlacement GetSpotShadowDirect(size_t lightIndex) const
    {
        return lightIndex < spotShadowDirect_.size() ? spotShadowDirect_[lightIndex] : ShadowAtlasPlacement{};
    }
    ShadowAtlasPlacement GetPointShadowDirect(size_t lightIndex) const
    {
        return lightIndex < pointShadowDirect_.size() ? pointShadowDirect_[lightIndex] : ShadowAtlasPlacement{};
    }
    std::uint32_t WantedSpotShadowPages() const { return wantedSpotPages_; }
    std::uint32_t WantedPointShadowPages() const { return wantedPointPages_; }
    // How many lights the last pack had to shrink / drop to fit the resident pages (0 = everything at its UE size).
    std::uint32_t ShadowAtlasShrunk() const { return atlasShrunk_; }
    std::uint32_t ShadowAtlasDropped() const { return atlasDropped_; }

    // Number of point lights casting a shadow this frame (<= the caller's maxCount).
    size_t GetShadowedPointCount() const { return shadowedPointLightIndices_.size(); }
    // Cube-atlas slot for a point-light index, or -1 if it is not shadowed.
    int GetPointShadowSlot(size_t lightIndex) const
    {
        return lightIndex < pointShadowSlot_.size() ? pointShadowSlot_[lightIndex] : -1;
    }
    // Inverse of the slot map: which point-light index owns a given cube slot.
    size_t GetShadowedPointLightIndex(size_t slot) const
    {
        return slot < shadowedPointLightIndices_.size() ? shadowedPointLightIndices_[slot] : 0;
    }

    bool EnsurePointLightBuffer(Renderer* renderer, size_t requiredLights);
    bool EnsureSpotLightBuffer(Renderer* renderer, size_t requiredLights);

    // Barrier plan step 4: read-only "is the buffer already big enough" for the recording
    // passes. They used to call Ensure* themselves, which could FREE the previous allocation
    // while an in-flight frame still referenced it — the DXGI_DEVICE_HUNG the --scene-stress
    // harness was built for. Growth now happens once per frame, before any recording.
    bool HasPointLightBuffer(size_t requiredLights) const {
        return static_cast<bool>(pointLightBuffer_) && pointLightCapacity_ >= requiredLights;
    }
    bool HasSpotLightBuffer(size_t requiredLights) const {
        return static_cast<bool>(spotLightBuffer_) && spotLightCapacity_ >= requiredLights;
    }

    // The light buffers are ring-buffered per in-flight frame (kFrameCount regions
    // in one resource): the CPU rewrites the buffer every frame while up to
    // kFrameCount frames are still being read by the GPU, so each frame must
    // read/write its OWN region. Pass the renderer's current frame index. Writing
    // or reading a shared region would be a cross-frame WAR hazard (an in-flight
    // frame reading a newer frame's data — e.g. a stale shadow-slot index).
    PointLightGpu* GetPointLightBufferCPU(UINT frame) const {
        return pointLightBufferCPU_ ? pointLightBufferCPU_ + frame * pointLightCapacity_ : nullptr;
    }
    SpotLightGpu* GetSpotLightBufferCPU(UINT frame) const {
        return spotLightBufferCPU_ ? spotLightBufferCPU_ + frame * spotLightCapacity_ : nullptr;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetPointLightSrv(UINT frame) const { return pointLightSrvHandles_[frame]; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetSpotLightSrv(UINT frame) const { return spotLightSrvHandles_[frame]; }

    // Plan S10e: UE's light mobility (ShadowScene.cpp:190-220, r.Shadow.Scene.LightActiveFrameCount): 1 on a frame
    // the light's SHADOW moved -- position, direction, reach, cone, near plane -- falling linearly to 0 over
    // `activeFrames` frames after the last move. A light first seen stands (a level load is not a move). Once a frame,
    // before the shadow views are built.
    void UpdateLightMobility(std::uint64_t frame, std::uint32_t activeFrames);
    float GetSpotMobility(size_t index) const { return Mobility(spotMovedFrame_, index); }
    float GetPointMobility(size_t index) const { return Mobility(pointMovedFrame_, index); }
    // Lights still moving or settling (mobility > 0): the VSM must keep updating while their bias decays.
    std::uint32_t GetMobileLightCount() const { return mobileLights_; }

    void Reset();

private:
    using ComPtr = Microsoft::WRL::ComPtr<ID3D12Resource>;

    // Plan S10e: what moves a light's shadow, as raw floats (compared bitwise); the frame it last changed.
    struct ShadowPose { float v[9] = {}; };
    static constexpr std::uint64_t kNeverMoved = ~0ull;
    float Mobility(const std::vector<std::uint64_t>& moved, size_t index) const;
    std::vector<ShadowPose> spotPose_, pointPose_;
    std::vector<std::uint64_t> spotMovedFrame_, pointMovedFrame_;
    std::uint64_t mobilityFrame_ = 0;
    std::uint32_t mobilityActiveFrames_ = 10;
    std::uint32_t mobileLights_ = 0;

    void ReleasePointLightBuffer(Renderer* renderer = nullptr);
    void ReleaseSpotLightBuffer(Renderer* renderer = nullptr);

    std::vector<PointLight> pointLights_;
    std::vector<SpotLight>  spotLights_;

    size_t cachedSpotLightCount_ = 0;
    // Per-frame shadow-slot mapping (see SelectShadowedSpots).
    std::vector<int> spotShadowSlot_;                       // parallel to spotLights_; -1 = unshadowed
    std::vector<std::uint32_t> shadowedSpotLightIndices_;   // slot -> spot-light index
    // Per-frame point cube-shadow-slot mapping (see SelectShadowedPoints).
    std::vector<int> pointShadowSlot_;                      // parallel to pointLights_; -1 = unshadowed
    std::vector<std::uint32_t> shadowedPointLightIndices_;  // slot -> point-light index
    std::uint32_t wantedShadowedSpots_ = 0;                 // S9e: the last selection's candidates
    std::uint32_t wantedShadowedPoints_ = 0;
    std::vector<float> spotShadowFade_;                     // S3: parallel to spotLights_
    std::vector<float> pointShadowFade_;                    // S3: parallel to pointLights_
    std::vector<std::uint32_t> spotShadowRes_;              // S4: parallel to spotLights_
    std::vector<std::uint32_t> pointShadowRes_;             // S4: parallel to pointLights_ (cube face)
    std::vector<float> spotShadowScore_;                    // S9: the S3 rank (wanted resolution), for the pack
    std::vector<float> pointShadowScore_;
    std::vector<ShadowAtlasPlacement> spotShadowPlace_;     // S9: parallel to spotLights_
    std::vector<ShadowAtlasPlacement> pointShadowPlace_;    // S9: parallel to pointLights_
    std::vector<ShadowAtlasPlacement> spotShadowDirect_;    // S7 direct read: parallel to spotLights_ (page -1 = none)
    std::vector<ShadowAtlasPlacement> pointShadowDirect_;   // ...and to pointLights_
    std::uint32_t wantedSpotPages_ = 1, wantedPointPages_ = 1;
    std::uint32_t atlasShrunk_ = 0, atlasDropped_ = 0;
    // Working storage of the selection and the pack (SelectShadowed*, PackShadowAtlases: one thread, one after
    // another), kept so a frame reuses the last one's capacity -- as locals they were ~35 vectors allocated and
    // freed a frame (memtrace, 2026-10-23).
    struct ShadowCandidate { std::uint32_t index; float score; };
    std::vector<ShadowCandidate> shadowCandidates_;
    std::vector<std::uint8_t> shadowWasShadowed_, shadowSlotHoles_;
    std::vector<std::uint32_t> shadowPreviousSlots_, shadowSelected_;
    render::ShadowPackScratch shadowPack_;
    // Each buffer holds render::kFrameCount contiguous regions of pointLightCapacity_
    // / spotLightCapacity_ elements; region f is written+read only by in-flight frame
    // f, with its own SRV in the *SrvHandles_ array. Capacity is PER REGION.
    // Step 6b part 2: on the wrapper — the explicit ClearResourceState in
    // Release*LightBuffer is no longer needed and was removed.
    GpuResource pointLightBuffer_;
    PointLightGpu* pointLightBufferCPU_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> pointLightSrvHeap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::kFrameCount> pointLightSrvHandles_{};
    size_t pointLightCapacity_ = 0;

    GpuResource spotLightBuffer_;
    SpotLightGpu* spotLightBufferCPU_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> spotLightSrvHeap_;
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, render::kFrameCount> spotLightSrvHandles_{};
    size_t spotLightCapacity_ = 0;
};

