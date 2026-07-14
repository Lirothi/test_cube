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
                AlbedoSRGB, // SRV will be *_SRGB
                NormalMap, // linear sampling; supports RGB or RG via a flag
                MetalRough, // linear sampling; R=metal, G=rough
                LinearData // any other linear channel
        };

        struct CreateDesc {
                std::wstring path; // Path to the file (PNG/JPG/TIFF/BMP via WIC; DDS directly)
                Usage usage = Usage::LinearData;
                bool normalIsRG = false; // When Usage::NormalMap and the texture stores only RG (BC5/RG8 or RG in an RGBA container)
                // WIC loads always get a CPU-built box-filter mip chain (DDS keeps the file's own
                // mips). >= 0: preserve the alpha-test coverage at this cutoff across the chain
                // (Castano) — without it, averaged alpha sinks below the cutoff and masked
                // foliage erodes/vanishes with distance.
                float alphaCoverageCutoff = -1.0f;
        };

public:
        // Load the file inside Texture2D (WIC -> RGBA8 for common formats, DDS without transcoding)
        bool CreateFromFile(Renderer* renderer,
                ID3D12GraphicsCommandList* uploadCmd,
                const CreateDesc& desc,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

        // Legacy path: create from an RGBA8 buffer (kept for compatibility)
        void CreateFromRGBA8(Renderer* renderer,
                ID3D12GraphicsCommandList* uploadCmd,
                const void* rgba8, UINT width, UINT height,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

        // Obtain the SRV GPU handle in the current frame's shader-visible heap
        D3D12_GPU_DESCRIPTOR_HANDLE GetSRVForFrame(Renderer* renderer);

        // CPU SRV if you need to copy it into your own tables
        D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return srvCPU_; }

	ID3D12Resource* GetResource() const { return tex_.Get(); }
	UINT GetWidth() const { return width_; }
	UINT GetHeight() const { return height_; }
	DXGI_FORMAT GetSrvFormat() const { return srvFormat_; }
private:
        // Loaders and upload helpers
        bool LoadRGBA8_WIC_(const std::wstring& path, std::vector<uint8_t>& outRGBA, UINT& outW, UINT& outH);
        void UploadRGBA8_(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmd,
                const void* rgba8, UINT width, UINT height,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive,
                DXGI_FORMAT resourceFmt);

        // C follow-up (DLSS shimmer): CPU box-filter mip chain for WIC loads. mips[0] must hold
        // the base level; appends levels down to 1x1. srgbColor averages RGB in linear space
        // (albedo); alpha always averages linearly. alphaCoverageCutoff >= 0 rescales each
        // level's alpha to keep its alpha-test coverage equal to mip 0's.
        static void BuildMipChainRGBA8_(std::vector<std::vector<uint8_t>>& mips, UINT width, UINT height,
                bool srgbColor, float alphaCoverageCutoff);
        void UploadRGBA8Mips_(Renderer* renderer, ID3D12GraphicsCommandList* uploadCmd,
                const std::vector<std::vector<uint8_t>>& mips, UINT width, UINT height,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive,
                DXGI_FORMAT resourceFmt);

        // Direct DDS loading (BC1/BC2/BC3/BC4/BC5/BC7 + RGBA8) with mipmaps, without transcoding
        bool CreateFromDDS_(Renderer* r, ID3D12GraphicsCommandList* uploadCmd,
                const CreateDesc& desc,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

        // Create the CPU SRV
        void CreateCpuSrv_(Renderer* renderer, DXGI_FORMAT srvFmt, UINT mipLevels);
private:
        Microsoft::WRL::ComPtr<ID3D12Resource> tex_;

        // CPU-only heap for the SRV (single descriptor)
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeapCPU_;
        D3D12_CPU_DESCRIPTOR_HANDLE srvCPU_{};

        // Cache of the staged GPU handle per frame
        UINT stagedFrame_ = UINT(-1);
        D3D12_GPU_DESCRIPTOR_HANDLE srvGPU_{};

        // Metadata
        UINT width_ = 0, height_ = 0;
        UINT mipLevels_ = 1;
        DXGI_FORMAT resourceFormat_ = DXGI_FORMAT_UNKNOWN; // Typically R8G8B8A8_TYPELESS / BC*_TYPELESS
        DXGI_FORMAT srvFormat_ = DXGI_FORMAT_UNKNOWN; // UNORM or SRGB
};