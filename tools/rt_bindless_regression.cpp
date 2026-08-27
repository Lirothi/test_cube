// Standalone D3D12 regression. Build tools/rt_bindless_regression.vcxproj (Release|x64).
// Uses the production BindlessTable, with real GPU reads delayed until three CPU frames have
// updated it. Null Mesh skips only VB/IB creation; material records, SRVs and uploads are real.
#include "rendering/rt/BindlessTable.h"
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

static void Require(bool condition, const char* message)
{
    if (condition) { return; }
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::fflush(stderr);
    // Do not unwind GPU resources if a failed assertion left the diagnostic queue blocked.
    TerminateProcess(GetCurrentProcess(), 1);
}

static void Check(HRESULT hr, const char* message)
{
    if (FAILED(hr)) { std::fprintf(stderr, "HRESULT=0x%08X\n", static_cast<unsigned>(hr)); }
    Require(SUCCEEDED(hr), message);
}

static ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE type,
                                    D3D12_RESOURCE_STATES state, UINT64 bytes,
                                    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> buffer;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&buffer)), "create buffer");
    return buffer;
}

int main()
{
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) { debug->EnableDebugLayer(); }
    ComPtr<ID3D12Device> device;
    Check(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "device");
    ComPtr<ID3D12InfoQueue> info;
    device.As(&info);

    rt::BindlessTable table;
    table.Init(device.Get());
    Require(table.Ready(), "bindless heap");
    Require(!table.UploadGeometryInfo(0), "empty table must not be ready");
    constexpr UINT frames = render::kFrameCount;
    static_assert(frames >= 3);
    static_assert(sizeof(rt::GeometryInfoGPU) == 64);
    std::array<int, 130> owners{};
    const auto update = [&](size_t owner, float roughness, float metalness = 0.0f) {
        return table.GetOrUpdateMesh(&owners[owner], nullptr, {}, {}, nullptr, roughness, metalness, false);
    };
    const auto editedId = update(0, 0.5f);
    const auto untouchedId = update(1, 0.5f);
    Require(editedId != untouchedId, "independently editable owners must have distinct records");
    Require(table.DescriptorSetCount() == 1, "identical descriptors should be shared");

    // Ordinary SRV table binding reads the exact SRV slots used by SM6.6 direct heap indexing.
    // uint4 rows intentionally match the 64-byte GeometryInfoGPU layout byte-for-byte.
    const char* shader = R"(
        struct Geometry { uint4 row0; uint4 row1; uint4 row2; uint4 row3; };
        StructuredBuffer<Geometry> geometry : register(t0);
        RWByteAddressBuffer result : register(u0);
        cbuffer Params : register(b0) { uint editedId; uint untouchedId; };
        [numthreads(1, 1, 1)] void main() {
            result.Store4(0, uint4(geometry[editedId].row1.x, geometry[untouchedId].row1.x,
                                  geometry[editedId].row1.y, geometry[untouchedId].row1.y));
        }
    )";
    ComPtr<ID3DBlob> cs, errors;
    const HRESULT compiled = D3DCompile(shader, std::strlen(shader), "bindless-regression", nullptr,
        nullptr, "main", "cs_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &errors);
    if (errors) { std::fprintf(stderr, "%s\n", static_cast<const char*>(errors->GetBufferPointer())); }
    Check(compiled, "shader compile");

    D3D12_DESCRIPTOR_RANGE range{ D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER params[3]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 1, &range };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[1].Descriptor = { 0, 0 };
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants = { 0, 0, 2 };
    D3D12_ROOT_SIGNATURE_DESC rootDesc{ 3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> rootBlob;
    Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors), "root signature blob");
    ComPtr<ID3D12RootSignature> root;
    Check(device->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                      IID_PPV_ARGS(&root)), "root signature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = root.Get();
    psoDesc.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ComPtr<ID3D12PipelineState> pso;
    Check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)), "compute pipeline");

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    Check(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "queue");
    std::array<ComPtr<ID3D12CommandAllocator>, frames> alloc;
    std::array<ComPtr<ID3D12GraphicsCommandList>, frames> cl;
    for (UINT f = 0; f < frames; ++f) {
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[f])), "allocator");
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[f].Get(), pso.Get(),
                                        IID_PPV_ARGS(&cl[f])), "command list");
        Check(cl[f]->Close(), "initial close");
    }
    ComPtr<ID3D12Fence> gate, complete;
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate fence");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&complete)), "completion fence");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "fence event");
    auto output = Buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                         16, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto readback = Buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, frames * 16);
    const UINT incr = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    constexpr UINT batches = 1000;
    for (UINT batch = 0; batch < batches; ++batch) {
        // GPU is held until ALL frame slots have uploaded distinct data. This makes accidental
        // shared buffer/SRV mutation deterministic, rather than relying on GPU/CPU timing.
        Check(queue->Wait(gate.Get(), batch + 1), "block diagnostic queue");
        std::array<std::array<float, 4>, frames> expected{};
        for (UINT f = 0; f < frames; ++f) {
            if (batch == 0 && f == 1) {
                // Grow after frame 0 was submitted: its original buffer must stay alive.
                for (size_t i = 2; i < owners.size(); ++i) { update(i, 0.5f); }
            }
            const float roughness = float((batch * frames + f) % 4096) / 4096.0f;
            const float metalness = 1.0f - roughness;
            Require(update(0, roughness, metalness) == editedId, "scalar edit changed InstanceID");
            Require(table.UploadGeometryInfo(f) && table.FrameReady(f), "frame upload");
            expected[f] = { roughness, 0.5f, metalness, 0.0f };
            Require(table.GeomInfoIndex(f) < rt::BindlessTable::kSceneBase, "geometry SRV overlaps scene slots");
            Check(alloc[f]->Reset(), "allocator reset after fence");
            Check(cl[f]->Reset(alloc[f].Get(), pso.Get()), "command list reset");
            auto* cmd = cl[f].Get();
            ID3D12DescriptorHeap* heaps[] = { table.Heap() };
            cmd->SetDescriptorHeaps(1, heaps);
            cmd->SetComputeRootSignature(root.Get());
            auto srv = table.Heap()->GetGPUDescriptorHandleForHeapStart();
            srv.ptr += UINT64(table.GeomInfoIndex(f)) * incr;
            cmd->SetComputeRootDescriptorTable(0, srv);
            cmd->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
            const UINT ids[] = { editedId, untouchedId };
            cmd->SetComputeRoot32BitConstants(2, 2, ids, 0);
            cmd->Dispatch(1, 1, 1);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
            cmd->ResourceBarrier(1, &barrier);
            cmd->CopyBufferRegion(readback.Get(), f * 16, output.Get(), 0, 16);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            cmd->ResourceBarrier(1, &barrier);
            Check(cmd->Close(), "close dispatch");
            ID3D12CommandList* lists[] = { cmd };
            queue->ExecuteCommandLists(1, lists);
        }
        Check(queue->Signal(complete.Get(), batch + 1), "signal completion");
        Check(gate->Signal(batch + 1), "release diagnostic queue");
        Check(complete->SetEventOnCompletion(batch + 1, event), "fence notification");
        Require(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "GPU completion timeout");
        Check(device->GetDeviceRemovedReason(), "device removed");
        void* data = nullptr;
        const D3D12_RANGE read{ 0, frames * 16 };
        Check(readback->Map(0, &read, &data), "readback map");
        Require(std::memcmp(data, expected.data(), frames * 16) == 0,
                "GPU read wrong frame snapshot or another owner's material changed");
        const D3D12_RANGE noWrite{ 0, 0 };
        readback->Unmap(0, &noWrite);
        Require(table.GeometryCount() == owners.size(), "material edits grew geometry table");
        Require(table.DescriptorSetCount() == 1, "material edits leaked descriptors");
    }
    std::printf("PASS: %u queued GPU snapshots, stable InstanceIDs, owner isolation, growth while in flight\n", batches * frames);
    std::printf("records=%u descriptorSets=%u deviceRemovedReason=0x%08X\n", table.GeometryCount(),
                table.DescriptorSetCount(), static_cast<unsigned>(device->GetDeviceRemovedReason()));

    // New texture descriptor sets must be bounded even after thousands of material changes.
    // Null buffer SRVs are legal; many distinct CPU handles exercise the real heap-capacity guard.
    table.Reset();
    table.Init(device.Get());
    constexpr UINT sets = (rt::BindlessTable::kMaxDescriptors - rt::BindlessTable::kGeoBase) / rt::BindlessTable::kDescPerGeom;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = sets + 1;
    ComPtr<ID3D12DescriptorHeap> source;
    Check(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&source)), "source descriptor heap");
    auto cpu = source->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i <= sets; ++i, cpu.ptr += incr) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.Buffer.NumElements = 1;
        device->CreateShaderResourceView(nullptr, &srv, cpu);
        const auto id = table.GetOrUpdateMesh(&owners[0], nullptr, cpu, {}, nullptr, 0.5f, 0.0f, false);
        Require((id == rt::BindlessTable::kInvalidGeometry) == (i == sets), "capacity guard");
    }
    Require(table.BuildFailed() && !table.UploadGeometryInfo(0) && !table.FrameReady(0), "failed table must not be dispatched");
    Require(table.GeometryCount() == 1 && table.DescriptorSetCount() == sets, "bounded table on exhaustion");
    table.Reset();
    Require(!table.BuildFailed() && table.GeometryCount() == 0 && table.DescriptorSetCount() == 0, "reset state");
    std::puts("PASS: descriptor capacity guard and reset");
    if (info) {
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
            SIZE_T bytes = 0;
            info->GetMessage(i, nullptr, &bytes);
            std::vector<uint8_t> storage(bytes);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(i, message, &bytes), "debug message");
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                std::fprintf(stderr, "%s\n", message->pDescription);
                Require(false, "D3D12 validation error");
            }
        }
        std::puts("PASS: D3D12 debug layer (no errors)");
    }
    CloseHandle(event);
    return 0;
}
