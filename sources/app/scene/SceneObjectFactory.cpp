#include "app/scene/SceneObjectFactory.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/math/Math.h"
#include "rendering/RenderLayers.h"
#include "rendering/meshes/StaticMesh.h"
#include "rendering/renderables/TransparentStaticMesh.h"
#include "vfx/ParticleEmitterObject.h"
#include "vfx/ParticlePresets.h"

using namespace Math;
using nlohmann::json;

namespace
{
    std::wstring Widen(const std::string& s)
    {
        return std::wstring(s.begin(), s.end());
    }

    float3 ToFloat3(const json& j, const float3& def = float3(0.0f, 0.0f, 0.0f))
    {
        if (!j.is_array() || j.size() < 3) { return def; }
        return float3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
    }

    float4 ToFloat4(const json& j, const float4& def = float4(0.0f, 0.0f, 0.0f, 0.0f))
    {
        if (!j.is_array() || j.size() < 4) { return def; }
        return float4(j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>());
    }

    RenderLayer RenderLayerFromString(const std::string& s)
    {
        if (s == "Terrain") { return RenderLayer::Terrain; }
        if (s == "Transparent") { return RenderLayer::Transparent; }
        if (s == "Sky") { return RenderLayer::Sky; }
        if (s == "Lights") { return RenderLayer::Lights; }
        if (s == "Gizmo") { return RenderLayer::Gizmo; }
        if (s == "Debug") { return RenderLayer::Debug; }
        return RenderLayer::Default;
    }

    std::vector<uint32_t> ToUIntVector(const json& value)
    {
        std::vector<uint32_t> result;
        if (!value.is_array()) { return result; }
        for (const json& entry : value)
        {
            if (entry.is_number_unsigned())
            {
                result.push_back(entry.get<uint32_t>());
            }
            else if (entry.is_number_integer())
            {
                const int64_t slot = entry.get<int64_t>();
                if (slot >= 0) { result.push_back(static_cast<uint32_t>(slot)); }
            }
        }
        return result;
    }
}

namespace SceneObjectFactory
{
    static json ParseJsonFile(const std::string& path)
    {
        std::ifstream f(path);
        if (!f) { return json{}; }
        std::stringstream ss;
        ss << f.rdbuf();
        json parsed = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
        return parsed.is_discarded() ? json{} : parsed;
    }

    // Parses one mesh.json, through `cache` when a scan supplies one. A discarded/absent asset is
    // cached too — repeating a failed open per object is exactly as wasteful as repeating a good one.
    static const json* LoadMeshAssetJson(const std::string& path, MeshAssetScanCache* cache)
    {
        if (cache)
        {
            auto it = cache->meshAssets.find(path);
            if (it == cache->meshAssets.end())
            {
                // A failed open is cached too: repeating it per object is exactly as wasteful as
                // repeating a successful one.
                it = cache->meshAssets.emplace(path, ParseJsonFile(path)).first;
            }
            return it->second.is_object() ? &it->second : nullptr;
        }
        // Uncached callers get the parse held in a thread-local slot valid until their next call,
        // which is all ResolveMeshAsset needs (it copies out of it before returning).
        static thread_local json scratch;
        scratch = ParseJsonFile(path);
        return scratch.is_object() ? &scratch : nullptr;
    }

    json ResolveMeshAsset(const json& o, MeshAssetScanCache* cache)
    {
        const auto meshIt = o.find("mesh");
        if (meshIt == o.end() || !meshIt->is_string()) { return o; }

        const json* assetPtr = LoadMeshAssetJson(meshIt->get<std::string>(), cache);
        if (!assetPtr) { return o; }
        const json& asset = *assetPtr;

        json eff = o;
        // Fold every asset key the object doesn't already carry — material, materials, shader,
        // inputLayout, renderLayer, texOffsScale, params, and any future field all become mesh
        // defaults the object can override. `geometry` -> `model`; `spawnScale` is a spawn-time
        // hint (consumed by the editor spawn factory), not a load field. All paths are cwd-relative,
        // matching every other engine path convention.
        for (auto it = asset.begin(); it != asset.end(); ++it)
        {
            const std::string& key = it.key();
            if (key == "spawnScale") { continue; }
            const std::string effKey = (key == "geometry") ? std::string("model") : key;
            if (!eff.contains(effKey)) { eff[effKey] = it.value(); }
        }
        return eff;
    }

