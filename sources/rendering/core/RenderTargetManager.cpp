#include "rendering/core/RenderTargetManager.h"
#include "rendering/core/TextureCreate.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include "core/Helpers.h"

using Microsoft::WRL::ComPtr;
using namespace Math;

D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredRtvAt(UINT idx) const {
    auto h = deferredRtvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(idx) * deferredRtvIncr_; return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredDsvAt(UINT idx) const {
    auto h = deferredDsvHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(idx) * deferredDsvIncr_; return h;
}
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredSrvAt(UINT idx) const {
    auto h = deferredSrvCpuHeap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(idx) * deferredSrvIncr_; return h;
}

D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredRtvCPU(UINT frame, DeferredRtvSlot slot) const {
    const UINT idx = frame * kDeferredRtvPerFrame + static_cast<UINT>(slot);
    return DeferredRtvAt(idx);
}
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredSrvCPU(UINT frame, DeferredSrvSlot slot) const {
    const UINT idx = frame * kDeferredSrvPerFrame + static_cast<UINT>(slot);
    return DeferredSrvAt(idx);
}
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredDsvCPU(UINT frame, DeferredDsvSlot slot) const {
    const UINT idx = frame * kDeferredDsvPerFrame + static_cast<UINT>(slot);
    return DeferredDsvAt(idx);
}
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredSpotShadowDsvCPU(UINT frame, UINT lightIndex) const {
    const UINT base = frame * kDeferredDsvPerFrame + static_cast<UINT>(DeferredDsvSlot::Count);
    return DeferredDsvAt(base + lightIndex);
}
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredPointShadowDsvCPU(UINT frame, UINT faceIndex) const {
    // Point-shadow cube DSVs sit after the named DSV slots AND the spot-shadow DSV block.
    const UINT base = frame * kDeferredDsvPerFrame
        + static_cast<UINT>(DeferredDsvSlot::Count) + LightManager::kMaxShadowedSpotLights;
    return DeferredDsvAt(base + faceIndex);
}

void RenderTargetManager::Create(ID3D12Device* dev, const Formats& formats, const Sizes& sizes, ResourceDeclarations decls)
{
    // Just in case: release old resources/heaps
    Destroy(decls);

    if (!dev) { return; }
    localShadowFull_ = true; // Create always builds full-res spot/point atlases (Step 24c)

    // --- Descriptor increments ---
    deferredRtvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    deferredDsvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    deferredSrvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // --- CPU-only descriptor heaps for offscreen targets (RTV/DSV/SRV) ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = render::kFrameCount * kDeferredRtvPerFrame;
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredRtvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = render::kFrameCount * kDeferredDsvPerFrame;  // Depth
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredDsvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = render::kFrameCount * kDeferredSrvPerFrame;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only staging
        ThrowIfFailed(dev->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&deferredSrvCpuHeap_)));
    }

    // --- Common placement parameters (Default heap) ---
    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    const UINT rtWidth = std::max(1u, sizes.renderWidth);
    const UINT rtHeight = std::max(1u, sizes.renderHeight);
    const UINT displayWidthClamped = std::max(1u, sizes.displayWidth);
    const UINT displayHeightClamped = std::max(1u, sizes.displayHeight);

    UINT currentTargetWidth = rtWidth;
    UINT currentTargetHeight = rtHeight;

    auto MakeTex2DDesc = [&](DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = currentTargetWidth ? currentTargetWidth : 1;
        rd.Height = currentTargetHeight ? currentTargetHeight : 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = fmt;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        rd.Flags = flags;
        return rd;
        };

    // ---- Shared factories ----
    auto CreateRT = [&](D3D12_RESOURCE_STATES canonical, // step 7 prereq: created DIRECTLY in its
                                          // resting state, so creation == canonical
        DXGI_FORMAT fmt,
        DeferredRtvSlot rtvSlot,
        DeferredSrvSlot srvSlot,
        DeferredSrvSlot uavSlot,
        UINT f,
        GpuResource& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outRTV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        float4 clear = float4(0, 0, 0, 0))
        {
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (uavSlot != DeferredSrvSlot::Count)
            {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, flags);

            D3D12_CLEAR_VALUE cv{}; cv.Format = fmt;
            cv.Color[0] = clear.x; cv.Color[1] = clear.y; cv.Color[2] = clear.z; cv.Color[3] = clear.w;

            ThrowIfFailed(render::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                canonical, &cv, outRes.GetAddressOfForCreate()));

            // RTV/SRV — ONLY for frame f
            outRTV = DeferredRtvCPU(f, rtvSlot);
            dev->CreateRenderTargetView(outRes.Get(), nullptr, outRTV);

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            outSRV = DeferredSrvCPU(f, srvSlot);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outSRV);

            D3D12_CPU_DESCRIPTOR_HANDLE outUAV{};
            if (uavSlot != DeferredSrvSlot::Count)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = fmt;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                outUAV = DeferredSrvCPU(f, uavSlot);
                dev->CreateUnorderedAccessView(outRes.Get(), nullptr, &ud, outUAV);
            }

            // Store the handles in deferred_[f]
            auto& D = deferred_[f];
            switch (rtvSlot) {
            case DeferredRtvSlot::GB0:        D.gbRTV[0] = outRTV; break;
            case DeferredRtvSlot::GB1:        D.gbRTV[1] = outRTV; break;
            case DeferredRtvSlot::GB2:        D.gbRTV[2] = outRTV; break;
            case DeferredRtvSlot::GBVelocity: D.gbRTV[3] = outRTV; break;
            case DeferredRtvSlot::GBAux:      D.gbAuxRTV = outRTV; break;
#if WITH_EDITOR
            case DeferredRtvSlot::ObjectID:   D.objectIDRTV = outRTV; break;
#endif
            case DeferredRtvSlot::Light:      D.lightRTV = outRTV; break;
            case DeferredRtvSlot::Scene:      D.sceneRTV = outRTV; break;
            default: break;
            }
            switch (srvSlot) {
            case DeferredSrvSlot::GB0:        D.gbSRV[0] = outSRV; break;
            case DeferredSrvSlot::GB1:        D.gbSRV[1] = outSRV; break;
            case DeferredSrvSlot::GB2:        D.gbSRV[2] = outSRV; break;
            case DeferredSrvSlot::GBVelocity: D.gbSRV[3] = outSRV; break;
            case DeferredSrvSlot::GBAux:      D.gbAuxSRV = outSRV; break;
            case DeferredSrvSlot::Depth:      D.depthSRV = outSRV; break;
            case DeferredSrvSlot::Stencil:    D.stencilSRV = outSRV; break;
            case DeferredSrvSlot::Light:      D.lightSRV = outSRV; break;
            case DeferredSrvSlot::Scene:      D.sceneSRV = outSRV; break;
            default: break;
            }
            if (uavSlot != DeferredSrvSlot::Count)
            {
                switch (uavSlot)
                {
                case DeferredSrvSlot::LightUAV: D.lightUAV = outUAV; break;
                case DeferredSrvSlot::SceneUAV: D.sceneUAV = outUAV; break;
                default: break;
                }
            }

            outRes.DeclareCreated(decls, canonical, nullptr);
        };

