#pragma once
#include <string>
#include "third_party/robin_hood.h"
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "materials/MaterialData.h"

class Renderer;
struct ID3D12GraphicsCommandList;

// Preset configuration (texture paths + feature flags)
struct MaterialPreset {
    std::wstring albedoPath;
    std::wstring mrPath;
    std::wstring normalPath;
    bool normalIsRG = true;
    bool useTBN     = true;
};

// Manages presets and the cache of loaded MaterialData
class MaterialDataManager {
public:
    // Register or replace a preset
    void RegisterPreset(const std::string& name, const MaterialPreset& preset);

    // Register all presets from a JSON file ({"presets": {name: {albedo, mr,
    // normal, normalIsRG, useTBN}}}). Returns false if the file is missing or
    // malformed.
    bool LoadPresetsFromJsonFile(const std::wstring& path);

    // Does the preset exist?
    bool HasPreset(const std::string& name) const;

    // Get or create MaterialData by preset name (lazy texture loading)
    std::shared_ptr<MaterialData> GetOrCreate(Renderer* renderer,
                                              ID3D12GraphicsCommandList* uploadCmdList,
                                              std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                              const std::string& name);

    // Direct access to an already loaded instance (or nullptr)
    std::shared_ptr<MaterialData> FindLoaded(const std::string& name) const;

    // Clear only the cache (keep the presets)
    void ClearCache();

    // Full reset (presets + cache)
    void ClearAll();

private:
    robin_hood::unordered_map<std::string, MaterialPreset> presets_;
    robin_hood::unordered_map<std::string, std::shared_ptr<MaterialData>> cache_;
};