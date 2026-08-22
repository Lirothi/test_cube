#pragma once
#include <d3d12.h>
#include <memory>
#include <wrl/client.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <optional>
#include <d3d12shader.h>
#include "core/profiling/Profiler.h"
#include "rendering/core/RenderContext.h"
#include "robin_hood.h"
#include <cassert>
#include <algorithm>

using namespace Microsoft::WRL;

class Renderer;

class Material {
public:
    // -------- Common section: list of defines (permutations) --------
    using DefineList = std::vector<std::pair<std::string, std::string>>; // NAME=VALUE

    // -------- Graphics --------
    struct GraphicsDesc {
        GraphicsDesc() { FillDefaultsTriangle(); }

        // What to compile
        std::wstring shaderFile;
        const char* vsEntry = "VSMain";
        const char* psEntry = "PSMain";
        DefineList   defines;              // << new: Shader Defines

        // IA
        std::string  inputLayoutKey = "PosColor";
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        // States
        D3D12_RASTERIZER_DESC    raster{};
        D3D12_BLEND_DESC         blend{};
        D3D12_DEPTH_STENCIL_DESC depth{};

        // RT/DS
        UINT        numRT = 1;
        DXGI_FORMAT rtvFormats[8] = {
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN
        };
        DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        UINT        sampleCount = 1;

        // RS flags
        D3D12_ROOT_SIGNATURE_FLAGS rsFlags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
            D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

        void FillDefaultsTriangle() {
            ZeroMemory(&raster, sizeof(raster));
            raster.FillMode = D3D12_FILL_MODE_SOLID;
            raster.CullMode = D3D12_CULL_MODE_BACK;
            raster.FrontCounterClockwise = FALSE;
            raster.DepthClipEnable = TRUE;

            ZeroMemory(&blend, sizeof(blend));
            for (int i = 0; i < 8; ++i) {
                blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }

            ZeroMemory(&depth, sizeof(depth));
            depth.DepthEnable = TRUE;
            depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            depth.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            depth.StencilEnable = FALSE;
        }
    };

    // -------- Compute --------
    struct ComputeDesc {
        std::wstring shaderFile;
        const char* csEntry = "main";
        DefineList   defines;                  // << new: Shader Defines
        D3D12_ROOT_SIGNATURE_FLAGS rsFlags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    };

    struct RootParameterInfo {
        // Tables are split by heap/resource kind so a slot can be keyed by the
        // table's base register (TableSRV at t0 and TableUAV at u0 don't collide).
        // Root SRV/UAV descriptors are not used by any shader and are unsupported.
        enum Type { Constants, CBV, TableSRV, TableUAV, TableSampler } type;
        UINT rootIndex = 0;       // Root parameter index
        UINT bindingRegister = 0; // Shader register (b#/t#/u#/s#) used for lookup in RenderContext
        UINT bindingSpace = 0;    // Register space for additional offsets
        UINT constantsCount = 0;  // Only for constants
    };

    Material() = default;

    void CreateGraphics(Renderer* renderer, const GraphicsDesc& gd);
    void CreateCompute(Renderer* renderer, const ComputeDesc& cd);

    // Legacy-compatible wrapper (kept to avoid breaking old code)
    void CreateCompute(Renderer* renderer, const std::wstring& shaderFile) {
        ComputeDesc cd{};
        cd.shaderFile = shaderFile;
        CreateCompute(renderer, cd);
    }

    bool IsCompute() const { return isCompute_; }

    ID3D12RootSignature* GetRootSignature() const { return rootSignature_.Get(); }
    ID3D12PipelineState* GetPipelineState() const { return pipelineState_.Get(); }

    void Bind(ID3D12GraphicsCommandList* cmdList, const RenderContext& ctx, bool wireframe = false) const;

    // Hot reload
    bool FSProbeAndFlagPending();
    bool HotReloadIfPending(Renderer* r, uint64_t frameIndex, uint64_t keepAliveFrames);
    void CollectRetired(uint64_t frameIndex, uint64_t keepAliveFrames);

    struct CBufferField {
        UINT        offset = 0;         // Byte offset of the field inside the cbuffer
        UINT        size = 0;           // Total field size (entire array size if it is an array)
        UINT        elementStride = 0;  // Byte stride of one array element (or size if not an array)
        UINT        elementCount = 1;   // Array capacity (1 if not an array)
    };
    struct CBufferInfo {
        UINT bindRegister = 0;    // bN
        UINT sizeBytes = 0;
        robin_hood::unordered_flat_map<std::string, CBufferField> fieldsByName; // name -> {offset,size}
    };

    struct CBFieldHandle {
        const CBufferField* field = nullptr;
        UINT destCBSizeBytes = 0;
    };

    const CBufferInfo* GetCBInfo(UINT bRegister) const;
    bool GetCBFieldInfo(UINT bRegister, const std::string& name, CBufferField& out) const;
    bool GetCBFieldOffset(UINT bRegister, const std::string& name, UINT& outOffset, UINT& outSize) const;
    UINT GetCBSizeBytes(UINT bRegister) const {const CBufferInfo* cb = GetCBInfo(bRegister); return cb ? cb->sizeBytes : 0u; }
    UINT GetCBSizeBytesAligned(UINT bRegister, UINT alignment) const {
        return (GetCBSizeBytes(bRegister) + (alignment - 1)) & ~(alignment - 1);
    }