#if WITH_EDITOR
    auto CreateObjectIdTarget = [&](UINT f)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(formats.objectID, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);

            D3D12_CLEAR_VALUE cv{};
            cv.Format = formats.objectID;
            cv.Color[0] = 0.0f;
            cv.Color[1] = 0.0f;
            cv.Color[2] = 0.0f;
            cv.Color[3] = 0.0f;

            auto& D = deferred_[f];
            ThrowIfFailed(render::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, D.objectID.GetAddressOfForCreate()));

            D.objectIDRTV = DeferredRtvCPU(f, DeferredRtvSlot::ObjectID);
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = formats.objectID;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            dev->CreateRenderTargetView(D.objectID.Get(), &rtvDesc, D.objectIDRTV);

            D.objectID.DeclareCreated(decls, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
        };
#endif

    auto CreateSrvTexture = [&](D3D12_RESOURCE_STATES canonical, // step 7 prereq: created DIRECTLY in its
                                          // resting state, so creation == canonical
        DXGI_FORMAT fmt,
        DeferredSrvSlot srvSlot,
        UINT f,
        GpuResource& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_NONE);

            ThrowIfFailed(render::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                canonical, nullptr, outRes.GetAddressOfForCreate()));

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFormat == DXGI_FORMAT_UNKNOWN ? fmt : srvFormat;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            outSRV = DeferredSrvCPU(f, srvSlot);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outSRV);

            // Declare BEFORE handing ownership over: a moved-from wrapper is empty, so declaring
            // after the move would silently register nothing.
            outRes.DeclareCreated(decls, canonical, nullptr);

            auto& D = deferred_[f];
            if (srvSlot == DeferredSrvSlot::SceneOpaque)
            {
                D.sceneOpaque = std::move(outRes);
                D.sceneOpaqueSRV = outSRV;
            }
            else if (srvSlot == DeferredSrvSlot::DepthCopy)
            {
                D.depthCopy = std::move(outRes);
                D.depthCopySRV = outSRV;
            }
        };

    auto CreateSrvUavTexture = [&](D3D12_RESOURCE_STATES canonical, // step 7 prereq: created DIRECTLY in its
                                          // resting state, so creation == canonical
        DXGI_FORMAT fmt,
        DeferredSrvSlot srvSlot,
        DeferredSrvSlot uavSlot,
        UINT f,
        GpuResource& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outUAV,
        UINT overrideWidth,
        UINT overrideHeight)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (overrideWidth > 0) { rd.Width = overrideWidth; }
            if (overrideHeight > 0) { rd.Height = overrideHeight; }

            ThrowIfFailed(render::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                canonical, nullptr, outRes.GetAddressOfForCreate()));

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = fmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            outSRV = DeferredSrvCPU(f, srvSlot);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outSRV);

            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = fmt;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

            outUAV = DeferredSrvCPU(f, uavSlot);
            dev->CreateUnorderedAccessView(outRes.Get(), nullptr, &ud, outUAV);

            auto& D = deferred_[f];
            if (srvSlot == DeferredSrvSlot::Reflection)
            {
                D.reflectionSRV = outSRV;
                D.reflectionUAV = outUAV;
            }
            else if (srvSlot == DeferredSrvSlot::ReflectionScratch)
            {
                D.reflectionScratchSRV = outSRV;
                D.reflectionScratchUAV = outUAV;
            }
            else if (srvSlot == DeferredSrvSlot::OceanReflection)
            {
                D.oceanReflectionSRV = outSRV;
                D.oceanReflectionUAV = outUAV;
            }
            else if (srvSlot == DeferredSrvSlot::Tonemap)
            {
                D.tonemapSRV = outSRV;
                D.tonemapUAV = outUAV;
            }
            else if (srvSlot == DeferredSrvSlot::Fxaa)
            {
                D.fxaaSRV = outSRV;
                D.fxaaUAV = outUAV;
            }

            outRes.DeclareCreated(decls, canonical, nullptr);
        };

    auto CreateDepth = [&](D3D12_RESOURCE_STATES canonical, // step 7 prereq: created DIRECTLY in its
                                          // resting state, so creation == canonical
        DXGI_FORMAT dsvFmt,
        DeferredDsvSlot dsvSlot,
        DeferredSrvSlot srvSlot,
        UINT f,
        GpuResource& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outDSV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outDepthSRV,
        float clearDepth = 0.0f,
        DXGI_FORMAT srvFmt = DXGI_FORMAT_UNKNOWN,
        DeferredSrvSlot stencilSrvSlot = DeferredSrvSlot::Count,
        D3D12_CPU_DESCRIPTOR_HANDLE* outStencilSRV = nullptr,
        DXGI_FORMAT stencilSrvFmt = DXGI_FORMAT_UNKNOWN)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(dsvFmt, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

            D3D12_CLEAR_VALUE cv{}; cv.Format = dsvFmt; cv.DepthStencil.Depth = clearDepth; cv.DepthStencil.Stencil = 0;
            ThrowIfFailed(render::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                canonical, &cv, outRes.GetAddressOfForCreate()));

            // DSV
            outDSV = DeferredDsvCPU(f, dsvSlot);
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = dsvFmt;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev->CreateDepthStencilView(outRes.Get(), &dv, outDSV);

            // Create an SRV for depth as R32_FLOAT
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFmt == DXGI_FORMAT_UNKNOWN ? formats.depthSrv : srvFmt;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            outDepthSRV = DeferredSrvCPU(f, srvSlot);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outDepthSRV);

            if (stencilSrvSlot != DeferredSrvSlot::Count && outStencilSRV)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC stencilDesc{};
                stencilDesc.Format = stencilSrvFmt == DXGI_FORMAT_UNKNOWN ? render::kDeferredStencilSrvFormat : stencilSrvFmt;
                stencilDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                stencilDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                stencilDesc.Texture2D.MipLevels = 1;
                stencilDesc.Texture2D.PlaneSlice = 1;

                *outStencilSRV = DeferredSrvCPU(f, stencilSrvSlot);
                dev->CreateShaderResourceView(outRes.Get(), &stencilDesc, *outStencilSRV);
            }

            outRes.DeclareCreated(decls, canonical, nullptr);
        };

    // Step 24f-2: CSM cascade atlas creation lives in CreateShadowResource (below) so the shadow-mode
    // residency toggle can shrink it to 1x1 in VSM mode (directional then comes from the clipmap).

    // Step 24c: spot + point shadow atlas creation lives in CreateSpotShadowResource /
    // CreatePointShadowResource (below) so the shadow-mode residency toggle can rebuild them at
    // 1x1 / full res. The per-frame loop calls them like the other CreateXxx helpers.

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        auto& D = deferred_[f];

        currentTargetWidth = rtWidth;
        currentTargetHeight = rtHeight;
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gb0, DeferredRtvSlot::GB0, DeferredSrvSlot::GB0, DeferredSrvSlot::Count, f, D.gb0, D.gbRTV[0], D.gbSRV[0]);
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gb1, DeferredRtvSlot::GB1, DeferredSrvSlot::GB1, DeferredSrvSlot::Count, f, D.gb1, D.gbRTV[1], D.gbSRV[1]);
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gb2, DeferredRtvSlot::GB2, DeferredSrvSlot::GB2, DeferredSrvSlot::Count, f, D.gb2, D.gbRTV[2], D.gbSRV[2]);
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.velocity, DeferredRtvSlot::GBVelocity, DeferredSrvSlot::GBVelocity, DeferredSrvSlot::Count, f, D.gbVelocity, D.gbRTV[3], D.gbSRV[3]);
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gbAux, DeferredRtvSlot::GBAux, DeferredSrvSlot::GBAux, DeferredSrvSlot::Count,
            f, D.gbAux, D.gbAuxRTV, D.gbAuxSRV, float4(1, 1, 0, 0));