    std::vector<std::string> MeshAssetErrors(const json& oIn, MeshAssetScanCache* cache)
    {
        namespace fs = std::filesystem;
        std::vector<std::string> errs;

        const auto exists = [cache](const std::string& p)
        {
            if (p.empty()) { return false; }
            if (cache)
            {
                auto it = cache->fileExists.find(p);
                if (it != cache->fileExists.end()) { return it->second; }
            }
            std::error_code ec;
            const bool present = fs::exists(fs::path(p), ec);
            if (cache) { cache->fileExists.emplace(p, present); }
            return present;
        };
        // A referenced texture is present if the file exists OR (H2) its .dds sibling does.
        const auto texExists = [&](const std::string& p)
        {
            if (exists(p)) { return true; }
            const size_t dot = p.find_last_of('.');
            const size_t slash = p.find_last_of("/\\");
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            {
                return exists(p.substr(0, dot) + ".dds");
            }
            return false;
        };

        const json o = ResolveMeshAsset(oIn, cache);

        // 1) Geometry: mesh.json resolvable + the geometry file present (fragment stripped).
        if (oIn.contains("mesh") && oIn["mesh"].is_string() && !o.contains("model"))
        {
            errs.push_back("mesh asset not found: " + oIn["mesh"].get<std::string>());
        }
        else
        {
            const std::string model = o.value("model", std::string());
            if (!model.empty())
            {
                const std::string file = model.substr(0, model.find('#'));
                if (!exists(file)) { errs.push_back("geometry not found: " + file); }
            }
        }

        // 2)+3) Named material presets (I0 per-file) and their textures. "auto"/glTF-embedded skipped.
        std::vector<std::string> mats;
        if (o.contains("material") && o["material"].is_string()) { mats.push_back(o["material"].get<std::string>()); }
        if (o.contains("materials") && o["materials"].is_array())
        {
            for (const auto& e : o["materials"]) { if (e.is_string()) { mats.push_back(e.get<std::string>()); } }
        }
        for (const std::string& m : mats)
        {
            if (m.empty() || m == "auto") { continue; }

            // A material's verdict depends only on its NAME, so it is computed once per scan even
            // though hundreds of objects reference the same preset.
            if (cache)
            {
                auto it = cache->materialErrors.find(m);
                if (it != cache->materialErrors.end())
                {
                    errs.insert(errs.end(), it->second.begin(), it->second.end());
                    continue;
                }
            }

            std::vector<std::string> matErrs;
            const std::string matFile = "data/materials/" + m + ".json";
            if (!exists(matFile))
            {
                matErrs.push_back("material not found: " + m);
            }
            else
            {
                const json mj = ParseJsonFile(matFile);
                if (!mj.is_object())
                {
                    matErrs.push_back("material unreadable: " + m);
                }
                else
                {
                    for (const char* key : { "albedo", "mr", "normal" })
                    {
                        if (mj.contains(key) && mj[key].is_string())
                        {
                            const std::string tp = mj[key].get<std::string>();
                            if (!tp.empty() && !texExists(tp))
                            {
                                matErrs.push_back("texture missing (" + m + "/" + key + "): " + tp);
                            }
                        }
                    }
                }
            }

            errs.insert(errs.end(), matErrs.begin(), matErrs.end());
            if (cache) { cache->materialErrors.emplace(m, std::move(matErrs)); }
        }
        return errs;
    }

