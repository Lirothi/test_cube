#include "rendering/rt/RtSmoke.h"

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace {

FILE* gLog = nullptr;

void Log(const char* fmt, ...)
{
    if (!gLog) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vfprintf(gLog, fmt, args);
    va_end(args);
    fflush(gLog);
}

void WriteVerdict(const char* outPath, const char* verdict)
{
    Log("verdict: %s\n", verdict);
    FILE* f = nullptr;
    fopen_s(&f, outPath, "w");
    if (f) {
        fprintf(f, "%s\n", verdict);
        fclose(f);
    }
}

// Committed buffer helper. UPLOAD heaps stay in GENERIC_READ; DEFAULT heaps take
// the requested state. Returns null on failure (logged by the caller).
ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* dev,
                                    UINT64 size,
                                    D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES initialState,
                                    D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = heapType;

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;

    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            initialState, nullptr, IID_PPV_ARGS(&res)))) {
        return nullptr;
    }
    return res;
}

// Allocate result + scratch buffers for an AS from its prebuild info and record
// the build. `result` ends in RAYTRACING_ACCELERATION_STRUCTURE state.
bool BuildAS(ID3D12Device5* dev5,
             ID3D12GraphicsCommandList4* cl4,
             const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& inputs,
             ComPtr<ID3D12Resource>& result,
             ComPtr<ID3D12Resource>& scratch,
             const char* label)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
    Log("%s prebuild: result=%llu scratch=%llu\n", label,
        info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes);
    if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0) {
        Log("%s: prebuild reported zero size\n", label);
        return false;
    }

    result = CreateBuffer(dev5, info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                          D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    scratch = CreateBuffer(dev5, info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    if (!result || !scratch) {
        Log("%s: result/scratch allocation failed\n", label);
        return false;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
    build.Inputs = inputs;
    build.DestAccelerationStructureData = result->GetGPUVirtualAddress();
    build.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
    cl4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

    // Make the build's writes visible to the next consumer (TLAS reads BLAS;
    // the host reads the TLAS after the fence).
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav.UAV.pResource = result.Get();
    cl4->ResourceBarrier(1, &uav);
    return true;
}

} // namespace

int RunRtSmoke(const char* outPath)
{
    fopen_s(&gLog, "rt_smoke.log", "w");
    Log("rt smoke harness\n");

#ifdef _DEBUG
    // Debug builds: enable the debug layer + GPU-based validation so the AS
    // builds below are validated (S3 verifies BLAS builds through this harness).
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            ComPtr<ID3D12Debug1> dbg1;
            if (SUCCEEDED(dbg.As(&dbg1))) {
                dbg1->SetEnableGPUBasedValidation(TRUE);
            }
            Log("debug layer + GPU-based validation enabled\n");
        }
    }
