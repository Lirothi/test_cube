#include "materials/MaterialDataManager.h"
#include "rendering/core/Renderer.h"

#include <fstream>
#include <sstream>

// nlohmann/json — single header
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

using Microsoft::WRL::ComPtr;

void MaterialDataManager::RegisterPreset(const std::string& name, const MaterialPreset& preset)
{
    presets_[name] = preset;
}

bool MaterialDataManager::LoadPresetsFromJsonFile(const std::wstring& path)
{
    std::ifstream f(path);
    if (!f) {
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
    if (j.is_discarded() || !j.contains("presets") || !j["presets"].is_object()) {
        return false;
    }

    const auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };

    for (const auto& [name, p] : j["presets"].items()) {
        if (!p.is_object()) {
            continue;
        }
        MaterialPreset preset;
        preset.albedoPath = widen(p.value("albedo", std::string{}));
        preset.mrPath     = widen(p.value("mr", std::string{}));
        preset.normalPath = widen(p.value("normal", std::string{}));
        preset.normalIsRG = p.value("normalIsRG", true);
        preset.useTBN     = p.value("useTBN", true);
        RegisterPreset(name, preset);
    }
    return true;
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
    // Already cached?
    if (auto it = cache_.find(name); it != cache_.end()) {
        return it->second;
    }

    // Does a preset exist?
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