#pragma once

// A boat's wake (2026-11-07, owner: "the yacht's wave disturbance; the reference is Crest", docs/ref/crest).
//
// Crest's dynamic waves are the model of the SYSTEM: a persistent field in a window that follows the camera, re-anchored
// by whole texels, absorbed toward its border, cleaned of non-finite values, its height added to the surface's
// displacement and its slope to the normal; the hull is an input that presses on it every step. What is NOT Crest's is
// the wave speed. Crest integrates the 2D wave equation per LOD cascade at the speed of that cascade's shortest wave
// (UpdateDynWaves.compute: c = sqrt(g * 2 texel / 2 pi)) -- a few m/s at the texels a boat is seen at -- so a hull at 16 m/s
// outruns every wave the field holds and leaves a Mach cone, narrowing with speed, where a real one leaves Kelvin's
// 19.47 degrees at any speed. Deep water's dispersion, omega = sqrt(g |k|), has no single speed to give a grid; it is exact
// per Fourier mode. So this field lives as Tessendorf's eWave ("Interactive Water Surfaces", 2004; "eWave", 2014): each
// step the height and vertical velocity go to the spectrum (the ocean's own FFT kernel, ocean_fft.hlsl), every mode turns
// through its own omega * dt -- exact, unconditionally stable at any frame time -- and come back.
//
// The hull is a moving PRESSURE (Havelock's ship): a head of water equal to how deep its underside stands under the
// waterline (buoyancy::ResolveFootprint, from the hull's own geometry). At rest the water under it sinks by exactly that
// depth and nowhere else -- hidden by the hull; moving, the same field throws the bow wave, the stern trough and the
// Kelvin wake, from nothing but the hull's shape and its path. The boat's speed is not an input.
//
// The spectrum has one depth for the whole window, the water does not: each step turns every mode four times, at
// omega^2 = g k tanh(k d) for 1, 3, 9 m and deep water, and each texel takes the two its own depth lies between (the shore
// depth map); land holds the field at 0, so the coast stops a wave and the island does not let it through.
//
// The FREE height (the field plus the hulls' own head: 0 under a hull at rest, the waves anything else would float on)
// goes to OceanReadback, so a boat rocks in another's wake.
//
// OFF (no boat in the world, or none for kLingerSeconds) records no dispatch and the surface does not read the field.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/ResourceDeclarations.h"

class Renderer;
class Material;
struct RenderGraphPassContext;
namespace buoyancy { struct HullFootprint; }

namespace ocean
{
// One hull pressing on the water this frame (Scene::Sync gathers them from the physics world's boats).
struct WakeHull
{
    Math::float2 position = Math::float2(0.0f, 0.0f); // world XZ of the mesh's origin
    Math::float2 forward = Math::float2(1.0f, 0.0f);  // world XZ of the bow's way, unit
    float scale = 1.0f;                               // the instance's scale (uniform)
    std::shared_ptr<const buoyancy::HullFootprint> footprint;
    // The breaking bow wave (vfx/VesselSpray::BowWaveOf), the wake's two foam arms: whose hull (a key across frames, never
    // followed), where the wave leaves each side of its waterline (world XZ; [0] port, [1] starboard), how much of it
    // there is by the bow's immersion (0 = the bow in the air .. 1 = in the water; its speed's share is the wake's own:
    // Tuning::bowSpeedFull), the hull's speed along its way (m/s) and its waterline's length (m): the diverging wave it
    // throws is about as long.
    const void* id = nullptr;
    Math::float2 bowRoot[2] = { Math::float2(0.0f, 0.0f), Math::float2(0.0f, 0.0f) };
    float bowShare = 0.0f;
    float speed = 0.0f;
    float length = 0.0f;
};
}

