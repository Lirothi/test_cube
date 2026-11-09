#pragma once

#include <cstdint>
#include <memory>
#include <wrl/client.h>
#include <d3d12.h>

#include "core/math/Math.h"
#include "app/scene/SceneView.h"
#include "rendering/core/ResourceDeclarations.h"

class Material;
class Renderer;

// Plan S's fallback WITHOUT ray tracing (`skyVis.fallback`; shaders/sky_occlusion_cs.hlsl says what it computes). With
// the RT sky visibility off -- or no RT hardware -- the sky used to light every point as if it saw all of it: a crown's
// lower fronds, the trunks and the sand under the canopy as bright as the open beach, the whole frame washed out
// (owner, 2026-11-09, the island at 18:00: "why is everything so pale without skyVis"). This is the world seen from
// above, around the camera: the scene's casters drawn top-down (their highest surface) and bottom-up (their lowest),
// and the terrain, each a D16 render of DepthSize() squared over the map's width, reduced into two levels the trace
// marches. Redrawn only when the camera has moved a snap step, the set of casters changed, a static caster moved (at
// most twice a second), the texel changed, or it never was: ~0.36 ms of GPU on the owner's island at 256 or 512 texels
// alike (the casters' draws, not the texels).
//
// Its size is two knobs (SceneRenderSettings::skyVisFallbackResolution / skyVisFallbackTexel): the occlusion level's
// texels a side and the world size of one. The texel moves only the placement (a redraw); the resolution re-creates the
// textures, at a GPU-idle point (Scene::ReconcileSkyOcclusion -> Resize).
//
// The views are culled by Scene::PrepareViews (a frame that draws enqueues them); SceneRenderer draws and reduces them
// in the prologue (Main_SkyOcclusionMap, Main_SkyOcclusionReduce) and traces after the G-buffer (Main_SkyVisFallback).
class SkyOcclusionMap
{
public:
    // The occlusion ("near") level's texels a side: a power of two in this range (ClampResolution).
    static constexpr std::uint32_t kMinResolution = 64u;
    static constexpr std::uint32_t kMaxResolution = 1024u;
    // The world size of a near texel (m): ClampTexel's range.
    static constexpr float kMinTexel = 0.125f;
    static constexpr float kMaxTexel = 4.0f;
    // Depth texels a near texel (a side): its coverage counts kDepthPerTexel^2 samples. The far level is a near level of
    // kFarPerTexel x kFarPerTexel texels.
    static constexpr std::uint32_t kDepthPerTexel = 4u;
    static constexpr std::uint32_t kFarPerTexel = 4u;
    // Metres above and below the centre's height (D16 over 1 km: 1.5 cm a step). 100 m left a camera 110 m up with no
    // ground and no crowns in the map at all: the open sky everywhere from a high view.
    static constexpr float kHalfHeight = 500.0f;
    // The centre moves in steps of this many near texels: a multiple of the far level's texel (4 near texels), so every
    // level keeps its texel grid's world place. 16 m at the default 1 m texel.
    static constexpr float kSnapTexels = 16.0f;
    static constexpr D3D12_RESOURCE_STATES kDepthRest =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    static constexpr D3D12_RESOURCE_STATES kLevelRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    static std::uint32_t ClampResolution(std::uint32_t resolution); // the nearest power of two in range
    static float ClampTexel(float texel);
    // What a resolution costs: the three D16 renders and the two RGBA16F levels (MB).
    static double MegabytesAt(std::uint32_t resolution);

    // Where the map stands: its centre, its size and the three cameras' matrices.
    struct Placement
    {
        Math::float3 centre = Math::float3(0.0f, 0.0f, 0.0f);
        float halfExtent = 0.0f; // metres either side of the centre
        float texel = 1.0f;      // a near texel (m)
        Math::mat4 topView = Math::mat4::Identity();
        Math::mat4 bottomView = Math::mat4::Identity();
        Math::mat4 proj = Math::mat4::Identity();
        float topEyeY = 0.0f;
        float bottomEyeY = 0.0f;
        float Snap() const { return kSnapTexels * texel; }
    };

    SkyOcclusionMap();
    ~SkyOcclusionMap();
    SkyOcclusionMap(const SkyOcclusionMap&) = delete;
    SkyOcclusionMap& operator=(const SkyOcclusionMap&) = delete;

    // Creates the renders and the two levels at `resolution` (once; a different one waits for Resize) and the three PSOs
    // once; idempotent. Before the graph (Scene::PrepareViews).
    void Prepare(Renderer* renderer, std::uint32_t resolution);
    bool Ready() const { return ready_ && !failed_; }
    // The textures exist (at Resolution()).
    bool Built() const { return ready_; }
    std::uint32_t Resolution() const { return nearSize_; }
    // The GPU is IDLE (the caller waited): the textures go, made again at `resolution` (0 = freed, Prepare makes them
    // when the map is wanted again). Nothing drawn survives it.
    void Resize(Renderer* renderer, std::uint32_t resolution);
    // A level was unloaded: nothing drawn is this level's (the resources stay).
    void Invalidate();