#if WITH_EDITOR
        CreateObjectIdTarget(f);
#endif

        CreateDepth(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.depth, DeferredDsvSlot::Depth, DeferredSrvSlot::Depth, f, D.depth, D.dsv, /*outDepthSRV*/ D.depthSRV,
            0.0f, DXGI_FORMAT_UNKNOWN, DeferredSrvSlot::Stencil, &D.stencilSRV, render::kDeferredStencilSrvFormat);
        CreateSrvTexture(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, formats.depth, DeferredSrvSlot::DepthCopy, f, D.depthCopy, D.depthCopySRV, formats.depthSrv);

        D.shadowRes = 4096; // could be driven by config/parameter
        CreateShadowResource(dev, decls, f, D.shadowRes);

        D.spotShadowRes = 512;
        CreateSpotShadowResource(dev, decls, f, D.spotShadowRes);

        D.pointShadowRes = 256;
        CreatePointShadowResource(dev, decls, f, D.pointShadowRes);

        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.light, DeferredRtvSlot::Light, DeferredSrvSlot::Light, DeferredSrvSlot::LightUAV, f, D.light, D.lightRTV, D.lightSRV);
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.sceneColor, DeferredRtvSlot::Scene, DeferredSrvSlot::Scene, DeferredSrvSlot::SceneUAV, f, D.scene, D.sceneRTV, D.sceneSRV);
        CreateSrvTexture(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, formats.sceneColor, DeferredSrvSlot::SceneOpaque, f, D.sceneOpaque, D.sceneOpaqueSRV);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.reflection, DeferredSrvSlot::Reflection, DeferredSrvSlot::ReflectionUAV, f, D.reflection, D.reflectionSRV, D.reflectionUAV, sizes.reflectionWidth, sizes.reflectionHeight);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.reflectionScratch, DeferredSrvSlot::ReflectionScratch, DeferredSrvSlot::ReflectionScratchUAV, f, D.reflectionScratch, D.reflectionScratchSRV, D.reflectionScratchUAV, sizes.reflectionWidth, sizes.reflectionHeight);
        // SSR temporal history. Same format and size as the reflection buffer it accumulates.
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.reflection, DeferredSrvSlot::ReflectionHistory, DeferredSrvSlot::ReflectionHistoryUAV, f, D.reflectionHistory, D.reflectionHistorySRV, D.reflectionHistoryUAV, sizes.reflectionWidth, sizes.reflectionHeight);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, formats.oceanReflection, DeferredSrvSlot::OceanReflection, DeferredSrvSlot::OceanReflectionUAV, f, D.oceanReflection, D.oceanReflectionSRV, D.oceanReflectionUAV, sizes.oceanReflectionWidth, sizes.oceanReflectionHeight);

        // S15 off-screen glass reflections (reflection res): a glass G-buffer (front-face
        // normal RTV + depth DSV) feeding a second rt_reflections_cs dispatch into glassReflection.
        currentTargetWidth = std::max(1u, sizes.reflectionWidth);
        currentTargetHeight = std::max(1u, sizes.reflectionHeight);
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gb1, DeferredRtvSlot::GlassReflNormal, DeferredSrvSlot::GlassReflNormal, DeferredSrvSlot::Count, f, D.glassReflNormal, D.glassReflNormalRTV, D.glassReflNormalSRV);
        CreateDepth(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.depth, DeferredDsvSlot::GlassReflDepth, DeferredSrvSlot::GlassReflDepth, f, D.glassReflDepth, D.glassReflDepthDSV, D.glassReflDepthSRV);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, formats.reflection, DeferredSrvSlot::GlassReflection, DeferredSrvSlot::GlassReflectionUAV, f, D.glassReflection, D.glassReflectionSRV, D.glassReflectionUAV, sizes.reflectionWidth, sizes.reflectionHeight);

        // P6B: half-resolution AO, sized off the RENDER resolution (not the display one) because it
        // is consumed by lighting/compose, which run before the upscaler.
        currentTargetWidth = std::max(1u, sizes.gtaoWidth);
        currentTargetHeight = std::max(1u, sizes.gtaoHeight);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gtao,
            DeferredSrvSlot::Gtao, DeferredSrvSlot::GtaoUAV, f, D.gtao, D.gtaoSRV, D.gtaoUAV,
            sizes.gtaoWidth, sizes.gtaoHeight);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gtao,
            DeferredSrvSlot::GtaoFiltered, DeferredSrvSlot::GtaoFilteredUAV, f,
            D.gtaoFiltered, D.gtaoFilteredSRV, D.gtaoFilteredUAV,
            sizes.gtaoWidth, sizes.gtaoHeight);
        // The temporal history is the one deferred target read ACROSS frames (frame N samples the
        // copy frame N-1 wrote, from the previous Deferred set). Nothing else about it is special:
        // it rests shader-readable like its siblings, so the compile sees UAV -> SRV inside the
        // pass and SRV -> UAV two frames later, both from ordinary declarations.
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gtao,
            DeferredSrvSlot::GtaoHistory, DeferredSrvSlot::GtaoHistoryUAV, f,
            D.gtaoHistory, D.gtaoHistorySRV, D.gtaoHistoryUAV,
            sizes.gtaoWidth, sizes.gtaoHeight);

        // The upsampled result is at RENDER resolution: it is consumed by lighting and compose,
        // which run before the upscaler.
        //
        // Rests SHADER-READABLE, and that is not a free choice. `Renderer::RenderImGui` shows a
        // target in the texture inspector with
        // `TransitionExplicit(cl, res, GetCanonicalState(res), PIXEL_SHADER_RESOURCE)` — it takes
        // the CANONICAL as the before-state and does not transition back. That is only sound while
        // the canonical shares a barrier LAYOUT with PIXEL_SHADER_RESOURCE, which
        // NON_PIXEL_SHADER_RESOURCE does (both are D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) and
        // UNORDERED_ACCESS does not. Resting this target as a UAV made the inspector leave it in
        // the SHADER_RESOURCE layout while the next frame's compiled barrier still claimed
        // UNORDERED_ACCESS — INCOMPATIBLE_BARRIER_LAYOUT, and a debug-layer break. Pass_Gtao
        // therefore transitions it back here at the end of its chain.
        currentTargetWidth = std::max(1u, sizes.renderWidth);
        currentTargetHeight = std::max(1u, sizes.renderHeight);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gtao,
            DeferredSrvSlot::GtaoUpsampled, DeferredSrvSlot::GtaoUpsampledUAV, f,
            D.gtaoUpsampled, D.gtaoUpsampledSRV, D.gtaoUpsampledUAV,
            sizes.renderWidth, sizes.renderHeight);

        // P6C: the hierarchical depth pyramid. The BUILD holds the whole chain in UNORDERED_ACCESS
        // (this engine's barrier layer transitions whole resources, so writing mip N while reading
        // mip N-1 needs one state that permits both), but it RESTS shader-readable because GTAO
        // samples it as an ordinary mipped SRV -- and because the texture inspector transitions out
        // of a resource's canonical without transitioning back, which is only sound for a canonical
        // whose barrier layout is SHADER_RESOURCE.
        {
            UINT w = std::max(1u, sizes.hzbWidth);
            UINT h = std::max(1u, sizes.hzbHeight);
            UINT mips = 1;
            while (((w >> mips) > 0 || (h >> mips) > 0) && mips < kHzbMaxMips)
            {
                ++mips;
            }

            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w;
            rd.Height = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = static_cast<UINT16>(mips);
            rd.Format = formats.hzb;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            // The two pyramids differ only in the reduction operator the shader applies, so they
            // are created by one helper -- a second copy of this block would be a second place for
            // a format or a mip count to drift.
            auto createPyramid = [&](GpuResource& res,
                                     D3D12_CPU_DESCRIPTOR_HANDLE& srv,
                                     std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kHzbMaxMips>& mipUavs,
                                     DeferredSrvSlot srvSlot,
                                     DeferredSrvSlot firstUavSlot)
            {
                ThrowIfFailed(render::CreateCommittedTexture(dev,
                    heapProps, D3D12_HEAP_FLAG_NONE, rd,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                    res.GetAddressOfForCreate()));
                res.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);

                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = formats.hzb;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = mips;
                srv = DeferredSrvCPU(f, srvSlot);
                dev->CreateShaderResourceView(res.Get(), &sd, srv);

                for (UINT m = 0; m < kHzbMaxMips; ++m)
                {
                    // Slots past the real mip count still get a descriptor, pointed at the last
                    // valid mip. A VOLATILE descriptor table may not contain a hole, and a stale
                    // handle is exactly the kind of thing that reads as "works until the
                    // resolution changes".
                    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                    ud.Format = formats.hzb;
                    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    ud.Texture2D.MipSlice = (m < mips) ? m : (mips - 1);
                    mipUavs[m] = DeferredSrvCPU(f, static_cast<DeferredSrvSlot>(
                        static_cast<UINT>(firstUavSlot) + m));
                    dev->CreateUnorderedAccessView(res.Get(), nullptr, &ud, mipUavs[m]);
                }
            };

            createPyramid(D.hzb, D.hzbSRV, D.hzbMipUAV,
                DeferredSrvSlot::Hzb, DeferredSrvSlot::HzbMipUav0);
            createPyramid(D.hzbClosest, D.hzbClosestSRV, D.hzbClosestMipUAV,
                DeferredSrvSlot::HzbClosest, DeferredSrvSlot::HzbClosestMipUav0);

            D.hzbMips = mips;
            D.hzbWidth = w;
            D.hzbHeight = h;
        }

        // Inspector preview. Rests SHADER-READABLE: the overlay transitions FROM a resource's
        // canonical into PIXEL_SHADER_RESOURCE without transitioning back, which is only sound when
        // the canonical already shares that barrier layout.
        currentTargetWidth = kDebugPreviewSize;
        currentTargetHeight = kDebugPreviewSize;
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.debugPreview,
            DeferredSrvSlot::DebugPreview, DeferredSrvSlot::DebugPreviewUAV, f,
            D.debugPreview, D.debugPreviewSRV, D.debugPreviewUAV,
            kDebugPreviewSize, kDebugPreviewSize);
        // Address only; the view itself is written per frame by the preview pass.
        D.debugPreviewSrcSRV = DeferredSrvCPU(f, DeferredSrvSlot::DebugPreviewSrc);

        currentTargetWidth = displayWidthClamped;
        currentTargetHeight = displayHeightClamped;
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.sceneColor, DeferredSrvSlot::DLSSOutput, DeferredSrvSlot::DLSSOutputUAV, f, D.dlssOutput, D.dlssOutputSRV, D.dlssOutputUAV, displayWidthClamped, displayHeightClamped);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, formats.backbufferResource, DeferredSrvSlot::Tonemap, DeferredSrvSlot::TonemapUAV, f, D.tonemap, D.tonemapSRV, D.tonemapUAV, displayWidthClamped, displayHeightClamped);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, formats.backbufferResource, DeferredSrvSlot::Fxaa, DeferredSrvSlot::FxaaUAV, f, D.fxaa, D.fxaaSRV, D.fxaaUAV, displayWidthClamped, displayHeightClamped);

        // Debug names so DRED / the debug layer print readable resource names in
        // page-fault reports (these are the prime use-after-free suspects during
        // the deferred-target recreate races the --scene-stress harness hunts).
        // Step 6b: name AND declare the resting state in one list. The four generic creators
        // above seeded the tracker with each target's CREATION state, which is where it actually
        // is; `resting` is where the frame LEAVES it, measured with --canonical-check. The two
        // differ for most targets — a G-buffer is created RENDER_TARGET and ends the frame
        // shader-readable — and it is the resting state that Step 7's compile seeds from.
        constexpr D3D12_RESOURCE_STATES kNps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr D3D12_RESOURCE_STATES kPs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        auto nameRes = [f, decls](ID3D12Resource* res, const wchar_t* base,
                                  D3D12_RESOURCE_STATES resting) {
            if (res) {
                wchar_t nm[96];
                swprintf_s(nm, L"Deferred[%u].%s", f, base);
                res->SetName(nm);
                // The registry captured a name at declaration time, which is BEFORE this block,
                // so every deferred target was logged as a raw pointer. Safe to re-read here —
                // they were all just created.
                decls.RefreshName(res);
                (void)resting; // creation state IS the canonical now — kept as documentation
            }
        };
        nameRes(D.gb0.Get(), L"GB0", kNps);
        nameRes(D.gb1.Get(), L"GB1", kNps);
        nameRes(D.gb2.Get(), L"GB2", kNps);
        nameRes(D.gbVelocity.Get(), L"GBVelocity", kNps);
        nameRes(D.gbAux.Get(), L"GBAux", kNps);