class OceanWake
{
public:
    // Authored in the ocean preset (OceanRenderConfig::wake*), clamped by OceanRenderable.
    struct Tuning
    {
        float strength = 1.0f;     // the hull's pressure: 1 = the head of water its draft stands for
        float damping = 1.5f;      // 1/s at the field's shortest wave (2 texels), falling as k^2 to the longer ones
        float breaking = 2.0f;     // 1/s a long wave in the shallows (d < wavelength / 20) runs down at; 0 = none
        float foamFade = 0.1f;     // 1/s the foam's lace runs down at
        float foamFadeDense = 1.5f; // 1/s per unit of foam on top: dense white water runs down fast
        float washFoam = 4.0f;     // foam a metre of head change under a moving hull leaves (the white water, its trail)
        float washBow = 0.0f;      // the share of it the hull's ARRIVAL lays (its bow), of what its leaving does
        float crestFoam = 1.0f;    // Crest's Wave Foam Strength (the steep crests)
        float crestCoverage = 0.55f; // Crest's Wave Foam Coverage: det J under it breaks
        float bowFoam = 1.0f;      // the breaking bow wave's foam (the two arms): 1 = a fresh front leaves white water
        float bowSpread = 1.0f;    // how fast its fronts run outward: 1 = the diverging wave's own speed
        float bowLife = 2.5f;      // s: a front's breaking runs down e-fold in this
        float bowLace = 0.15f;     // the share of a breaker's foam the water keeps once it has passed
        float bowThrow = 0.09f;    // s of the hull's speed the chine's sheet carries a front off the waterline at birth
        float bowWidth = 0.9f;     // m: the breaker band's half width at birth (at least 1.5 texels)
        float bowWiden = 0.25f;    // m/s it widens
        float bowScallop = 0.2f;   // a leaving point's wave speed is 1 +- this x a short random walk
        float bowSpeedFull = 12.0f; // m/s of the hull at which the arms are whole (from kFrontSpeedOn)
        bool bowBreaks = true;     // an arm ends where the bow leaves the water and starts anew where it comes back (false: the
                                   // line runs on across the jump -- the A/B, ocean.wakeBowBreaks)
        float bowFadeIn = 0.3f;    // s of the hull's speed over which an arm's breaker comes in aft of the roots (0 = whole
                                   // from the hull): the surface's gate (BowGates), the residue's first points
        float foamSpread = 0.3f;   // m2/s the laid foam spreads at
        float texel = 0.5f;        // metres a texel: 512 of them make the window
    };

    // The shore depth map as the surf sim reads it (OceanSurfSim::ShoreDepthWindow): uv = offset * invExtent + 0.5,
    // v flipped; water depth = lerp(zNear, zFar, raw) - camHeight.
    struct ShoreWindow
    {
        Math::float2 center = Math::float2(0.0f, 0.0f);
        float invExtent = 0.0f;
        float zNear = 0.0f;
        float zFar = 0.0f;
        float camHeight = 0.0f;
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
        ID3D12Resource* resource = nullptr; // declared read-only at its canonical state
    };

    void EnsureResources(Renderer* renderer);
    bool IsReady() const { return created_; }
    // The fields have been written: the surface may read them.
    bool HasSimulated() const { return everSimulated_; }

    // At Scene::Sync, the render half parked: this frame's hulls.
    void SetHulls(std::vector<ocean::WakeHull> hulls);
    // Where each hull's breakers come in, THIS frame: the foam's field (and the arms' band in it) steps at most 30 times a
    // second, a hull at 17 m/s runs 0.56 m between two steps, and anything that starts at the hull and stands on the
    // water meanwhile jumps forward with each step (owner: "the foam there appears abruptly, it shows even at speed").
    // So the band is laid whole from the hull, and the surface lets it in over a GATE that rides with the hull: nothing at
    // the roots' line, all of it `fadeInSeconds` of the hull's speed aft. Two float4 a hull: (the roots' midpoint x z, the
    // hull's forward x z), (the gate's length m, half the lane it covers m, 0, 0). Returns the hulls written.
    std::uint32_t BowGates(Math::float4* out, std::uint32_t maxHulls, float fadeInSeconds) const;
    // At the ocean's SyncSceneState: the window follows the camera (whole texels, decided before the graph).
    void TickWindow(Math::float2 cameraXZ, float texel);
    // Something to show: a hull now, or a wake still running down after the last one went.
    bool Active() const { return active_; }

