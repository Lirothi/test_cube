#include "rendering/core/RenderTargetManager.h"
#include "rendering/core/GpuAlloc.h"
#include "rendering/core/TextureCreate.h"
#include "rendering/shadows/ShadowSettings.h" // render::g_csmAtlasRes + the S5 gutter

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
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredSpotShadowDsvCPU(UINT frame, UINT page) const {
    const UINT base = frame * kDeferredDsvPerFrame + static_cast<UINT>(DeferredDsvSlot::Count);
    return DeferredDsvAt(base + page);
}
D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetManager::DeferredPointShadowDsvCPU(UINT frame, UINT page) const {
    // Point-page DSVs sit after the named DSV slots AND the spot-page DSV block (plan S9).
    const UINT base = frame * kDeferredDsvPerFrame
        + static_cast<UINT>(DeferredDsvSlot::Count) + render::kLocalShadowMaxPagesLimit;
    return DeferredDsvAt(base + page);
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
        ThrowIfFailed(gpu::CreateDescriptorHeap(dev, &desc, MemTag::Frame, L"RTM.RtvHeap", IID_PPV_ARGS(&deferredRtvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = render::kFrameCount * kDeferredDsvPerFrame;  // Depth
        ThrowIfFailed(gpu::CreateDescriptorHeap(dev, &desc, MemTag::Frame, L"RTM.DsvHeap", IID_PPV_ARGS(&deferredDsvHeap_)));
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = render::kFrameCount * kDeferredSrvPerFrame;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only staging
        ThrowIfFailed(gpu::CreateDescriptorHeap(dev, &desc, MemTag::Frame, L"RTM.SrvCpuHeap", IID_PPV_ARGS(&deferredSrvCpuHeap_)));
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

    // The census name of a deferred target: the same "Deferred[f].X" the naming block below sets.
    auto DeferredName = [](UINT f, const wchar_t* base) {
        std::array<wchar_t, 96> nm{};
        swprintf_s(nm.data(), nm.size(), L"Deferred[%u].%s", f, base);
        return nm;
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
        MemTag tag,
        const wchar_t* name,
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

            ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                canonical, &cv, tag, DeferredName(f, name).data(), outRes.GetAddressOfForCreate()));

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
            ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, MemTag::FrameGbuffer, DeferredName(f, L"ObjectID").data(),
                D.objectID.GetAddressOfForCreate()));

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
        MemTag tag,
        const wchar_t* name,
        DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_NONE);

            ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                canonical, nullptr, tag, DeferredName(f, name).data(), outRes.GetAddressOfForCreate()));

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
        UINT overrideHeight,
        MemTag tag,
        const wchar_t* name)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(fmt, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (overrideWidth > 0) { rd.Width = overrideWidth; }
            if (overrideHeight > 0) { rd.Height = overrideHeight; }

            ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                canonical, nullptr, tag, DeferredName(f, name).data(), outRes.GetAddressOfForCreate()));

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
        MemTag tag,
        const wchar_t* name,
        float clearDepth = 0.0f,
        DXGI_FORMAT srvFmt = DXGI_FORMAT_UNKNOWN,
        DeferredSrvSlot stencilSrvSlot = DeferredSrvSlot::Count,
        D3D12_CPU_DESCRIPTOR_HANDLE* outStencilSRV = nullptr,
        DXGI_FORMAT stencilSrvFmt = DXGI_FORMAT_UNKNOWN)
        {
            D3D12_RESOURCE_DESC rd = MakeTex2DDesc(dsvFmt, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);

            D3D12_CLEAR_VALUE cv{}; cv.Format = dsvFmt; cv.DepthStencil.Depth = clearDepth; cv.DepthStencil.Stencil = 0;
            ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                canonical, &cv, tag, DeferredName(f, name).data(), outRes.GetAddressOfForCreate()));

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
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gb0, DeferredRtvSlot::GB0, DeferredSrvSlot::GB0, DeferredSrvSlot::Count, f, D.gb0, D.gbRTV[0], D.gbSRV[0], MemTag::FrameGbuffer, L"GB0");
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gb1, DeferredRtvSlot::GB1, DeferredSrvSlot::GB1, DeferredSrvSlot::Count, f, D.gb1, D.gbRTV[1], D.gbSRV[1], MemTag::FrameGbuffer, L"GB1");
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gb2, DeferredRtvSlot::GB2, DeferredSrvSlot::GB2, DeferredSrvSlot::Count, f, D.gb2, D.gbRTV[2], D.gbSRV[2], MemTag::FrameGbuffer, L"GB2");
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.velocity, DeferredRtvSlot::GBVelocity, DeferredSrvSlot::GBVelocity, DeferredSrvSlot::Count, f, D.gbVelocity, D.gbRTV[3], D.gbSRV[3], MemTag::FrameGbuffer, L"GBVelocity");
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gbAux, DeferredRtvSlot::GBAux, DeferredSrvSlot::GBAux, DeferredSrvSlot::Count,
            f, D.gbAux, D.gbAuxRTV, D.gbAuxSRV, MemTag::FrameGbuffer, L"GBAux", float4(1, 1, 0, 0));
#if WITH_EDITOR
        CreateObjectIdTarget(f);
