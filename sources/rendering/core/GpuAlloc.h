#pragma once
// Every GPU allocation, with its owner -- memory profiling plan M1 (docs/memory_profiling_plan.md §4.2).
//
// The engine's D3D12 objects that hold memory are created HERE and nowhere else (enforced by
// tools/check_gpu_alloc.py): committed resources, descriptor heaps, query heaps. Each wrapper is
// the D3D12 call with two more arguments -- the owner's MemTag and a debug name -- and records the
// object in the GPU census: tag, name, bytes (GetResourceAllocationInfo: what the implicit heap of
// a committed resource really takes, not the desc's width), pool (local = DEFAULT, non-local =
// UPLOAD/READBACK), shape.
//
// Release needs no call. A small COM object rides on the D3D object as private data
// (SetPrivateDataInterface); D3D releases it when the object is destroyed, whichever path killed
// it -- a retire bin, a ComPtr, a keep-alive vector, a swapchain resize -- and that release takes
// the record out. CanonicalStateRegistry::Forget could not be the hook: not every release path
// unregisters there (ResourceDeclarations.h:55-58).
//
// UE reports each D3D12 texture by hand for the same reason ("On Windows there is no way to hook
// into the low level d3d allocations and frees", D3D12Texture.cpp:101-107); its key is a fake
// address `this+1` (D3D12Resources.h:749-753). Ours is the private-data object itself.

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

#include "core/memory/MemTags.h"

namespace gpu
{
// The census pools: where the bytes live on a discrete GPU.
enum class MemPool : std::uint8_t
{
    Local = 0,    // DEFAULT (and GPU_UPLOAD, CUSTOM L1): video memory
    NonLocal = 1, // UPLOAD, READBACK, CUSTOM L0: system memory the GPU maps (write-combined on the CPU side)
    Count
};

// The D3D12 calls, plus owner and name. `name` may be null (the object stays unnamed, the census
// says "(unnamed)"); when given it is also set as the object's debug name.
HRESULT CreateCommittedResource(ID3D12Device* device, const D3D12_HEAP_PROPERTIES* heapProps,
                                D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC* desc,
                                D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
                                MemTag tag, const wchar_t* name, REFIID riid, void** out);

// The texture path (render::CreateCommittedTexture: the enhanced-barrier layout when that path is
// on, byte-for-byte the legacy call otherwise), plus owner and name.
HRESULT CreateCommittedTexture(ID3D12Device* device, const D3D12_HEAP_PROPERTIES& heapProps,
                               D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC& desc,
                               D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE* clearValue,
                               MemTag tag, const wchar_t* name, ID3D12Resource** out);

HRESULT CreateDescriptorHeap(ID3D12Device* device, const D3D12_DESCRIPTOR_HEAP_DESC* desc,
                             MemTag tag, const wchar_t* name, REFIID riid, void** out);

HRESULT CreateQueryHeap(ID3D12Device* device, const D3D12_QUERY_HEAP_DESC* desc,
                        MemTag tag, const wchar_t* name, REFIID riid, void** out);

// A resource the engine did not create but owns the memory of (the swapchain's buffers, made by
// DXGI). Recorded like the rest; released with the object.
void TrackExternalResource(ID3D12Device* device, ID3D12Resource* resource, MemTag tag, const wchar_t* name);

// ---- Readout ----

struct TagTotals
{
    std::uint64_t bytes[static_cast<int>(MemPool::Count)] = {}; // resources, by pool
    std::uint32_t resources = 0;
    std::uint64_t bufferSlack = 0;     // committed size - buffer width, summed (the 64 KB granule)
    std::uint64_t descriptorBytes = 0; // descriptor/query heaps: NumDescriptors x increment, an estimate
    std::uint32_t heaps = 0;           // descriptor + query heaps
};

struct CensusTotals
{
    TagTotals tags[kMemTagCount];
    TagTotals all;
    std::uint64_t created = 0;  // objects ever recorded
    std::uint64_t released = 0; // objects whose release came back
};

// A consistent snapshot of the per-tag totals (one lock).
void ReadCensus(CensusTotals& out);

// One live object, as recorded at creation -- the table the memory report prints (the shape of
// UE's rhi.DumpResourceMemory, RHI.cpp:879-1096).
struct RecordInfo
{
    std::string name;
    MemTag tag = MemTag::Untagged;
    MemPool pool = MemPool::Local;
    const char* kind = "";   // "buffer", "texture", "descheap", "queryheap"
    std::uint64_t bytes = 0; // committed size; heaps: the estimate
    std::uint64_t slack = 0; // buffers: committed size - width
};

// Copies every live record out under the lock. Allocates: for reports, not per frame.
void SnapshotRecords(std::vector<RecordInfo>& out);
} // namespace gpu
