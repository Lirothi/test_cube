#pragma once
#include <d3d12.h>
#include <memory>
#include <wrl/client.h>
#include <string>
#include <unordered_map>
#include "RenderContext.h"
#include <filesystem>
#include <mutex>

using namespace Microsoft::WRL;

class Renderer;

class Material {
public:
    struct GraphicsDesc {
        GraphicsDesc()
        {
            FillDefaultsTriangle();
        }
        // что компилим
        std::wstring shaderFile;
        const char* vsEntry = "VSMain";
        const char* psEntry = "PSMain";

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

        // заполняем дефолтные стейты
        void FillDefaultsTriangle() {
            // raster
            ZeroMemory(&raster, sizeof(raster));
            raster.FillMode = D3D12_FILL_MODE_SOLID;
            raster.CullMode = D3D12_CULL_MODE_BACK;
            raster.FrontCounterClockwise = FALSE;
            raster.DepthClipEnable = TRUE;
            // blend
            ZeroMemory(&blend, sizeof(blend));
            for (int i = 0; i < 8; ++i)
            {
                blend.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            }
            // depth
            ZeroMemory(&depth, sizeof(depth));
            depth.DepthEnable = TRUE;
            depth.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            depth.StencilEnable = FALSE;
        }
    };

    struct RootParameterInfo {
        enum Type { Constants, CBV, SRV, UAV, Table, TableSampler } type;
        UINT rootIndex = 0;      // индекс root-параметра
        UINT bindingRegister = 0;// номер регистра b0/t0/u0 для поиска в RenderContext
        UINT constantsCount = 0; // только для constants
    };

    Material() = default;
    void CreateGraphics(Renderer* renderer, const GraphicsDesc& gd);
    void CreateCompute(Renderer* renderer, const std::wstring& shaderFile);

    bool IsCompute() const { return isCompute_; }

    ID3D12RootSignature* GetRootSignature() const { return rootSignature_.Get(); }
    ID3D12PipelineState* GetPipelineState() const { return pipelineState_.Get(); }

    void Bind(ID3D12GraphicsCommandList* cmdList, const RenderContext& ctx) const;

    // Фоновая проверка ФС (быстрая): если есть изменения — ставит pending=true
    bool FSProbeAndFlagPending();
    // Вызывать в главном потоке: если pending=true, пересоберёт PSO/RS и сбросит флаг
    bool HotReloadIfPending(Renderer* r, uint64_t frameIndex, uint64_t keepAliveFrames);
    // Очистка «пенсионеров» (старых PSO/RS) после безопасного количества кадров
    void CollectRetired(uint64_t frameIndex, uint64_t keepAliveFrames);

private:
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pipelineState_;
    bool isCompute_ = false;
    std::vector<RootParameterInfo> rootParams_; // соответствует root signature

    // кэш для пересборки
    GraphicsDesc     cachedGfxDesc_{};
    std::wstring     shaderFileCS_;
    std::string      csEntry_ = "main";

    // вотч-лист
    std::vector<std::wstring>                     watchedFiles_;
    std::vector<std::filesystem::file_time_type>  watchedTimes_;
    mutable std::mutex                            watchMtx_;

    // флаг «нужна пересборка»
    std::atomic<bool> pendingReload_{ false };

    struct RetiredState {
        ComPtr<ID3D12PipelineState> pso;
        ComPtr<ID3D12RootSignature> rs;
        uint64_t retireFrame = 0;
    };
    std::vector<RetiredState> retired_;

    // общие билдеры (без дублирования)
    bool BuildGraphicsPSO(Renderer* r, const GraphicsDesc& gd,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& outRS,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPSO,
        std::vector<RootParameterInfo>& outParams,
        std::vector<std::wstring>& outIncludes);

    bool BuildComputePSO(Renderer* r, const std::wstring& csFile, const char* csEntry,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& outRS,
        Microsoft::WRL::ComPtr<ID3D12PipelineState>& outPSO,
        std::vector<RootParameterInfo>& outParams,
        std::vector<std::wstring>& outIncludes);

    static HRESULT CompileWithIncludes(const std::wstring& file,
        const char* entry, const char* target, UINT flags,
        Microsoft::WRL::ComPtr<ID3DBlob>& outBlob,
        std::vector<std::wstring>& outIncludes);

    void RefreshWatchTimes_(); // под мьютексом
};

// MaterialManager: manages unique materials by key (e.g., shader file name)
class MaterialManager {
public:
    std::shared_ptr<Material> GetOrCreateGraphics(Renderer* r, const Material::GraphicsDesc& gd) {
        std::wstring fmts = L"";
        for (UINT i = 0; i < gd.numRT; ++i) {
            fmts += std::to_wstring((int)(gd.numRT == 1 ? gd.rtvFormat : gd.rtvFormats[i])) + L",";
        }
        std::wstring key = L"G2|" + gd.shaderFile + L"|" +
            std::wstring(gd.inputLayoutKey.begin(), gd.inputLayoutKey.end()) + L"|" +
            std::to_wstring((int)gd.topologyType) + L"|" +
            fmts + L"|" + std::to_wstring((int)gd.dsvFormat);
        auto it = materials_.find(key);
        if (it != materials_.end()) { return it->second; }
        auto m = std::make_shared<Material>();
        m->CreateGraphics(r, gd);
        materials_[key] = m;
        return m;
    }
    std::shared_ptr<Material> GetOrCreateCompute(Renderer* renderer, const std::wstring& shaderFile) {
        std::wstring key = L"C|" + shaderFile;
        auto it = materials_.find(key);
        if (it != materials_.end()) {
            return it->second;
        }
        auto mat = std::make_shared<Material>();
        mat->CreateCompute(renderer, shaderFile);
        materials_[key] = mat;
        return mat;
    }

    // Разовое «сканирование» файлов в фоне (если не идёт уже)
    bool RequestFSProbeAsync();
    // Применить pending-пересборки и подчистить пенсионеров (звать в кадре)
    void ApplyPendingHotReloads(Renderer* r, uint64_t frameIndex, uint64_t keepAliveFrames);
    // Узнать, не идёт ли уже сканирование (для Renderer::Update)
    bool IsProbeInFlight() const { return fsProbeInFlight_.load(std::memory_order_acquire); }

    void Clear() {
        materials_.clear();
    }

private:
    std::unordered_map<std::wstring, std::shared_ptr<Material>> materials_;
    std::atomic<bool> fsProbeInFlight_{ false };
};