#include "MaterialDataManager.h"
#include "Renderer.h"

using Microsoft::WRL::ComPtr;

void MaterialDataManager::RegisterPreset(const std::string& name, const MaterialPreset& preset)
{
    presets_[name] = preset;
}

bool MaterialDataManager::HasPreset(const std::string& name) const
{
    return presets_.find(name) != presets_.end();
}

std::shared_ptr<MaterialData> MaterialDataManager::FindLoaded(const std::string& name) const
{
    auto it = cache_.find(name);
    if (it != cache_.end()) {
        return it->second;
    }
    return {};
}

std::shared_ptr<MaterialData> MaterialDataManager::GetOrCreate(Renderer* renderer,
                                                               ID3D12GraphicsCommandList* uploadCmdList,
                                                               std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
                                                               const std::string& name)
{
    // уже есть в кэше?
    if (auto it = cache_.find(name); it != cache_.end()) {
        return it->second;
    }

    // есть пресет?
    auto pit = presets_.find(name);
    if (pit == presets_.end()) {
        return {};
    }

    const MaterialPreset& p = pit->second;
    auto md = std::make_shared<MaterialData>();
    md->normalIsRG = p.normalIsRG;
    md->useTBN     = p.useTBN;

    if (!p.albedoPath.empty()) { (void)md->LoadAlbedo(renderer, uploadCmdList, p.albedoPath, uploadKeepAlive); }
    if (!p.mrPath.empty())     { (void)md->LoadMR    (renderer, uploadCmdList, p.mrPath,     uploadKeepAlive); }
    if (!p.normalPath.empty()) { (void)md->LoadNormal(renderer, uploadCmdList, p.normalPath, uploadKeepAlive); }

    cache_[name] = md;
    return md;
}

void MaterialDataManager::ClearCache()
{
    cache_.clear();
}

void MaterialDataManager::ClearAll()
{
    cache_.clear();
    presets_.clear();
}