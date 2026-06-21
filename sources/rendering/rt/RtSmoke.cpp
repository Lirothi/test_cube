#include "rendering/rt/RtSmoke.h"
#include "rendering/rt/AccelerationStructure.h"
#include "rendering/meshes/Mesh.h"

#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <exception>
#include <vector>

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

    // --- Queue / allocator / command list (QI to CommandList4) + fence ---
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12GraphicsCommandList4> cl4;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    bool ready =
        SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))) &&
        SUCCEEDED(cl.As(&cl4)) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE evt = ready ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    if (!ready || !evt) {
        Log("command-list/fence bring-up failed (CommandList4 unavailable?)\n");
        WriteVerdict(outPath, "FAIL cmdlist4");
        if (evt) { CloseHandle(evt); }
        if (gLog) { fclose(gLog); gLog = nullptr; }
        return 1;
    }

    // Submit the open list, signal `value`, and block until the GPU reaches it.
    UINT64 fenceValue = 0;
    auto submitAndWait = [&]() {
        ID3D12CommandList* lists[] = { cl4.Get() };
        queue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++fenceValue;
        queue->Signal(fence.Get(), value);
        if (fence->GetCompletedValue() < value) {
            fence->SetEventOnCompletion(value, evt);
            WaitForSingleObject(evt, INFINITE);
        }
    };

    int result = 1; // FAIL until proven otherwise
    bool removed = false;
    D3D12_GPU_VIRTUAL_ADDRESS blasAddr = 0;
    bool tlasBuilt = false;

    try {
        // --- Submission 1: upload a real Mesh (one triangle) via the engine's
        //     Mesh path. After this executes, its VB/IB decay to COMMON. ---
        Mesh mesh;
        std::vector<ComPtr<ID3D12Resource>> uploadKeepAlive;
        {
            VertexPNTUV verts[3] = {};
            verts[0].position = { 0.0f, 0.0f, 0.0f };
            verts[1].position = { 1.0f, 0.0f, 0.0f };
            verts[2].position = { 0.0f, 1.0f, 0.0f };
            const uint32_t indices[3] = { 0, 1, 2 };
            mesh.CreateGPUFlexible(device.Get(), cl.Get(), &uploadKeepAlive,
                                   verts, 3, sizeof(VertexPNTUV),
                                   indices, 3, DXGI_FORMAT_R32_UINT);
        }
        cl4->Close();
        submitAndWait(); // mesh VB/IB now resident; uploadKeepAlive can be released

        // --- Submission 2: build the mesh BLAS via the S3 manager, then a
        //     single-instance TLAS referencing it. ---
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);

        rt::AccelerationStructureManager asManager;
        asManager.Init(device5.Get());
        const rt::Blas& blas = asManager.GetOrBuildBlas(&mesh, cl4.Get());
        blasAddr = blas.Address();
        Log("blas address: 0x%llX\n", static_cast<unsigned long long>(blasAddr));

        ComPtr<ID3D12Resource> tlas, tlasScratch, instanceDescs;
        if (blasAddr != 0) {
            D3D12_RAYTRACING_INSTANCE_DESC inst{};
            inst.Transform[0][0] = 1.0f;
            inst.Transform[1][1] = 1.0f;
            inst.Transform[2][2] = 1.0f;
            inst.InstanceMask = 0xFF;
            inst.AccelerationStructure = blasAddr;

            instanceDescs = CreateBuffer(device.Get(), sizeof(inst), D3D12_HEAP_TYPE_UPLOAD,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
            if (instanceDescs) {
                void* mapped = nullptr;
                D3D12_RANGE noRead{ 0, 0 };
                if (SUCCEEDED(instanceDescs->Map(0, &noRead, &mapped))) {
                    memcpy(mapped, &inst, sizeof(inst));
                    instanceDescs->Unmap(0, nullptr);

                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
                    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                    inputs.NumDescs = 1;
                    inputs.InstanceDescs = instanceDescs->GetGPUVirtualAddress();

                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
                    device5->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
                    Log("tlas prebuild: result=%llu scratch=%llu\n",
                        info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes);

                    tlas = CreateBuffer(device.Get(), info.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
                                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    tlasScratch = CreateBuffer(device.Get(), info.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    if (tlas && tlasScratch) {
                        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
                        build.Inputs = inputs;
                        build.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
                        build.ScratchAccelerationStructureData = tlasScratch->GetGPUVirtualAddress();
                        cl4->BuildRaytracingAccelerationStructure(&build, 0, nullptr);

                        D3D12_RESOURCE_BARRIER uav{};
                        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                        uav.UAV.pResource = tlas.Get();
                        cl4->ResourceBarrier(1, &uav);
                        tlasBuilt = true;
                    }
                }
            }
        }

        cl4->Close();
        submitAndWait();

        // GPU finished consuming scratch — safe to release it now.
        asManager.ReleaseCompletedScratch();

        const HRESULT dr = device->GetDeviceRemovedReason();
        removed = FAILED(dr);
        Log("device removed reason: 0x%08lX\n", static_cast<unsigned long>(dr));
        if (blasAddr != 0 && tlasBuilt && !removed) {
            result = 0;
        }
    }
    catch (const std::exception& e) {
        Log("exception: %s\n", e.what());
        WriteVerdict(outPath, "FAIL exception");
        CloseHandle(evt);
        if (gLog) { fclose(gLog); gLog = nullptr; }
        return 1;
    }

    if (result == 0) {
        char verdict[64];
        std::snprintf(verdict, sizeof(verdict), "PASS tier=%d", static_cast<int>(tier));
        WriteVerdict(outPath, verdict);
    } else if (removed) {
        WriteVerdict(outPath, "FAIL device-removed");
    } else if (blasAddr == 0) {
        WriteVerdict(outPath, "FAIL blas-build");
    } else {
        WriteVerdict(outPath, "FAIL tlas-build");
    }

    CloseHandle(evt);
    if (gLog) { fclose(gLog); gLog = nullptr; }
    return result;
}