#endif

        CreateDepth(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.depth, DeferredDsvSlot::Depth, DeferredSrvSlot::Depth, f, D.depth, D.dsv, /*outDepthSRV*/ D.depthSRV,
            MemTag::FrameGbuffer, L"Depth",
            0.0f, DXGI_FORMAT_UNKNOWN, DeferredSrvSlot::Stencil, &D.stencilSRV, render::kDeferredStencilSrvFormat);
        CreateSrvTexture(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, formats.depth, DeferredSrvSlot::DepthCopy, f, D.depthCopy, D.depthCopySRV, MemTag::FrameGbuffer, L"DepthCopy", formats.depthSrv);

        // S3.5: ONE atlas for all frames — created after this loop. The per-frame field is kept
        // because readers (lighting CB, Renderer::BindShadowTarget) index it through Deferred(f).
        // The knob, not a literal: render::g_csmAtlasRes (ShadowSettings.h). Clamped here so a
        // settings file or a --set cannot ask for a size the device will refuse.
        D.shadowRes = std::clamp(render::g_csmAtlasRes, render::kCsmAtlasResMin, render::kCsmAtlasResMax);

        // Local-light shadow plan S5: the atlases themselves are created once, after this loop. S4: their slices are
        // the knob's largest shadow (a spot's), the point faces half of it (UE's largest cube).
        D.spotShadowRes = LocalShadowSliceRes(render::g_localShadowMaxRes);
        D.pointShadowRes = D.spotShadowRes / 2u;

        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.light, DeferredRtvSlot::Light, DeferredSrvSlot::Light, DeferredSrvSlot::LightUAV, f, D.light, D.lightRTV, D.lightSRV, MemTag::FrameColor, L"Light");
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.sceneColor, DeferredRtvSlot::Scene, DeferredSrvSlot::Scene, DeferredSrvSlot::SceneUAV, f, D.scene, D.sceneRTV, D.sceneSRV, MemTag::FrameColor, L"Scene");
        CreateSrvTexture(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, formats.sceneColor, DeferredSrvSlot::SceneOpaque, f, D.sceneOpaque, D.sceneOpaqueSRV, MemTag::FrameColor, L"SceneOpaque");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.reflection, DeferredSrvSlot::Reflection, DeferredSrvSlot::ReflectionUAV, f, D.reflection, D.reflectionSRV, D.reflectionUAV, sizes.reflectionWidth, sizes.reflectionHeight, MemTag::Reflect, L"Reflection");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.reflectionScratch, DeferredSrvSlot::ReflectionScratch, DeferredSrvSlot::ReflectionScratchUAV, f, D.reflectionScratch, D.reflectionScratchSRV, D.reflectionScratchUAV, sizes.reflectionWidth, sizes.reflectionHeight, MemTag::Reflect, L"ReflectionScratch");
        // SSR temporal history. Same format and size as the reflection buffer it accumulates.
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.reflection, DeferredSrvSlot::ReflectionHistory, DeferredSrvSlot::ReflectionHistoryUAV, f, D.reflectionHistory, D.reflectionHistorySRV, D.reflectionHistoryUAV, sizes.reflectionWidth, sizes.reflectionHeight, MemTag::Reflect, L"ReflectionHistory");
        // RT gather-then-shade payload (async prep): radiancePart + mode in the reflection's own
        // HDR format, the lit-HDR reuse uv in RG16_UNORM (see RenderTargetManager.h note).
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.reflection, DeferredSrvSlot::RtPayload, DeferredSrvSlot::RtPayloadUAV, f, D.rtPayload, D.rtPayloadSRV, D.rtPayloadUAV, sizes.reflectionWidth, sizes.reflectionHeight, MemTag::Reflect, L"RtPayload");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, DXGI_FORMAT_R16G16_UNORM, DeferredSrvSlot::RtPayloadUv, DeferredSrvSlot::RtPayloadUvUAV, f, D.rtPayloadUv, D.rtPayloadUvSRV, D.rtPayloadUvUAV, sizes.reflectionWidth, sizes.reflectionHeight, MemTag::Reflect, L"RtPayloadUv");
        // The water's screen-space reflection (ocean G-buffer plan S3): written, resolved and read by
        // compute only (Main_WaterReflection), so both it and its history rest NON_PIXEL.
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.oceanReflection, DeferredSrvSlot::OceanReflection, DeferredSrvSlot::OceanReflectionUAV, f, D.oceanReflection, D.oceanReflectionSRV, D.oceanReflectionUAV, sizes.oceanReflectionWidth, sizes.oceanReflectionHeight, MemTag::Reflect, L"OceanReflection");
        // Its temporal history: same size and format.
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.oceanReflection, DeferredSrvSlot::OceanReflectionHistory, DeferredSrvSlot::OceanReflectionHistoryUAV, f, D.oceanReflectionHistory, D.oceanReflectionHistorySRV, D.oceanReflectionHistoryUAV, sizes.oceanReflectionWidth, sizes.oceanReflectionHeight, MemTag::Reflect, L"OceanReflectionHistory");

        // S15 off-screen glass reflections (reflection res): a glass G-buffer (front-face
        // normal RTV + depth DSV) feeding a second rt_reflections_cs dispatch into glassReflection.
        currentTargetWidth = std::max(1u, sizes.reflectionWidth);
        currentTargetHeight = std::max(1u, sizes.reflectionHeight);
        CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gb1, DeferredRtvSlot::GlassReflNormal, DeferredSrvSlot::GlassReflNormal, DeferredSrvSlot::Count, f, D.glassReflNormal, D.glassReflNormalRTV, D.glassReflNormalSRV, MemTag::Reflect, L"GlassReflNormal");
        CreateDepth(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.depth, DeferredDsvSlot::GlassReflDepth, DeferredSrvSlot::GlassReflDepth, f, D.glassReflDepth, D.glassReflDepthDSV, D.glassReflDepthSRV, MemTag::Reflect, L"GlassReflDepth");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, formats.reflection, DeferredSrvSlot::GlassReflection, DeferredSrvSlot::GlassReflectionUAV, f, D.glassReflection, D.glassReflectionSRV, D.glassReflectionUAV, sizes.reflectionWidth, sizes.reflectionHeight, MemTag::Reflect, L"GlassReflection");

        // P6B: half-resolution AO, sized off the RENDER resolution (not the display one) because it
        // is consumed by lighting/compose, which run before the upscaler.
        currentTargetWidth = std::max(1u, sizes.gtaoWidth);
        currentTargetHeight = std::max(1u, sizes.gtaoHeight);
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gtao,
            DeferredSrvSlot::Gtao, DeferredSrvSlot::GtaoUAV, f, D.gtao, D.gtaoSRV, D.gtaoUAV,
            sizes.gtaoWidth, sizes.gtaoHeight, MemTag::Ao, L"Gtao");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gtao,
            DeferredSrvSlot::GtaoFiltered, DeferredSrvSlot::GtaoFilteredUAV, f,
            D.gtaoFiltered, D.gtaoFilteredSRV, D.gtaoFilteredUAV,
            sizes.gtaoWidth, sizes.gtaoHeight, MemTag::Ao, L"GtaoFiltered");
        // The temporal history is the one deferred target read ACROSS frames (frame N samples the
        // copy frame N-1 wrote, from the previous Deferred set). Nothing else about it is special:
        // it rests shader-readable like its siblings, so the compile sees UAV -> SRV inside the
        // pass and SRV -> UAV two frames later, both from ordinary declarations.
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.gtao,
            DeferredSrvSlot::GtaoHistory, DeferredSrvSlot::GtaoHistoryUAV, f,
            D.gtaoHistory, D.gtaoHistorySRV, D.gtaoHistoryUAV,
            sizes.gtaoWidth, sizes.gtaoHeight, MemTag::Ao, L"GtaoHistory");
        // Plan S: the RT sky visibility, on the same half-resolution grid (and read across frames
        // the same way: frame N blends in the copy frame N-1 wrote).
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, render::kSkyVisFormat,
            DeferredSrvSlot::SkyVis, DeferredSrvSlot::SkyVisUAV, f,
            D.skyVis, D.skyVisSRV, D.skyVisUAV,
            sizes.gtaoWidth, sizes.gtaoHeight, MemTag::Ao, L"SkyVisibility");
        // ...its temporal accumulation, before the spatial filter (the history the next frame reads).
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, render::kSkyVisFormat,
            DeferredSrvSlot::SkyVisAccum, DeferredSrvSlot::SkyVisAccumUAV, f,
            D.skyVisAccum, D.skyVisAccumSRV, D.skyVisAccumUAV,
            sizes.gtaoWidth, sizes.gtaoHeight, MemTag::Ao, L"SkyVisibilityAccum");

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
            sizes.renderWidth, sizes.renderHeight, MemTag::Ao, L"GtaoUpsampled");

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
                                     DeferredSrvSlot firstUavSlot,
                                     const wchar_t* name)
            {
                ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                    heapProps, D3D12_HEAP_FLAG_NONE, rd,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                    MemTag::FrameGbuffer, DeferredName(f, name).data(),
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
                DeferredSrvSlot::Hzb, DeferredSrvSlot::HzbMipUav0, L"Hzb");
            createPyramid(D.hzbClosest, D.hzbClosestSRV, D.hzbClosestMipUAV,
                DeferredSrvSlot::HzbClosest, DeferredSrvSlot::HzbClosestMipUav0, L"HzbClosest");

            D.hzbMips = mips;
            D.hzbWidth = w;
            D.hzbHeight = h;
        }

        // The glossy filter's two pyramids (reflection_pyramid_cs.hlsl / reflection_glossy_cs.hlsl): the reflection's
        // and, on the same grid, the geometry under it (world normal, device depth -- fp16 holds a reverse-Z depth to
        // 0.05 %). The HZB's shape and rule: built in UNORDERED_ACCESS (mip N reads mip N-1 through its UAV), rest --
        // and are read -- as mipped SRVs.
        {
            const UINT w = std::max(1u, sizes.reflectionWidth);
            const UINT h = std::max(1u, sizes.reflectionHeight);
            UINT mips = 1;
            while (((w >> mips) > 0 || (h >> mips) > 0) && mips < kReflectionPyramidMaxMips)
            {
                ++mips;
            }
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w;
            rd.Height = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = static_cast<UINT16>(mips);
            rd.Format = formats.reflection;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            auto createReflectionPyramid = [&](GpuResource& res,
                                               D3D12_CPU_DESCRIPTOR_HANDLE& srv,
                                               std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kReflectionPyramidMaxMips>& mipUavs,
                                               DeferredSrvSlot srvSlot,
                                               DeferredSrvSlot firstUavSlot,
                                               const wchar_t* name)
            {
                ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                    heapProps, D3D12_HEAP_FLAG_NONE, rd,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                    MemTag::Reflect, DeferredName(f, name).data(),
                    res.GetAddressOfForCreate()));
                res.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);

                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = formats.reflection;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = mips;
                srv = DeferredSrvCPU(f, srvSlot);
                dev->CreateShaderResourceView(res.Get(), &sd, srv);
                for (UINT m = 0; m < kReflectionPyramidMaxMips; ++m)
                {
                    // The HZB's rule: slots past the real mip count still get a descriptor, at the last real mip.
                    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                    ud.Format = formats.reflection;
                    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    ud.Texture2D.MipSlice = (m < mips) ? m : (mips - 1);
                    mipUavs[m] = DeferredSrvCPU(f, static_cast<DeferredSrvSlot>(static_cast<UINT>(firstUavSlot) + m));
                    dev->CreateUnorderedAccessView(res.Get(), nullptr, &ud, mipUavs[m]);
                }
            };
            createReflectionPyramid(D.reflectionPyramid, D.reflectionPyramidSRV, D.reflectionPyramidMipUAV,
                DeferredSrvSlot::ReflectionPyramid, DeferredSrvSlot::ReflectionPyramidMipUav0, L"ReflectionPyramid");
            createReflectionPyramid(D.reflectionGeomPyramid, D.reflectionGeomPyramidSRV, D.reflectionGeomPyramidMipUAV,
                DeferredSrvSlot::ReflectionGeomPyramid, DeferredSrvSlot::ReflectionGeomPyramidMipUav0,
                L"ReflectionGeomPyramid");
            D.reflectionPyramidMips = mips;
        }

        // Volumetric fog (plan part A): the two froxel volumes, RGBA16F 3D, one SRV + one UAV each,
        // resting NON_PIXEL (both are written and read by compute; compose reads the integrated one).
        // Sized from the render resolution over kFogGridPixels, like everything else in this set.
        {
            const UINT w = std::max(1u, sizes.fogGridWidth);
            const UINT h = std::max(1u, sizes.fogGridHeight);
            const UINT d = std::max(1u, sizes.fogGridDepth);
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            rd.Width = w;
            rd.Height = h;
            rd.DepthOrArraySize = static_cast<UINT16>(d);
            rd.MipLevels = 1;
            rd.Format = formats.fog;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            auto createVolume = [&](GpuResource& res, D3D12_CPU_DESCRIPTOR_HANDLE& srv, D3D12_CPU_DESCRIPTOR_HANDLE& uav,
                                    DeferredSrvSlot srvSlot, DeferredSrvSlot uavSlot, const wchar_t* name)
            {
                ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                    heapProps, D3D12_HEAP_FLAG_NONE, rd,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                    MemTag::Fog, DeferredName(f, name).data(),
                    res.GetAddressOfForCreate()));
                res.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = formats.fog;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture3D.MipLevels = 1;
                srv = DeferredSrvCPU(f, srvSlot);
                dev->CreateShaderResourceView(res.Get(), &sd, srv);
                D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                ud.Format = formats.fog;
                ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
                ud.Texture3D.MipSlice = 0;
                ud.Texture3D.FirstWSlice = 0;
                ud.Texture3D.WSize = d;
                uav = DeferredSrvCPU(f, uavSlot);
                dev->CreateUnorderedAccessView(res.Get(), nullptr, &ud, uav);
            };
            createVolume(D.fogScatter, D.fogScatterSRV, D.fogScatterUAV,
                         DeferredSrvSlot::FogScatter, DeferredSrvSlot::FogScatterUAV, L"FogScatter");
            createVolume(D.fogIntegrated, D.fogIntegratedSRV, D.fogIntegratedUAV,
                         DeferredSrvSlot::FogIntegrated, DeferredSrvSlot::FogIntegratedUAV, L"FogIntegrated");
            D.fogGridWidth = w;
            D.fogGridHeight = h;
            D.fogGridDepth = d;
        }

        // Plan A7 light shafts: UE CreateLightShaftTexture x2 (LightShaftRendering.cpp), half the
        // render resolution, compute-written and compute-read, resting NON_PIXEL like the AO chain.
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.lightShaft,
            DeferredSrvSlot::LightShaftA, DeferredSrvSlot::LightShaftAUAV, f,
            D.lightShaftA, D.lightShaftASRV, D.lightShaftAUAV, sizes.lightShaftWidth, sizes.lightShaftHeight, MemTag::Shafts, L"LightShaftA");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.lightShaft,
            DeferredSrvSlot::LightShaftB, DeferredSrvSlot::LightShaftBUAV, f,
            D.lightShaftB, D.lightShaftBSRV, D.lightShaftBUAV, sizes.lightShaftWidth, sizes.lightShaftHeight, MemTag::Shafts, L"LightShaftB");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.lightShaft,
            DeferredSrvSlot::LightShaftHistory, DeferredSrvSlot::LightShaftHistoryUAV, f,
            D.lightShaftHistory, D.lightShaftHistorySRV, D.lightShaftHistoryUAV, sizes.lightShaftWidth, sizes.lightShaftHeight, MemTag::Shafts, L"LightShaftHistory");
        D.lightShaftWidth = std::max(1u, sizes.lightShaftWidth);
        D.lightShaftHeight = std::max(1u, sizes.lightShaftHeight);

        // Plan C2 volumetric clouds: the half-res march (colour + front depth) and its full-res
        // reconstruction (the same pair at the render size; the previous slot's is the history).
        // Compute-written and compute-read, resting NON_PIXEL like the fog volumes.
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.cloud,
            DeferredSrvSlot::CloudTrace, DeferredSrvSlot::CloudTraceUAV, f,
            D.cloudTrace, D.cloudTraceSRV, D.cloudTraceUAV, sizes.cloudWidth, sizes.cloudHeight, MemTag::Cloud, L"CloudTrace");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.cloudDepth,
            DeferredSrvSlot::CloudTraceDepth, DeferredSrvSlot::CloudTraceDepthUAV, f,
            D.cloudTraceDepth, D.cloudTraceDepthSRV, D.cloudTraceDepthUAV, sizes.cloudWidth, sizes.cloudHeight, MemTag::Cloud, L"CloudTraceDepth");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.cloud,
            DeferredSrvSlot::CloudResolved, DeferredSrvSlot::CloudResolvedUAV, f,
            D.cloudResolved, D.cloudResolvedSRV, D.cloudResolvedUAV, sizes.cloudResolvedWidth, sizes.cloudResolvedHeight, MemTag::Cloud, L"CloudResolved");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.cloudDepth,
            DeferredSrvSlot::CloudResolvedDepth, DeferredSrvSlot::CloudResolvedDepthUAV, f,
            D.cloudResolvedDepth, D.cloudResolvedDepthSRV, D.cloudResolvedDepthUAV, sizes.cloudResolvedWidth, sizes.cloudResolvedHeight, MemTag::Cloud, L"CloudResolvedDepth");
        D.cloudWidth = std::max(1u, sizes.cloudWidth);
        D.cloudHeight = std::max(1u, sizes.cloudHeight);
        D.cloudResolvedWidth = std::max(1u, sizes.cloudResolvedWidth);
        D.cloudResolvedHeight = std::max(1u, sizes.cloudResolvedHeight);

        // P8: the bloom pyramid. Same construction as the HZB chain above -- one SRV over the whole
        // thing, one UAV per mip, built entirely in UNORDERED_ACCESS -- but sized off the DISPLAY
        // resolution, because bloom runs after the upscaler on the image the tonemap reads.
        {
            UINT w = std::max(1u, sizes.bloomWidth);
            UINT h = std::max(1u, sizes.bloomHeight);
            UINT mips = 1;
            while (((w >> mips) > 0 && (h >> mips) > 0) && mips < kBloomMaxMips)
            {
                ++mips;
            }

            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w;
            rd.Height = h;
            rd.DepthOrArraySize = 1;
            rd.MipLevels = static_cast<UINT16>(mips);
            rd.Format = formats.bloom;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

            auto createBloomChain = [&](GpuResource& res,
                                        D3D12_CPU_DESCRIPTOR_HANDLE& srv,
                                        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kBloomMaxMips>& mipUavs,
                                        DeferredSrvSlot srvSlot,
                                        DeferredSrvSlot firstUavSlot,
                                        const wchar_t* name)
            {
                ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                    heapProps, D3D12_HEAP_FLAG_NONE, rd,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                    MemTag::Bloom, DeferredName(f, name).data(),
                    res.GetAddressOfForCreate()));
                res.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);

                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format = formats.bloom;
                sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Texture2D.MipLevels = mips;
                srv = DeferredSrvCPU(f, srvSlot);
                dev->CreateShaderResourceView(res.Get(), &sd, srv);

                for (UINT m = 0; m < kBloomMaxMips; ++m)
                {
                    // Same rule as the HZB chain: slots past the real mip count still get a valid
                    // descriptor, aimed at the last real mip. A VOLATILE table may not have a hole.
                    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
                    ud.Format = formats.bloom;
                    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    ud.Texture2D.MipSlice = (m < mips) ? m : (mips - 1);
                    mipUavs[m] = DeferredSrvCPU(f, static_cast<DeferredSrvSlot>(
                        static_cast<UINT>(firstUavSlot) + m));
                    dev->CreateUnorderedAccessView(res.Get(), nullptr, &ud, mipUavs[m]);
                }
            };

            currentTargetWidth = w;
            currentTargetHeight = h;
            createBloomChain(D.bloomDown, D.bloomDownSRV, D.bloomDownMipUAV,
                DeferredSrvSlot::BloomDown, DeferredSrvSlot::BloomDownMipUav0, L"BloomDown");
            createBloomChain(D.bloomUp, D.bloomUpSRV, D.bloomUpMipUAV,
                DeferredSrvSlot::BloomUp, DeferredSrvSlot::BloomUpMipUav0, L"BloomUp");

            D.bloomMips = mips;
            D.bloomWidth = w;
            D.bloomHeight = h;
        }

        // P8C: the convolution grids are ONE set for every frame in flight, created after this loop.

        // P8C-2: the lens-flare accumulation target. A RENDER target on purpose -- the scatter is
        // an instanced additive raster pass (that is UE's mechanism and the whole point), while
        // everything else in the bloom chain is compute.
        {
            const UINT lw = std::max(1u, sizes.lensFlareWidth);
            const UINT lh = std::max(1u, sizes.lensFlareHeight);
            currentTargetWidth = lw;
            currentTargetHeight = lh;
            CreateRT(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.bloom,
                DeferredRtvSlot::LensFlare, DeferredSrvSlot::LensFlareAccum, DeferredSrvSlot::Count,
                f, D.lensFlare, D.lensFlareRTV, D.lensFlareSRV, MemTag::Bloom, L"LensFlareAccum");
            D.lensFlareWidth = lw;
            D.lensFlareHeight = lh;
        }

        // P8C-2l: the streak pyramid's own two halves, so it no longer borrows mip 1 of the bloom
        // chains -- those belong to the pyramid bloom method whenever it runs.
        {
            const UINT sw = std::max(1u, sizes.streakWidth);
            const UINT sh = std::max(1u, sizes.streakHeight);
            currentTargetWidth = sw;
            currentTargetHeight = sh;
            CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.bloom,
                DeferredSrvSlot::StreakA, DeferredSrvSlot::StreakAUAV, f,
                D.streakA, D.streakASRV, D.streakAUAV, sw, sh, MemTag::Bloom, L"StreakPyramidA");
            CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.bloom,
                DeferredSrvSlot::StreakB, DeferredSrvSlot::StreakBUAV, f,
                D.streakB, D.streakBSRV, D.streakBUAV, sw, sh, MemTag::Bloom, L"StreakPyramidB");
            D.streakWidth = sw;
            D.streakHeight = sh;
        }

        // The bloom's light accumulated over frames (bloom_conv_cs.hlsl stage 12), half the render
        // resolution, compute-written and -read, resting NON_PIXEL like the light shafts' history.
        {
            const UINT hw = std::max(1u, sizes.bloomHistoryWidth);
            const UINT hh = std::max(1u, sizes.bloomHistoryHeight);
            currentTargetWidth = hw;
            currentTargetHeight = hh;
            CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.bloom,
                DeferredSrvSlot::BloomHistory, DeferredSrvSlot::BloomHistoryUAV, f,
                D.bloomHistory, D.bloomHistorySRV, D.bloomHistoryUAV, hw, hh, MemTag::Bloom, L"BloomHistory");
            D.bloomHistoryWidth = hw;
            D.bloomHistoryHeight = hh;
        }

        // Inspector preview. Rests SHADER-READABLE: the overlay transitions FROM a resource's
        // canonical into PIXEL_SHADER_RESOURCE without transitioning back, which is only sound when
        // the canonical already shares that barrier layout.
        currentTargetWidth = kDebugPreviewSize;
        currentTargetHeight = kDebugPreviewSize;
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.debugPreview,
            DeferredSrvSlot::DebugPreview, DeferredSrvSlot::DebugPreviewUAV, f,
            D.debugPreview, D.debugPreviewSRV, D.debugPreviewUAV,
            kDebugPreviewSize, kDebugPreviewSize, MemTag::FrameColor, L"DebugPreview");
        // Address only; the view itself is written per frame by the preview pass.
        D.debugPreviewSrcSRV = DeferredSrvCPU(f, DeferredSrvSlot::DebugPreviewSrc);

        currentTargetWidth = displayWidthClamped;
        currentTargetHeight = displayHeightClamped;
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, formats.sceneColor, DeferredSrvSlot::DLSSOutput, DeferredSrvSlot::DLSSOutputUAV, f, D.dlssOutput, D.dlssOutputSRV, D.dlssOutputUAV, displayWidthClamped, displayHeightClamped, MemTag::FrameColor, L"DlssOutput");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, formats.backbufferResource, DeferredSrvSlot::Tonemap, DeferredSrvSlot::TonemapUAV, f, D.tonemap, D.tonemapSRV, D.tonemapUAV, displayWidthClamped, displayHeightClamped, MemTag::FrameColor, L"Tonemap");
        CreateSrvUavTexture(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, formats.backbufferResource, DeferredSrvSlot::Fxaa, DeferredSrvSlot::FxaaUAV, f, D.fxaa, D.fxaaSRV, D.fxaaUAV, displayWidthClamped, displayHeightClamped, MemTag::FrameColor, L"Fxaa");

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
        // Legacy shadow atlases: ONE each since local-light shadow plan S5, named where they are created.
        nameRes(D.light.Get(), L"Light", kNps);
        nameRes(D.scene.Get(), L"Scene", kNps);
        nameRes(D.sceneOpaque.Get(), L"SceneOpaque", kPs);
        nameRes(D.dlssOutput.Get(), L"DlssOutput", kNps);
        nameRes(D.reflection.Get(), L"Reflection", kNps);
        nameRes(D.reflectionScratch.Get(), L"ReflectionScratch", kNps);
        nameRes(D.reflectionHistory.Get(), L"ReflectionHistory", kNps);
        nameRes(D.reflectionPyramid.Get(), L"ReflectionPyramid", kNps);
        nameRes(D.reflectionGeomPyramid.Get(), L"ReflectionGeomPyramid", kNps);
        // Sampled by the forward ocean/glass draws, so they rest PIXEL-readable, not NPS.
        nameRes(D.oceanReflection.Get(), L"OceanReflection", kNps);
        nameRes(D.oceanReflectionHistory.Get(), L"OceanReflectionHistory", kNps);
        nameRes(D.glassReflNormal.Get(), L"GlassReflNormal", kNps);
        nameRes(D.glassReflDepth.Get(), L"GlassReflDepth", kNps);
        nameRes(D.glassReflection.Get(), L"GlassReflection", kPs);
        nameRes(D.gtao.Get(), L"Gtao", kNps);
        nameRes(D.gtaoFiltered.Get(), L"GtaoFiltered", kNps);
        nameRes(D.gtaoHistory.Get(), L"GtaoHistory", kNps);
        nameRes(D.gtaoUpsampled.Get(), L"GtaoUpsampled", kNps);
        nameRes(D.skyVis.Get(), L"SkyVisibility", kNps);
        nameRes(D.skyVisAccum.Get(), L"SkyVisibilityAccum", kNps);
        // The bloom's convolution grids: ONE set since the 2026-10-22 census, named where they are created.
        nameRes(D.lensFlare.Get(), L"LensFlareAccum", kNps);
        nameRes(D.streakA.Get(), L"StreakPyramidA", kNps);
        nameRes(D.streakB.Get(), L"StreakPyramidB", kNps);
        nameRes(D.bloomHistory.Get(), L"BloomHistory", kNps);
        nameRes(D.bloomDown.Get(), L"BloomDown", kNps);
        nameRes(D.bloomUp.Get(), L"BloomUp", kNps);
        nameRes(D.hzb.Get(), L"Hzb", kNps);
        nameRes(D.hzbClosest.Get(), L"HzbClosest", kNps);
        nameRes(D.fogScatter.Get(), L"FogScatter", kNps);
        nameRes(D.fogIntegrated.Get(), L"FogIntegrated", kNps);
        nameRes(D.lightShaftA.Get(), L"LightShaftA", kNps);
        nameRes(D.lightShaftB.Get(), L"LightShaftB", kNps);
        nameRes(D.lightShaftHistory.Get(), L"LightShaftHistory", kNps);
        nameRes(D.cloudTrace.Get(), L"CloudTrace", kNps);
        nameRes(D.cloudTraceDepth.Get(), L"CloudTraceDepth", kNps);
        nameRes(D.cloudResolved.Get(), L"CloudResolved", kNps);
        nameRes(D.cloudResolvedDepth.Get(), L"CloudResolvedDepth", kNps);
        nameRes(D.debugPreview.Get(), L"DebugPreview", kNps);
        // Tonemap/FXAA end as the compute outputs they are — the resolve flips them back.
        nameRes(D.tonemap.Get(), L"Tonemap", D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        nameRes(D.fxaa.Get(), L"Fxaa", D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // P8C: the convolution grids. Three of them -- two ping-ponged through the transform, one
    // holding the kernel's cached spectrum -- all at the padded power-of-two size, and ONE set of
    // them for every frame in flight, like the shadow atlas below: only the bloom inside
    // Main_Tonemap touches them, start to finish within its frame on the graphics queue, so a
    // second frame's copy would hold nothing the first one does not (2 x 96 MB at 1440p). They
    // rest shader-readable like everything else here, and the transform flips them to UAV for its
    // own duration. The views stay per-frame, in the per-frame heaps, built against the one set.
    {
        const UINT fw = std::max(1u, sizes.bloomFftWidth);
        const UINT fh = std::max(1u, sizes.bloomFftHeight);
        currentTargetWidth = fw;
        currentTargetHeight = fh;
        const D3D12_RESOURCE_DESC rd = MakeTex2DDesc(formats.bloomFft, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto createGrid = [&](GpuResource& res, const wchar_t* name) {
            ThrowIfFailed(gpu::CreateCommittedTexture(dev,
                heapProps, D3D12_HEAP_FLAG_NONE, rd,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, MemTag::Bloom, name,
                res.GetAddressOfForCreate()));
            res.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr); // created in its resting state
        };
        createGrid(bloomFftA_, L"BloomFftA");
        createGrid(bloomFftB_, L"BloomFftB");
        createGrid(bloomFftKernel_, L"BloomFftKernel");

        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = formats.bloomFft;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = formats.bloomFft;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        for (UINT f = 0; f < render::kFrameCount; ++f)
        {
            auto& D = deferred_[f];
            auto views = [&](ID3D12Resource* res, DeferredSrvSlot srvSlot, DeferredSrvSlot uavSlot,
                             D3D12_CPU_DESCRIPTOR_HANDLE& srv, D3D12_CPU_DESCRIPTOR_HANDLE& uav) {
                srv = DeferredSrvCPU(f, srvSlot);
                dev->CreateShaderResourceView(res, &sd, srv);
                uav = DeferredSrvCPU(f, uavSlot);
                dev->CreateUnorderedAccessView(res, nullptr, &ud, uav);
            };
            D.bloomFftA = bloomFftA_.Get();
            D.bloomFftB = bloomFftB_.Get();
            D.bloomFftKernel = bloomFftKernel_.Get();
            views(D.bloomFftA, DeferredSrvSlot::BloomFftA, DeferredSrvSlot::BloomFftAUAV, D.bloomFftASRV, D.bloomFftAUAV);
            views(D.bloomFftB, DeferredSrvSlot::BloomFftB, DeferredSrvSlot::BloomFftBUAV, D.bloomFftBSRV, D.bloomFftBUAV);
            views(D.bloomFftKernel, DeferredSrvSlot::BloomFftKernel, DeferredSrvSlot::BloomFftKernelUAV,
                  D.bloomFftKernelSRV, D.bloomFftKernelUAV);
            D.bloomFftWidth = fw;
            D.bloomFftHeight = fh;
            D.bloomFftImageWidth = std::max(1u, sizes.bloomFftImageWidth);
            D.bloomFftImageHeight = std::max(1u, sizes.bloomFftImageHeight);
        }
    }

    // S3.5: the cascade shadow atlas is ONE resource for every frame in flight, so it is
    // created here rather than inside the loop above; the per-frame DSV/SRV are built inside.
    CreateShadowResource(dev, decls, deferred_[0].shadowRes);
    // Local-light shadow plan S5: the spot / point atlases likewise.
    CreateSpotShadowResource(dev, decls, deferred_[0].spotShadowRes, localSpotPages_);
    CreatePointShadowResource(dev, decls, deferred_[0].pointShadowRes, localPointPages_);
}

// Step 24f-2: CSM cascade atlas (R16_TYPELESS 2D, DSV=D16, SRV=R16). Writes deferred_[f].shadow + its
// views. `resolution` = full-res (Legacy) or 1 (VSM: tiny, directional comes from the clipmap).
void RenderTargetManager::CreateShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT resolution)
{
    if (!dev) { return; }
    if (resolution == 0) { resolution = render::g_csmAtlasRes; }

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

    // S3.5: ONE resource for every frame in flight. GetAddressOfForCreate resets + unregisters the
    // outgoing one first, so a rebuild (resize, residency toggle) cannot leave a stale declaration
    // behind at a recycled address.
    ThrowIfFailed(gpu::CreateCommittedTexture(dev,
        heapProps, D3D12_HEAP_FLAG_NONE, rd,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &cv, MemTag::ShadowCsm, L"CascadeShadow",
        shadowAtlas_.GetAddressOfForCreate()));

    shadowAtlas_.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr); // created in its resting state

    // The views DO stay per-frame: they live in the per-frame descriptor heaps. Only the resource
    // is shared, and every frame's DSV/SRV is built against that one resource.
    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        auto& D = deferred_[f];
        D.shadow = shadowAtlas_.Get();

        D.shadowDSV = DeferredDsvCPU(f, DeferredDsvSlot::Shadow);
        D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
        dsv.Format = DXGI_FORMAT_D16_UNORM;
        dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        dev->CreateDepthStencilView(D.shadow, &dsv, D.shadowDSV);

        D.shadowSRV = DeferredSrvCPU(f, DeferredSrvSlot::Shadow);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R16_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(D.shadow, &sd, D.shadowSRV);
    }
}

