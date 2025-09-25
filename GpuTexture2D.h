#pragma once

#include <wrl.h>
#include <d3d12.h>
#include <vector>

class Renderer;

class GpuTexture2D {
public:
    GpuTexture2D() = default;

    bool Create(Renderer* renderer,
        UINT width,
        UINT height,
        DXGI_FORMAT format,
        UINT mipLevels,
        D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* clearValue = nullptr);

    bool CreateFromData(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmdList,
        DXGI_FORMAT format,
        UINT width,
        UINT height,
        UINT mipLevels,
        D3D12_RESOURCE_FLAGS flags,
        const void* data,
        size_t rowPitchBytes,
        D3D12_RESOURCE_STATES finalState,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive = nullptr);

    void CreateViews(Renderer* renderer,
        bool createSrv,
        bool createUav,
        DXGI_FORMAT srvFormat,
        DXGI_FORMAT uavFormat,
        UINT mipLevels = 1);

    ID3D12Resource* GetResource() const { return resource_.Get(); }
    DXGI_FORMAT GetFormat() const { return format_; }
    UINT GetWidth() const { return width_; }
    UINT GetHeight() const { return height_; }
    UINT GetMipLevels() const { return mipLevels_; }

    D3D12_CPU_DESCRIPTOR_HANDLE GetSrvCPU() const { return srvCPU_; }
    D3D12_CPU_DESCRIPTOR_HANDLE GetUavCPU() const { return uavCPU_; }
    bool HasSrv() const { return srvCPU_.ptr != 0; }
    bool HasUav() const { return uavCPU_.ptr != 0; }

private:
    void AllocateDescriptorHeap_(ID3D12Device* device, UINT count);

    Microsoft::WRL::ComPtr<ID3D12Resource> resource_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpuHeap_;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCPU_{};
    D3D12_CPU_DESCRIPTOR_HANDLE uavCPU_{};

    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    UINT width_ = 0;
    UINT height_ = 0;
    UINT mipLevels_ = 1;
    D3D12_RESOURCE_FLAGS flags_ = D3D12_RESOURCE_FLAG_NONE;
};
