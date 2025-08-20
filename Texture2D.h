#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <cstdint>
#include <vector>

class Renderer;

class Texture2D {
public:
    // Создание из RGBA8 буфера (W*H*4). uploadCmd — любой командный лист для копирования (например, твой uploadCmdList в InitScene)
    // keepAlive — в этот вектор положим upload-ресурс до завершения копирования (чтобы не освободился раньше времени).
    void CreateFromRGBA8(Renderer* renderer,
                         ID3D12GraphicsCommandList* uploadCmd,
                         const void* rgba8, UINT width, UINT height,
                         std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

    // Получить GPU handle SRV в shader-visible куче текущего кадра (выполняет CopyDescriptorsSimple один раз на кадр)
    D3D12_GPU_DESCRIPTOR_HANDLE GetSRVForFrame(Renderer* renderer);
	D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return srvCPU_; }

    ID3D12Resource* GetResource() const { return tex_.Get(); }

private:
    void CreateCpuSrv_(Renderer* renderer, DXGI_FORMAT fmt);
    void UploadRGBA8_(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmd,
                      const void* rgba8, UINT width, UINT height,
                      std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive,
                      DXGI_FORMAT fmt);

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> tex_;
    // CPU-only heap для SRV (1 дескриптор)
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeapCPU_;
    D3D12_CPU_DESCRIPTOR_HANDLE srvCPU_{};

    // Кэш «состейдженного» GPU handle на кадр
    UINT stagedFrame_ = UINT(-1);
    D3D12_GPU_DESCRIPTOR_HANDLE srvGPU_{};
};