#if WITH_EDITOR
        // Never read as an SRV in a normal frame; the pick readback copies from it and puts it
        // back, so it rests where it was created.
        nameRes(D.objectID.Get(), L"ObjectID", D3D12_RESOURCE_STATE_RENDER_TARGET);
#endif
        nameRes(D.depth.Get(), L"Depth", kNps);
        nameRes(D.depthCopy.Get(), L"DepthCopy", kPs);
        // Legacy shadow atlases: written every frame, never sampled after the last light pass.
        nameRes(D.shadow.Get(), L"CascadeShadow", kNps);
        nameRes(D.spotShadow.Get(), L"SpotShadow", kNps);
        nameRes(D.pointShadow.Get(), L"PointShadow", kNps | kPs);
        nameRes(D.light.Get(), L"Light", kNps);
        nameRes(D.scene.Get(), L"Scene", kNps);
        nameRes(D.sceneOpaque.Get(), L"SceneOpaque", kPs);
        nameRes(D.dlssOutput.Get(), L"DlssOutput", kNps);
        nameRes(D.reflection.Get(), L"Reflection", kNps);
        nameRes(D.reflectionScratch.Get(), L"ReflectionScratch", kNps);
        nameRes(D.reflectionHistory.Get(), L"ReflectionHistory", kNps);
        // Sampled by the forward ocean/glass draws, so they rest PIXEL-readable, not NPS.
        nameRes(D.oceanReflection.Get(), L"OceanReflection", kPs);
        nameRes(D.glassReflNormal.Get(), L"GlassReflNormal", kNps);
        nameRes(D.glassReflDepth.Get(), L"GlassReflDepth", kNps);
        nameRes(D.glassReflection.Get(), L"GlassReflection", kPs);
        nameRes(D.gtao.Get(), L"Gtao", kNps);
        nameRes(D.gtaoFiltered.Get(), L"GtaoFiltered", kNps);
        nameRes(D.gtaoHistory.Get(), L"GtaoHistory", kNps);
        nameRes(D.gtaoUpsampled.Get(), L"GtaoUpsampled", kNps);
        nameRes(D.hzb.Get(), L"Hzb", kNps);
        nameRes(D.hzbClosest.Get(), L"HzbClosest", kNps);
        nameRes(D.debugPreview.Get(), L"DebugPreview", kNps);
        // Tonemap/FXAA end as the compute outputs they are — the resolve flips them back.
        nameRes(D.tonemap.Get(), L"Tonemap", D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        nameRes(D.fxaa.Get(), L"Fxaa", D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
}

// Step 24f-2: CSM cascade atlas (R16_TYPELESS 2D, DSV=D16, SRV=R16). Writes deferred_[f].shadow + its
// views. `resolution` = full-res (Legacy) or 1 (VSM: tiny, directional comes from the clipmap).
void RenderTargetManager::CreateShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT f, UINT resolution)
{
    if (!dev) { return; }
    if (resolution == 0) { resolution = 4096; }
    auto& D = deferred_[f];

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = resolution;
    rd.Height = resolution;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R16_TYPELESS;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = DXGI_FORMAT_D16_UNORM;
    cv.DepthStencil.Depth = 1.0f;
    cv.DepthStencil.Stencil = 0;

    ThrowIfFailed(render::CreateCommittedTexture(dev,
        heapProps, D3D12_HEAP_FLAG_NONE, rd,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &cv, D.shadow.GetAddressOfForCreate()));

    D.shadowDSV = DeferredDsvCPU(f, DeferredDsvSlot::Shadow);
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D16_UNORM;
    dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dev->CreateDepthStencilView(D.shadow.Get(), &dsv, D.shadowDSV);

    D.shadowSRV = DeferredSrvCPU(f, DeferredSrvSlot::Shadow);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R16_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(D.shadow.Get(), &sd, D.shadowSRV);

    D.shadow->SetName(L"CascadeShadow"); // also created by SetLocalShadowResidency, after Create's naming block
    D.shadow.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr); // created in its resting state
}

