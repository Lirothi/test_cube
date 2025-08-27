#include "InstanceBuffer.h"

#include "Helpers.h"
#include "Renderer.h"
#include "UploadManager.h"

void InstanceBuffer::Create(ID3D12Device* device, UINT numInstances,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive)
{
    instanceCount_ = numInstances;
    const size_t byteSize = sizeof(InstanceData) * size_t(instanceCount_);

    // Подготовим стартовые данные под заливку (identity + нули)
    std::vector<InstanceData> init(instanceCount_);
    for (UINT i = 0; i < instanceCount_; ++i) {
        DirectX::XMStoreFloat4x4(&init[i].world, DirectX::XMMatrixIdentity());
        init[i].rotationY = 0.0f;
        init[i]._pad[0] = init[i]._pad[1] = init[i]._pad[2] = 0.0f;
    }

    // Через UploadManager создаём Default-буфер с флагом UAV и сразу переводим в UAV-состояние
    UploadManager up(device, uploadCmdList);
    buffer_ = up.CreateBufferWithData(
        init.data(), byteSize,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Забираем upload-ресурсы, чтобы их не прибило до исполнения копии
    up.StealKeepAlive(uploadKeepAlive);

    // Создаём постоянные CPU-дескрипторы (SRV/UAV) — дальше в каждом кадре копируем их в shader-visible heap
    CreateCpuDescriptors_(device);
}

void InstanceBuffer::CreateCpuDescriptors_(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = 2;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // CPU-only
    ThrowIfFailed(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpuHeap_)));
    cpuHeap_->SetName(L"InstanceBuffer_DESCRIPTOR_HEAP");

    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srvCPU_ = cpuHeap_->GetCPUDescriptorHandleForHeapStart();
    uavCPU_ = srvCPU_; uavCPU_.ptr += inc;

    // SRV (структурированный буфер)
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_UNKNOWN;
    srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.FirstElement = 0;
    srv.Buffer.NumElements = instanceCount_;
    srv.Buffer.StructureByteStride = sizeof(InstanceData);
    srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(buffer_.Get(), &srv, srvCPU_);

    // UAV (структурированный буфер)
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.FirstElement = 0;
    uav.Buffer.NumElements = instanceCount_;
    uav.Buffer.StructureByteStride = sizeof(InstanceData);
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(buffer_.Get(), nullptr, &uav, uavCPU_);
}

D3D12_GPU_DESCRIPTOR_HANDLE InstanceBuffer::GetSRVForFrame(Renderer* renderer)
{
    auto& da = renderer->GetDescAlloc();           // транзиентный shader-visible heap (ресетится в EndFrame)
    auto h = da.Alloc();
    renderer->GetDevice()->CopyDescriptorsSimple(
        1, h.cpu, srvCPU_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return h.gpu;
}

D3D12_GPU_DESCRIPTOR_HANDLE InstanceBuffer::GetUAVForFrame(Renderer* renderer)
{
    auto& da = renderer->GetDescAlloc();
    auto h = da.Alloc();
    renderer->GetDevice()->CopyDescriptorsSimple(
        1, h.cpu, uavCPU_, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return h.gpu;
}
