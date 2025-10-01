#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <string>
#include <vector>

class Renderer;

class TextureCube {
public:
    TextureCube() = default;

    // Загрузка готовой DDS-кубкарты (или cube array) с диска.
    // Требуется DDS с DXT10-хедером (DXGI_FORMAT = float/HDR/BC6H и т.п.). Мипы поддерживаются.
    bool CreateFromDDS(Renderer* renderer,
                       ID3D12GraphicsCommandList* uploadCmd,
                       const std::wstring& path,
                       std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

    // Получить SRV для текущего кадра (скопирует CPU SRV в shader-visible heap кадра).
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVForFrame(Renderer* r);

    // При необходимости — CPU SRV (постоянный).
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return srvCPU_; }

    // Метаданные / ресурс
    ID3D12Resource* GetResource() const { return tex_.Get(); }
    UINT            GetWidth()   const { return width_; }
    UINT            GetHeight()  const { return height_; }
    UINT            GetMips()    const { return mipLevels_; }
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

    // CPU-only heap на 1 дескриптор (SRV)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeapCPU_;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCPU_{};

    // Кэш «состейдженного» GPU handle на кадр (как в Texture2D)
    UINT stagedFrame_ = UINT(-1);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGPU_{};

    // Метаданные
    UINT       width_ = 0, height_ = 0;
    UINT       mipLevels_ = 1;
    UINT       arraySize_ = 6;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};