    // Render thread, Scene::PrepareViews: places the views around the camera and decides whether this frame draws --
    // the camera crossed a snap step, the caster set changed, the texel changed, or a static caster moved (`casterMoved`;
    // redrawn at most every kMovedRedrawFrames while something keeps moving: an editor drag).
    void Update(const Math::float3& cameraPos, std::uint32_t casterSetVersion, bool casterMoved, bool wanted, float texel);
    static constexpr std::uint32_t kMovedRedrawFrames = 30u;
    std::uint32_t DepthSize() const { return nearSize_ * kDepthPerTexel; }
    std::uint32_t NearSize() const { return nearSize_; }
    std::uint32_t FarSize() const { return nearSize_ / kFarPerTexel; }
    bool DrawThisFrame() const { return draw_; }
    SceneView& ObjectsView() { return objectsView_; }
    SceneView& TerrainView() { return terrainView_; }
    const Placement& Current() const { return current_; }

    // The graph's serial build: the draw pass registered (its reduce may follow), the reduce registered (the levels
    // hold the current placement from here on).
    void MarkDrawRegistered() { drawRegistered_ = true; }
    bool DrawRegistered() const { return drawRegistered_; }
    void MarkReduced();
    // The levels hold a placement: the trace may read them, with Drawn()'s numbers.
    bool Valid() const { return drawn_; }
    const Placement& Drawn() const { return drawnPlacement_; }

    // The trace's and the reduce's numbers for a placement: (min x, min z, 1 / width, near texel) and
    // (top eye y, bottom eye y, near, far - near).
    static Math::float4 MapParams(const Placement& p);
    static Math::float4 HeightParams(const Placement& p);
    // The edge: the horizontal radius around the camera the map traces in full whatever the camera's place inside its
    // snap cell (half the width, less half a snap step and the reach) and where, at `fade` of it, the fade starts. The
    // trace's (x, z of the camera, start, 1 / width) for a camera at `camera`.
    static float FullRadius(const Placement& p, float reach);
    static Math::float4 FadeParams(const Placement& p, const Math::float3& camera, float reach, float fade);

    ID3D12Resource* TopDepth() const { return top_.Get(); }
    ID3D12Resource* BottomDepth() const { return bottom_.Get(); }
    ID3D12Resource* TerrainDepth() const { return terrain_.Get(); }
    ID3D12Resource* NearLevel() const { return near_.Get(); }
    ID3D12Resource* FarLevel() const { return far_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE TopDsv() const { return dsv_[0]; }
    D3D12_CPU_DESCRIPTOR_HANDLE BottomDsv() const { return dsv_[1]; }
    D3D12_CPU_DESCRIPTOR_HANDLE TerrainDsv() const { return dsv_[2]; }
    D3D12_CPU_DESCRIPTOR_HANDLE TopSrv() const { return srv_[0]; }
    D3D12_CPU_DESCRIPTOR_HANDLE BottomSrv() const { return srv_[1]; }
    D3D12_CPU_DESCRIPTOR_HANDLE TerrainSrv() const { return srv_[2]; }
    D3D12_CPU_DESCRIPTOR_HANDLE NearSrv() const { return srv_[3]; }
    D3D12_CPU_DESCRIPTOR_HANDLE NearUav() const { return srv_[4]; }
    D3D12_CPU_DESCRIPTOR_HANDLE FarSrv() const { return srv_[5]; }
    D3D12_CPU_DESCRIPTOR_HANDLE FarUav() const { return srv_[6]; }

    Material* ReduceMaterial() const { return reduceMaterial_.get(); }
    Material* ReduceFarMaterial() const { return reduceFarMaterial_.get(); }
    Material* TraceMaterial() const { return traceMaterial_.get(); }

private:
    Placement PlaceAt(const Math::float3& centre, float texel) const;
    void PlaceViews(const Placement& p);
    bool CreateTextures(Renderer* renderer, std::uint32_t resolution);
    void ReleaseTextures();

    GpuResource top_;
    GpuResource bottom_;
    GpuResource terrain_;
    GpuResource near_;
    GpuResource far_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv_[3]{};
    D3D12_CPU_DESCRIPTOR_HANDLE srv_[7]{};
    std::shared_ptr<Material> reduceMaterial_;
    std::shared_ptr<Material> reduceFarMaterial_;
    std::shared_ptr<Material> traceMaterial_;
    bool pipelines_ = false; // the heaps and the PSOs (made once)
    bool ready_ = false;     // ...and the textures, at nearSize_
    bool failed_ = false;
    std::uint32_t nearSize_ = 0;

    SceneView objectsView_{};
    SceneView terrainView_{};
    Placement current_{};
    Placement drawnPlacement_{};
    bool draw_ = false;
    bool drawRegistered_ = false;
    bool drawn_ = false;
    std::uint32_t drawnCasterSet_ = 0;
    std::uint32_t pendingCasterSet_ = 0;
    bool moveOutstanding_ = false;       // a static caster moved since the last draw
    std::uint32_t framesSinceDraw_ = 0;  // frames since the last draw was decided
};
