#include "materials/MaterialDataManager.h"
#include "core/logging/Log.h"
#include "rendering/core/Renderer.h"
#include "rendering/meshes/MeshManager.h" // GltfMaterialDesc + DescribeGltfMaterial (A3)

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

// nlohmann/json — single header
#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

using Microsoft::WRL::ComPtr;

namespace
{
    // Schema v2 (I0): one flat JSON object per material. Texture keys + feature flags + optional
    // parameter DEFAULTS. Shared by the per-file loader and the legacy monolith entries (whose
    // objects use the same keys, minus the params that never existed there).
    MaterialPreset ParseMaterialPresetJson(const nlohmann::json& p)
    {
        const auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };
        MaterialPreset preset;
        preset.albedoPath   = widen(p.value("albedo", std::string{}));
        preset.mrPath       = widen(p.value("mr", std::string{}));
        preset.normalPath   = widen(p.value("normal", std::string{}));
        preset.emissivePath = widen(p.value("emissive", std::string{}));
        preset.shaderPath   = widen(p.value("shader", std::string{}));
        preset.normalIsRG   = p.value("normalIsRG", true);

        if (const auto it = p.find("shadingModel"); it != p.end())
        {
            ShadingModel parsed = ShadingModel::DefaultLit;
            const bool valid = it->is_string() &&
                TryParseShadingModel(it->get_ref<const std::string&>(), parsed);
            if (valid)
            {
                preset.shadingModel = parsed;
            }
            else
            {
                const std::string value = it->is_string()
                    ? it->get_ref<const std::string&>()
                    : std::string("<non-string>");
                LOG_WARNING(logging::LogCategory::Asset, "unknown shadingModel '{}'; using defaultLit", value);
            }
        }

        preset.alphaTest   = p.value("alphaTest", false);
        preset.alphaCutoff = p.value("alphaCutoff", 0.5f);
        preset.twoSided    = p.value("twoSided", false);

        // Optional param defaults — only mark hasParams when the file actually carries one, so
        // migrated/param-less materials leave per-object params untouched (demo levels identical).
        const auto readFloats = [&](const char* key, float* dst, size_t n) -> bool
        {
            const auto it = p.find(key);
            if (it == p.end() || !it->is_array() || it->size() < n) { return false; }
            for (size_t i = 0; i < n; ++i) { dst[i] = (*it)[i].get<float>(); }
            return true;
        };
        float v4[4];
        if (readFloats("subsurfaceColor", v4, 3))
        {
            preset.surfaceParams.subsurfaceColor = float3(v4[0], v4[1], v4[2]);
        }
        preset.surfaceParams.transmissionStrength = p.value("transmissionStrength", 0.0f);
        preset.surfaceParams.indirectSpecularScale =
            std::clamp(p.value("indirectSpecularScale", 1.0f), 0.0f, 1.0f);
        preset.surfaceParams.ambientOcclusion = p.value("ambientOcclusion", 1.0f);
        preset.surfaceParams.transmissionAlbedoPower =
            std::clamp(p.value("transmissionAlbedoPower", 0.6f), 0.0f, 4.0f);
        preset.surfaceParams.transmissionNormalWeight =
            std::clamp(p.value("transmissionNormalWeight", 0.35f), 0.0f, 1.0f);
        preset.surfaceParams.terrainZoneSize =
            std::clamp(p.value("terrainZoneSize", 4.0f), 0.25f, 64.0f);
        preset.surfaceParams.terrainRotationDegrees =
            std::clamp(p.value("terrainRotation", 180.0f), 0.0f, 180.0f);
        preset.surfaceParams.terrainScaleVariation =
            std::clamp(p.value("terrainScaleVariation", 0.25f), 0.0f, 0.75f);
        preset.surfaceParams.terrainBlend =
            std::clamp(p.value("terrainBlend", 0.35f), 0.0f, 1.0f);
        preset.surfaceParams.terrainEdgeBreakup =
            std::clamp(p.value("terrainEdgeBreakup", 0.09f), 0.0f, 0.45f);
        preset.surfaceParams.terrainEdgeDetail =
            std::clamp(p.value("terrainEdgeDetail", 3.5f), 0.5f, 12.0f);

