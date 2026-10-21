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

class Renderer;
class Frustum;

class LightManager {
public:
    // The total spot-light count is unbounded (the structured buffer grows on
    // demand). Only shadow CASTERS are capped per frame - the highest-priority
    // kMaxShadowedSpotLights for the camera - which sizes the shadow-view arrays and the
    // GPU cull's view slots. Distant spots still light the scene, just without a shadow.
    //
    // Local-light shadow plan S9: the atlas modes (Legacy / SDSM) take up to 32 spots and 16
    // points -- UE has no count at all, but our views are fixed slots (render::kMaxShadowViews);
    // the atlas itself is a page budget (PackShadowAtlases). Plan S10b: VSM takes the same 32 + 16
    // (its views are the cull slots minus the cascades). kVsmShadowed* is only the size of the
    // layout's PREFIX -- the spots and faces before the clipmap slots.
    static constexpr std::uint32_t kMaxShadowedSpotLights = render::kShadowViewSpots;
    static constexpr std::uint32_t kMaxShadowedPointLights = render::kShadowViewPoints;
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
                                   // page; S4: z = 1 / the light's face resolution; w reserved.
        // Plan S9: the light's 3f x 2f block in its atlas page, in page UV -- xy = the block's corner, zw = one face's
        // size (face k at xy + (k % 3, k / 3) * zw). Mirrored by pointlight_cs.hlsl, glass.hlsl and rt_lights.hlsli.
        Math::float4 shadowAtlas;
    };

    struct alignas(16) SpotLightGpu
    {
        Math::float4 positionRange;      // xyz = position, w = range
        Math::float4 directionCosOuter;  // xyz = direction (normalized), w = cos(outer)
        Math::float4 colorIntensity;     // xyz = color, w = intensity
        Math::float4 shadowParams;       // x = cos(inner), y = shadow index, z = invAngleRange, w = depth bias
        Math::float4 shadowParams2;      // x = normal bias (WS), y = volumetric scattering intensity (plan A4d),
                                         // z = UE's shadow fade (S3: lerp(1, shadow, z)), w reserved
        Math::mat4   viewProj;
        // Local-light shadow plan S2: x = the projection's near plane, y = UE's soft-transition scale (1/m), z = UE's
        // sharpen (7 x ShadowSharpen + 1), w = 1: UE's 6x6 tent filter, 0: the old 3x3 box. Mirrored by
        // spotlight_cs.hlsl, glass.hlsl and rt_lights.hlsli.
        Math::float4 shadowParams3;
        // Plan S9: the light's square in its atlas page -- xy = its corner in page UV, z = its size in page UV, w = the
        // page. Same mirrors.
        Math::float4 shadowAtlas;
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
    // kMaxShadowedSpotLights to cast shadows, and assign each a shadow slot in
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
    // (GetSpotShadowResolution). Plan S9: up to `maxCount` lights (kMaxShadowedSpotLights -- the atlas modes and, since
    // plan S10b, VSM); PackShadowAtlases then places them on the atlases.
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

    // Number of spots casting a shadow this frame (<= kMaxShadowedSpotLights).
    size_t GetShadowedSpotCount() const { return shadowedSpotLightIndices_.size(); }
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
    // kMaxShadowedPointLights to cast (cube) shadows, assigning each a cube slot in
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
    std::uint32_t WantedSpotShadowPages() const { return wantedSpotPages_; }
    std::uint32_t WantedPointShadowPages() const { return wantedPointPages_; }
    // How many lights the last pack had to shrink / drop to fit the resident pages (0 = everything at its UE size).
    std::uint32_t ShadowAtlasShrunk() const { return atlasShrunk_; }
    std::uint32_t ShadowAtlasDropped() const { return atlasDropped_; }

    // Number of point lights casting a shadow this frame (<= kMaxShadowedPointLights).
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

    void Reset();

private:
    using ComPtr = Microsoft::WRL::ComPtr<ID3D12Resource>;

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
    std::vector<float> spotShadowFade_;                     // S3: parallel to spotLights_
    std::vector<float> pointShadowFade_;                    // S3: parallel to pointLights_
    std::vector<std::uint32_t> spotShadowRes_;              // S4: parallel to spotLights_
    std::vector<std::uint32_t> pointShadowRes_;             // S4: parallel to pointLights_ (cube face)
    std::vector<float> spotShadowScore_;                    // S9: the S3 rank (wanted resolution), for the pack
    std::vector<float> pointShadowScore_;
    std::vector<ShadowAtlasPlacement> spotShadowPlace_;     // S9: parallel to spotLights_
    std::vector<ShadowAtlasPlacement> pointShadowPlace_;    // S9: parallel to pointLights_
    std::uint32_t wantedSpotPages_ = 1, wantedPointPages_ = 1;
    std::uint32_t atlasShrunk_ = 0, atlasDropped_ = 0;
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

