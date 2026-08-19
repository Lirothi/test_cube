#pragma once

#include <d3d12.h>
#include <array>
#include <wrl/client.h>

#include "rendering/lighting/LightManager.h"
#include "rendering/core/ResourceDeclarations.h"
#include "rendering/core/RenderConstants.h"
#include "core/math/Math.h"

// Owns the per-frame deferred render targets (G-buffer, light/scene color,
// shadow atlases, reflection/tonemap/FXAA/DLSS intermediates) and the CPU-only
// RTV/DSV/SRV descriptor heaps that view them. Formats and sizes are supplied
// by the caller (Renderer), which remains the single home for them.
// P6C: enough levels to take a 4K render resolution's half-res mip 0 (1920x1080) down to 1x1,
// which needs 11; 13 leaves headroom and costs nothing but descriptor slots.
inline constexpr unsigned kHzbMaxMips = 13;

// P8: bloom pyramid levels. Mip 0 is half the DISPLAY resolution (bloom runs after the upscaler),
// so 8 levels take 1280x720 down to 5x3 -- past that the taps are wider than the screen and the
// level contributes a constant. Same shape as the HZB chain: one SRV over the whole thing, one UAV
// per mip, and the whole chain sits in UNORDERED_ACCESS while it is built.
inline constexpr unsigned kBloomMaxMips = 8;

// Inspector preview surface. Square and fixed: the inspector draws it with the SOURCE aspect
// ratio, so this only sets sampling density, and a fixed size means no reallocation when the
// selected target changes.
inline constexpr unsigned kDebugPreviewSize = 1024;

class RenderTargetManager
{
public:
    struct DeferredTargets {
        static constexpr size_t kResourceCount =
#if WITH_EDITOR
            26; // Runtime targets plus the editor-only objectID target.
#else
            25;
#endif
        // Resources
        GpuResource gb0;   // albedo+metal
        GpuResource gb1;   // encoded normal RGB; A2 remains unused
        GpuResource gb2;   // emissive
        GpuResource gbVelocity; // motion vectors
        GpuResource gbAux; // AO, indirect specular scale, shading model
#if WITH_EDITOR
        GpuResource objectID; // editor object id (0 = none)
#endif
        GpuResource depth;
        GpuResource depthCopy; // Copy of depth before transparent pass
        GpuResource light;
        GpuResource scene;
        GpuResource sceneOpaque; // Copy of opaque scene color for refraction
        GpuResource tonemap; // Tonemap output (R8G8B8A8)
        GpuResource fxaa;    // FXAA output (R8G8B8A8)
        GpuResource reflection;        // premultiplied; compose samples this after blur
        GpuResource reflectionScratch; // ping-pong/scratch target for reflection filtering
        GpuResource oceanReflection;   // premultiplied ocean SSR sampled by transparent ocean
        GpuResource shadow; // R16_TYPELESS atlas (DSV=D16, SRV=R16)
        GpuResource spotShadow; // R16_TYPELESS array for spot lights
        GpuResource pointShadow; // R16_TYPELESS cube array (6*kMaxShadowedPointLights slices), DSV=D16/SRV=R16: depth-cube point shadows
        GpuResource dlssOutput; // scene color format, upscaled
        // S15 off-screen glass reflections: a reflection-res glass G-buffer (front-face
        // normal + depth) feeding a second rt_reflections_cs dispatch into glassReflection.
        GpuResource glassReflNormal; // glass front-face world normal
        GpuResource glassReflDepth;  // glass front-face depth (R32 SRV)
        GpuResource glassReflection; // premultiplied glass reflection
        // P6B: half-resolution screen-space ambient occlusion (GTAO). Half res because the signal
        // is low frequency and the pass is bandwidth bound; the edge-aware upsample is what puts it
        // back on geometry edges. R8_UNORM: AO is a visibility fraction in [0,1] and 8 bits of it
        // is below the noise of any screen-space estimate.
        GpuResource gtao;
        // P6B items 3-5: the rest of the AO chain. `gtaoFiltered` is the bilateral denoise output,
        // `gtaoHistory` the temporal accumulation (this frame's result AND next frame's history --
        // the previous frame's copy lives in the previous Deferred set, see
        // Renderer::GetDeferredForPrevFrame), and `gtaoUpsampled` the render-resolution result the
        // lighting/compose consumers will sample.
        GpuResource gtaoFiltered;
        GpuResource gtaoHistory;
        GpuResource gtaoUpsampled;
        // SSR temporal resolve: the accumulated reflection. This frame's output AND next frame's
        // history (the previous frame's copy lives in the previous Deferred set -- see
        // Renderer::GetDeferredForPrevFrame), exactly like gtaoHistory.
        GpuResource reflectionHistory;
        // P6C: hierarchical depth pyramid. Mip 0 is HALF the render resolution (each texel reduces
        // a 2x2 depth quad), deliberately the same grid the GTAO chain already runs on. The
        // reduction is MIN of device Z = the FURTHEST surface in the tile under reversed-Z; see the
        // shader for why a horizon search wants that and a ray march wants the opposite.
        GpuResource hzb;
        // P6C step 6: the SECOND pyramid, reduced with MAX of device Z = the CLOSEST surface in the
        // tile. A ray march needs this one and a horizon search needs the other: a march must never
        // skip past a surface the tile really contains, an AO search must never invent one. Same
        // dimensions, same mip count, built by the same dispatch.
        GpuResource hzbClosest;
        // P8: the bloom pyramid's two chains. `bloomDown` holds the thresholded image reduced level
        // by level, `bloomUp` the tent reconstruction on the way back; the upsample of level N reads
        // BOTH up[N+1] and down[N], which is why one ping-ponged chain would not do. Mip 0 of
        // `bloomUp` is what the tonemap samples.
        GpuResource bloomDown;
        GpuResource bloomUp;
        // The texture inspector's preview surface. ImGui can only tint an image by a value it
        // packs to 8 bits, so anything needing to BRIGHTEN a target has to happen before ImGui
        // sees it; the inspector resamples into this and ImGui draws it untinted.
        GpuResource debugPreview;