// Step 24c: spot shadow atlas (R16_TYPELESS 2D-array, DSV=D16 per slice, SRV=R16 Texture2DArray).
// Writes deferred_[f].spotShadow + its views. `resolution` = full-res (Legacy) or 1 (VSM: tiny).
void RenderTargetManager::CreateSpotShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT f, UINT resolution)
{
    if (!dev) { return; }
    if (resolution == 0) { resolution = 512; }
    auto& D = deferred_[f];

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D16_UNORM;
    clear.DepthStencil.Depth = 1.0f;
    clear.DepthStencil.Stencil = 0;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.DepthOrArraySize = static_cast<UINT16>(LightManager::kMaxShadowedSpotLights);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    ThrowIfFailed(render::CreateCommittedTexture(dev,
        heapProps, D3D12_HEAP_FLAG_NONE, desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clear, D.spotShadow.GetAddressOfForCreate()));

    D.spotShadowSRV = DeferredSrvCPU(f, DeferredSrvSlot::SpotShadow);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = LightManager::kMaxShadowedSpotLights;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.PlaneSlice = 0;
    srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    dev->CreateShaderResourceView(D.spotShadow.Get(), &srvDesc, D.spotShadowSRV);

    for (UINT i = 0; i < LightManager::kMaxShadowedSpotLights; ++i)
    {
        D.spotShadowDSV[i] = DeferredSpotShadowDsvCPU(f, i);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        dsv.Texture2DArray.ArraySize = 1;
        dsv.Texture2DArray.FirstArraySlice = i;
        dsv.Texture2DArray.MipSlice = 0;
        dev->CreateDepthStencilView(D.spotShadow.Get(), &dsv, D.spotShadowDSV[i]);
    }

    D.spotShadow->SetName(L"SpotShadow");
    D.spotShadow.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
}

