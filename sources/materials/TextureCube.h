#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <string>
#include <vector>

class Renderer;

class TextureCube {
public:
    TextureCube() = default;

    // Load a ready-made DDS cubemap (or cube array) from disk.
    // Requires a DDS with a DXT10 header (DXGI_FORMAT = float/HDR/BC6H, etc.). Mipmaps are supported.
    bool CreateFromDDS(Renderer* renderer,
                       ID3D12GraphicsCommandList* uploadCmd,
                       const std::wstring& path,
                       std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

    // Retrieve the SRV for the current frame (copies the CPU SRV into the frame's shader-visible heap).
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVForFrame(Renderer* r);

    // Provide the CPU SRV (persistent) if needed.
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return srvCPU_; }

    // Metadata / resource
    ID3D12Resource* GetResource() const { return tex_.Get(); }
    UINT            GetWidth()   const { return width_; }
    UINT            GetHeight()  const { return height_; }
    UINT            GetMips()    const { return mipLevels_; }
    UINT            GetArraySize() const { return arraySize_; }
    DXGI_FORMAT     GetFormat()  const { return format_; }
    bool            IsArray()    const { return arraySize_ > 6; }

private:
    // helpers
    bool  LoadFileToMemory_(const std::wstring& path, std::vector<uint8_t>& data);
    bool  ParseDDS_(const uint8_t* bytes, size_t size,
                    UINT& outW, UINT& outH, UINT& outMips, UINT& outArray,
                    DXGI_FORMAT& outFmt, size_t& outDataOffset, bool& outIsCube);
    void  CreateSrvCPU_(Renderer* r, DXGI_FORMAT srvFmt, UINT mipLevels, UINT arraySize);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> tex_;

    // CPU-only heap with one descriptor (SRV)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeapCPU_;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCPU_{};

    // Cache of the staged GPU handle per frame (same as Texture2D)
    UINT stagedFrame_ = UINT(-1);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGPU_{};

    // Metadata
    UINT       width_ = 0, height_ = 0;
    UINT       mipLevels_ = 1;
    UINT       arraySize_ = 6;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};