        if (readFloats("tint", v4, 4))
        {
            preset.params.baseColor = float4(v4[0], v4[1], v4[2], v4[3]);
            preset.hasParams = true;
        }
        if (readFloats("metalRough", v4, 2))
        {
            preset.params.metalRough = float2(v4[0], v4[1]);
            preset.hasParams = true;
        }
        if (p.contains("useMR"))
        {
            preset.params.SetUseMR(p.value("useMR", true));
            preset.hasParams = true;
        }
        if (p.contains("multiplyMR"))
        {
            preset.params.SetMultiplyMR(p.value("multiplyMR", false));
            preset.hasParams = true;
        }
        if (readFloats("texOffsScale", v4, 4))
        {
            preset.params.texOffsScale = float4(v4[0], v4[1], v4[2], v4[3]);
            preset.hasParams = true;
        }
        if (readFloats("emissiveColor", v4, 3))
        {
            preset.params.emissiveColor = float3(v4[0], v4[1], v4[2]);
            preset.hasParams = true;
        }
        if (p.contains("emissiveStrength"))
        {
            preset.params.emissiveStrength = p.value("emissiveStrength", 0.0f);
            preset.hasParams = true;
        }
        if (p.contains("normalStrength"))
        {
            preset.params.SetNormalStrength(p.value("normalStrength", 1.0f));
            preset.hasParams = true;
        }
        return preset;
    }
}

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

    for (const auto& [name, p] : j["presets"].items()) {
        if (!p.is_object()) {
            continue;
        }
        RegisterPreset(name, ParseMaterialPresetJson(p));
    }
    return true;
}

bool MaterialDataManager::LoadPresetsFromDirectory(const std::wstring& directory)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::is_directory(directory, ec)) {
        return false;
    }

    size_t loaded = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(directory, ec)) {
        if (ec) { break; }
        std::error_code fileEc;
        if (!entry.is_regular_file(fileEc)) { continue; }
        const fs::path& p = entry.path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext != ".json") { continue; }

        std::ifstream f(p);
        if (!f) { continue; }
        std::stringstream ss;
        ss << f.rdbuf();
        const nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, false, /*ignore_comments=*/true);
        if (j.is_discarded() || !j.is_object()) { continue; }

        RegisterPreset(p.stem().string(), ParseMaterialPresetJson(j));
        ++loaded;
    }
    return loaded > 0;
}

bool MaterialDataManager::LoadPresetFromFile(const std::wstring& path, const std::string& registerAs)
{
    namespace fs = std::filesystem;
    const fs::path materialPath(path);
    std::error_code ec;
    if (!fs::is_regular_file(materialPath, ec))
    {
        return false;
    }

    std::string extension = materialPath.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".json")
    {
        return false;
    }

    std::ifstream file(materialPath);
    if (!file)
    {
        return false;
    }
    std::stringstream contents;
    contents << file.rdbuf();
    const nlohmann::json json = nlohmann::json::parse(contents.str(), nullptr, false,
        /*ignore_comments=*/true);
    if (json.is_discarded() || !json.is_object())
    {
        return false;
    }

    // Register under the caller's name when it gave one. The lazy path looks a preset up by the
    // name a level wrote -- which may carry a SUBFOLDER, e.g. `_sweep/d0` -- and then concatenates
    // `data/materials/<name>.json`. Keying on the file's stem instead would register `d0`, the
    // find() right after would miss, and the object would silently render with no material at all:
    // the level looks like it lost its geometry. The startup scan passes no name and keeps the
    // stem, which is the same thing for a flat folder.
    RegisterPreset(registerAs.empty() ? materialPath.stem().string() : registerAs,
                   ParseMaterialPresetJson(json));
    return true;
}