// Step 24c: point shadow cube-array (6*N slices of R16_TYPELESS; DSV=D16 per face, SRV=R16 cube
// array). Mirrors CreateSpotShadowResource, cube-ified. `resolution` = full-res (Legacy) or 1 (VSM).
void RenderTargetManager::CreatePointShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT f, UINT resolution)
{
    if (!dev) { return; }
    if (resolution == 0) { resolution = 512; }
    auto& D = deferred_[f];
    constexpr UINT kFaces = 6 * LightManager::kMaxShadowedPointLights;

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_CLEAR_VALUE clear{};
    clear.Format = DXGI_FORMAT_D16_UNORM;
    clear.DepthStencil.Depth = 1.0f; // standard depth (1.0 = far), like the spot atlas
    clear.DepthStencil.Stencil = 0;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = resolution;
    desc.Height = resolution;
    desc.DepthOrArraySize = static_cast<UINT16>(kFaces);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    ThrowIfFailed(render::CreateCommittedTexture(dev,
        heapProps, D3D12_HEAP_FLAG_NONE, desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, D.pointShadow.GetAddressOfForCreate()));

    D.pointShadowSRV = DeferredSrvCPU(f, DeferredSrvSlot::PointShadow);
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.TextureCubeArray.MostDetailedMip = 0;
    srvDesc.TextureCubeArray.MipLevels = 1;
    srvDesc.TextureCubeArray.First2DArrayFace = 0;
    srvDesc.TextureCubeArray.NumCubes = LightManager::kMaxShadowedPointLights;
    srvDesc.TextureCubeArray.ResourceMinLODClamp = 0.0f;
    dev->CreateShaderResourceView(D.pointShadow.Get(), &srvDesc, D.pointShadowSRV);

    for (UINT face = 0; face < kFaces; ++face)
    {
        D.pointShadowDSV[face] = DeferredPointShadowDsvCPU(f, face);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsv.Flags = D3D12_DSV_FLAG_NONE;
        dsv.Texture2DArray.ArraySize = 1;
        dsv.Texture2DArray.FirstArraySlice = face;
        dsv.Texture2DArray.MipSlice = 0;
        dev->CreateDepthStencilView(D.pointShadow.Get(), &dsv, D.pointShadowDSV[face]);
    }

    D.pointShadow->SetName(L"PointShadow");
    D.pointShadow.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr);
}

