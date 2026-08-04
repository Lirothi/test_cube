#include "editor/assets/MaterialFileGen.h"
#if WITH_EDITOR

#include "rendering/meshes/MeshManager.h"

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace materialgen
{
    std::string WriteFromGltf(const std::string& geometry, int ordinal,
        const std::string& name, bool overwrite)
    {
        const GltfMaterialDesc d = MeshManager::DescribeGltfMaterial(geometry, ordinal);
        if (!d.valid) { return "auto"; } // null-material slot -> resolve from glTF at runtime

        const fs::path matPath = fs::path("data/materials") / (name + ".json");
        std::error_code ec;
        fs::create_directories("data/materials", ec);
        if (!overwrite && fs::exists(matPath, ec)) { return name; } // preserve prior/edited file

        // Preset paths are stored relative to the working dir WITH FORWARD SLASHES (the same
        // convention AssetImporter documents). These arrive from ResolveTexUri, which joins the
        // glTF's URI onto the staging directory as it was handed in — on Windows that is
        // "import_staging\rocks" + "/textures/x.png", i.e. mixed separators.
        //
        // That mattered: ImportPanel::RepointPresetPaths rewrites "import_staging/<name>/" to
        // "models/<name>/" after the import copies the converted DDS out of staging, and it does a
        // TEXTUAL prefix match. Mixed separators never matched, so the repoint silently did
        // nothing and every generated preset kept pointing at the raw staging PNG — which is how a
        // 256-pixel thumbnail ended up decoding 58 MB of source art with the converted DDS sitting
        // unused next to it.
        const auto normalize = [](std::string p)
        {
            std::replace(p.begin(), p.end(), '\\', '/');
            return p;
        };

        nlohmann::json m = nlohmann::json::object();
        if (!d.albedoPath.empty()) { m["albedo"] = normalize(d.albedoPath); }
        if (!d.mrPath.empty()) { m["mr"] = normalize(d.mrPath); }
        if (!d.normalPath.empty()) { m["normal"] = normalize(d.normalPath); }
        m["shadingModel"] = "defaultLit";
        m["subsurfaceColor"] = { 1.0f, 1.0f, 1.0f };
        m["transmissionStrength"] = 0.0f;
        m["transmissionAlbedoPower"] = 0.6f;
        m["transmissionNormalWeight"] = 0.35f;
        m["indirectSpecularScale"] = 1.0f;
        m["ambientOcclusion"] = 1.0f;
        m["normalIsRG"] = false;
        if (d.alphaMask) { m["alphaTest"] = true; m["alphaCutoff"] = d.alphaCutoff; }
        if (d.doubleSided) { m["twoSided"] = true; }
        // Factors are baked into the DDS by H6 when a texture exists; only surface them as a param
        // when there's no texture to carry them.
        if (d.albedoPath.empty() &&
            (d.baseColor[0] != 1.0f || d.baseColor[1] != 1.0f || d.baseColor[2] != 1.0f))
        {
            m["tint"] = { d.baseColor[0], d.baseColor[1], d.baseColor[2], d.baseColor[3] };
        }
        if (d.mrPath.empty()) { m["metalRough"] = { d.metallic, d.roughness }; }
        if (d.emissive[0] != 0.0f || d.emissive[1] != 0.0f || d.emissive[2] != 0.0f)
        {
            m["emissiveColor"] = { d.emissive[0], d.emissive[1], d.emissive[2] };
            m["emissiveStrength"] = 1.0f;
        }

        std::ofstream mo(matPath, std::ios::trunc);
        if (!mo) { return {}; }
        mo << m.dump(2) << '\n';
        return name;
    }
}

#endif // WITH_EDITOR