        // CPU descriptors
        D3D12_CPU_DESCRIPTOR_HANDLE gbRTV[4]{};
        D3D12_CPU_DESCRIPTOR_HANDLE gbAuxRTV{};
#if WITH_EDITOR
        D3D12_CPU_DESCRIPTOR_HANDLE objectIDRTV{};
#endif
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        D3D12_CPU_DESCRIPTOR_HANDLE gbSRV[4]{}; // GB0,GB1,GB2,GBVelocity
        D3D12_CPU_DESCRIPTOR_HANDLE gbAuxSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE depthSRV{};  // Depth(R32F)
        D3D12_CPU_DESCRIPTOR_HANDLE stencilSRV{}; // Stencil(G8)
        D3D12_CPU_DESCRIPTOR_HANDLE depthCopySRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE lightRTV{}, lightSRV{}, lightUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRTV{}, sceneSRV{}, sceneUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneOpaqueSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE tonemapSRV{}, tonemapUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE fxaaSRV{}, fxaaUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE reflectionSRV{}, reflectionUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE reflectionScratchSRV{}, reflectionScratchUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE reflectionHistorySRV{}, reflectionHistoryUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE oceanReflectionSRV{}, oceanReflectionUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDSV{}, shadowSRV{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, LightManager::kMaxShadowedSpotLights> spotShadowDSV{};
        D3D12_CPU_DESCRIPTOR_HANDLE spotShadowSRV{};
        // One DSV per cube face (6 * kMaxShadowedPointLights); one cube-array depth SRV.
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6 * LightManager::kMaxShadowedPointLights> pointShadowDSV{};
        D3D12_CPU_DESCRIPTOR_HANDLE pointShadowSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE dlssOutputSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE dlssOutputUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE glassReflNormalRTV{}, glassReflNormalSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE glassReflDepthDSV{}, glassReflDepthSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE glassReflectionSRV{}, glassReflectionUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE gtaoSRV{}, gtaoUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE gtaoFilteredSRV{}, gtaoFilteredUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE gtaoHistorySRV{}, gtaoHistoryUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE gtaoUpsampledSRV{}, gtaoUpsampledUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE hzbSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE hzbClosestSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE debugPreviewSRV{}, debugPreviewUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE debugPreviewSrcSRV{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kHzbMaxMips> hzbMipUAV{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kHzbMaxMips> hzbClosestMipUAV{};
        UINT hzbMips = 0;                 // levels actually created for the current size
        UINT hzbWidth = 1, hzbHeight = 1; // mip 0 dimensions

        // P8 bloom pyramid, at DISPLAY resolution: it runs after the upscaler, on the same image
        // the tonemap reads.
        D3D12_CPU_DESCRIPTOR_HANDLE bloomDownSRV{}, bloomUpSRV{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBloomMaxMips> bloomDownMipUAV{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBloomMaxMips> bloomUpMipUAV{};
        UINT bloomMips = 0;
        UINT bloomWidth = 1, bloomHeight = 1; // mip 0 dimensions

        UINT shadowRes = 4096; // atlas 4096x4096, tile size 2048
        UINT spotShadowRes = 512;
        UINT pointShadowRes = 256; // per cube face
    };

    struct Formats {
        DXGI_FORMAT gb0;
        DXGI_FORMAT gb1;
        DXGI_FORMAT gb2;
        DXGI_FORMAT gbAux;
        DXGI_FORMAT velocity;
#if WITH_EDITOR
        DXGI_FORMAT objectID;
#endif
        DXGI_FORMAT depth;
        DXGI_FORMAT depthSrv;
        DXGI_FORMAT light;
        DXGI_FORMAT sceneColor;
        DXGI_FORMAT reflection;
        DXGI_FORMAT reflectionScratch;
        DXGI_FORMAT oceanReflection;
        DXGI_FORMAT backbufferResource; // tonemap/FXAA targets
        DXGI_FORMAT gtao;               // P6B ambient occlusion
        DXGI_FORMAT hzb;                // P6C hierarchical depth
        DXGI_FORMAT bloom;              // P8 bloom pyramid (HDR)
        DXGI_FORMAT debugPreview;       // texture-inspector preview (RGBA8)
    };

    struct Sizes {
        UINT renderWidth = 1, renderHeight = 1;     // internal render resolution
        UINT displayWidth = 1, displayHeight = 1;   // window resolution (tonemap/FXAA/DLSS out)
        UINT reflectionWidth = 1, reflectionHeight = 1;
        UINT oceanReflectionWidth = 1, oceanReflectionHeight = 1;
        UINT gtaoWidth = 1, gtaoHeight = 1;
        UINT hzbWidth = 1, hzbHeight = 1;   // P6C mip 0 (half the render resolution)
        // P8 mip 0, half the DISPLAY resolution: bloom runs after the upscaler, so it is sized off
        // the image the tonemap actually reads, not off the internal render target.
        UINT bloomWidth = 1, bloomHeight = 1;
    };

    void Create(ID3D12Device* dev, const Formats& formats, const Sizes& sizes, ResourceDeclarations decls);
    void Destroy(ResourceDeclarations decls);

    bool IsCreated() const { return deferredRtvHeap_ != nullptr; }
    DeferredTargets& Deferred(UINT frame) { return deferred_[frame]; }
    const DeferredTargets& Deferred(UINT frame) const { return deferred_[frame]; }

    // Step 24c: shrink/restore the LEGACY local-light shadow atlases (spot + point) — full-res in
    // Legacy mode, 1x1 (negligible memory) in VSM mode, where local shadows come from the VSM pool.
    // The resources stay VALID (just tiny), so every render-graph declaration / SRV bind keeps
    // working with no null handling. MUST be called at GPU idle (the shadow-mode reconcile waits).
    void SetLocalShadowResidency(ID3D12Device* dev, ResourceDeclarations decls, bool full);
    bool IsLocalShadowFull() const { return localShadowFull_; }

private:
    enum class DeferredRtvSlot : UINT {
        GB0, GB1, GB2, GBVelocity, GBAux,
#if WITH_EDITOR
        ObjectID,
#endif
        Light, Scene, GlassReflNormal, Count
    };
    enum class DeferredSrvSlot : UINT { GB0, GB1, GB2, GBVelocity, GBAux, Depth, Stencil, DepthCopy, Light, LightUAV, Scene, SceneUAV, SceneOpaque, Reflection, ReflectionScratch, OceanReflection, Shadow, SpotShadow, PointShadow, ReflectionUAV, ReflectionScratchUAV, OceanReflectionUAV, Tonemap, TonemapUAV, Fxaa, FxaaUAV, DLSSOutput, DLSSOutputUAV, GlassReflNormal, GlassReflDepth, GlassReflection, GlassReflectionUAV, Gtao, GtaoUAV, GtaoFiltered, GtaoFilteredUAV, GtaoHistory, GtaoHistoryUAV, GtaoUpsampled, GtaoUpsampledUAV, ReflectionHistory, ReflectionHistoryUAV,
    // P6C hierarchical depth. One SRV over the whole chain plus one UAV PER MIP: the build writes
    // mip N while reading mip N-1, and this engine's barrier layer transitions whole resources
    // (ALL_SUBRESOURCES), so the chain stays in UNORDERED_ACCESS for the duration of the build and
    // the mips talk to each other through UAVs instead of a per-subresource SRV flip.
    Hzb, HzbClosest,
    DebugPreview, DebugPreviewUAV,
    // The SOURCE the inspector picked. No view is created up front -- the preview pass
    // rebuilds one here each frame for whatever target, mip and swizzle was requested.
    DebugPreviewSrc,
    HzbMipUav0, HzbMipUav1, HzbMipUav2, HzbMipUav3, HzbMipUav4, HzbMipUav5, HzbMipUav6,
    HzbMipUav7, HzbMipUav8, HzbMipUav9, HzbMipUav10, HzbMipUav11, HzbMipUav12,
    HzbClosestMipUav0, HzbClosestMipUav1, HzbClosestMipUav2, HzbClosestMipUav3, HzbClosestMipUav4,
    HzbClosestMipUav5, HzbClosestMipUav6, HzbClosestMipUav7, HzbClosestMipUav8, HzbClosestMipUav9,
    HzbClosestMipUav10, HzbClosestMipUav11, HzbClosestMipUav12,
    // P8 bloom. Two chains: DOWN carries the thresholded image reduced level by level, UP carries
    // the tent reconstruction walking back. They are separate resources rather than one ping-ponged
    // chain because the upsample of level N reads BOTH up[N+1] and down[N].
    BloomDown, BloomUp,
    BloomDownMipUav0, BloomDownMipUav1, BloomDownMipUav2, BloomDownMipUav3,
    BloomDownMipUav4, BloomDownMipUav5, BloomDownMipUav6, BloomDownMipUav7,
    BloomUpMipUav0, BloomUpMipUav1, BloomUpMipUav2, BloomUpMipUav3,
    BloomUpMipUav4, BloomUpMipUav5, BloomUpMipUav6, BloomUpMipUav7,
    Count };
    enum class DeferredDsvSlot : UINT { Depth, Shadow, GlassReflDepth, Count };

    static constexpr UINT kDeferredRtvPerFrame = (UINT)DeferredRtvSlot::Count;
    static constexpr UINT kDeferredSrvPerFrame = (UINT)DeferredSrvSlot::Count;
    // DSV heap per frame: named slots, then the spot-shadow DSV block, then the
    // point-shadow cube DSV block (one DSV per cube face).
    static constexpr UINT kDeferredDsvPerFrame = (UINT)DeferredDsvSlot::Count
        + LightManager::kMaxShadowedSpotLights
        + 6 * LightManager::kMaxShadowedPointLights;

    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvCPU(UINT frame, DeferredRtvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvCPU(UINT frame, DeferredSrvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvCPU(UINT frame, DeferredDsvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSpotShadowDsvCPU(UINT frame, UINT lightIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredPointShadowDsvCPU(UINT frame, UINT faceIndex) const;

    // Step 24c: (re)create the spot / point shadow atlas resource + its SRV/DSV views at `resolution`
    // for frame `f` (operates on deferred_[f]). Extracted from Create()'s lambdas so the residency
    // toggle can rebuild them at 1x1 / full res.
    void CreateShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT f, UINT resolution); // Step 24f-2: CSM atlas
    void CreateSpotShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT f, UINT resolution);
    void CreatePointShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT f, UINT resolution);

    // CPU heaps for offscreen resources
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> deferredRtvHeap_;   // RTV shared across frames
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> deferredDsvHeap_;   // DSV shared across frames
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> deferredSrvCpuHeap_;// SRV CPU-only
    UINT deferredRtvIncr_ = 0, deferredDsvIncr_ = 0, deferredSrvIncr_ = 0;

    // Per-frame sets
    DeferredTargets deferred_[render::kFrameCount];

    bool localShadowFull_ = true; // Step 24c: spot/point atlases at full res (Legacy) vs 1x1 (VSM)
};