void RenderTargetManager::SetLocalShadowResidency(ID3D12Device* dev, ResourceDeclarations decls, bool full)
{
    if (full == localShadowFull_ || !dev) { return; }
    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        auto& D = deferred_[f];
        // Untrack the outgoing resources (ReleaseAndGetAddressOf inside the create calls frees them),
        // so a re-used address can't inherit a stale tracked state.
        if (D.shadow) { decls.Forget(D.shadow.Get()); }          // Step 24f-2: CSM cascade atlas
        if (D.spotShadow) { decls.Forget(D.spotShadow.Get()); }
        if (D.pointShadow) { decls.Forget(D.pointShadow.Get()); }
        // Keep the configured resolutions; only the created size changes (1 = tiny placeholder). VSM mode
        // retires the CSM cascade atlas (~96 MB across frames) as well: the render graph omits the
        // Main_CSM pass in VSM mode, so nothing renders into this 1x1 placeholder.
        CreateShadowResource(dev, decls, f, full ? D.shadowRes : 1u);
        CreateSpotShadowResource(dev, decls, f, full ? D.spotShadowRes : 1u);
        CreatePointShadowResource(dev, decls, f, full ? D.pointShadowRes : 1u);
    }
    localShadowFull_ = full;
}

void RenderTargetManager::Destroy(ResourceDeclarations decls)
{
    deferredRtvHeap_.Reset(); deferredDsvHeap_.Reset(); deferredSrvCpuHeap_.Reset();
    std::vector<ID3D12Resource*> released;
    released.reserve(render::kFrameCount * DeferredTargets::kResourceCount);

    // The wrapper unregisters on Reset, so `released` is now only the ForgetMany safety net for
    // anything declared outside a wrapper — kept, and harmless for the rest.
    auto collect = [&released](GpuResource& res) {
        if (ID3D12Resource* ptr = res.Get()) {
            released.push_back(ptr);
            res.Reset();
        }
    };

    for (UINT f = 0; f < render::kFrameCount; ++f) {
        auto& D = deferred_[f];
        collect(D.gb0);
        collect(D.gb1);
        collect(D.gb2);
        collect(D.gbVelocity);
        collect(D.gbAux);
#if WITH_EDITOR
        collect(D.objectID);
#endif
        collect(D.depth);
        collect(D.depthCopy);
        collect(D.light);
        collect(D.scene);
        collect(D.sceneOpaque);
        collect(D.tonemap);
        collect(D.fxaa);
        collect(D.reflection);
        collect(D.reflectionScratch);
        collect(D.reflectionHistory);
        collect(D.oceanReflection);
        collect(D.shadow);
        collect(D.spotShadow);
        collect(D.pointShadow);
        collect(D.dlssOutput);
        collect(D.glassReflNormal);
        collect(D.glassReflDepth);
        collect(D.glassReflection);
    }

    decls.ForgetMany(released);

    for (DeferredTargets& D : deferred_) {
        D = DeferredTargets{};
    }
}
