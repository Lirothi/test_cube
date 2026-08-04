#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <cstdint>
#include <vector>
#include <string>

#include "rendering/core/ResourceDeclarations.h"

class Renderer;


class Texture2D {
public:
        // Step 6b part 2: registration dies with the texture. Textures were the largest remaining
        // leak once naming made them visible — nothing unregistered them on a level reload, so the
        // canonical table kept dangling keys and a recycled address could inherit a stale state.
        // This is the GpuResource guarantee applied in place; swapping tex_ for a GpuResource
        // outright is the tidier end state but has to thread through the upload path, which still
        // uses tex_ between creation and declaration.
        ~Texture2D() = default; // the wrapper unregisters
        Texture2D() = default;
        Texture2D(const Texture2D&) = delete;
        Texture2D& operator=(const Texture2D&) = delete;
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

        // NEVER decode on this thread: inside this scope, CreateFromFile asks the decode cache
        // instead and, when the image is not ready yet, FAILS the load and records it. The caller
        // discards whatever it was building and retries on a later frame, by which time a worker
        // has finished. That is what makes "the main thread does not stall" independent of knowing
        // in advance which textures a material will touch — no prediction, just ask and bounce.
        //
        // Scoped and thread-local, so only the code inside opts in; the game path is untouched.
        class DeferDecodeScope
        {
        public:
                DeferDecodeScope();
                ~DeferDecodeScope();
                // True when at least one texture in this scope was not ready.
                bool AnyPending() const;
                DeferDecodeScope(const DeferDecodeScope&) = delete;
                DeferDecodeScope& operator=(const DeferDecodeScope&) = delete;
        private:
                bool prevDefer_ = false;
                bool prevPending_ = false;
        };

        // The CPU half of a WIC load: resolve the DDS sibling, decode to RGBA8, apply the
        // normalIsRG fixup, build the box-filter mip chain. NO DEVICE, NO COMMAND LIST — safe on
        // a worker, which is the whole point (see materials/TextureDecodeCache.h). Returns false
        // when the path resolves to a DDS (nothing to decode) or the decode fails.
        static bool DecodeToMips(const CreateDesc& desc,
                std::vector<std::vector<uint8_t>>& outMips, UINT& outW, UINT& outH);
        // The path DecodeToMips/CreateFromFile actually read, after DDS-sibling resolution.
        static std::wstring ResolveSourcePath(const std::wstring& requested);

private:
        // Loaders and upload helpers
        static bool LoadRGBA8_WIC_(const std::wstring& path, std::vector<uint8_t>& outRGBA, UINT& outW, UINT& outH);
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
        GpuResource tex_;
        // Step 6b: a DISTINCT debug name per texture. Every texture used to be called
        // "Tex2D_RESOURCE", which made them indistinguishable in DRED/the debug layer and made
        // the canonical registry's duplicate-name leak check report one giant false positive.
        std::wstring debugName_ = L"Tex2D";


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