#endif

    // --- Device (default hardware adapter, FL 11_0 like the renderer) ---
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        Log("D3D12CreateDevice failed\n");
        WriteVerdict(outPath, "FAIL device-create");
        if (gLog) { fclose(gLog); gLog = nullptr; }
        return 1;
    }

    // --- DXR capability (mirrors GraphicsDevice S1 detection) ---
    ComPtr<ID3D12Device5> device5;
    D3D12_RAYTRACING_TIER tier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    if (SUCCEEDED(device.As(&device5))) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
            tier = options5.RaytracingTier;
        }
    }
    Log("RaytracingTier: %d (1_0=%d, 1_1=%d)\n", static_cast<int>(tier),
        static_cast<int>(D3D12_RAYTRACING_TIER_1_0), static_cast<int>(D3D12_RAYTRACING_TIER_1_1));

    if (!device5 || tier < D3D12_RAYTRACING_TIER_1_1) {
        // No hardware ray tracing — this is a graceful skip, not a failure.
        WriteVerdict(outPath, "SKIP no-rt");
        if (gLog) { fclose(gLog); gLog = nullptr; }
        return 0;
    }

    int result = 1; // FAIL until proven otherwise
    bool removed = false;

    // --- Queue / allocator / command list (QI to CommandList4) ---
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12GraphicsCommandList4> cl4;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    bool ready =
        SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))) &&
        SUCCEEDED(cl.As(&cl4));
    if (!ready) {
        Log("command-list bring-up failed (CommandList4 unavailable?)\n");
        WriteVerdict(outPath, "FAIL cmdlist4");
        if (gLog) { fclose(gLog); gLog = nullptr; }
        return 1;
    }

    // --- Geometry: one triangle in an UPLOAD buffer (GENERIC_READ satisfies
    //     the NON_PIXEL_SHADER_RESOURCE state AS build inputs require). ---
    const float verts[3][3] = {
        { 0.0f, 0.0f, 0.0f },
        { 1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f },
    };
    ComPtr<ID3D12Resource> vb = CreateBuffer(device.Get(), sizeof(verts), D3D12_HEAP_TYPE_UPLOAD,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    if (vb) {
        void* mapped = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        if (SUCCEEDED(vb->Map(0, &noRead, &mapped))) {
            memcpy(mapped, verts, sizeof(verts));
            vb->Unmap(0, nullptr);
        } else {
            vb.Reset();
        }
    }

    ComPtr<ID3D12Resource> blas, blasScratch, tlas, tlasScratch, instanceDescs;
    bool built = false;
    if (vb) {
        // BLAS over the single triangle (non-indexed, opaque, fast trace).
        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        geom.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
        geom.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
        geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        geom.Triangles.VertexCount = 3;
        geom.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
        geom.Triangles.IndexCount = 0;

        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs{};
        blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        blasInputs.NumDescs = 1;
        blasInputs.pGeometryDescs = &geom;

        if (BuildAS(device5.Get(), cl4.Get(), blasInputs, blas, blasScratch, "blas")) {
            // One identity-transform instance referencing the BLAS.
            D3D12_RAYTRACING_INSTANCE_DESC inst{};
            inst.Transform[0][0] = 1.0f;
            inst.Transform[1][1] = 1.0f;
            inst.Transform[2][2] = 1.0f;
            inst.InstanceMask = 0xFF;
            inst.AccelerationStructure = blas->GetGPUVirtualAddress();

            instanceDescs = CreateBuffer(device.Get(), sizeof(inst), D3D12_HEAP_TYPE_UPLOAD,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
            if (instanceDescs) {
                void* mapped = nullptr;
                D3D12_RANGE noRead{ 0, 0 };
                if (SUCCEEDED(instanceDescs->Map(0, &noRead, &mapped))) {
                    memcpy(mapped, &inst, sizeof(inst));
                    instanceDescs->Unmap(0, nullptr);

                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs{};
                    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                    tlasInputs.NumDescs = 1;
                    tlasInputs.InstanceDescs = instanceDescs->GetGPUVirtualAddress();

                    built = BuildAS(device5.Get(), cl4.Get(), tlasInputs, tlas, tlasScratch, "tlas");
                }
            }
        }
    }

    // --- Execute + fence wait (always close the open list before submitting) ---
    HRESULT closeHr = cl4->Close();
    if (built && SUCCEEDED(closeHr)) {
        ID3D12CommandList* lists[] = { cl4.Get() };
        queue->ExecuteCommandLists(1, lists);

        ComPtr<ID3D12Fence> fence;
        if (SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (evt) {
                queue->Signal(fence.Get(), 1);
                if (fence->GetCompletedValue() < 1) {
                    fence->SetEventOnCompletion(1, evt);
                    WaitForSingleObject(evt, INFINITE);
                }
                CloseHandle(evt);

                const HRESULT dr = device->GetDeviceRemovedReason();
                removed = FAILED(dr);
                Log("device removed reason: 0x%08lX\n", static_cast<unsigned long>(dr));
                if (!removed) {
                    result = 0;
                }
            }
        }
    } else {
        Log("skipping execute: built=%d closeHr=0x%08lX\n", built ? 1 : 0,
            static_cast<unsigned long>(closeHr));
    }

    if (result == 0) {
        char verdict[64];
        std::snprintf(verdict, sizeof(verdict), "PASS tier=%d", static_cast<int>(tier));
        WriteVerdict(outPath, verdict);
    } else {
        WriteVerdict(outPath, removed ? "FAIL device-removed" : "FAIL as-build");
    }

    if (gLog) { fclose(gLog); gLog = nullptr; }
    return result;
}
