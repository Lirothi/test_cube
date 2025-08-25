#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <cstdint>
#include <vector>
#include <string>

class Renderer;

class Texture2D {
public:
    enum class Usage : uint32_t {
        AlbedoSRGB,  // SRV будет *_SRGB
        NormalMap,   // линейная выборка; поддержка RGB или RG (флагом)
        MetalRough,  // линейная выборка; R=metal, G=rough
        LinearData   // любой другой линейный канал
    };

    struct CreateDesc {
        std::wstring path;           // путь к файлу (PNG/JPG/TIFF/BMP и т.п. через WIC)
        Usage        usage = Usage::LinearData;
        bool         normalIsRG = false;   // если Usage::NormalMap и текстура содержит только RG (BC5/RG8, либо RG в RGBA-контейнере)
        bool         generateMips = false; // зарезервировано (сейчас 1 мип)
    };

public:
    // Новый: загрузка файла внутри Texture2D (WIC -> RGBA8 память -> Upload)
    bool CreateFromFile(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmd,
        const CreateDesc& desc,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

    // Старый: создание из RGBA8 буфера (оставлено для совместимости)
    void CreateFromRGBA8(Renderer* renderer,
        ID3D12GraphicsCommandList* uploadCmd,
        const void* rgba8, UINT width, UINT height,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

    // Получить GPU handle SRV в shader-visible куче текущего кадра
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVForFrame(Renderer* renderer);

    // CPU SRV, если нужно напрямую копировать в свои таблицы
    D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return srvCPU_; }

    ID3D12Resource* GetResource() const { return tex_.Get(); }
    UINT GetWidth() const { return width_; }
    UINT GetHeight() const { return height_; }
    DXGI_FORMAT GetSrvFormat() const { return srvFormat_; }

private:
    // загрузка и аплоад
    bool LoadRGBA8_WIC_(const std::wstring& path, std::vector<uint8_t>& outRGBA, UINT& outW, UINT& outH);
    void UploadRGBA8_(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmd,
        const void* rgba8, UINT width, UINT height,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive,
        DXGI_FORMAT resourceFmt);

    void CreateCpuSrv_(Renderer* renderer, DXGI_FORMAT srvFmt);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> tex_;

    // CPU-only heap для SRV (1 дескриптор)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeapCPU_;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCPU_{};

    // Кэш staged GPU handle на кадр
    UINT stagedFrame_ = UINT(-1);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGPU_{};

    // Метаданные
    UINT       width_ = 0, height_ = 0;
    DXGI_FORMAT resourceFormat_ = DXGI_FORMAT_UNKNOWN; // обычно R8G8B8A8_TYPELESS
    DXGI_FORMAT srvFormat_ = DXGI_FORMAT_UNKNOWN; // UNORM или SRGB
};