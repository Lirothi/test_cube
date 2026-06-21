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

    // Bindless capability probe (S9): resource binding tier + highest shader model.
    // SM6.6 dynamic resources (ResourceDescriptorHeap[]) need HighestShaderModel
    // >= 0x66; the cleanest bindless path. Tier 3 also allows unbounded tables.
    {
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)))) {
            Log("ResourceBindingTier: %d (1,2,3)\n", static_cast<int>(options.ResourceBindingTier));
        }
        D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_7 };
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm)))) {
            Log("HighestShaderModel: 0x%02X (SM6.6 dynamic resources need >= 0x66)\n",
                static_cast<unsigned>(sm.HighestShaderModel));
        }
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

        // --- Submission 2: build a 2-instance TLAS via the S4 builder. It
        //     builds the mesh BLAS (S3) on demand on the same list, then the
        //     TLAS over two transformed instances of it. ---
        alloc->Reset();
        cl->Reset(alloc.Get(), nullptr);

        rt::AccelerationStructureManager asManager;
        asManager.Init(device5.Get());

        rt::InstanceEntry entries[2] = {};
        DirectX::XMStoreFloat4x4(&entries[0].world, DirectX::XMMatrixIdentity());
        DirectX::XMStoreFloat4x4(&entries[1].world, DirectX::XMMatrixTranslation(2.0f, 0.0f, 0.0f));
        entries[0].mesh = &mesh; entries[0].instanceId = 0;
        entries[1].mesh = &mesh; entries[1].instanceId = 1;

        asManager.BuildTlas(std::span<const rt::InstanceEntry>(entries, 2), cl4.Get(), /*frameIndex*/ 0);

        cl4->Close();
        submitAndWait();

        // GPU finished consuming scratch — safe to release it now.
        asManager.ReleaseCompletedScratch();

        // The BLAS was built on demand inside BuildTlas; fetch the cached address.
        blasAddr = asManager.GetOrBuildBlas(&mesh, nullptr).Address();
        const UINT tlasInstances = asManager.TlasInstanceCount(0);
        const D3D12_CPU_DESCRIPTOR_HANDLE tlasSrv = asManager.TlasSrvCpu(0);
        tlasBuilt = (tlasInstances == 2) && (tlasSrv.ptr != 0);
        Log("blas address: 0x%llX, tlas instances: %u, tlas srv: 0x%llX\n",
            static_cast<unsigned long long>(blasAddr), tlasInstances,
            static_cast<unsigned long long>(tlasSrv.ptr));

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
