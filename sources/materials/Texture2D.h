#pragma once
#include <wrl.h>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <string>

#include "rendering/core/ResourceDeclarations.h"
#include "rendering/streaming/DdsMipTable.h"

class Renderer;
namespace streaming { class TextureStreaming; }


class Texture2D {
public:
        // Step 6b part 2: registration dies with the texture. Textures were the largest remaining
        // leak once naming made them visible — nothing unregistered them on a level reload, so the
        // canonical table kept dangling keys and a recycled address could inherit a stale state.
        // This is the GpuResource guarantee applied in place; swapping tex_ for a GpuResource
        // outright is the tidier end state but has to thread through the upload path, which still
        // uses tex_ between creation and declaration.
        ~Texture2D(); // the wrapper unregisters the resource; the streaming registry is left here too
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
                // Texture streaming A1 (docs/texture_streaming_vt_plan.md). `streamable` lets a DDS
                // load stop short of the full chain: the resource is created with `residentMips`
                // levels (0 = all of them) holding the SMALLEST mips -- the tail UE always keeps
                // inline (NUM_INLINE_DERIVED_MIPS = 7, streaming::kNonStreamingMips). A file whose
                // mip table fails its self-check ignores the request and loads whole. Nothing sets
                // it before A3's manager exists, so with the defaults every load is byte-identical
                // to what it was. Not part of the shared-cache key: the resource is one per file for
                // every consumer, and what is resident is the manager's decision, not the caller's.
                bool streamable = false;
                UINT residentMips = 0;
        };

public:
        // Load the file inside Texture2D (WIC -> RGBA8 for common formats, DDS without transcoding).
        //
        // SHARED BY (resolved path, usage, normalIsRG, alphaCoverageCutoff) — the same identity the
        // decode cache uses, because those are exactly the inputs that change the PIXELS. Two
        // materials naming one file get ONE GPU texture and one SRV; this object becomes a view of
        // it. Measured on `demo` before the cache: four materials named damaged_plaster_normal.dds
        // and each held its own 684 KB copy, which is what the canonical registry's duplicate-name
        // counter had been reporting for months.
        //
        // The cache holds WEAK references, so a texture dies with its last owner and a level switch
        // frees VRAM exactly as it did before. A failed load is never cached.
        bool CreateFromFile(Renderer* renderer,
                ID3D12GraphicsCommandList* uploadCmd,
                const CreateDesc& desc,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);

        // Drop the shared-texture cache's own bookkeeping. Entries are weak, so this frees nothing
        // by itself — it exists so shutdown does not walk a map of dangling weak_ptrs.
        static void ClearCache();
        // Drop only the entries whose file was just rewritten on disk (a re-import). The predicate
        // is handed the RESOLVED path the entry is indexed under. Returns how many entries went.
        //
        // Weak entries, so this frees nothing and changes nothing already on screen: a material
        // built before the import still owns the texture it loaded. It makes the next LOAD read
        // disk — which is only half the fix, and the caller owes the other half (respawning what
        // was placed) or the level ends up disagreeing with itself.
        static std::size_t EvictIf(const std::function<bool(const std::wstring& resolvedPath)>& pred);
        // Since process start. `entries` counts live shared textures; `saved` is the number of
        // loads the cache turned into views, i.e. the GPU copies that were NOT made.
        static void CacheStats(std::uint32_t& saved, std::uint32_t& loaded, std::size_t& entries);

        // Legacy path: create from an RGBA8 buffer (kept for compatibility)
        // `debugLabel` identifies THIS texture (a font name, say). It matters beyond readability:
        // the canonical registry's leak check counts live entries per debug NAME, so several
        // textures sharing one name read as a leak that is not there — the same false positive
        // step 6b removed for file-backed textures by naming them after their path. A texture
        // built from memory has no path, so the caller has to supply the identity; without one
        // this falls back to a per-texture serial, which is unique but says nothing.
        void CreateFromRGBA8(Renderer* renderer,
                ID3D12GraphicsCommandList* uploadCmd,
                const void* rgba8, UINT width, UINT height,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive,
                const wchar_t* debugLabel = nullptr);

        // Obtain the SRV GPU handle in the current frame's shader-visible heap
        D3D12_GPU_DESCRIPTOR_HANDLE GetSRVForFrame(Renderer* renderer);

        // Every accessor reads through Source_(): when this object is a VIEW of a cached texture,
        // the resource, descriptor and metadata all live in the shared instance. Reading the local
        // members instead would silently hand back a null resource on a cache hit.

        // CPU SRV if you need to copy it into your own tables
        D3D12_CPU_DESCRIPTOR_HANDLE GetSRVCPU() const { return Source_().srvCPU_; }

	ID3D12Resource* GetResource() const { return Source_().tex_.Get(); }
	UINT GetWidth() const { return Source_().width_; }
	UINT GetHeight() const { return Source_().height_; }
	DXGI_FORMAT GetSrvFormat() const { return Source_().srvFormat_; }

        // Texture streaming A1 (see CreateDesc::streamable). The FILE's mip table -- offset, pitch
        // and size of every level whether resident or not -- and what of it is on the GPU now.
        // IsStreamable() is the load-time self-check's verdict: every mip's byte range computed
        // from the header matched both the file's size and D3D's own footprints, so a stream-in
        // can read and copy by this table alone. WIC loads, cubes and a DDS that failed are not.
        bool IsStreamable() const { return Source_().streamable_; }
        const streaming::DdsMipTable& GetMipTable() const { return Source_().mipTable_; }
        UINT GetResidentMips() const { return Source_().residentMips_; } // == the resource's mip count
        UINT GetFileMipCount() const { return Source_().mipTable_.mipCount; }
        const std::wstring& GetSourcePath() const { return Source_().sourcePath_; } // the file read
        // Slot in the streaming registry; -1 = not registered. Owners only (a view never registers).
        int StreamingIndex() const { return streamingIndex_; }
        int StreamingOwnerIndex() const { return Source_().streamingIndex_; } // the shared owner's slot, from a view too
        void AttachStreaming(streaming::TextureStreaming* s, int index) { streaming_ = s; streamingIndex_ = index; }
        void DetachStreaming() { streaming_ = nullptr; streamingIndex_ = -1; }
        // Bumped by every AdoptResource: consumers that COPY the CPU SRV (RT bindless sets) key on it.
        std::uint32_t GetSrvGeneration() const { return Source_().srvGeneration_; }
        // Bytes of the mips on the GPU (the resident tail of the file's table); 0 for a WIC texture.
        UINT64 GetResidentBytes() const;
        // Texture streaming A2: swap in a resource holding `residentMips` mips (copies already on the
        // GPU) and hand back the old resource for the retire bin. The SRV is rewritten IN PLACE in
        // the same CPU heap: every consumer that keeps the CPU handle and copies it per frame
        // (ShadowGpuData::maskedAlbedoSrvs_, material tables) sees the new resource on its next
        // copy; the one consumer that keeps a GPU COPY (rt::BindlessTable) keys on the generation.
        void AdoptResource(Renderer* r, GpuResource&& newRes, UINT residentMips, GpuResource& outOld);
        // Since process start: DDS loads whose mip table passed the self-check, DDS loads that
        // failed it (reported by name when they did), and WIC loads (no table at all).
        static void StreamingStats(std::uint32_t& streamable, std::uint32_t& nonStreamable,
                std::uint32_t& png);

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

        // The instance that actually OWNS the GPU objects: the shared one when this is a view of a
        // cached texture, otherwise this object. A shared instance never itself has `shared_` set,
        // so this recurses at most once.
        const Texture2D& Source_() const { return shared_ ? *shared_ : *this; }
        // Everything CreateFromFile used to do, minus the cache lookup. Runs on a cache miss only.
        bool LoadFromFileUncached_(Renderer* renderer,
                ID3D12GraphicsCommandList* uploadCmd,
                const CreateDesc& resolvedDesc,
                std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* keepAlive);