    void ApplyStaticMeshJsonProperties(StaticMesh& mesh, const json& o)
    {
        mesh.SetRecomputeNormalSlots(ToUIntVector(
            o.value("recomputeNormalSlots", json::array())));

        // mesh.json "chunkGrid": the .mesh.bin was baked with its LOD0 split into an N x N grid of
        // submesh chunks. The value itself is a BAKE parameter; at runtime all it says is "these
        // submeshes are spatial chunks", which turns them into independent shadow casters.
        if (o.contains("chunkGrid") && o["chunkGrid"].is_number_integer())
        {
            const int grid = o["chunkGrid"].get<int>();
            if (grid > 0) { mesh.SetChunkGrid(static_cast<unsigned int>(grid)); }
        }

        if (o.contains("rotationDeg"))
        {
            mesh.SetRotationEulerDeg(ToFloat3(o["rotationDeg"]));
        }

        auto& mp = mesh.MaterialParamsRef();
        using ParamField = GBufferRenderable::MaterialParamField;
        if (o.contains("texOffsScale"))
        {
            mp.texOffsScale = ToFloat4(o["texOffsScale"], mp.texOffsScale);
            mesh.MarkMaterialParamOverride(ParamField::TexOffsScale);
        }
        if (o.contains("normalStrength"))
        {
            mp.texFlags.w = o["normalStrength"].get<float>();
            mesh.MarkMaterialParamOverride(ParamField::NormalStrength);
        }
        if (o.contains("useMR"))
        {
            mp.SetUseMR(o["useMR"].get<bool>());
            mesh.MarkMaterialParamOverride(ParamField::UseMR);
        }
        // Self-illumination on slot 0 (glowing embers etc). The override mask keeps explicitly
        // authored object values while unrelated fields continue to inherit from the material.
        if (o.contains("emissiveColor"))
        {
            mp.emissiveColor = ToFloat3(o["emissiveColor"], mp.emissiveColor);
            mesh.MarkMaterialParamOverride(ParamField::EmissiveColor);
        }
        if (o.contains("emissiveStrength"))
        {
            mp.emissiveStrength = o["emissiveStrength"].get<float>();
            mesh.MarkMaterialParamOverride(ParamField::EmissiveStrength);
        }
        if (o.contains("metalRough"))
        {
            const json& mr = o["metalRough"];
            if (mr.is_array() && mr.size() >= 2)
            {
                mp.metalRough = float2(mr[0].get<float>(), mr[1].get<float>());
                mesh.MarkMaterialParamOverride(ParamField::MetalRough);
            }
        }

        // W3: per-object wind sway strength (0 = rigid). Applied uniformly to every slot at Init
        // (submesh sync), so it is a per-object property rather than a slot-0 material override.
        if (o.contains("windStrength"))
        {
            mesh.SetWindStrength(o["windStrength"].get<float>());
        }

        // Per-ASSET wind tuning. These normally live in models/<name>.mesh.json and reach us here
        // because ResolveMeshAsset folds every asset key the object does not already carry, so a
        // level object can still override either one.
        if (o.contains("windTrunkStiffness") && o["windTrunkStiffness"].is_number())
        {
            mesh.SetWindTrunkStiffness(o["windTrunkStiffness"].get<float>());
        }
        if (o.contains("windFoliage") && o["windFoliage"].is_array())
        {
            // One weight per material slot (0 = woody, 1 = leaves). Slots past the end fall back to
            // the alpha-mask heuristic in ResolveMaterialSlots.
            std::vector<float> weights;
            weights.reserve(o["windFoliage"].size());
            for (const auto& v : o["windFoliage"])
            {
                weights.push_back(v.is_number() ? v.get<float>() : 0.0f);
            }
            mesh.SetWindFoliageWeights(std::move(weights));
        }

        if (o.contains("renderLayer"))
        {
            mesh.SetRenderLayer(RenderLayerFromString(o["renderLayer"].get<std::string>()));
        }
    }

