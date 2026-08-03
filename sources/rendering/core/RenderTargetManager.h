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
class RenderTargetManager
{
public:
    struct DeferredTargets {
        static constexpr size_t kResourceCount =
#if WITH_EDITOR
            23; // Runtime targets plus the editor-only objectID target.
#else
            22;
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
    };

    struct Sizes {
        UINT renderWidth = 1, renderHeight = 1;     // internal render resolution
        UINT displayWidth = 1, displayHeight = 1;   // window resolution (tonemap/FXAA/DLSS out)
        UINT reflectionWidth = 1, reflectionHeight = 1;
        UINT oceanReflectionWidth = 1, oceanReflectionHeight = 1;
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
    enum class DeferredSrvSlot : UINT { GB0, GB1, GB2, GBVelocity, GBAux, Depth, Stencil, DepthCopy, Light, LightUAV, Scene, SceneUAV, SceneOpaque, Reflection, ReflectionScratch, OceanReflection, Shadow, SpotShadow, PointShadow, ReflectionUAV, ReflectionScratchUAV, OceanReflectionUAV, Tonemap, TonemapUAV, Fxaa, FxaaUAV, DLSSOutput, DLSSOutputUAV, GlassReflNormal, GlassReflDepth, GlassReflection, GlassReflectionUAV, Count };
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