// Step 24c: spot shadow atlas (R16_TYPELESS 2D-array, DSV=D16 per slice, SRV=R16 Texture2DArray).
// Writes spotShadowAtlas_ and every frame's alias + views. `resolution` = full-res (Legacy) or 1 (VSM: tiny).
// Local-light shadow plan S9: the slices are PAGES of resolution^2, each holding the squares LightManager packed.
void RenderTargetManager::CreateSpotShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT resolution, UINT pages)
{
    if (!dev) { return; }
    if (resolution == 0) { resolution = 512; }
    pages = std::clamp(pages, 1u, render::kLocalShadowMaxPagesLimit);
    for (UINT f = 0; f < render::kFrameCount; ++f) { deferred_[f].spotShadowPages = pages; }

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
    desc.DepthOrArraySize = static_cast<UINT16>(pages);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // S5: ONE resource for every frame in flight; GetAddressOfForCreate resets + unregisters the outgoing one.
    ThrowIfFailed(gpu::CreateCommittedTexture(dev,
        heapProps, D3D12_HEAP_FLAG_NONE, desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clear, MemTag::ShadowLocal, L"SpotShadow",
        spotShadowAtlas_.GetAddressOfForCreate()));
    spotShadowAtlas_.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr);

    // The views stay per-frame (they live in the per-frame descriptor heaps), all against the one resource.
    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        auto& D = deferred_[f];
        D.spotShadow = spotShadowAtlas_.Get();

        D.spotShadowSRV = DeferredSrvCPU(f, DeferredSrvSlot::SpotShadow);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R16_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = pages;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.PlaneSlice = 0;
        srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
        dev->CreateShaderResourceView(D.spotShadow, &srvDesc, D.spotShadowSRV);

        for (UINT i = 0; i < pages; ++i)
        {
            D.spotShadowDSV[i] = DeferredSpotShadowDsvCPU(f, i);
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DXGI_FORMAT_D16_UNORM;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsv.Flags = D3D12_DSV_FLAG_NONE;
            dsv.Texture2DArray.ArraySize = 1;
            dsv.Texture2DArray.FirstArraySlice = i;
            dsv.Texture2DArray.MipSlice = 0;
            dev->CreateDepthStencilView(D.spotShadow, &dsv, D.spotShadowDSV[i]);
        }
    }
}

