#include "app/scene/SceneObjectFactory.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/JsonField.h" // R9.6b: Value / As / Find -- strict only under the level loader's scope
#include "core/logging/Log.h"
#include "core/math/Math.h"
#include "core/schema/KeyTable.h" // R9.3: the object's table of keys
#include "core/vfs/Vfs.h"
#include "ocean/BuoyancyJson.h"
#include "physics/CollisionJson.h"
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
        return vfs::Widen(s);
    }

    // R9.6b: an element of another type keeps the default's component (the level loader's StrictScope: it throws).
    float3 ToFloat3(const json& j, const float3& def = float3(0.0f, 0.0f, 0.0f))
    {
        if (!j.is_array() || j.size() < 3) { return def; }
        return float3(jsonfield::As(j[0], def.x), jsonfield::As(j[1], def.y), jsonfield::As(j[2], def.z));
    }

    float4 ToFloat4(const json& j, const float4& def = float4(0.0f, 0.0f, 0.0f, 0.0f))
    {
        if (!j.is_array() || j.size() < 4) { return def; }
        return float4(jsonfield::As(j[0], def.x), jsonfield::As(j[1], def.y), jsonfield::As(j[2], def.z),
                      jsonfield::As(j[3], def.w));
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
        vfs::InStream f(path, /*binary*/ false);
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

    namespace
    {
        thread_local MeshAssetScanCache* t_loadScopeCache = nullptr; // MeshAssetLoadScope
    }

    MeshAssetLoadScope::MeshAssetLoadScope()
        : outer_(t_loadScopeCache)
    {
        t_loadScopeCache = &cache_;
    }

    MeshAssetLoadScope::~MeshAssetLoadScope()
    {
        t_loadScopeCache = outer_;
    }

    json ResolveMeshAsset(const json& o, MeshAssetScanCache* cache)
    {
        const auto meshIt = o.find("mesh");
        if (meshIt == o.end() || !meshIt->is_string()) { return o; }
        if (!cache) { cache = t_loadScopeCache; } // G10d: a level load reads each mesh.json once

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
            // A boat's numbers go key by key: a copy's "vessel" says only what it does differently (the Inspector's Boat
            // section, physics::VesselGroupPerCopy), the rest is its asset's.
            else if (key == "vessel" && it.value().is_object() && eff[effKey].is_object())
            {
                json& own = eff[effKey];
                for (auto v = it.value().begin(); v != it.value().end(); ++v)
                {
                    if (!own.contains(v.key())) { own[v.key()] = v.value(); }
                }
            }
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
            const bool present = vfs::Exists(p);
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
            const std::string model = jsonfield::Value(o, "model", std::string());
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

    // Architecture plan R9.3: a static mesh object's keys -- the level object's, with every key of its mesh.json folded
    // in by ResolveMeshAsset -- as ONE table (core/schema/KeyTable.h): the factory reads an object by walking it, and
    // MCP editor_guide lists it. A row's read is the key's old block, verbatim and in the old order; a key whose meaning
    // spans several (the LOD pixel error and the LOD group, the collision block, the mass, the vessel, the motion) is one
    // row. Nothing writes these rows: the editor keeps an object's JSON as the file has it.
    const schema::Table<StaticMesh>& StaticMeshKeys()
    {
        using S = StaticMesh;
        using ParamField = GBufferRenderable::MaterialParamField;
        static const schema::Table<S> table{ {
            schema::Custom<S>("recomputeNormalSlots", [](const json& o, S& mesh) {
                mesh.SetRecomputeNormalSlots(ToUIntVector(
                    jsonfield::Value(o, "recomputeNormalSlots", json::array())));
            }, nullptr).Label("Recompute normals", 0.0f, "Material slots whose normals are rebuilt from the geometry at load."),

            // mesh.json "chunkGrid": the .mesh.bin was baked with its LOD0 split into an N x N grid of
            // submesh chunks. The value itself is a BAKE parameter; at runtime all it says is "these
            // submeshes are spatial chunks", which turns them into independent shadow casters.
            schema::Custom<S>("chunkGrid", [](const json& o, S& mesh) {
                if (o.contains("chunkGrid") && o["chunkGrid"].is_number_integer())
                {
                    const int grid = o["chunkGrid"].get<int>();
                    if (grid > 0) { mesh.SetChunkGrid(static_cast<unsigned int>(grid)); }
                }
            }, nullptr).Label("Chunk grid", 0.0f, "The bake split LOD0 into N x N chunks: each is its own shadow caster."),

            // mesh.json "lodDistanceScale": per-asset multiplier on every LOD switch DISTANCE. Runtime
            // only -- it changes nothing about the baked geometry, so it must not reach MeshLoadOptions
            // or the .bin cache key. See RenderableObject::SetLodDistanceScale for why UE needs the
            // equivalent (its ScreenSize is authored per LOD per mesh).
            schema::Custom<S>("lodDistanceScale", [](const json& o, S& mesh) {
                if (o.contains("lodDistanceScale") && o["lodDistanceScale"].is_number())
                {
                    mesh.SetLodDistanceScale(o["lodDistanceScale"].get<float>());
                }
            }, nullptr).Label("LOD distance scale", 0.01f, "Multiplies every LOD switch distance of the asset."),

            // mesh.json "lodScreenSize" ([LOD0, LOD1, ...], UE's authored ScreenSize per LOD) and
            // "lodPixelError" (the PixelError UE's automatic screen sizes are estimated with): the per-mesh
            // half of UE's LOD selection (render::g_lodUeScreenSize, LodSelect.h). Runtime only, like the scale.
            schema::Custom<S>("lodScreenSize", [](const json& o, S& mesh) {
                if (o.contains("lodScreenSize") && o["lodScreenSize"].is_array())
                {
                    std::vector<float> sizes;
                    for (const json& v : o["lodScreenSize"]) { sizes.push_back(v.is_number() ? v.get<float>() : 0.0f); }
                    mesh.SetLodScreenSizes(sizes);
                }
            }, nullptr).Label("LOD screen sizes", 0.0f, "UE's authored ScreenSize per LOD: [LOD0, LOD1, ...]."),

            // The PixelError: the asset's own "lodPixelError", else its UE LOD group's ("lodGroup": SmallProp, LargeProp,
            // LevelArchitecture...; render::kUeLodGroups), else the global lod.pixelError (UE's 8).
            schema::Custom<S>("lodPixelError", [](const json& o, S& mesh) {
                if (o.contains("lodPixelError") && o["lodPixelError"].is_number() && o["lodPixelError"].get<float>() > 0.0f)
                {
                    mesh.SetLodPixelError(o["lodPixelError"].get<float>());
                }
                else if (o.contains("lodGroup") && o["lodGroup"].is_string())
                {
                    mesh.SetLodPixelError(render::UeLodGroupPixelError(o["lodGroup"].get<std::string>()));
                }
            }, nullptr).Label("LOD pixel error", 0.1f, "The PixelError UE's screen sizes are estimated with; absent: the "
                              "\"lodGroup\" one (SmallProp, LargeProp, LevelArchitecture...), else lod.pixelError."),

            // "windFoliage": one weight per material slot (0 = woody, 1 = leaves). A foliage asset (any slot weight > 0)
            // bakes its LODs with the leaf prune and the slot drops, which its recorded deviation does not see -- see
            // RenderableObject::LodSelectionBounds for what it selects with instead. Slots past the end fall back to the
            // alpha-mask heuristic in ResolveMaterialSlots. (The collision row reads it too: a leaf does not collide.)
            schema::Custom<S>("windFoliage", [](const json& o, S& mesh) {
                if (o.contains("windFoliage") && o["windFoliage"].is_array())
                {
                    bool foliage = false;
                    for (const json& v : o["windFoliage"]) { foliage = foliage || (v.is_number() && v.get<float>() > 0.0f); }
                    mesh.SetLodFoliage(foliage);
                    std::vector<float> weights;
                    weights.reserve(o["windFoliage"].size());
                    for (const auto& v : o["windFoliage"])
                    {
                        weights.push_back(v.is_number() ? v.get<float>() : 0.0f);
                    }
                    mesh.SetWindFoliageWeights(std::move(weights));
                }
            }, nullptr).Label("Wind foliage", 0.0f, "Per material slot: 0 = woody, 1 = leaves (sway, LOD prune, no collision)."),

            // mesh.json "uvDensity" (texture streaming A1): per-slot world size of a unit-UV square,
            // measured by the bake. Read, never derived here: the geometry is in the .bin and the
            // manifest is what describes it. A manifest that predates the key gets one warning per
            // process and no density; the streaming manager then sizes mips from the object's bounds,
            // UE's own fallback for a reference it cannot measure. `--uv-density=<mesh.json>` fills
            // the key in place, without a re-bake.
            schema::Custom<S>("uvDensity", [](const json& o, S& mesh) {
                if (o.contains("uvDensity") && o["uvDensity"].is_array())
                {
                    std::vector<float> density;
                    for (const json& v : o["uvDensity"]) { density.push_back(v.is_number() ? v.get<float>() : 0.0f); }
                    mesh.SetUvDensities(std::move(density));
                }
                else
                {
                    LOG_WARNING_ONCE(logging::LogCategory::Asset,
                        "mesh.json without uvDensity (texel factor falls back to object bounds), first: {}",
                        jsonfield::Value(o, "mesh", jsonfield::Value(o, "model", std::string("?"))));
                }
            }, nullptr).Label("UV density", 0.0f, "Per slot, the world size of a unit-UV square (the bake measures it)."),

            schema::Custom<S>("rotationDeg", [](const json& o, S& mesh) {
                if (o.contains("rotationDeg"))
                {
                    mesh.SetRotationEulerDeg(ToFloat3(o["rotationDeg"]));
                }
            }, nullptr).Label("Rotation (deg)", 1.0f, "Euler angles [x, y, z] in degrees."),

            // Slot 0's material overrides -- "texOffsScale", "normalStrength", "useMR", "useAlbedo", "baseColor",
            // "emissiveColor", "emissiveStrength", "metalRough" -- ONE row: MaterialParamsRef() bumps the material's
            // content version and marks the params dirty, and the hand code called it once, for all eight.
            schema::Custom<S>("materialOverrides", [](const json& o, S& mesh) {
                auto& mp = mesh.MaterialParamsRef();
                if (o.contains("texOffsScale"))
                {
                    mp.texOffsScale = ToFloat4(o["texOffsScale"], mp.texOffsScale);
                    mesh.MarkMaterialParamOverride(ParamField::TexOffsScale);
                }
                // R9.6b: a key of another type is as if absent here -- no value, no override mark -- except under the
                // level loader's StrictScope, where Find throws as get<T>() did.
                if (const auto v = jsonfield::Find<float>(o, "normalStrength"))
                {
                    mp.texFlags.w = *v;
                    mesh.MarkMaterialParamOverride(ParamField::NormalStrength);
                }
                if (const auto v = jsonfield::Find<bool>(o, "useMR"))
                {
                    mp.SetUseMR(*v);
                    mesh.MarkMaterialParamOverride(ParamField::UseMR);
                }
                // false = the albedo texture is off and baseColor is the colour (a flat material).
                if (const auto v = jsonfield::Find<bool>(o, "useAlbedo"))
                {
                    mp.SetUseAlbedo(*v);
                    mesh.MarkMaterialParamOverride(ParamField::UseAlbedo);
                }
                // Self-illumination on slot 0 (glowing embers etc). The override mask keeps explicitly
                // authored object values while unrelated fields continue to inherit from the material.
                // A flat tint multiplied into the albedo, per object. [r, g, b] keeps the material's
                // own alpha, which is what someone painting a palm red means; [r, g, b, a] is there
                // because alpha is part of this field and refusing to write it would be a rule with
                // nothing behind it.
                if (o.contains("baseColor"))
                {
                    const json& c = o["baseColor"];
                    if (c.is_array() && c.size() == 3)
                    {
                        const float3 rgb = ToFloat3(c, float3(mp.baseColor.x, mp.baseColor.y, mp.baseColor.z));
                        mp.baseColor = float4(rgb.x, rgb.y, rgb.z, mp.baseColor.w);
                    }
                    else
                    {
                        mp.baseColor = ToFloat4(c, mp.baseColor);
                    }
                    mesh.MarkMaterialParamOverride(ParamField::BaseColor);
                }
                if (o.contains("emissiveColor"))
                {
                    mp.emissiveColor = ToFloat3(o["emissiveColor"], mp.emissiveColor);
                    mesh.MarkMaterialParamOverride(ParamField::EmissiveColor);
                }
                if (const auto v = jsonfield::Find<float>(o, "emissiveStrength"))
                {
                    mp.emissiveStrength = *v;
                    mesh.MarkMaterialParamOverride(ParamField::EmissiveStrength);
                }
                if (o.contains("metalRough"))
                {
                    const json& mr = o["metalRough"];
                    if (mr.is_array() && mr.size() >= 2)
                    {
                        mp.metalRough = float2(jsonfield::As(mr[0], mp.metalRough.x), jsonfield::As(mr[1], mp.metalRough.y));
                        mesh.MarkMaterialParamOverride(ParamField::MetalRough);
                    }
                }
            }, nullptr).Label("Slot 0 material", 0.0f, "\"texOffsScale\" [u, v, su, sv], \"normalStrength\", \"useMR\", \"useAlbedo\" "
                              "(false: baseColor is the colour), \"baseColor\" [r, g, b(, a)] multiplied into the albedo, "
                              "\"emissiveColor\" [r, g, b], \"emissiveStrength\", \"metalRough\" [metallic, roughness]."),

            // W3: per-object wind sway strength (0 = rigid). Applied uniformly to every slot at Init
            // (submesh sync), so it is a per-object property rather than a slot-0 material override.
            schema::Custom<S>("windStrength", [](const json& o, S& mesh) {
                if (const auto v = jsonfield::Find<float>(o, "windStrength"))
                {
                    mesh.SetWindStrength(*v);
                }
            }, nullptr).Label("Wind strength", 0.01f, "How much it sways in the wind (0 = rigid)."),

            // Per-ASSET wind tuning. These normally live in models/<name>/<name>.mesh.json and reach us here
            // because ResolveMeshAsset folds every asset key the object does not already carry, so a
            // level object can still override either one.
            schema::Custom<S>("windTrunkStiffness", [](const json& o, S& mesh) {
                if (o.contains("windTrunkStiffness") && o["windTrunkStiffness"].is_number())
                {
                    mesh.SetWindTrunkStiffness(o["windTrunkStiffness"].get<float>());
                }
            }, nullptr).Label("Trunk stiffness", 0.01f, "The asset's trunk against the wind."),

            schema::Custom<S>("renderLayer", [](const json& o, S& mesh) {
                if (const auto v = jsonfield::Find<std::string>(o, "renderLayer"))
                {
                    mesh.SetRenderLayer(RenderLayerFromString(*v));
                }
            }, nullptr).Label("Render layer", 0.0f, "Default, Terrain, Transparent, Sky, Lights, Gizmo or Debug."),

            // UE's Cast Shadow (UPrimitiveComponent::CastShadow): "castShadow": false keeps the object out of every shadow --
            // the sun's and the local lights' maps, VSM, the RT shadow rays. Absent = casts (the Inspector writes the key
            // only while off). Contact shadows trace the depth buffer and still see it (UE: a separate bCastContactShadow).
            schema::Custom<S>("castShadow", [](const json& o, S& mesh) {
                if (o.contains("castShadow") && o["castShadow"].is_boolean())
                {
                    mesh.SetCastsShadow(o["castShadow"].get<bool>());
                }
            }, nullptr).Label("Cast shadow", 0.0f, "false = out of every shadow (sun, local lights, VSM, RT); absent = casts."),
            // How much of a cutout card the visibility rays see (RenderableObject::SetRtCutoutOpacity): the asset's mesh.json
            // "rtCutoutOpacity", folded in by ResolveMeshAsset like every asset key; absent = solid.
            schema::Custom<S>("rtCutoutOpacity", [](const json& o, S& mesh) {
                if (o.contains("rtCutoutOpacity") && o["rtCutoutOpacity"].is_number())
                {
                    mesh.SetRtCutoutOpacity(o["rtCutoutOpacity"].get<float>());
                }
            }, nullptr).Label("RT cutout opacity", 0.01f, "How much of a cutout card the visibility rays see; absent = solid."),
            // Its LODs' and cluster DAG's pixel error, a share of the frame's (Mesh::SetLodPixelErrorScale): mesh.json
            // "lodPixelErrorScale", absent = 1.
            schema::Custom<S>("lodPixelErrorScale", [](const json& o, S& mesh) {
                if (o.contains("lodPixelErrorScale") && o["lodPixelErrorScale"].is_number())
                {
                    mesh.SetLodPixelErrorScale(o["lodPixelErrorScale"].get<float>());
                }
            }, nullptr).Label("LOD pixel error scale", 0.01f, "A share of the frame's pixel error for its LODs and cluster DAG."),
            // The water kept out of it (ocean/WaterMask.h): the asset's mesh.json "waterMask", absent = off.
            schema::Custom<S>("waterMask", [](const json& o, S& mesh) {
                if (o.contains("waterMask") && o["waterMask"].is_boolean())
                {
                    mesh.SetWaterMask(o["waterMask"].get<bool>());
                }
            }, nullptr).Label("Water mask", 0.0f, "Keeps the water out of the hull (ocean/WaterMask.h); absent = off."),
            // Buoyancy: "buoyant" is the per-OBJECT switch (the Inspector's checkbox, absent = off);
            // "buoyancy" says where and how it floats and normally comes from the asset's mesh.json,
            // folded in by ResolveMeshAsset like every other asset key. See ocean/BuoyancyLayout.h.
            schema::Custom<S>("buoyant", [](const json& o, S& mesh) {
                if (o.contains("buoyant") && o["buoyant"].is_boolean())
                {
                    mesh.SetBuoyant(o["buoyant"].get<bool>());
                }
            }, nullptr).Label("Buoyant", 0.0f, "Floats on the ocean (the object's switch; absent = off, a vessel = on)."),
            schema::Custom<S>("buoyancy", [](const json& o, S& mesh) {
                if (o.contains("buoyancy"))
                {
                    buoyancy::Settings settings;
                    if (buoyancy::ReadSettings(o["buoyancy"], settings))
                    {
                        mesh.SetBuoyancySettings(std::move(settings));
                    }
                }
            }, nullptr).Label("Buoyancy", 0.0f, "Where and how it floats (normally the asset's mesh.json)."),
            // Physics (docs/physics_plan.md): "physics" is the per-OBJECT choice, absent = Static (UE's BlockAll: the world collides
            // with what it draws); "collision" says how and normally comes from the asset's mesh.json, folded in like every other
            // asset key. A leaf does not collide: unless the block names its own list, the slots with "windFoliage" > 0 are out.
            // The row reads "collision", "windFoliage", "mass", "vessel", "buoyant" and "physics" together.
            schema::Custom<S>("collision", [](const json& o, S& mesh) {
                physics::CollisionSettings collision;
                bool namedSlots = false;
                if (o.contains("collision")) { physics::ReadCollisionSettings(o["collision"], collision, &namedSlots); }
                if (!namedSlots && o.contains("windFoliage") && o["windFoliage"].is_array())
                {
                    int slot = 0;
                    for (const auto& v : o["windFoliage"])
                    {
                        if (v.is_number() && v.get<float>() > 0.0f) { collision.excludeSlots.push_back(slot); }
                        ++slot;
                    }
                }
                // The object's own kilograms (the Inspector's Mass) over the asset's density.
                if (o.contains("mass") && o["mass"].is_number()) { collision.mass = o["mass"].get<float>(); }
                mesh.SetCollisionSettings(std::move(collision));
                physics::Motion motion = physics::Motion::Static;
                // A boat is a boat wherever it is put (docs/play_mode_plan.md): a copy of a "vessel" asset floats and moves unless the object says otherwise.
                if (o.contains("vessel") && o["vessel"].is_object())
                {
                    auto vessel = std::make_shared<physics::VesselSettings>();
                    physics::ReadVesselSettings(o["vessel"], *vessel);
                    mesh.SetVesselSettings(std::move(vessel));
                    motion = physics::Motion::Dynamic;
                    if (!o.contains("buoyant")) { mesh.SetBuoyant(true); }
                }
                if (o.contains("physics") && o["physics"].is_string()) { physics::MotionFromName(o["physics"].get<std::string>(), motion); }
                mesh.SetPhysicsMotion(motion);
            }, nullptr).Label("Collision / physics", 0.0f, "\"collision\" (how), \"mass\" (kg), \"vessel\" (a boat: dynamic, "
                              "buoyant), \"physics\" (static / dynamic / kinematic; absent = static)."),
            // Plan L: "onAt": "night" (or "day") -- the mesh's emissive (a bulb, a lit window) comes on with the night
            // lamps (rendering/lighting/NightLamps.h); absent = always.
            schema::Custom<S>("onAt", [](const json& o, S& mesh) {
                if (o.contains("onAt") && o["onAt"].is_string())
                {
                    mesh.SetLampSwitch(LampSwitchFromName(o["onAt"].get<std::string>()));
                }
            }, nullptr).Label("On at", 0.0f, "\"night\" or \"day\": its emissive comes on with the night lamps; absent = always."),
        } };
        return table;
    }

    void ApplyStaticMeshJsonProperties(StaticMesh& mesh, const json& o)
    {
        StaticMeshKeys().Read(o, mesh);
    }

    std::unique_ptr<RenderableObjectBase> CreateStaticMeshFromJson(const json& oIn)
    {
        // J1: expand a mesh-asset reference (`"mesh"`) into effective render fields; a plain
        // object (legacy `"model"` + inline plumbing) passes through untouched.
        const json o = ResolveMeshAsset(oIn);

        const std::string model = jsonfield::Value(o, "model", std::string{});
        const std::string material = jsonfield::Value(o, "material", std::string{});
        const std::string layout = jsonfield::Value(o, "inputLayout", std::string("PosNormTanUV"));
        const std::wstring shader = Widen(jsonfield::Value(o, "shader", std::string("shaders/gbuffer.hlsl")));
        const float3 pos = ToFloat3(jsonfield::Value(o, "position", json::array()));
        const float3 scale = ToFloat3(jsonfield::Value(o, "scale", json::array()), float3(1.0f, 1.0f, 1.0f));

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
        const std::string model = jsonfield::Value(o, "model", std::string{});
        const float3 pos = ToFloat3(jsonfield::Value(o, "position", json::array()));
        const float3 scale = ToFloat3(jsonfield::Value(o, "scale", json::array()), float3(1.0f, 1.0f, 1.0f));

        auto glass = std::make_unique<TransparentStaticMesh>(&scene, model, pos, scale, 0.0f);
        TransparentMeshKeys().Read(o, *glass);
        return glass;
    }

    // Architecture plan R9.3: a glass object's keys, as StaticMeshKeys() is the mesh's.
    const schema::Table<TransparentStaticMesh>& TransparentMeshKeys()
    {
        using S = TransparentStaticMesh;
        static const schema::Table<S> table{ {
            schema::Custom<S>("recomputeNormalSlots", [](const json& o, S& glass) {
                glass.SetRecomputeNormalSlots(ToUIntVector(jsonfield::Value(o, "recomputeNormalSlots", json::array())));
            }, nullptr).Label("Recompute normals", 0.0f, "Material slots whose normals are rebuilt from the geometry at load."),
            schema::Custom<S>("tint", [](const json& o, S& glass) {
                if (o.contains("tint")) { glass.SetTint(ToFloat3(o["tint"], float3(1.0f, 1.0f, 1.0f))); }
            }, nullptr).Label("Tint", 0.01f, "The glass's colour [r, g, b]."),
            schema::Custom<S>("absorption", [](const json& o, S& glass) {
                if (o.contains("absorption")) { glass.SetAbsorption(ToFloat3(o["absorption"])); }
            }, nullptr).Label("Absorption", 0.01f, "What the glass absorbs per metre [r, g, b]."),
            // R9.6b: Find -- a key of another type is as if absent, except under the level loader's StrictScope.
            schema::Custom<S>("thickness", [](const json& o, S& glass) {
                if (const auto v = jsonfield::Find<float>(o, "thickness")) { glass.SetThickness(*v); }
            }, nullptr).Label("Thickness", 0.001f, "Metres of glass the absorption runs through."),
            schema::Custom<S>("reflectionStrength", [](const json& o, S& glass) {
                if (const auto v = jsonfield::Find<float>(o, "reflectionStrength")) { glass.SetReflectionStrength(*v); }
            }, nullptr).Label("Reflection strength", 0.01f, "Scales the reflection."),
            schema::Custom<S>("refractionDistortion", [](const json& o, S& glass) {
                if (const auto v = jsonfield::Find<float>(o, "refractionDistortion")) { glass.SetRefractionDistortion(*v); }
            }, nullptr).Label("Refraction distortion", 0.01f, "How much the scene behind bends."),
            schema::Custom<S>("roughness", [](const json& o, S& glass) {
                if (const auto v = jsonfield::Find<float>(o, "roughness")) { glass.SetRoughness(*v); }
            }, nullptr).Label("Roughness", 0.01f, "Frosted (1) to clear (0)."),
            schema::Custom<S>("ior", [](const json& o, S& glass) {
                if (const auto v = jsonfield::Find<float>(o, "ior")) { glass.SetIor(*v); }
            }, nullptr).Label("IOR", 0.01f, "Index of refraction (glass 1.5)."),
            schema::Custom<S>("normalMap", [](const json& o, S& glass) {
                if (const auto v = jsonfield::Find<std::string>(o, "normalMap")) { glass.SetNormalMap(Widen(*v)); }
            }, nullptr).Label("Normal map", 0.0f, "A texture path for the glass's surface."),
        } };
        return table;
    }

    std::unique_ptr<RenderableObjectBase> CreateParticleEmitterFromJson(const json& o)
    {
        // Resolve the emitter desc (preset < overrides < inline), then place it. The emitter
        // only reads its object position for spawning, but the full transform is set so gizmo
        // edits + save round-trip like any other object.
        vfx::EmitterDesc d = vfx::ResolveEmitterDesc(o);
        auto emitter = std::make_unique<ParticleEmitterObject>(d);
        emitter->SetPosition(ToFloat3(jsonfield::Value(o, "position", json::array())));
        if (o.contains("rotationDeg")) { emitter->SetRotationEulerDeg(ToFloat3(o["rotationDeg"])); }
        if (o.contains("scale")) { emitter->SetScale(ToFloat3(jsonfield::Value(o, "scale", json::array()), float3(1.0f, 1.0f, 1.0f))); }
        return emitter;
    }
}