    // `shore` null = deep water everywhere; `hold` = this frame the shore maps are re-rendered after this pass, so it
    // must not read them: the field is handed over as it stands (a step of no time), and nothing crosses the coast.
    // `stepInterval` (s, ocean::g_wakeStepHz): a frame closer than this to the last step does not step -- no pass, the field and
    // its time kept; 0 = every frame.
    std::function<void(RenderGraphPassContext)> BuildPass(RenderGraphPassContext& ctx, float timeSeconds,
        const Tuning& tuning, const ShoreWindow* shore, bool hold, float stepInterval);

    // xy: window centre (world XZ), z: 1 / half extent, w: texel world size.
    Math::float4 GetWindowParams() const;
    // The field (x: height, y: vertical velocity), pixel-readable after the pass.
    D3D12_CPU_DESCRIPTOR_HANDLE GetSrv() const;
    // The foam (RGBA16F: x 0..2, 1 and over fresh white water; yz the way it was laid times it), pixel-readable after the
    // pass: this frame's of the two.
    D3D12_CPU_DESCRIPTOR_HANDLE GetFoamSrv() const;
    // The free height (R16F, left as an unordered-access target) when frame `frame`'s pass wrote it (the renderer's
    // total frame number), else null: what OceanReadback copies for the buoyancy.
    // Not only a frame that stepped (ocean::g_wakeStepHz): between steps the texture holds the last one, which is what the
    // field is -- a copy waiting for a step lost the boats their wake on every copy the 30 Hz rhythms did not line up.
    ID3D12Resource* GetFreeHeightResource(std::uint64_t frame) const
    {
        return frame != 0 && everSimulated_ ? free_.Get() : nullptr;
    }
    // ocean::g_wakeStepHz: how long ago (s, on the ocean's clock) the field was last stepped -- the surface carries the height
    // on by the vertical velocity this far (wakeParams3.y). 0 before a step and on a held clock.
    float SecondsSinceStep(float now) const
    {
        return stepTime_ >= 0.0f ? std::clamp(now - stepTime_, 0.0f, kMaxStep) : 0.0f;
    }

    static constexpr std::uint32_t kResolution = 512u;

    // Readouts (get_readouts `wake`; the game half reads them while the render half steps: atomics): the bow waves'
    // fronts alive after the last step, the points on them, and how many fronts it handed to the GPU (at most kMaxFronts).
    std::uint32_t FrontCount() const { return frontsAlive_.load(std::memory_order_relaxed); }
    std::uint32_t FrontPointCount() const { return frontPoints_.load(std::memory_order_relaxed); }
    std::uint32_t FrontsDrawn() const { return frontsDrawn_.load(std::memory_order_relaxed); }
    // The breaks made in the arms since the process started (a bow that left the water, or came back into it), and the
    // longest piece of the lines the last step handed to the GPU that carried foam at BOTH its ends, m: a point leaves
    // every 0.125 s, so 2-3 m at 17 m/s -- a piece of 10 m is an arm drawn across a jump (it stays in its line, running
    // down, for as long as its points live: a reader that polls sees it).
    std::uint32_t FrontBreaks() const { return frontBreaks_.load(std::memory_order_relaxed); }
    float FrontLongestLit() const { return frontLongestLit_.load(std::memory_order_relaxed); }

private:
    static constexpr std::uint32_t kMaxHulls = 16u;
    // The breaking bow waves: MAX_FRONTS / MAX_FRONT_POINTS of ocean_wake_cs.hlsl -- MUST MATCH.
    static constexpr std::uint32_t kMaxFronts = 8u;
    static constexpr std::uint32_t kMaxFrontPoints = 128u;
    static constexpr std::uint32_t kFrontPiece = 16u; // points a piece of a front (FRONT_PIECE): each has its own box
    static constexpr std::uint32_t kMaxFootprints = 16u;
    static constexpr float kSnapTexels = 8.0f;
    static constexpr float kLingerSeconds = 30.0f; // a wake outlives its boat this long, then the field is let go
    static constexpr float kMaxStep = 1.0f / 20.0f; // seconds: a hitch does not teleport the hull's pressure

