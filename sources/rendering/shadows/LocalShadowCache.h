#pragma once

// Local-light shadow plan S7 (docs/local_light_shadows_plan.md): UE's whole-scene shadow cache for the atlas modes'
// spot / point lights (r.Shadow.CacheWholeSceneShadows, ShadowSetup.cpp:3867-4090). A light's STATIC casters are
// rendered once into a persistent pool and copied into its atlas rectangle every frame; only the movable ones are
// drawn on top (SDCM_MovablePrimitivesOnly). A miss -- the light moved, its resolution changed, the static set
// changed, or it is new -- renders the statics into the pool first (SDCM_StaticPrimitivesOnly). What is "static" is
// decided per caster in ShadowGpuData (CasterLod bit 0x100): moved recently, IsDynamicCaster, wind or GI = movable,
// UE's IsMeshShapeOftenMoving with r.Shadow.CacheWPOPrimitives 0 (a swaying palm is WPO there).
//
// The pool is pages of PageSize^2 (D16), carved by a buddy allocator into power-of-two squares: a spot's square of its
// resolution, a point light's six faces as one 3f x 2f block (or, while 4f exceeds a page, six face squares). Its own
// descriptor heaps, so the per-frame deferred heaps do not grow. A light with nothing movable in reach is read straight
// from it (Scene::PrepareViews, UE's SDCM_MovablePrimitivesOnly without subject primitives): no copy, no draw.
// The CPU side runs on the main thread: Scene requests every shadowed view after the atlas pack, the cache pass's
// builder commits the fills it recorded.

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "rendering/core/ResourceDeclarations.h"

class Material;
class Renderer;

namespace render {

class LocalShadowCache
{
public:
    enum class Mode : std::uint8_t { Uncached = 0, Hit = 1, Miss = 2 };
    struct Rect
    {
        std::int32_t page = -1;
        std::uint32_t x = 0, y = 0, size = 0;
    };
    // What a cached depth was rendered with. Equal key = the same picture. `id` names the view: the light's slot in
    // LightManager's arrays, the face and the kind (LocalShadowCache::SpotId / PointId).
    struct Key
    {
        std::uint32_t id = 0;
        std::array<float, 32> viewProj{}; // the view matrix, then the projection (compared exactly, as UE compares)
        std::uint32_t res = 0;
        // What the cached depth MEANS: plan S4, bit 0 = the linear depth (render::SpotShadowLinearDepth /
        // PointShadowLinearDepth), else the perspective one. The switches flip it under an unchanged projection.
        // Bit 1: the light is past render::g_localShadowWindRange -- its swaying casters are in the picture, rigid.
        std::uint32_t encoding = 0;
    };
    static constexpr std::uint32_t SpotId(std::uint32_t lightIndex) { return (lightIndex << 4) | 0xFu; }
    static constexpr std::uint32_t PointId(std::uint32_t lightIndex, std::uint32_t face) { return (lightIndex << 4) | face; }
    // A point light's six faces as ONE entry: a 3f x 2f block, face k at (k % 3, k / 3) faces from its corner -- the
    // atlas page's layout, so a receiver can read the light straight from the pool (Request with pointBlock).
    static constexpr std::uint32_t PointBlockId(std::uint32_t lightIndex) { return (lightIndex << 4) | 0xEu; }
    // The pool's resting state: the copies (pixel shaders) and the direct reads (the spot / point light passes and the
    // fog are compute, glass is a pixel shader) all read it there.
    static constexpr D3D12_RESOURCE_STATES kRestState =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

    // ---- GPU side: the pool (recreated only where the GPU is idle -- Scene::ReconcileShadowMode) -------------------
    // Recreation empties the cache. `pages` 0 frees it.
    void EnsurePool(ID3D12Device* dev, ResourceDeclarations decls, std::uint32_t pageSize, std::uint32_t pages);
    ID3D12Resource* Pool() const { return pool_.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE PoolSrv() const;
    std::uint32_t ResidentPages() const { return pages_; }
    std::uint32_t PageSize() const { return pageSize_; }
    // The rectangle as the depth target: DSV of its page, viewport + scissor = the rectangle, cleared to 1 on request.
    void BindTarget(ID3D12GraphicsCommandList* cl, const Rect& r, bool clear) const;
    // shaders/shadow_cache_copy.hlsl: a depth-only PSO writing SV_Depth from a pool rectangle into the bound target's
    // viewport (D3D12 copies depth only as whole subresources; UE copies its cached maps with a draw too).
    Material* CopyMaterial(Renderer* renderer);

    // ---- CPU side, per frame, main thread -----------------------------------------------------------------------
    // A changed `staticVersion` (ShadowGpuData::LocalStaticVersion) invalidates every entry.
    void BeginFrame(std::uint64_t frame, std::uint32_t staticVersion);
    // `pointBlock`: key.res is a FACE's size and the entry a point light's 3f x 2f block (PointBlockId; its corner and
    // the face size come back in `out`), placed in one buddy square of 4f with the unused quarter-squares handed back --
    // only while 4f fits a page (PointBlockFits).
    Mode Request(const Key& key, Rect& out, bool pointBlock = false);
    bool PointBlockFits(std::uint32_t faceRes) const { return faceRes >= kMinBlock && 4u * faceRes <= pageSize_; }
    // The misses requested this frame got their fill recorded (`ran`), or did not and stay invalid.
    void CommitFills(bool ran);
    // This frame's misses hold their statics (CommitFills(true)): the passes may copy a miss, not only a hit.
    bool FillsRan() const { return fillsRan_; }
    // Pages the cache could have used this frame (a request found no room even after evicting what this frame did not
    // use); Scene grows the pool to it at the next reconcile, up to its cap.
    std::uint32_t WantedPages() const { return wantedPages_; }
    struct Stats
    {
        std::uint32_t hits = 0, misses = 0, uncached = 0, evicted = 0;
        std::uint32_t entries = 0;
        std::uint32_t direct = 0; // lights read straight from the pool this frame (Scene's CountDirect)
    };
    const Stats& FrameStats() const { return stats_; }
    void CountDirect() { ++stats_.direct; }

private:
    struct Block
    {
        std::uint16_t page = 0;
        std::uint16_t x = 0, y = 0; // in units of kMinBlock
    };
    struct Entry
    {
        Key key{};
        Rect rect{};
        bool valid = false;
        bool pending = false;
        bool block = false; // a point light's 3f x 2f block (rect = its corner, size = one face)
        std::uint64_t lastUsed = 0;
    };

    void ResetAllocator();
    bool Allocate(std::uint32_t size, Rect& out);
    bool AllocatePointBlock(std::uint32_t faceRes, Rect& out);
    void Free(const Rect& r);
    void FreeEntry(const Entry& e);
    std::uint32_t LevelOf(std::uint32_t size) const;

    // GPU
    GpuResource pool_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsvHeap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeap_;
    std::uint32_t dsvIncrement_ = 0;
    std::uint32_t pageSize_ = 0;
    std::uint32_t pages_ = 0;
    std::shared_ptr<Material> copyMat_;
    bool copyMatTried_ = false;

    // CPU
    static constexpr std::uint32_t kMinBlock = 32u;
    std::vector<std::vector<Block>> freeLists_; // per level, 0 = a whole page
    std::unordered_map<std::uint32_t, Entry> entries_;
    std::uint64_t frame_ = 0;
    std::uint32_t staticVersion_ = 0;
    std::uint32_t wantedPages_ = 0;
    bool fillsRan_ = false;
    Stats stats_{};
};

} // namespace render
