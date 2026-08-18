#include "ocean/OceanWetness.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Helpers.h"
#include "materials/Material.h"
#include "rendering/core/ComputeDispatch.h"
#include "rendering/core/Renderer.h"
#include "rendering/core/RenderGraph.h"
#include "rendering/core/TextureCreate.h"

using Microsoft::WRL::ComPtr;

namespace
{
struct WetnessUpdateCB
{
    std::uint32_t resolution = 0;
    float wetAmount = 0.0f;
    float dryAmount = 0.0f;
    std::uint32_t clearHistory = 0;
    std::int32_t shiftX = 0;
    std::int32_t shiftY = 0;
};
}

void OceanWetness::EnsureResources(Renderer* renderer)
{
    if (created_ || !renderer || !renderer->GetDevice())
    {
        return;
    }

    auto* device = renderer->GetDevice();

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kResolution;
    desc.Height = kResolution;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    // The two halves of this ping-pong REST IN DIFFERENT STATES, and each half's resting state is
    // the same whichever side of the swap it is playing this frame. Getting that distinction wrong
    // is what made --canonical-check report all four every frame:
    //
    //  * history: BuildUpdatePass leaves the read slot NON_PIXEL and ends by putting the write slot
    //    there too, and Compose then reads the current one as an SRV -- so BOTH histories rest in
    //    NON_PIXEL_SHADER_RESOURCE, on every frame, regardless of parity. Declaring UAV described
    //    the state they are in for one dispatch, not the one they sit in.
    //  * stamp: the ocean's own draw stamps into the CURRENT one as a UAV, so UAV is right for it --
    //    but the other one used to be abandoned in NON_PIXEL, which is why the report alternated
    //    between StampA and StampB. BuildUpdatePass now hands the read slot back (see below), so
    //    both rest in UAV and this declaration is true for either.
    //
    // These are TEXTURES: unlike buffers (P12.2), their creation state is honoured, so it has to
    // match the declaration or the first compiled barrier transitions from a state nothing is in.
    constexpr D3D12_RESOURCE_STATES kHistoryRest = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    constexpr D3D12_RESOURCE_STATES kStampRest = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    for (int i = 0; i < 2; ++i)
    {
        ComPtr<ID3D12Resource> resource;
        ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE,
            desc, kHistoryRest, nullptr, &resource));
        history_[i].Attach(renderer->Declarations(), std::move(resource),
            kHistoryRest, i == 0 ? L"Ocean.WetnessA" : L"Ocean.WetnessB");

        ComPtr<ID3D12Resource> stampResource;
        ThrowIfFailed(render::CreateCommittedTexture(device, heapProps, D3D12_HEAP_FLAG_NONE,
            desc, kStampRest, nullptr, &stampResource));
        stamp_[i].Attach(renderer->Declarations(), std::move(stampResource),
            kStampRest, i == 0 ? L"Ocean.WetnessStampA" : L"Ocean.WetnessStampB");
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.NumDescriptors = 8;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap_)));

    const UINT increment =
        device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE handle = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
    auto next = [&]()
    {
        const D3D12_CPU_DESCRIPTOR_HANDLE result = handle;
        handle.ptr += increment;
        return result;
    };

    for (int i = 0; i < 2; ++i)
    {
        historySrv_[i] = next();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_UINT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(history_[i].Get(), &srvDesc, historySrv_[i]);
    }
    for (int i = 0; i < 2; ++i)
    {
        stampSrv_[i] = next();
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = DXGI_FORMAT_R32_UINT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(stamp_[i].Get(), &srvDesc, stampSrv_[i]);
    }
    for (int i = 0; i < 2; ++i)
    {
        historyUav_[i] = next();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(history_[i].Get(), nullptr, &uavDesc, historyUav_[i]);
    }
    for (int i = 0; i < 2; ++i)
    {
        stampUav_[i] = next();
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_R32_UINT;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(stamp_[i].Get(), nullptr, &uavDesc, stampUav_[i]);
    }

    Material::ComputeDesc materialDesc{};
    materialDesc.shaderFile = L"shaders/ocean_wetness_update_cs.hlsl";
    materialDesc.csEntry = "CSMain";
    updateMaterial_ = renderer->GetMaterialManager()->GetOrCreateCompute(renderer, materialDesc);

    created_ = true;
}