private:
        // Non-null when this object is a VIEW of a cached texture rather than the owner of one. It
        // is also the keep-alive: the cache itself only holds weak references, so the shared
        // texture lives exactly as long as the last material pointing at it.
        std::shared_ptr<Texture2D> shared_;
        GpuResource tex_;
        // Step 6b: a DISTINCT debug name per texture. Every texture used to be called
        // "Tex2D_RESOURCE", which made them indistinguishable in DRED/the debug layer and made
        // the canonical registry's duplicate-name leak check report one giant false positive.
        std::wstring debugName_ = L"Tex2D";


        // CPU-only heap for the SRV (single descriptor)
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srvHeapCPU_;
        D3D12_CPU_DESCRIPTOR_HANDLE srvCPU_{};

        // Cache of the staged GPU handle per frame
        std::uint64_t stagedFrame_ = UINT64_MAX; // MONOTONIC frame number, not the in-flight slot
        D3D12_GPU_DESCRIPTOR_HANDLE srvGPU_{};

        // Metadata
        UINT width_ = 0, height_ = 0; // the FILE's mip 0 -- what the texture IS; the resource may hold less
        UINT mipLevels_ = 1;          // the RESOURCE's mip count (== residentMips_ for a DDS)
        DXGI_FORMAT resourceFormat_ = DXGI_FORMAT_UNKNOWN; // Typically R8G8B8A8_TYPELESS / BC*_TYPELESS
        DXGI_FORMAT srvFormat_ = DXGI_FORMAT_UNKNOWN; // UNORM or SRGB

        // Texture streaming A1 (filled by CreateFromDDS_; see the accessors above).
        streaming::DdsMipTable mipTable_;
        std::wstring sourcePath_;   // the resolved path the loader read (debugName_ carries a prefix)
        UINT residentMips_ = 0;     // mips on the GPU: the LAST residentMips_ entries of mipTable_
        int streamingIndex_ = -1;
        bool streamable_ = false;
        std::uint32_t srvGeneration_ = 0;
        streaming::TextureStreaming* streaming_ = nullptr; // set by the registry; nulled on unregister
};