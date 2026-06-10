#pragma once

#include <cstdint>
#include <initializer_list>
#include <utility>

#include <d3d12.h>

#include "rendering/core/Renderer.h"
#include "rendering/core/RenderContext.h"
#include "rendering/core/RenderContextPool.h"
#include "materials/Material.h"

// Records one compute dispatch into an already-open command list: allocates and
// writes the pass constant buffer, stages the SRV/UAV descriptor tables, binds the
// material and dispatches ceil(width/8) x ceil(height/8) thread groups, optionally
// followed by a UAV barrier on one resource.
// Resource state transitions stay at the call site (until the render graph owns them).
inline constexpr UINT kComputeDispatchGroupSize = 8;

template <typename WriteCBFn>
inline void RecordComputeDispatch(
    Renderer* renderer,
    ID3D12GraphicsCommandList* cl,
    Material* material,
    UINT cbSizeBytes,
    WriteCBFn&& writeConstants, // void(uint8_t* dest); called only when cbSizeBytes > 0
    std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> srvTable,
    std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> uavTable,
    D3D12_GPU_DESCRIPTOR_HANDLE samplerTable,
    UINT width, UINT height,
    ID3D12Resource* uavBarrierResource = nullptr)
{
    auto h = renderer->GetRenderContextPool()->Acquire();
    RenderContext& rc = h.ref();

    if (cbSizeBytes > 0)
    {
        auto cb = renderer->GetFrameResource()->AllocDynamic(cbSizeBytes, Renderer::kConstantBufferAlignment);
        std::forward<WriteCBFn>(writeConstants)(static_cast<uint8_t*>(cb.cpu));
        rc.cbv[0] = cb.gpu;
    }
    if (srvTable.size() > 0)
    {
        rc.table[0] = renderer->StageSrvUavTable(srvTable).gpu;
    }
    if (uavTable.size() > 0)
    {
        rc.table[1] = renderer->StageSrvUavTable(uavTable).gpu;
    }
    rc.samplerTable[0] = samplerTable;

    material->Bind(cl, rc);

    const UINT groupsX = (width + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
    const UINT groupsY = (height + kComputeDispatchGroupSize - 1u) / kComputeDispatchGroupSize;
    if (groupsX > 0 && groupsY > 0)
    {
        cl->Dispatch(groupsX, groupsY, 1);
    }

    if (uavBarrierResource)
    {
        renderer->UAVBarrier(cl, uavBarrierResource);
    }
}

// Overload for passes without a constant buffer.
inline void RecordComputeDispatch(
    Renderer* renderer,
    ID3D12GraphicsCommandList* cl,
    Material* material,
    std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> srvTable,
    std::initializer_list<D3D12_CPU_DESCRIPTOR_HANDLE> uavTable,
    D3D12_GPU_DESCRIPTOR_HANDLE samplerTable,
    UINT width, UINT height,
    ID3D12Resource* uavBarrierResource = nullptr)
{
    RecordComputeDispatch(renderer, cl, material, 0u, [](uint8_t*) {},
        srvTable, uavTable, samplerTable, width, height, uavBarrierResource);
}