// Step 24c: point shadow atlas (R16_TYPELESS slices; DSV=D16 per slice, SRV=R16 2D array). Mirrors
// CreateSpotShadowResource. `resolution` = the largest cube face (Legacy) or 1 (VSM).
// Local-light shadow plan S9: the slices are PAGES of 3 x 2 faces of `resolution` -- exactly one block of the largest
// cube; each light's six faces are one 3f x 2f block LightManager packed into a page.
void RenderTargetManager::CreatePointShadowResource(ID3D12Device* dev, ResourceDeclarations decls, UINT resolution, UINT pages)
{
    if (!dev) { return; }
    if (resolution == 0) { resolution = 512; }
    pages = std::clamp(pages, 1u, render::kLocalShadowMaxPagesLimit);
    for (UINT f = 0; f < render::kFrameCount; ++f) { deferred_[f].pointShadowPages = pages; }

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
    desc.Width = 3u * resolution;
    desc.Height = 2u * resolution;
    desc.DepthOrArraySize = static_cast<UINT16>(pages);
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    // S5: ONE resource for every frame in flight, as the spot atlas.
    ThrowIfFailed(gpu::CreateCommittedTexture(dev,
        heapProps, D3D12_HEAP_FLAG_NONE, desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear, MemTag::ShadowLocal, L"PointShadow",
        pointShadowAtlas_.GetAddressOfForCreate()));
    pointShadowAtlas_.DeclareCreated(decls, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr);

    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        auto& D = deferred_[f];
        D.pointShadow = pointShadowAtlas_.Get();

        // Plan S4: a 2D ARRAY, not a cube array -- each light's faces are rectangles of its own resolution, which a
        // hardware cube lookup (always the whole face) cannot address. The shaders pick the face and its texel
        // themselves (local_shadow_filter.hlsli PointShadowCubeUV, D3D's cube face convention).
        D.pointShadowSRV = DeferredSrvCPU(f, DeferredSrvSlot::PointShadow);
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R16_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MostDetailedMip = 0;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.FirstArraySlice = 0;
        srvDesc.Texture2DArray.ArraySize = pages;
        srvDesc.Texture2DArray.PlaneSlice = 0;
        srvDesc.Texture2DArray.ResourceMinLODClamp = 0.0f;
        dev->CreateShaderResourceView(D.pointShadow, &srvDesc, D.pointShadowSRV);

        for (UINT page = 0; page < pages; ++page)
        {
            D.pointShadowDSV[page] = DeferredPointShadowDsvCPU(f, page);
            D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DXGI_FORMAT_D16_UNORM;
            dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            dsv.Flags = D3D12_DSV_FLAG_NONE;
            dsv.Texture2DArray.ArraySize = 1;
            dsv.Texture2DArray.FirstArraySlice = page;
            dsv.Texture2DArray.MipSlice = 0;
            dev->CreateDepthStencilView(D.pointShadow, &dsv, D.pointShadowDSV[page]);
        }
    }
}