    std::uint32_t FootprintSlot(const std::shared_ptr<const buoyancy::HullFootprint>& footprint);

    GpuResource work_;   // x,y: h + i v in space, then their spectrum; z,w: the hull's pressure, then its spectrum
    GpuResource field_;  // the stepped spectrum at 1 and 3 m, then the field in space: x height, y vertical velocity,
                         // z the free height, w the head
    GpuResource spec_;   // the stepped spectrum at 7.5 m and in deep water
    GpuResource free_;   // the free height, R16F: the readback's copy
    GpuResource jac_;    // the free height's Jacobian terms (crest foam): spectrum, then space
    GpuResource foam_[2]; // RGBA16F, in turn: last step's and this step's (x foam, yz the way it was laid times it)
    Microsoft::WRL::ComPtr<ID3D12Resource> footprints_; // upload heap, persistently mapped: kMaxFootprints grids
    float* footprintsMapped_ = nullptr;
    std::uint32_t footprintFloats_ = 0;  // floats a slot
    std::map<const buoyancy::HullFootprint*, std::uint32_t> footprintSlots_;
    std::vector<std::shared_ptr<const buoyancy::HullFootprint>> footprintKeep_; // a slot's grid outlives its pointer's key
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE workUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE fieldUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE workFftUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE fieldFftUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE specUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE specFftUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE freeUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE jacUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE jacFftUav_{};
    D3D12_CPU_DESCRIPTOR_HANDLE foamUav_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE foamSrv_[2]{};
    D3D12_CPU_DESCRIPTOR_HANDLE fieldSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE footprintsSrv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE shoreNullSrv_{}; // a type-correct null Texture2D<float> for a frame without a shore map
    std::shared_ptr<Material> packMaterial_;
    std::shared_ptr<Material> stepMaterial_;
    std::shared_ptr<Material> composeMaterial_;
    std::shared_ptr<Material> fftMaterial_;

    std::vector<ocean::WakeHull> hulls_;
    Math::float2 center_ = Math::float2(0.0f, 0.0f);
    Math::float2 pendingCenter_ = Math::float2(0.0f, 0.0f);
    int pendingShiftX_ = 0;
    int pendingShiftY_ = 0;
    float texel_ = 0.5f;
    float previousTime_ = -1.0f; // the ocean clock the field stands at (its last step)
    float stepTime_ = -1.0f;     // the same, for SecondsSinceStep (-1 = never stepped)
    float lastHullTime_ = -1.0e9f;
    bool hasCenter_ = false;
    bool needsClear_ = true;
    bool created_ = false;
    bool everSimulated_ = false;
    bool active_ = false;
    std::uint64_t ranFrame_ = 0; // the renderer's total frame number whose graph recorded the pass
    std::uint32_t foamNext_ = 0; // which of foam_ this step writes (and the surface reads)
    std::vector<Math::float2> lastHullPositions_; // last step's, to tell a hull's own motion from a jump