bool MaterialDataManager::HasPreset(const std::string& name) const
{
    return presets_.find(name) != presets_.end();
}

const MaterialPreset* MaterialDataManager::FindPreset(const std::string& name) const
{
    const auto it = presets_.find(name);
    return (it == presets_.end()) ? nullptr : &it->second;
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

    // Does a preset exist? If not, lazy-load its file — materials created AFTER startup (importer,
    // I1/I3 editor) aren't in presets_ yet; data/materials/<name>.json is the source of truth.
    auto pit = presets_.find(name);
    if (pit == presets_.end()) {
        const std::wstring path = L"data/materials/" + std::wstring(name.begin(), name.end()) + L".json";
        if (LoadPresetFromFile(path, name)) {
            pit = presets_.find(name);
        }
        if (pit == presets_.end()) {
            return {};
        }
    }

    const MaterialPreset& p = pit->second;
    auto md = std::make_shared<MaterialData>();
    md->normalIsRG = p.normalIsRG;
    md->shadingModel = p.shadingModel;
    md->surfaceParams = p.surfaceParams;

    // I0 schema v2: alpha-test/two-sided ride the same MaterialData fields the glTF path uses,
    // so the per-slot PSO plumbing (ALPHA_TEST define, cull mode) works unchanged. Param defaults
    // are stashed for GBufferRenderable::Init to seed into non-overridden slots.
    md->alphaMask   = p.alphaTest;
    md->alphaCutoff = p.alphaCutoff;
    md->doubleSided = p.twoSided || p.shadingModel == ShadingModel::TwoSidedFoliage;
    md->shaderOverride = p.shaderPath;
    if (p.hasParams)
    {
        md->hasPresetParams = true;
        md->presetParams = p.params;
    }

    if (!p.albedoPath.empty()) { (void)md->LoadAlbedo(renderer, uploadCmdList, p.albedoPath, uploadKeepAlive); }
    if (!p.mrPath.empty())     { (void)md->LoadMR    (renderer, uploadCmdList, p.mrPath,     uploadKeepAlive); }
    if (!p.normalPath.empty()) { (void)md->LoadNormal(renderer, uploadCmdList, p.normalPath, uploadKeepAlive); }

    cache_[name] = md;
    return md;
}

