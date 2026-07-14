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

    // A3/B2: get or create a runtime auto-material from a glTF material selector ("path.gltf#N").
    // groupOrdinal >= 0 addresses submesh/group i of a multi-submesh load (see
    // MeshManager::DescribeGltfMaterial); -1 = the selector's own group. Loads the glTF's
    // textures (glTF MR channel layout, RGB normals), stashes the imported per-object defaults
    // on MaterialData::gltfDefaultParams and the alpha/emissive fields for Parts C/D. Cached by
    // (selector, ordinal). Returns nullptr if the glTF has no material for that group.
    std::shared_ptr<MaterialData> GetOrCreateFromGltf(Renderer* renderer,
                                                      ID3D12GraphicsCommandList* uploadCmdList,
                                                      std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                                      const std::string& gltfSelector,
                                                      int groupOrdinal = -1);

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