    const GraphicsDesc& GetCachedGraphicsDesc() const { return cachedGfxDesc_; }

    CBFieldHandle ComputeCBFieldHandle(UINT bRegister, const std::string& name) const;
    CBFieldHandle ComputeCB0FieldHandle(const std::string& name) const { return ComputeCBFieldHandle(0, name); }

    template<typename T>
    bool CopyData(const CBufferField& info, const T& value, uint8_t* destCB, UINT destCBSizeBytes, UINT idx)
    {
        const UINT stride = (info.elementStride ? info.elementStride : info.size);
        const UINT count = (info.elementCount ? info.elementCount : 1);

        if (idx >= count) { return false; }

        const size_t dstOff = size_t(info.offset) + size_t(idx) * size_t(stride);
        if (dstOff >= destCBSizeBytes) { return false; }
        if (dstOff + stride > destCBSizeBytes) { return false; }

        uint8_t* dst = destCB + dstOff;
        const size_t copyBytes = std::min<size_t>(sizeof(T), stride);
        std::memcpy(dst, &value, copyBytes);
        return true;
    }

    template<typename T>
    bool UpdateCBField(const CBFieldHandle& handle,
        const T& value, uint8_t* destCB,
        std::optional<UINT> arrayIdxParam = std::nullopt)
    {
        if (!destCB) { return false; }
        if (!handle.field) { return false; }

        return CopyData(*handle.field, value, destCB, handle.destCBSizeBytes, arrayIdxParam ? *arrayIdxParam : 0);
    }

private:
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    mutable ComPtr<ID3D12PipelineState> pipelineStateWire_;
    ComPtr<ID3DBlob> vertexShader_;
    ComPtr<ID3DBlob> pixelShader_;
    Renderer* renderer_ = nullptr;
    mutable std::mutex wireframeMtx_;
    bool isCompute_ = false;
    std::vector<RootParameterInfo> rootParams_;

    // Cache for rebuilding
    GraphicsDesc cachedGfxDesc_{};
    ComputeDesc  cachedCmpDesc_{};

    // Watch list
    std::vector<std::wstring>                     watchedFiles_;
    std::vector<std::filesystem::file_time_type>  watchedTimes_;
    mutable std::mutex                            watchMtx_;
    std::atomic<bool> pendingReload_{ false };

    struct RetiredState {
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12PipelineState> wirePso;
        ComPtr<ID3D12RootSignature> rs;
        uint64_t retireFrame = 0;
    };
    std::vector<RetiredState> retired_;

    robin_hood::unordered_map<UINT, CBufferInfo> cbInfos_; // bReg -> info

    static void ReflectShaderBlob(ID3DBlob* blob,
        robin_hood::unordered_map<UINT, CBufferInfo>& io);
    static void ProcessReflection(ID3D12ShaderReflection* refl,
        robin_hood::unordered_map<UINT, CBufferInfo>& io);

    // Shared builders
    bool BuildGraphicsPSO(Renderer* r, const GraphicsDesc& gd,
        ComPtr<ID3D12RootSignature>& outRS,
        ComPtr<ID3D12PipelineState>& outPSO,
        ComPtr<ID3DBlob>& outVS,
        ComPtr<ID3DBlob>& outPS,
        std::vector<RootParameterInfo>& outParams,
        std::vector<std::wstring>& outIncludes);

    bool BuildComputePSO(Renderer* r, const ComputeDesc& cd,
        ComPtr<ID3D12RootSignature>& outRS,
        ComPtr<ID3D12PipelineState>& outPSO,
        std::vector<RootParameterInfo>& outParams,
        std::vector<std::wstring>& outIncludes);

    // Compilation with includes and defines
    static HRESULT CompileWithIncludes(const std::wstring& file,
        const char* entry, const char* target, UINT flags,
        const DefineList& defines,
        ComPtr<ID3DBlob>& outBlob,
        std::vector<std::wstring>& outIncludes);

    bool EnsureWireframePipeline_() const;
    void RefreshWatchTimes_();
};

// -------- Manager --------
class MaterialManager {
public:
    std::shared_ptr<Material> GetOrCreateGraphics(Renderer* r, const Material::GraphicsDesc& gd);
    std::shared_ptr<Material> GetOrCreateCompute(Renderer* r, const Material::ComputeDesc& cd);

    // Legacy-compatible wrapper
    std::shared_ptr<Material> GetOrCreateCompute(Renderer* r, const std::wstring& shaderFile) {
        Material::ComputeDesc cd{}; cd.shaderFile = shaderFile;
        return GetOrCreateCompute(r, cd);
    }

    bool RequestFSProbeAsync();
    bool ApplyPendingHotReloads(Renderer* r, uint64_t frameIndex, uint64_t keepAliveFrames);
    bool IsProbeInFlight() const { return fsProbeInFlight_.load(std::memory_order_acquire); }

    void Clear();

private:
    // key → material
    std::unordered_map<std::wstring, std::shared_ptr<Material>> materials_;
    std::atomic<bool> fsProbeInFlight_{ false };

private:
    static std::wstring BuildKey(const Material::GraphicsDesc& gd);
    static std::wstring BuildKey(const Material::ComputeDesc& cd);
};