void RenderTargetManager::SetLocalShadowResidency(ID3D12Device* dev, ResourceDeclarations decls, bool full)
{
    if (full == localShadowFull_ || !dev) { return; }
    // Keep the configured resolutions; only the created size changes (1 = tiny placeholder). Local-light
    // shadow plan S5: the spot / point atlases are ONE resource each, rebuilt once (GetAddressOfForCreate
    // inside resets and unregisters the outgoing one, so a re-used address can't inherit a stale state).
    CreateSpotShadowResource(dev, decls, full ? deferred_[0].spotShadowRes : 1u, full ? localSpotPages_ : 1u);
    CreatePointShadowResource(dev, decls, full ? deferred_[0].pointShadowRes : 1u, full ? localPointPages_ : 1u);
    // VSM mode retires the CSM cascade atlas as well: the render graph omits the Main_CSM pass in VSM
    // mode, so nothing renders into this 1x1 placeholder.
    // S3.5: the cascade atlas is ONE resource, so it is forgotten and rebuilt once, not per frame.
    // (GetAddressOfForCreate inside does the Forget; the explicit one here would be redundant.)
    CreateShadowResource(dev, decls, full ? deferred_[0].shadowRes : 1u);
    localShadowFull_ = full;
}

UINT RenderTargetManager::LocalShadowSliceRes(UINT knob)
{
    // A power of two: every light's square is one (S4), and must fit the slice exactly.
    UINT res = render::kLocalShadowMaxResMin;
    const UINT wanted = std::clamp(knob, render::kLocalShadowMaxResMin, render::kLocalShadowMaxResMax);
    while (res * 2u <= wanted) { res *= 2u; }
    return res;
}