    std::unique_ptr<RenderableObjectBase> CreateStaticMeshFromJson(const json& oIn)
    {
        // J1: expand a mesh-asset reference (`"mesh"`) into effective render fields; a plain
        // object (legacy `"model"` + inline plumbing) passes through untouched.
        const json o = ResolveMeshAsset(oIn);

        const std::string model = o.value("model", std::string{});
        const std::string material = o.value("material", std::string{});
        const std::string layout = o.value("inputLayout", std::string("PosNormTanUV"));
        const std::wstring shader = Widen(o.value("shader", std::string("shaders/gbuffer.hlsl")));
        const float3 pos = ToFloat3(o.value("position", json::array()));
        const float3 scale = ToFloat3(o.value("scale", json::array()), float3(1.0f, 1.0f, 1.0f));

        auto mesh = std::make_unique<StaticMesh>(model, material, layout, shader);
        // B2: optional per-slot material list for multi-submesh assets: "materials": ["a","auto",...]
        // (slot i = entry i; missing slots default to "auto" for glTF models). The scalar
        // "material" stays as slot 0 back-compat.
        if (o.contains("materials") && o["materials"].is_array())
        {
            std::vector<std::string> slots;
            for (const auto& e : o["materials"])
            {
                if (e.is_string()) { slots.push_back(e.get<std::string>()); }
            }
            mesh->SetSlotPresets(std::move(slots));
        }
        mesh->SetPosition(pos);
        mesh->SetScale(scale);
        ApplyStaticMeshJsonProperties(*mesh, o);
        return mesh;
    }

    std::unique_ptr<RenderableObjectBase> CreateTransparentMeshFromJson(Scene& scene, const json& oIn)
    {
        const json o = ResolveMeshAsset(oIn); // J1: transparent meshes may reference a mesh asset too
        const std::string model = o.value("model", std::string{});
        const float3 pos = ToFloat3(o.value("position", json::array()));
        const float3 scale = ToFloat3(o.value("scale", json::array()), float3(1.0f, 1.0f, 1.0f));

        auto glass = std::make_unique<TransparentStaticMesh>(&scene, model, pos, scale, 0.0f);
        glass->SetRecomputeNormalSlots(ToUIntVector(
            o.value("recomputeNormalSlots", json::array())));
        if (o.contains("tint")) { glass->SetTint(ToFloat3(o["tint"], float3(1.0f, 1.0f, 1.0f))); }
        if (o.contains("absorption")) { glass->SetAbsorption(ToFloat3(o["absorption"])); }
        if (o.contains("thickness")) { glass->SetThickness(o["thickness"].get<float>()); }
        if (o.contains("reflectionStrength")) { glass->SetReflectionStrength(o["reflectionStrength"].get<float>()); }
        if (o.contains("refractionDistortion")) { glass->SetRefractionDistortion(o["refractionDistortion"].get<float>()); }
        if (o.contains("roughness")) { glass->SetRoughness(o["roughness"].get<float>()); }
        if (o.contains("ior")) { glass->SetIor(o["ior"].get<float>()); }
        if (o.contains("normalMap")) { glass->SetNormalMap(Widen(o["normalMap"].get<std::string>())); }
        return glass;
    }

    std::unique_ptr<RenderableObjectBase> CreateParticleEmitterFromJson(const json& o)
    {
        // Resolve the emitter desc (preset < overrides < inline), then place it. The emitter
        // only reads its object position for spawning, but the full transform is set so gizmo
        // edits + save round-trip like any other object.
        vfx::EmitterDesc d = vfx::ResolveEmitterDesc(o);
        auto emitter = std::make_unique<ParticleEmitterObject>(d);
        emitter->SetPosition(ToFloat3(o.value("position", json::array())));
        if (o.contains("rotationDeg")) { emitter->SetRotationEulerDeg(ToFloat3(o["rotationDeg"])); }
        if (o.contains("scale")) { emitter->SetScale(ToFloat3(o.value("scale", json::array()), float3(1.0f, 1.0f, 1.0f))); }
        return emitter;
    }
}