void OceanWetness::TickWindow(Math::float2 cameraXZ)
{
    if (!created_)
    {
        return;
    }

    const float snap = kTexelWorld * kSnapTexels;
    const Math::float2 target(
        std::floor(cameraXZ.x / snap) * snap + snap * 0.5f,
        std::floor(cameraXZ.y / snap) * snap + snap * 0.5f);

    if (!hasCenter_)
    {
        center_ = target;
        pendingCenter_ = target;
        hasCenter_ = true;
        return;
    }

    pendingShiftX_ = static_cast<int>(std::lround((target.x - center_.x) / kTexelWorld));
    pendingShiftY_ = static_cast<int>(std::lround((target.y - center_.y) / kTexelWorld));
    pendingCenter_ = target;
}

std::function<void(RenderGraphPassContext)> OceanWetness::BuildUpdatePass(
    RenderGraphPassContext& ctx,
    float timeSeconds,
    float wetTimeSeconds,
    float dryTimeSeconds)
{
    if (!created_ || !updateMaterial_)
    {
        return {};
    }

    const float elapsed = previousTime_ >= 0.0f
        ? std::max(0.0f, timeSeconds - previousTime_)
        : 0.0f;
    previousTime_ = timeSeconds;

    bool relocate = pendingShiftX_ != 0 || pendingShiftY_ != 0;
    int shiftX = pendingShiftX_;
    int shiftY = pendingShiftY_;
    if (relocate)
    {
        center_ = pendingCenter_;
    }
    pendingShiftX_ = 0;
    pendingShiftY_ = 0;

    const bool clearHistory = needsInitialClear_;
    if (clearHistory)
    {
        needsInitialClear_ = false;
        relocate = true;
        shiftX = static_cast<int>(kResolution);
        shiftY = 0;
    }

    if (!relocate && elapsed <= 0.0f)
    {
        return {};
    }

    const std::uint32_t readIndex = current_;
    const std::uint32_t writeIndex = 1u - readIndex;
    current_ = writeIndex;

    const float wetTime = std::max(wetTimeSeconds, 0.05f);
    const float dryTime = std::max(dryTimeSeconds, 0.05f);
    const float wetAmount = std::clamp(elapsed / wetTime, 0.0f, 1.0f);
    const float dryAmount = std::clamp(elapsed / dryTime, 0.0f, 1.0f);

    ctx.NextPoint();
    const std::uint32_t updatePoint = ctx.usePoint ? *ctx.usePoint : 0u;
    ctx.Use(history_[readIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(stamp_[readIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(history_[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.Use(stamp_[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.NextPoint();
    ctx.Use(history_[writeIndex].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ctx.Use(stamp_[writeIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Hand the READ stamp back. Without this the slot the update sampled was abandoned in
    // NON_PIXEL while its partner sat in UAV, so each stamp's resting state flipped with the
    // parity of the swap and neither could match one declaration. Both rest in UAV now.
    ctx.Use(stamp_[readIndex].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    return [this, readIndex, writeIndex, shiftX, shiftY, wetAmount, dryAmount,
               clearHistory, updatePoint]
        (RenderGraphPassContext pass)
    {
        Renderer* renderer = pass.renderer;
        auto token = pass.BeginCL();
        ID3D12GraphicsCommandList* cl = token.cl;
        renderer->EmitPoint(cl, updatePoint);

        WetnessUpdateCB constants{};
        constants.resolution = kResolution;
        constants.wetAmount = wetAmount;
        constants.dryAmount = dryAmount;
        constants.clearHistory = clearHistory ? 1u : 0u;
        constants.shiftX = shiftX;
        constants.shiftY = shiftY;

        RecordComputeDispatch(renderer, cl, updateMaterial_.get(), sizeof(constants),
            [&constants](std::uint8_t* destination)
            {
                std::memcpy(destination, &constants, sizeof(constants));
            },
            { historySrv_[readIndex], stampSrv_[readIndex] },
            { historyUav_[writeIndex], stampUav_[writeIndex] }, {},
            kResolution, kResolution, history_[writeIndex].Get());
        renderer->UAVBarrier(cl, stamp_[writeIndex].Get());

        renderer->EmitPoint(cl, updatePoint + 1u);
        pass.EndCL(token);
    };
}

ID3D12Resource* OceanWetness::GetCurrentResource() const
{
    return created_ ? history_[current_].Get() : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE OceanWetness::GetCurrentSrv() const
{
    return created_ ? historySrv_[current_] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

ID3D12Resource* OceanWetness::GetCurrentStampResource() const
{
    return created_ ? stamp_[current_].Get() : nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE OceanWetness::GetCurrentStampUav() const
{
    return created_ ? stampUav_[current_] : D3D12_CPU_DESCRIPTOR_HANDLE{};
}

Math::float4 OceanWetness::GetWindowParams() const
{
    return Math::float4(center_.x, center_.y, 1.0f / kHalfExtent, kTexelWorld);
}