void RenderTargetManager::SetLocalShadowMaxResolution(ID3D12Device* dev, ResourceDeclarations decls, UINT knob)
{
    const UINT res = LocalShadowSliceRes(knob);
    if (!dev || res == deferred_[0].spotShadowRes) { return; }
    for (UINT f = 0; f < render::kFrameCount; ++f)
    {
        deferred_[f].spotShadowRes = res;
        deferred_[f].pointShadowRes = res / 2u;
    }
    // As the cascade atlas: resident only outside VSM; otherwise the size is remembered for SetLocalShadowResidency.
    if (localShadowFull_)
    {
        CreateSpotShadowResource(dev, decls, res, localSpotPages_);
        CreatePointShadowResource(dev, decls, res / 2u, localPointPages_);
    }
}

void RenderTargetManager::SetLocalShadowPages(ID3D12Device* dev, ResourceDeclarations decls, UINT spotPages, UINT pointPages)
{
    spotPages = std::clamp(spotPages, 1u, render::kLocalShadowMaxPagesLimit);
    pointPages = std::clamp(pointPages, 1u, render::kLocalShadowMaxPagesLimit);
    const bool spotChanged = (spotPages != localSpotPages_);
    const bool pointChanged = (pointPages != localPointPages_);
    localSpotPages_ = spotPages;   // remembered: a resize's Create and the residency toggle build these counts
    localPointPages_ = pointPages;
    if (!dev || !localShadowFull_) { return; } // VSM: the 1x1 placeholders stay
    if (spotChanged) { CreateSpotShadowResource(dev, decls, deferred_[0].spotShadowRes, spotPages); }
    if (pointChanged) { CreatePointShadowResource(dev, decls, deferred_[0].pointShadowRes, pointPages); }
}

