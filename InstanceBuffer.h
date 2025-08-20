// InstanceBuffer.h
#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>
#include <vector>

class Renderer;

#pragma pack(push, 16)
struct InstanceData {
    DirectX::XMFLOAT4X4 world;
    float rotationY;
    float _pad[3];
};
#pragma pack(pop)

class InstanceBuffer {
public:
    InstanceBuffer() = default;

    void Create(ID3D12Device* device, UINT numInstances, ID3D12GraphicsCommandList* uploadCmdList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive);

    ID3D12Resource* GetResource() const { return buffer_.Get(); }
    UINT GetInstanceCount() const { return instanceCount_; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVForFrame(Renderer* renderer);
    D3D12_GPU_DESCRIPTOR_HANDLE GetUAVForFrame(Renderer* renderer);
	D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return srvCPU_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetUAVCPU() const { return uavCPU_; }

private:
    void CreateCpuDescriptors_(ID3D12Device* device);

    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    UINT instanceCount_ = 0;

    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpuHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCPU_{};
    D3D12_CPU_DESCRIPTOR_HANDLE uavCPU_{};
};