    // The breaking bow wave's FRONTS (2026-11-10, owner: "two more trails, as in the aerial photographs"). A planing hull
    // throws a sheet off each chine and a diverging wave behind it that breaks as it runs outward: from above, two arms
    // of white water that leave the bow's shoulders and open at some ten degrees, their outer edge sharp and scalloped,
    // lace behind them. The field's own waves cannot break there (a linear pressure at 0.5 m texels has no chine), so
    // the breaking is carried by points: one leaves each side of the waterline every kFrontSpacing seconds and runs
    // outward ACROSS THE WAY THE HULL HEADED THEN, in the world -- thrown fast for a third of a second (the sheet), then
    // at the diverging wave's own speed -- its breaking running down with its age. The line through a side's points is
    // that side's front. The foam's Pack draws the breaker ON it every step (the foam texture's w: a band with a sharp
    // toe outward and a tail behind, not kept -- it is where the front is now) and lays a thinner residue under it into
    // the foam that stays (the lace between the arms and the trail). Laid whole into the kept foam, every texel a front
    // had passed stayed white for seconds and the arms and the trail were one solid wedge. Kept in the world, so a turn
    // leaves the arms on the path the hull took (lines computed from the heading NOW swung off it: the first spray
    // foam's flaw).
    struct FrontPoint
    {
        Math::float2 position = Math::float2(0.0f, 0.0f); // world XZ
        Math::float2 outward = Math::float2(1.0f, 0.0f);  // unit: across the hull's way when it left, away from the hull
        float spread = 0.0f;   // m/s outward for good: the diverging wave's
        float thrown = 0.0f;   // m/s outward on top at birth, gone in kFrontThrowSeconds: the sheet off the chine
        float strength = 0.0f; // foam a texel takes as it passes, at birth
        float age = 0.0f;      // s
    };
    struct Front
    {
        const void* hull = nullptr;
        int side = 0;
        std::vector<FrontPoint> points; // newest first
        float sinceRelease = 1.0e9f;    // s since a point last left
        float wobble = 0.0f;            // the scallops: a slow random walk a leaving point takes its speed from
        std::uint32_t random = 1u;
        bool seen = false;
        bool open = false;              // a bow wave is leaving this side: its line runs to the hull
        bool ending = false;            // the wave stopped a moment ago: `end` waits to see whether it is a jump
        FrontPoint end;                 // a point of no strength made where it stopped; it runs on with the others
    };
    struct FrontSource // a hull's side this step
    {
        const void* hull = nullptr;
        int side = 0;
        Math::float2 root = Math::float2(0.0f, 0.0f);
        Math::float2 outward = Math::float2(1.0f, 0.0f);
        float share = 0.0f;
        float speed = 0.0f;
        float length = 0.0f;
    };
    struct RecordPass;
    static FrontPoint BirthOf(const FrontSource& source, const Tuning& tuning, float spreadJitter, float throwJitter,
                              float strengthJitter);
    void StepFronts(float dt, bool keep, const std::vector<FrontSource>& sources, const Tuning& tuning, RecordPass& r);
    std::vector<Front> fronts_;
    std::atomic<std::uint32_t> frontsAlive_{ 0u };
    std::atomic<std::uint32_t> frontPoints_{ 0u };
    std::atomic<std::uint32_t> frontsDrawn_{ 0u };
    std::atomic<std::uint32_t> frontBreaks_{ 0u };
    std::atomic<float> frontLongestLit_{ 0.0f };

    // BuildPass' decisions for its record body (read, not captured: std::function keeps 48 bytes inline).
    struct HullGpu
    {
        float posX, posZ, fwdX, fwdZ;
        float scale, u0, v0, invCellU;
        float invCellV, along, across, offset;
    };
    struct RecordPass
    {
        float dt = 0.0f;
        int shiftX = 0;
        int shiftY = 0;
        bool clear = false;
        Math::float2 center = Math::float2(0.0f, 0.0f);
        float texel = 0.5f;
        Tuning tuning{};
        std::uint32_t hullCount = 0;
        HullGpu hulls[kMaxHulls]{};
        std::uint32_t frontCount = 0;
        Math::float4 frontBounds[kMaxFronts * (kMaxFrontPoints / kFrontPiece)]{}; // a piece's (min x, min z, max x, max z), the band's reach in
        Math::float4 frontInfo[kMaxFronts]{};    // (points, which side of the line is outward +-1, the residue's rate factor, 0)
        Math::float4 frontPoints[kMaxFronts * kMaxFrontPoints]{}; // (x, z, the band's foam, half width)
        ShoreWindow shore{};
        bool hasShore = false;
        bool wash = false;         // the hulls moved as boats do this step: no new or jumped one
        bool frontsLay = false;    // the bow waves' fronts draw and lay foam this step (time passed, the fronts kept)
        std::uint32_t foamNext = 0;
        std::uint32_t uavPoint = 0;
    };
    RecordPass record_;
};