void RenderTargetManager::SetCsmAtlasResolution(ID3D12Device* dev, ResourceDeclarations decls, UINT resolution)
{
    resolution = std::clamp(resolution, render::kCsmAtlasResMin, render::kCsmAtlasResMax);
    if (!dev || resolution == deferred_[0].shadowRes) { return; }
    for (UINT f = 0; f < render::kFrameCount; ++f) { deferred_[f].shadowRes = resolution; }
    // Only rebuild if the atlas is actually resident. In VSM mode it is the 1x1 placeholder and
    // the new size is simply remembered above -- SetLocalShadowResidency(true) will use it.
    if (localShadowFull_) { CreateShadowResource(dev, decls, resolution); }
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
        collect(D.reflectionPyramid);
        collect(D.reflectionGeomPyramid);
        collect(D.oceanReflection);
        collect(D.oceanReflectionHistory);
        D.shadow = nullptr; // S3.5: non-owning alias; shadowAtlas_ below is the owner
        D.spotShadow = nullptr;  // S5: non-owning aliases; the atlases below are the owners
        D.pointShadow = nullptr;
        D.bloomFftA = nullptr;   // non-owning aliases; bloomFftA_ / B_ / Kernel_ below are the owners
        D.bloomFftB = nullptr;
        D.bloomFftKernel = nullptr;
        collect(D.dlssOutput);
        collect(D.glassReflNormal);
        collect(D.glassReflDepth);
        collect(D.glassReflection);
        collect(D.lensFlare);
        collect(D.streakA);
        collect(D.streakB);
        collect(D.bloomHistory);
        collect(D.fogScatter);
        collect(D.fogIntegrated);
        collect(D.cloudTrace);
        collect(D.cloudTraceDepth);
        collect(D.cloudResolved);
        collect(D.cloudResolvedDepth);
        collect(D.lightShaftA);
        collect(D.lightShaftB);
        collect(D.lightShaftHistory);
    }

    shadowAtlas_.Reset(); // S3.5: the single cascade atlas; Reset unregisters it
    spotShadowAtlas_.Reset();  // local-light shadow plan S5, likewise
    pointShadowAtlas_.Reset();
    bloomFftA_.Reset(); // the single set of bloom convolution grids, likewise
    bloomFftB_.Reset();
    bloomFftKernel_.Reset();

    decls.ForgetMany(released);

    for (DeferredTargets& D : deferred_) {
        D = DeferredTargets{};
    }
}
