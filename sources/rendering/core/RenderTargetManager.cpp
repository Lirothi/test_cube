#include "rendering/core/RenderTargetManager.h"

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

void RenderTargetManager::Create(ID3D12Device* dev, const Formats& formats, const Sizes& sizes, ResourceStateTracker& tracker)
{
    // Just in case: release old resources/heaps
    Destroy(tracker);

    if (!dev) { return; }

    // --- Descriptor increments ---
    deferredRtvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    deferredDsvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    deferredSrvIncr_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // --- CPU-only descriptor heaps for offscreen targets (RTV/DSV/SRV) ---
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = render::kFrameCount * kDeferredRtvPerFrame;  // GB0,GB1,GB2,Velocity,ObjectID,Light,Scene,DLSS bias
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
    auto CreateRT = [&](DXGI_FORMAT fmt,
        DeferredRtvSlot rtvSlot,
        DeferredSrvSlot srvSlot,
        DeferredSrvSlot uavSlot,
        UINT f,
        ComPtr<ID3D12Resource>& outRes,
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

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&outRes)));

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
            case DeferredRtvSlot::ObjectID:   D.objectIDRTV = outRTV; break;
            case DeferredRtvSlot::Light:      D.lightRTV = outRTV; break;
            case DeferredRtvSlot::Scene:      D.sceneRTV = outRTV; break;
            case DeferredRtvSlot::DlssBias:   D.dlssBiasRTV = outRTV; break;
            default: break;
            }
            switch (srvSlot) {
            case DeferredSrvSlot::GB0:        D.gbSRV[0] = outSRV; break;
            case DeferredSrvSlot::GB1:        D.gbSRV[1] = outSRV; break;
            case DeferredSrvSlot::GB2:        D.gbSRV[2] = outSRV; break;
            case DeferredSrvSlot::GBVelocity: D.gbSRV[3] = outSRV; break;
            case DeferredSrvSlot::Depth:      D.depthSRV = outSRV; break;
            case DeferredSrvSlot::Light:      D.lightSRV = outSRV; break;
            case DeferredSrvSlot::Scene:      D.sceneSRV = outSRV; break;
            case DeferredSrvSlot::DlssBias:   D.dlssBiasSRV = outSRV; break;
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

            tracker.SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        };

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
            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&D.objectID)));

            D.objectIDRTV = DeferredRtvCPU(f, DeferredRtvSlot::ObjectID);
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
            rtvDesc.Format = formats.objectID;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            dev->CreateRenderTargetView(D.objectID.Get(), &rtvDesc, D.objectIDRTV);

            tracker.SetResourceState(D.objectID.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
        };

    auto CreateSrvTexture = [&](DXGI_FORMAT fmt,
        DeferredSrvSlot srvSlot,
        UINT f,
        ComPtr<ID3D12Resource>& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_NONE);

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&outRes)));

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = srvFormat == DXGI_FORMAT_UNKNOWN ? fmt : srvFormat;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;

            outSRV = DeferredSrvCPU(f, srvSlot);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outSRV);

            auto& D = deferred_[f];
            if (srvSlot == DeferredSrvSlot::SceneOpaque)
            {
                D.sceneOpaque = outRes;
                D.sceneOpaqueSRV = outSRV;
            }
            else if (srvSlot == DeferredSrvSlot::DepthCopy)
            {
                D.depthCopy = outRes;
                D.depthCopySRV = outSRV;
            }
            tracker.SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        };

    auto CreateSrvUavTexture = [&](DXGI_FORMAT fmt,
        DeferredSrvSlot srvSlot,
        DeferredSrvSlot uavSlot,
        UINT f,
        ComPtr<ID3D12Resource>& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outUAV,
        UINT overrideWidth,
        UINT overrideHeight)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (overrideWidth > 0) { rd.Width = overrideWidth; }
            if (overrideHeight > 0) { rd.Height = overrideHeight; }

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outRes)));

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

            tracker.SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        };

    auto CreateDepth = [&](DXGI_FORMAT dsvFmt,
        UINT f,
        ComPtr<ID3D12Resource>& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outDSV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outDepthSRV)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(dsvFmt, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

            D3D12_CLEAR_VALUE cv{}; cv.Format = dsvFmt; cv.DepthStencil.Depth = 0.0f; cv.DepthStencil.Stencil = 0;
            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&outRes)));

            auto& D = deferred_[f];

            // DSV
            outDSV = DeferredDsvCPU(f, DeferredDsvSlot::Depth);
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
            dv.Format = dsvFmt;
            dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev->CreateDepthStencilView(outRes.Get(), &dv, outDSV);
            D.dsv = outDSV;

            // Create an SRV for depth as R32_FLOAT
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = formats.depthSrv;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            outDepthSRV = DeferredSrvCPU(f, DeferredSrvSlot::Depth);
            dev->CreateShaderResourceView(outRes.Get(), &sd, outDepthSRV);
            D.depthSRV = outDepthSRV;

            tracker.SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        };

    auto CreateShadow = [&](UINT f,
        Microsoft::WRL::ComPtr<ID3D12Resource>& outRes,
        D3D12_CPU_DESCRIPTOR_HANDLE& outDSV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        UINT resolution)
        {
            // Shadows use a typeless texture with DSV=D16 and SRV=R16
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

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&outRes)));

            // DSV — goes into its dedicated shadow slot
            outDSV = DeferredDsvCPU(f, DeferredDsvSlot::Shadow);
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DXGI_FORMAT_D16_UNORM;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev->CreateDepthStencilView(outRes.Get(), &dsv, outDSV);

            // SRV — also stored in the shadow slot
            outSRV = DeferredSrvCPU(f, DeferredSrvSlot::Shadow);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R16_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(outRes.Get(), &sd, outSRV);

            tracker.SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        };

    auto CreateSpotShadow = [&](UINT frameIndex,
        ComPtr<ID3D12Resource>& outRes,
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, LightManager::kMaxSpotLights>& outDSV,
        D3D12_CPU_DESCRIPTOR_HANDLE& outSRV,
        UINT resolution)
        {
            if (resolution == 0) { resolution = 512; }

            D3D12_CLEAR_VALUE clear{};
            clear.Format = DXGI_FORMAT_D16_UNORM;
            clear.DepthStencil.Depth = 1.0f;
            clear.DepthStencil.Stencil = 0;

            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Alignment = 0;
            desc.Width = resolution;
            desc.Height = resolution;
            desc.DepthOrArraySize = static_cast<UINT16>(LightManager::kMaxSpotLights);
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R16_TYPELESS;
            desc.SampleDesc.Count = 1;
            desc.SampleDesc.Quality = 0;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

            ThrowIfFailed(dev->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(outRes.ReleaseAndGetAddressOf())));

            outSRV = DeferredSrvCPU(frameIndex, DeferredSrvSlot::SpotShadow);
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = DXGI_FORMAT_R16_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.FirstArraySlice = 0;
            srvDesc.Texture2DArray.ArraySize = LightManager::kMaxSpotLights;
            srvDesc.Texture2DArray.MostDetailedMip = 0;
            srvDesc.Texture2DArray.PlaneSlice = 0;
            srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
            dev->CreateShaderResourceView(outRes.Get(), &srvDesc, outSRV);

            for (UINT i = 0; i < LightManager::kMaxSpotLights; ++i)
            {
                outDSV[i] = DeferredSpotShadowDsvCPU(frameIndex, i);
                D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
                dsv.Format = DXGI_FORMAT_D16_UNORM;
                dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                dsv.Flags = D3D12_DSV_FLAG_NONE;
                dsv.Texture2DArray.ArraySize = 1;
                dsv.Texture2DArray.FirstArraySlice = i;
                dsv.Texture2DArray.MipSlice = 0;
                dev->CreateDepthStencilView(outRes.Get(), &dsv, outDSV[i]);
            }

            tracker.SetResourceState(outRes.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        };

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        auto& D = deferred_[f];

        currentTargetWidth = rtWidth;
        currentTargetHeight = rtHeight;
        CreateRT(formats.gb0, DeferredRtvSlot::GB0, DeferredSrvSlot::GB0, DeferredSrvSlot::Count, f, D.gb0, D.gbRTV[0], D.gbSRV[0]);
        CreateRT(formats.gb1, DeferredRtvSlot::GB1, DeferredSrvSlot::GB1, DeferredSrvSlot::Count, f, D.gb1, D.gbRTV[1], D.gbSRV[1]);
        CreateRT(formats.gb2, DeferredRtvSlot::GB2, DeferredSrvSlot::GB2, DeferredSrvSlot::Count, f, D.gb2, D.gbRTV[2], D.gbSRV[2]);
        CreateRT(formats.velocity, DeferredRtvSlot::GBVelocity, DeferredSrvSlot::GBVelocity, DeferredSrvSlot::Count, f, D.gbVelocity, D.gbRTV[3], D.gbSRV[3]);
        CreateObjectIdTarget(f);

        CreateDepth(formats.depth, f, D.depth, D.dsv, /*outDepthSRV*/ D.depthSRV);
        CreateSrvTexture(formats.depth, DeferredSrvSlot::DepthCopy, f, D.depthCopy, D.depthCopySRV, formats.depthSrv);

        D.shadowRes = 4096; // could be driven by config/parameter
        CreateShadow(f, D.shadow, D.shadowDSV, D.shadowSRV, D.shadowRes);

        D.spotShadowRes = 512;
        CreateSpotShadow(f, D.spotShadow, D.spotShadowDSV, D.spotShadowSRV, D.spotShadowRes);

        CreateRT(formats.light, DeferredRtvSlot::Light, DeferredSrvSlot::Light, DeferredSrvSlot::LightUAV, f, D.light, D.lightRTV, D.lightSRV);
        CreateRT(formats.sceneColor, DeferredRtvSlot::Scene, DeferredSrvSlot::Scene, DeferredSrvSlot::SceneUAV, f, D.scene, D.sceneRTV, D.sceneSRV);
        CreateRT(formats.dlssBias, DeferredRtvSlot::DlssBias, DeferredSrvSlot::DlssBias, DeferredSrvSlot::Count, f, D.dlssBias, D.dlssBiasRTV, D.dlssBiasSRV, float4(0, 0, 0, 0));
        CreateSrvTexture(formats.sceneColor, DeferredSrvSlot::SceneOpaque, f, D.sceneOpaque, D.sceneOpaqueSRV);
        CreateSrvUavTexture(formats.reflection, DeferredSrvSlot::Reflection, DeferredSrvSlot::ReflectionUAV, f, D.reflection, D.reflectionSRV, D.reflectionUAV, sizes.reflectionWidth, sizes.reflectionHeight);
        CreateSrvUavTexture(formats.reflectionScratch, DeferredSrvSlot::ReflectionScratch, DeferredSrvSlot::ReflectionScratchUAV, f, D.reflectionScratch, D.reflectionScratchSRV, D.reflectionScratchUAV, sizes.reflectionWidth, sizes.reflectionHeight);
        CreateSrvUavTexture(formats.oceanReflection, DeferredSrvSlot::OceanReflection, DeferredSrvSlot::OceanReflectionUAV, f, D.oceanReflection, D.oceanReflectionSRV, D.oceanReflectionUAV, sizes.oceanReflectionWidth, sizes.oceanReflectionHeight);

        // S15 off-screen glass reflections (reflection res): a glass G-buffer (front-face
        // normal RTV + depth DSV) feeding a second rt_reflections_cs dispatch into glassReflection.
        currentTargetWidth = std::max(1u, sizes.reflectionWidth);
        currentTargetHeight = std::max(1u, sizes.reflectionHeight);
        CreateRT(formats.gb1, DeferredRtvSlot::GlassReflNormal, DeferredSrvSlot::GlassReflNormal, DeferredSrvSlot::Count, f, D.glassReflNormal, D.glassReflNormalRTV, D.glassReflNormalSRV);
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(formats.depth, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
            D3D12_CLEAR_VALUE cv{}; cv.Format = formats.depth; cv.DepthStencil.Depth = 0.0f; cv.DepthStencil.Stencil = 0;
            ThrowIfFailed(dev->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&D.glassReflDepth)));
            D.glassReflDepthDSV = DeferredDsvCPU(f, DeferredDsvSlot::GlassReflDepth);
            D3D12_DEPTH_STENCIL_VIEW_DESC dv{}; dv.Format = formats.depth; dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            dev->CreateDepthStencilView(D.glassReflDepth.Get(), &dv, D.glassReflDepthDSV);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{}; sd.Format = formats.depthSrv; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
            D.glassReflDepthSRV = DeferredSrvCPU(f, DeferredSrvSlot::GlassReflDepth);
            dev->CreateShaderResourceView(D.glassReflDepth.Get(), &sd, D.glassReflDepthSRV);
            tracker.SetResourceState(D.glassReflDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        CreateSrvUavTexture(formats.reflection, DeferredSrvSlot::GlassReflection, DeferredSrvSlot::GlassReflectionUAV, f, D.glassReflection, D.glassReflectionSRV, D.glassReflectionUAV, sizes.reflectionWidth, sizes.reflectionHeight);

        currentTargetWidth = displayWidthClamped;
        currentTargetHeight = displayHeightClamped;
        CreateSrvUavTexture(formats.sceneColor, DeferredSrvSlot::DLSSOutput, DeferredSrvSlot::DLSSOutputUAV, f, D.dlssOutput, D.dlssOutputSRV, D.dlssOutputUAV, displayWidthClamped, displayHeightClamped);
        CreateSrvUavTexture(formats.backbufferResource, DeferredSrvSlot::Tonemap, DeferredSrvSlot::TonemapUAV, f, D.tonemap, D.tonemapSRV, D.tonemapUAV, displayWidthClamped, displayHeightClamped);
        CreateSrvUavTexture(formats.backbufferResource, DeferredSrvSlot::Fxaa, DeferredSrvSlot::FxaaUAV, f, D.fxaa, D.fxaaSRV, D.fxaaUAV, displayWidthClamped, displayHeightClamped);

        // Debug names so DRED / the debug layer print readable resource names in
        // page-fault reports (these are the prime use-after-free suspects during
        // the deferred-target recreate races the --scene-stress harness hunts).
        auto nameRes = [f](ID3D12Resource* res, const wchar_t* base) {
            if (res) {
                wchar_t nm[96];
                swprintf_s(nm, L"Deferred[%u].%s", f, base);
                res->SetName(nm);
            }
        };
        nameRes(D.gb0.Get(), L"GB0");
        nameRes(D.gb1.Get(), L"GB1");
        nameRes(D.gb2.Get(), L"GB2");
        nameRes(D.gbVelocity.Get(), L"GBVelocity");
        nameRes(D.objectID.Get(), L"ObjectID");
        nameRes(D.depth.Get(), L"Depth");
        nameRes(D.depthCopy.Get(), L"DepthCopy");
        nameRes(D.shadow.Get(), L"CascadeShadow");
        nameRes(D.spotShadow.Get(), L"SpotShadow");
        nameRes(D.light.Get(), L"Light");
        nameRes(D.scene.Get(), L"Scene");
        nameRes(D.sceneOpaque.Get(), L"SceneOpaque");
        nameRes(D.dlssBias.Get(), L"DlssBias");
        nameRes(D.dlssOutput.Get(), L"DlssOutput");
        nameRes(D.reflection.Get(), L"Reflection");
        nameRes(D.reflectionScratch.Get(), L"ReflectionScratch");
        nameRes(D.oceanReflection.Get(), L"OceanReflection");
        nameRes(D.glassReflNormal.Get(), L"GlassReflNormal");
        nameRes(D.glassReflDepth.Get(), L"GlassReflDepth");
        nameRes(D.glassReflection.Get(), L"GlassReflection");
        nameRes(D.tonemap.Get(), L"Tonemap");
        nameRes(D.fxaa.Get(), L"Fxaa");
    }
}

void RenderTargetManager::Destroy(ResourceStateTracker& tracker)
{
    deferredRtvHeap_.Reset(); deferredDsvHeap_.Reset(); deferredSrvCpuHeap_.Reset();
    std::vector<ID3D12Resource*> released;
    released.reserve(render::kFrameCount * DeferredTargets::kResourceCount);

    auto collect = [&released](ComPtr<ID3D12Resource>& res) {
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
        collect(D.objectID);
        collect(D.depth);
        collect(D.depthCopy);
        collect(D.light);
        collect(D.scene);
        collect(D.dlssBias);
        collect(D.sceneOpaque);
        collect(D.tonemap);
        collect(D.fxaa);
        collect(D.reflection);
        collect(D.reflectionScratch);
        collect(D.oceanReflection);
        collect(D.shadow);
        collect(D.spotShadow);
        collect(D.dlssOutput);
        collect(D.glassReflNormal);
        collect(D.glassReflDepth);
        collect(D.glassReflection);
    }

    tracker.ForgetResources(released);

    for (DeferredTargets& D : deferred_) {
        D = DeferredTargets{};
    }
}