std::shared_ptr<MaterialData> MaterialDataManager::GetOrCreateFromGltf(Renderer* renderer,
    ID3D12GraphicsCommandList* uploadCmdList,
    std::vector<ComPtr<ID3D12Resource>>* uploadKeepAlive,
    const std::string& gltfSelector,
    int groupOrdinal)
{
    const std::string key = "gltf::" + gltfSelector + "@" + std::to_string(groupOrdinal);
    if (auto it = cache_.find(key); it != cache_.end()) {
        return it->second;
    }

    const GltfMaterialDesc d = MeshManager::DescribeGltfMaterial(gltfSelector, groupOrdinal);
    if (!d.valid) {
        return {}; // no material on this group
    }

    const auto widen = [](const std::string& s) { return std::wstring(s.begin(), s.end()); };

    auto md = std::make_shared<MaterialData>();
    md->fromGltf = true;
    // H6 convention: a .dds MR is ALWAYS engine-layout (R=metal, G=rough) — the importer repacks
    // glTF's G=rough/B=metal on conversion and is the only .dds producer. A raw png/jpg MR (an
    // unimported staging asset previewed straight from the glTF) is still glTF-layout, so it
    // keeps the MR_LAYOUT_GLTF shader swizzle. Texture2D::CreateFromFile (H2) prefers the
    // sibling .dds, so mirror exactly that resolution here.
    bool mrIsEngineLayoutDds = false;
    if (!d.mrPath.empty())
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path mrPath(d.mrPath);
        std::string ext = mrPath.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".dds")
        {
            mrIsEngineLayoutDds = true;
        }
        else
        {
            fs::path sibling = mrPath;
            sibling.replace_extension(".dds");
            mrIsEngineLayoutDds = fs::exists(sibling, ec);
        }
    }
    md->mrLayoutGltf = !mrIsEngineLayoutDds; // raw glTF MR packs B=metal, G=rough
    md->normalIsRG = false;      // glTF normal maps are RGB
    // Set the mask fields BEFORE LoadAlbedo: the WIC mip build preserves alpha-test coverage
    // for masked slots (LoadAlbedo reads alphaMask/alphaCutoff).
    md->alphaMask = d.alphaMask;
    md->alphaCutoff = d.alphaCutoff;

    if (!d.albedoPath.empty()) { (void)md->LoadAlbedo(renderer, uploadCmdList, widen(d.albedoPath), uploadKeepAlive); }
    if (!d.mrPath.empty())     { (void)md->LoadMR    (renderer, uploadCmdList, widen(d.mrPath),     uploadKeepAlive); }
    if (!d.normalPath.empty()) { (void)md->LoadNormal(renderer, uploadCmdList, widen(d.normalPath), uploadKeepAlive); }

    // Imported per-object defaults (seeded into GBufferRenderable::matParams_ at Init). Factors
    // MULTIPLY the texture channels in-shader, so baseColorFactor -> tint and metallic/roughness
    // factors -> the metalRough fallback/scale; texFlags gate which textures are actually sampled.
    MaterialParams& p = md->gltfDefaultParams;
    p.baseColor  = float4(d.baseColor[0], d.baseColor[1], d.baseColor[2], d.baseColor[3]);
    p.metalRough = float2(d.metallic, d.roughness);
    p.SetUseAlbedo(md->hasAlbedo);
    p.SetUseMR(md->hasMR);
    p.SetMultiplyMR(md->mrLayoutGltf); // raw glTF factors multiply; imported DDS already bakes them
    p.SetUseNormal(md->hasNormal);
    p.SetNormalStrength(d.normalScale);
    // D: glTF emissiveFactor drives self-illumination (strength 1; KHR_emissive_strength not
    // parsed — no staged asset uses it). emissiveTexture recorded below but not sampled yet.
    p.emissiveColor = float3(d.emissive[0], d.emissive[1], d.emissive[2]);
    p.emissiveStrength = (d.emissive[0] != 0.0f || d.emissive[1] != 0.0f || d.emissive[2] != 0.0f) ? 1.0f : 0.0f;

    // Recorded for later parts (not applied in A3). alphaMask/alphaCutoff moved above LoadAlbedo.
    md->doubleSided = d.doubleSided;
    md->emissiveFactor = float3(d.emissive[0], d.emissive[1], d.emissive[2]);
    md->emissiveTexPath = d.emissivePath;

    cache_[key] = md;
    return md;
}

void MaterialDataManager::ClearCache()
{
    cache_.clear();
}

void MaterialDataManager::EvictCached(const std::string& name)
{
    cache_.erase(name);
}

std::size_t MaterialDataManager::EvictCachedForTextures(
    const std::function<bool(const std::wstring&)>& pred,
    std::vector<std::string>* outEvicted)
{
    if (!pred) { return 0; }

    // Collect first, erase after: robin_hood rehashes on erase, so mutating mid-iteration is not
    // safe the way it is for std::unordered_map.
    std::vector<std::string> doomed;
    for (const auto& [name, data] : cache_)
    {
        if (data && data->UsesTexture(pred)) { doomed.push_back(name); }
    }
    for (const std::string& name : doomed)
    {
        cache_.erase(name);
        if (outEvicted) { outEvicted->push_back(name); }
    }
    return doomed.size();
}

void MaterialDataManager::ClearAll()
{
    cache_.clear();
    presets_.clear();
}
