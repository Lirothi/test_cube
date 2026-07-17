#include "app/scene/SceneObjectFactory.h"

#include <fstream>
#include <sstream>
#include <string>

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
}

namespace SceneObjectFactory
{
    json ResolveMeshAsset(const json& o)
    {
        const auto meshIt = o.find("mesh");
        if (meshIt == o.end() || !meshIt->is_string()) { return o; }

        std::ifstream f(meshIt->get<std::string>());
        if (!f) { return o; }
        std::stringstream ss;
        ss << f.rdbuf();
        const json asset = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
        if (asset.is_discarded() || !asset.is_object()) { return o; }

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

    void ApplyStaticMeshJsonProperties(StaticMesh& mesh, const json& o)
    {
        if (o.contains("rotationDeg"))
        {
            mesh.SetRotationEulerDeg(ToFloat3(o["rotationDeg"]));
        }

        auto& mp = mesh.MaterialParamsRef();
        if (o.contains("texOffsScale")) { mp.texOffsScale = ToFloat4(o["texOffsScale"], mp.texOffsScale); }
        if (o.contains("normalStrength")) { mp.texFlags.w = o["normalStrength"].get<float>(); }
        if (o.contains("useMR")) { mp.SetUseMR(o["useMR"].get<bool>()); }
        // D: self-illumination on slot 0 (glowing embers etc). Survives Init for non-glTF meshes
        // (glTF "auto" slots seed from the material — a known A3/B2 layering limit).
        if (o.contains("emissiveColor")) { mp.emissiveColor = ToFloat3(o["emissiveColor"], mp.emissiveColor); }
        if (o.contains("emissiveStrength")) { mp.emissiveStrength = o["emissiveStrength"].get<float>(); }
        if (o.contains("metalRough"))
        {
            const json& mr = o["metalRough"];
            if (mr.is_array() && mr.size() >= 2)
            {
                mp.metalRough = float2(mr[0].get<float>(), mr[1].get<float>());
            }
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
