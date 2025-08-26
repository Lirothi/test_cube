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
#include <d3d12shader.h>

#include "RenderContext.h"

using namespace Microsoft::WRL;

class Renderer;

class Material {
public:
    // -------- Общая часть: список дефайнов (пермутации) --------
    using DefineList = std::vector<std::pair<std::string, std::string>>; // NAME=VALUE

    // -------- Graphics --------
    struct GraphicsDesc {
        GraphicsDesc() { FillDefaultsTriangle(); }

        // что компилим
        std::wstring shaderFile;
        const char* vsEntry = "VSMain";
        const char* psEntry = "PSMain";
        DefineList   defines;              // << новые: Shader Defines

        // IA
        std::string  inputLayoutKey = "PosColor";
        D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

        // стейты
        D3D12_RASTERIZER_DESC    raster{};
        D3D12_BLEND_DESC         blend{};
        D3D12_DEPTH_STENCIL_DESC depth{};

        // RT/DS
        UINT        numRT = 1;
        DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        DXGI_FORMAT rtvFormats[8] = {
            DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN,
            DXGI_FORMAT_UNKNOWN,        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN
        };
        DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT;
        UINT        sampleCount = 1;

        // RS-флаги
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
            depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            depth.StencilEnable = FALSE;
        }
    };

    // -------- Compute --------
    struct ComputeDesc {
        std::wstring shaderFile;
        const char* csEntry = "main";
        DefineList   defines;                  // << новые: Shader Defines
        D3D12_ROOT_SIGNATURE_FLAGS rsFlags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    };

    struct RootParameterInfo {
        enum Type { Constants, CBV, SRV, UAV, Table, TableSampler } type;
        UINT rootIndex = 0;       // индекс root-параметра
        UINT bindingRegister = 0; // номер регистра b0/t0/u0 для поиска в RenderContext
        UINT constantsCount = 0;  // только для constants
    };

    Material() = default;

    void CreateGraphics(Renderer* renderer, const GraphicsDesc& gd);
    void CreateCompute(Renderer* renderer, const ComputeDesc& cd);

    // Старая совместимая обёртка (оставил, чтобы не ломать старый код)
    void CreateCompute(Renderer* renderer, const std::wstring& shaderFile) {
        ComputeDesc cd{};
        cd.shaderFile = shaderFile;
        CreateCompute(renderer, cd);
    }

    bool IsCompute() const { return isCompute_; }

    ID3D12RootSignature* GetRootSignature() const { return rootSignature_.Get(); }
    ID3D12PipelineState* GetPipelineState() const { return pipelineState_.Get(); }

    void Bind(ID3D12GraphicsCommandList* cmdList, const RenderContext& ctx) const;

    // Хот-релоад
    bool FSProbeAndFlagPending();
    bool HotReloadIfPending(Renderer* r, uint64_t frameIndex, uint64_t keepAliveFrames);
    void CollectRetired(uint64_t frameIndex, uint64_t keepAliveFrames);

    struct CBufferField {
        std::string name;
        UINT        offset = 0;
        UINT        size = 0;
    };
    struct CBufferInfo {
        UINT bindRegister = 0;    // bN
        UINT sizeBytes = 0;
        std::unordered_map<std::string, CBufferField> fieldsByName; // name -> {offset,size}
    };

    const CBufferInfo* GetCBInfo(UINT bRegister) const;
    bool GetCBFieldOffset(UINT bRegister, const std::string& name, UINT& outOffset, UINT& outSize) const;
    UINT GetCBSizeBytes(UINT bRegister) const {const CBufferInfo* cb = GetCBInfo(bRegister); return cb ? cb->sizeBytes : 0u; }
    UINT GetCBSizeBytesAligned(UINT bRegister, UINT alignment) const {
        return (GetCBSizeBytes(bRegister) + (alignment - 1)) & ~(alignment - 1);
    }
    template<typename T> bool UpdateCBField(UINT bRegister, const std::string& name, const T& value, uint8_t* destCB)
    {
        UINT off = 0, sz = 0, bytes = sizeof(T);
        if (GetCBFieldOffset(bRegister, name, off, sz)) {
            std::memcpy(destCB + off, &value, (bytes < sz ? bytes : sz));
            return true;
        }

        return false;
    }
    template<typename T> bool UpdateCB0Field(const std::string& name, const T& value, uint8_t* destCB)
    {
        return UpdateCBField(0, name, value, destCB);
    }

private:
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    bool isCompute_ = false;
    std::vector<RootParameterInfo> rootParams_;

    // кэш для пересборки
    GraphicsDesc cachedGfxDesc_{};
    ComputeDesc  cachedCmpDesc_{};

    // watch-лист
    std::vector<std::wstring>                     watchedFiles_;
    std::vector<std::filesystem::file_time_type>  watchedTimes_;
    mutable std::mutex                            watchMtx_;
    std::atomic<bool> pendingReload_{ false };

    struct RetiredState {
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12RootSignature> rs;
        uint64_t retireFrame = 0;
    };
    std::vector<RetiredState> retired_;

    std::unordered_map<UINT, CBufferInfo> cbInfos_; // bReg -> info

    static void ReflectShaderBlob(ID3DBlob* blob,
        std::unordered_map<UINT, CBufferInfo>& io);
    static void ProcessReflection(ID3D12ShaderReflection* refl,
        std::unordered_map<UINT, CBufferInfo>& io);

    // общие билдеры
    bool BuildGraphicsPSO(Renderer* r, const GraphicsDesc& gd,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& outRS,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPSO,
        std::vector<RootParameterInfo>& outParams,
        std::vector<std::wstring>& outIncludes);

    bool BuildComputePSO(Renderer* r, const ComputeDesc& cd,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& outRS,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPSO,
        std::vector<RootParameterInfo>& outParams,
        std::vector<std::wstring>& outIncludes);

    // компиляция с includes и defines
    static HRESULT CompileWithIncludes(const std::wstring& file,
        const char* entry, const char* target, UINT flags,
        const DefineList& defines,
        Microsoft::WRL::ComPtr<ID3DBlob>& outBlob,
        std::vector<std::wstring>& outIncludes);

    void RefreshWatchTimes_();
};

// -------- Manager --------
class MaterialManager {
public:
    std::shared_ptr<Material> GetOrCreateGraphics(Renderer* r, const Material::GraphicsDesc& gd);
    std::shared_ptr<Material> GetOrCreateCompute(Renderer* r, const Material::ComputeDesc& cd);

    // Старая совместимая обёртка
    std::shared_ptr<Material> GetOrCreateCompute(Renderer* r, const std::wstring& shaderFile) {
        Material::ComputeDesc cd{}; cd.shaderFile = shaderFile;
        return GetOrCreateCompute(r, cd);
    }

    bool RequestFSProbeAsync();
    void ApplyPendingHotReloads(Renderer* r, uint64_t frameIndex, uint64_t keepAliveFrames);
    bool IsProbeInFlight() const { return fsProbeInFlight_.load(std::memory_order_acquire); }

    void Clear() { materials_.clear(); }

private:
    // key → material
    std::unordered_map<std::wstring, std::shared_ptr<Material>> materials_;
    std::atomic<bool> fsProbeInFlight_{ false };

private:
    static std::wstring BuildKey(const Material::GraphicsDesc& gd);
    static std::wstring BuildKey(const Material::ComputeDesc& cd);
};