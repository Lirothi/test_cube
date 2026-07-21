#pragma once
#include <string>
#include "third_party/robin_hood.h"
#include <memory>
#include <vector>
#include <wrl/client.h>

#include "materials/MaterialData.h"

class Renderer;
struct ID3D12GraphicsCommandList;

// Material asset configuration (schema v2, I0). One material = one file
// `data/materials/<name>.json` (filename stem = material name); the legacy monolithic
// data/materials.json {"presets":{...}} is still readable during migration.
struct MaterialPreset {
    // Textures (engine-ready paths; H2 resolves DDS siblings).
    std::wstring albedoPath;
    std::wstring mrPath;      // R=metal, G=rough (H6: always engine layout on disk)
    std::wstring normalPath;
    std::wstring emissivePath; // RESERVED: parsed + persisted, not consumed yet (gbuffer SRV table is 3)
    bool normalIsRG = true;
    bool useTBN     = true;
    ShadingModel shadingModel = ShadingModel::DefaultLit;

    // Optional gbuffer shader override ("shader" key) for feature materials (vegetation sway
    // etc.). Empty = the object's own shader (level JSON, default shaders/gbuffer.hlsl).
    // Contract: the shader must keep the standard PerObject b0 layout, and its CSM counterpart
    // must exist as <name>_csm.hlsl (the shadow pass derives it by suffix). Auto-instancing is
    // disabled for objects whose materials override the shader (no instanced counterpart).
    std::wstring shaderPath;

    // Optional parameter DEFAULTS. hasParams is true when the file carried any param key; a slot
    // seeds from these only when the level JSON didn't override it (per-object params win).
    bool           hasParams = false;
    MaterialParams params;

    // Alpha test / cull features (drive the slot's PSO exactly like the glTF fields do).
    bool  alphaTest = false;
    float alphaCutoff = 0.5f;
    bool  twoSided = false;
};

// Manages presets and the cache of loaded MaterialData
class MaterialDataManager {
public:
    // Register or replace a preset
    void RegisterPreset(const std::string& name, const MaterialPreset& preset);

    // Register all presets from a JSON file ({"presets": {name: {albedo, mr,
    // normal, normalIsRG, useTBN}}}). Returns false if the file is missing or
    // malformed. LEGACY (pre-I0 monolith) — kept for migration safety.
    bool LoadPresetsFromJsonFile(const std::wstring& path);

    // I0: register every material file in a directory (data/materials/*.json, flat
    // schema-v2 objects, name = filename stem). Returns true if at least one loaded.
    bool LoadPresetsFromDirectory(const std::wstring& directory);

    // Register one schema-v2 material file immediately. The editor uses this after creating,
    // duplicating, or renaming a material so it can be assigned without restarting the level.
    bool LoadPresetFromFile(const std::wstring& path);

    // I2: drop the cached MaterialData for a name so the next GetOrCreate rebuilds it (after its
    // definition or textures changed on disk). Live objects keep their shared_ptr until respawned.
    void EvictCached(const std::string& name);

    // Does the preset exist?
    bool HasPreset(const std::string& name) const;

    size_t PresetCount() const { return presets_.size(); }

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
