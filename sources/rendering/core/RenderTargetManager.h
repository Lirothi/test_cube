#pragma once

#include <d3d12.h>
#include <array>
#include <wrl/client.h>

#include "rendering/lighting/LightManager.h"
#include "rendering/core/ResourceStateTracker.h"
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
        static constexpr size_t kResourceCount = 24; // gb0,gb1,gb2,gbVelocity,objectID,depth,depthCopy,light,scene,sceneOpaque,dlssBias,tonemap,fxaa,reflection,reflectionScratch,oceanReflection,shadow,spotShadow,pointShadow,pointShadowDepth,dlssOutput,glassReflNormal,glassReflDepth,glassReflection
        // Resources
        Microsoft::WRL::ComPtr<ID3D12Resource> gb0;   // albedo+metal
        Microsoft::WRL::ComPtr<ID3D12Resource> gb1;   // normalOcta+rough
        Microsoft::WRL::ComPtr<ID3D12Resource> gb2;   // emissive
        Microsoft::WRL::ComPtr<ID3D12Resource> gbVelocity; // motion vectors
        Microsoft::WRL::ComPtr<ID3D12Resource> objectID; // editor object id (0 = none)
        Microsoft::WRL::ComPtr<ID3D12Resource> depth;
        Microsoft::WRL::ComPtr<ID3D12Resource> depthCopy; // Copy of depth before transparent pass
        Microsoft::WRL::ComPtr<ID3D12Resource> light;
        Microsoft::WRL::ComPtr<ID3D12Resource> scene;
        Microsoft::WRL::ComPtr<ID3D12Resource> sceneOpaque; // Copy of opaque scene color for refraction
        Microsoft::WRL::ComPtr<ID3D12Resource> dlssBias;
        Microsoft::WRL::ComPtr<ID3D12Resource> tonemap; // Tonemap output (R8G8B8A8)
        Microsoft::WRL::ComPtr<ID3D12Resource> fxaa;    // FXAA output (R8G8B8A8)
        Microsoft::WRL::ComPtr<ID3D12Resource> reflection;        // premultiplied; compose samples this after blur
        Microsoft::WRL::ComPtr<ID3D12Resource> reflectionScratch; // ping-pong/scratch target for reflection filtering
        Microsoft::WRL::ComPtr<ID3D12Resource> oceanReflection;   // premultiplied ocean SSR sampled by transparent ocean
        Microsoft::WRL::ComPtr<ID3D12Resource> shadow; // R16_TYPELESS atlas (DSV=D16, SRV=R16)
        Microsoft::WRL::ComPtr<ID3D12Resource> spotShadow; // R16_TYPELESS array for spot lights
        Microsoft::WRL::ComPtr<ID3D12Resource> pointShadow;      // R16_FLOAT cube array (6*kMaxShadowedPointLights slices): linear-distance point shadows
        Microsoft::WRL::ComPtr<ID3D12Resource> pointShadowDepth; // D16 scratch depth shared across cube faces (rasterization only)
        Microsoft::WRL::ComPtr<ID3D12Resource> dlssOutput; // scene color format, upscaled
        // S15 off-screen glass reflections: a reflection-res glass G-buffer (front-face
        // normal + depth) feeding a second rt_reflections_cs dispatch into glassReflection.
        Microsoft::WRL::ComPtr<ID3D12Resource> glassReflNormal; // glass front-face world normal
        Microsoft::WRL::ComPtr<ID3D12Resource> glassReflDepth;  // glass front-face depth (R32 SRV)
        Microsoft::WRL::ComPtr<ID3D12Resource> glassReflection; // premultiplied glass reflection

        // CPU descriptors
        D3D12_CPU_DESCRIPTOR_HANDLE gbRTV[4]{};
        D3D12_CPU_DESCRIPTOR_HANDLE objectIDRTV{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        D3D12_CPU_DESCRIPTOR_HANDLE gbSRV[4]{}; // GB0,GB1,GB2,GBVelocity
        D3D12_CPU_DESCRIPTOR_HANDLE depthSRV{};  // Depth(R32F)
        D3D12_CPU_DESCRIPTOR_HANDLE stencilSRV{}; // Stencil(G8)
        D3D12_CPU_DESCRIPTOR_HANDLE depthCopySRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE lightRTV{}, lightSRV{}, lightUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneRTV{}, sceneSRV{}, sceneUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE sceneOpaqueSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE dlssBiasRTV{}, dlssBiasSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE tonemapSRV{}, tonemapUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE fxaaSRV{}, fxaaUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE reflectionSRV{}, reflectionUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE reflectionScratchSRV{}, reflectionScratchUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE oceanReflectionSRV{}, oceanReflectionUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE shadowDSV{}, shadowSRV{};
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, LightManager::kMaxShadowedSpotLights> spotShadowDSV{};
        D3D12_CPU_DESCRIPTOR_HANDLE spotShadowSRV{};
        // One RTV per cube face (6 * kMaxShadowedPointLights); one shared depth DSV; one cube-array SRV.
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 6 * LightManager::kMaxShadowedPointLights> pointShadowRTV{};
        D3D12_CPU_DESCRIPTOR_HANDLE pointShadowDSV{};
        D3D12_CPU_DESCRIPTOR_HANDLE pointShadowSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE dlssOutputSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE dlssOutputUAV{};
        D3D12_CPU_DESCRIPTOR_HANDLE glassReflNormalRTV{}, glassReflNormalSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE glassReflDepthDSV{}, glassReflDepthSRV{};
        D3D12_CPU_DESCRIPTOR_HANDLE glassReflectionSRV{}, glassReflectionUAV{};

        UINT shadowRes = 4096; // atlas 4096x4096, tile size 2048
        UINT spotShadowRes = 512;
        UINT pointShadowRes = 512; // per cube face
    };

    struct Formats {
        DXGI_FORMAT gb0;
        DXGI_FORMAT gb1;
        DXGI_FORMAT gb2;
        DXGI_FORMAT velocity;
        DXGI_FORMAT objectID;
        DXGI_FORMAT depth;
        DXGI_FORMAT depthSrv;
        DXGI_FORMAT light;
        DXGI_FORMAT sceneColor;
        DXGI_FORMAT dlssBias;
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

    void Create(ID3D12Device* dev, const Formats& formats, const Sizes& sizes, ResourceStateTracker& tracker);
    void Destroy(ResourceStateTracker& tracker);

    bool IsCreated() const { return deferredRtvHeap_ != nullptr; }
    DeferredTargets& Deferred(UINT frame) { return deferred_[frame]; }
    const DeferredTargets& Deferred(UINT frame) const { return deferred_[frame]; }

private:
    enum class DeferredRtvSlot : UINT { GB0, GB1, GB2, GBVelocity, ObjectID, Light, Scene, DlssBias, GlassReflNormal, Count };
    enum class DeferredSrvSlot : UINT { GB0, GB1, GB2, GBVelocity, Depth, Stencil, DepthCopy, Light, LightUAV, Scene, SceneUAV, SceneOpaque, DlssBias, Reflection, ReflectionScratch, OceanReflection, Shadow, SpotShadow, PointShadow, ReflectionUAV, ReflectionScratchUAV, OceanReflectionUAV, Tonemap, TonemapUAV, Fxaa, FxaaUAV, DLSSOutput, DLSSOutputUAV, GlassReflNormal, GlassReflDepth, GlassReflection, GlassReflectionUAV, Count };
    enum class DeferredDsvSlot : UINT { Depth, Shadow, GlassReflDepth, PointShadowDepth, Count };

    // The point shadow atlas needs one RTV per cube face; reserve that block after the
    // named RTV slots (mirrors how the spot shadow DSVs sit after the named DSV slots).
    static constexpr UINT kDeferredRtvPerFrame = (UINT)DeferredRtvSlot::Count + 6 * LightManager::kMaxShadowedPointLights;
    static constexpr UINT kDeferredSrvPerFrame = (UINT)DeferredSrvSlot::Count;
    static constexpr UINT kDeferredDsvPerFrame = (UINT)DeferredDsvSlot::Count + LightManager::kMaxShadowedSpotLights;

    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvAt(UINT idx) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredRtvCPU(UINT frame, DeferredRtvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSrvCPU(UINT frame, DeferredSrvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredDsvCPU(UINT frame, DeferredDsvSlot slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredSpotShadowDsvCPU(UINT frame, UINT lightIndex) const;
    D3D12_CPU_DESCRIPTOR_HANDLE DeferredPointShadowRtvCPU(UINT frame, UINT faceIndex) const;

    // CPU heaps for offscreen resources
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> deferredRtvHeap_;   // RTV shared across frames
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> deferredDsvHeap_;   // DSV shared across frames
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> deferredSrvCpuHeap_;// SRV CPU-only
    UINT deferredRtvIncr_ = 0, deferredDsvIncr_ = 0, deferredSrvIncr_ = 0;

    // Per-frame sets
    DeferredTargets deferred_[render::kFrameCount];
};
