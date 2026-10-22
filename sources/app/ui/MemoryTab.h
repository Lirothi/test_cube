#pragma once

#include <vector>

#include "rendering/core/MemoryReport.h"

struct ID3D12Device;

// The Memory tab of Developer Controls -- memory profiling plan M2 (docs/memory_profiling_plan.md).
//
// What the memory line says, live, and what a line cannot fit: the process and the adapter, the GPU
// census as a tag tree with its change since a snapshot (Unreal Insights' Size A / Size B / Diff),
// and every GPU object, sortable and filterable (the shape of UE's rhi.DumpResourceMemory). MCP
// `get_memory` reads the same numbers.
//
// COST RULE: nothing is read unless the tab is drawn, and then twice a second, not per frame -- a
// census copy and a records snapshot are cheap but not free, and these numbers move slowly.
class MemoryTab
{
public:
    void Draw(ID3D12Device* device);

private:
    void Refresh(ID3D12Device* device);
    void RebuildOrder();

    double lastRefreshSec_ = -1.0;
    render::MemoryPlatform platform_;
    gpu::CensusTotals census_;
    mem::CpuTagTotals cpu_; // --memtrack only (M3)
    bool tracking_ = false;
    std::vector<gpu::RecordInfo> records_;
    std::vector<int> order_; // indices into records_ after the filter and the sort
    bool orderDirty_ = true;
    char filter_[96] = {};
    int sortColumn_ = 4; // MB
    bool sortAscending_ = false;
    bool haveSnapshot_ = false;
    render::MemorySnapshot snapshot_;
};
