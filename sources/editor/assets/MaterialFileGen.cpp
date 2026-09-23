#include "editor/assets/MaterialFileGen.h"
#if WITH_EDITOR

#include "core/logging/Log.h"
#include "rendering/meshes/Mesh.h"
#include "rendering/meshes/MeshManager.h"

#pragma warning(push)
#pragma warning(disable: 26819)
#include "third_party/json/json.hpp"
#pragma warning(pop)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <unordered_map>

namespace fs = std::filesystem;

namespace materialgen
{
    std::vector<float> MeasureSeamRatios(const std::string& geometry)
    {
        MeshCpuData cpu;
        MeshLoadOptions options;
        options.generateTangentSpace = false; // positions are all this reads
        MeshManager parser;
        if (!parser.ParseFileCpu(geometry, cpu, options) || cpu.vertices.empty())
        {
            return {};
        }

        // Weld on a grid of 1e-5 of the bounding diagonal: exporters split vertices at every UV
        // and normal seam, and an unwelded mesh would read as a pile of open patches.
        DirectX::XMFLOAT3 lo = cpu.vertices[0].position;
        DirectX::XMFLOAT3 hi = lo;
        for (const VertexPNTUV& v : cpu.vertices)
        {
            lo = { std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z) };
            hi = { std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z) };
        }
        const double dx = hi.x - lo.x, dy = hi.y - lo.y, dz = hi.z - lo.z;
        const double diagonal = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double cell = diagonal > 0.0 ? diagonal * 1.0e-5 : 1.0;
        struct CellHash
        {
            std::size_t operator()(const std::array<std::int64_t, 3>& c) const noexcept
            {
                return static_cast<std::size_t>(c[0] * 73856093LL ^ c[1] * 19349663LL ^ c[2] * 83492791LL);
            }
        };
        std::unordered_map<std::array<std::int64_t, 3>, std::uint32_t, CellHash> cells;
        std::vector<std::uint32_t> weld(cpu.vertices.size());
        std::vector<DirectX::XMFLOAT3> welded;
        for (std::size_t i = 0; i < cpu.vertices.size(); ++i)
        {
            const DirectX::XMFLOAT3& p = cpu.vertices[i].position;
            const std::array<std::int64_t, 3> key{
                static_cast<std::int64_t>(std::llround(p.x / cell)),
                static_cast<std::int64_t>(std::llround(p.y / cell)),
                static_cast<std::int64_t>(std::llround(p.z / cell)) };
            const auto [it, inserted] = cells.try_emplace(key, static_cast<std::uint32_t>(welded.size()));
            if (inserted) { welded.push_back(p); }
            weld[i] = it->second;
        }

        std::vector<Mesh::Submesh> submeshes = cpu.submeshes;
        if (submeshes.empty())
        {
            submeshes.push_back({ 0, static_cast<std::uint32_t>(cpu.indices.size()), 0 });
        }
        std::vector<float> ratios;
        ratios.reserve(submeshes.size());
        for (const Mesh::Submesh& submesh : submeshes)
        {
            // Directed-edge counts: a sealed edge is used exactly once each way.
            std::unordered_map<std::uint64_t, std::uint32_t> directed;
            double area = 0.0;
            const std::size_t end = std::min<std::size_t>(cpu.indices.size(),
                static_cast<std::size_t>(submesh.indexOffset) + submesh.indexCount);
            for (std::size_t t = submesh.indexOffset; t + 2 < end; t += 3)
            {
                const std::uint32_t tri[3] = { weld[cpu.indices[t]], weld[cpu.indices[t + 1]],
                    weld[cpu.indices[t + 2]] };
                if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) { continue; }
                const DirectX::XMFLOAT3& a = welded[tri[0]];
                const DirectX::XMFLOAT3& b = welded[tri[1]];
                const DirectX::XMFLOAT3& c = welded[tri[2]];
                const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
                const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
                const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
                area += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
                for (int e = 0; e < 3; ++e)
                {
                    ++directed[(static_cast<std::uint64_t>(tri[e]) << 32) | tri[(e + 1) % 3]];
                }
            }
            double seam = 0.0;
            for (const auto& [edge, count] : directed)
            {
                const std::uint32_t u = static_cast<std::uint32_t>(edge >> 32);
                const std::uint32_t v = static_cast<std::uint32_t>(edge & 0xffffffffu);
                const auto reverse = directed.find((static_cast<std::uint64_t>(v) << 32) | u);
                const std::uint32_t back = reverse == directed.end() ? 0u : reverse->second;
                if (count == 1 && back == 1) { continue; }
                // Each undirected edge once: from its lower end, or from whichever direction exists.
                if (back != 0 && u > v) { continue; }
                const DirectX::XMFLOAT3& a = welded[u];
                const DirectX::XMFLOAT3& b = welded[v];
                const double ex = b.x - a.x, ey = b.y - a.y, ez = b.z - a.z;
                seam += std::sqrt(ex * ex + ey * ey + ez * ez);
            }
            constexpr double kPi = 3.14159265358979323846;
            ratios.push_back(area > 0.0
                ? static_cast<float>(seam * seam / (4.0 * kPi * area))
                : std::numeric_limits<float>::infinity());
        }
        return ratios;
    }

    TwoSidedDecision DecideTwoSided(bool gltfDoubleSided, bool alphaMask, bool measured,
        float seamRatio)
    {
        if (!gltfDoubleSided)
        {
            return { false, "single-sided: the glTF does not ask for doubleSided" };
        }
        if (alphaMask)
        {
            return { true, "two-sided: alpha-masked card, glTF doubleSided kept" };
        }
        if (!measured)
        {
            return { true, "two-sided: geometry not readable, glTF doubleSided kept" };
        }
        if (seamRatio < kClosedSeamRatio)
        {
            return { false, "single-sided: closed surface, glTF doubleSided dropped" };
        }
        return { true, "two-sided: open surface, glTF doubleSided kept" };
    }

    bool IsAlphaCutout(const GltfMaterialDesc& d, float* cutoffOut)
    {
        const bool cutout = d.alphaMask || (d.alphaBlend && !d.albedoPath.empty());
        if (cutoffOut) { *cutoffOut = d.alphaMask ? d.alphaCutoff : 0.5f; }
        return cutout;
    }

    bool IsFoliageCard(const GltfMaterialDesc& d)
    {
        // Two-sided by the glTF's own say: a cutout card is open by construction, so the seam
        // measure DecideTwoSided applies to opaque surfaces never overrules it.
        return d.valid && d.doubleSided && IsAlphaCutout(d);
    }

    std::string WriteFromGltf(const std::string& geometry, int ordinal,
        const std::string& name, bool overwrite, SurfaceMeasure* measure)
    {
        const GltfMaterialDesc d = MeshManager::DescribeGltfMaterial(geometry, ordinal);
        if (!d.valid) { return "auto"; } // null-material slot -> resolve from glTF at runtime
        float cutoff = 0.5f;
        const bool cutout = IsAlphaCutout(d, &cutoff);
        const bool foliage = IsFoliageCard(d);

        const fs::path matPath = fs::path("data/materials") / (name + ".json");
        std::error_code ec;
        fs::create_directories("data/materials", ec);
        if (!overwrite && fs::exists(matPath, ec)) { return name; } // preserve prior/edited file

        // Only an opaque double-sided material is worth measuring: nothing else can change.
        bool measured = false;
        float ratio = 0.0f;
        if (d.doubleSided && !cutout)
        {
            SurfaceMeasure local;
            SurfaceMeasure& surface = measure ? *measure : local;
            if (!surface.measured)
            {
                surface.seamRatios = MeasureSeamRatios(geometry);
                surface.measured = true;
            }
            measured = ordinal >= 0 && static_cast<std::size_t>(ordinal) < surface.seamRatios.size();
            ratio = measured ? surface.seamRatios[static_cast<std::size_t>(ordinal)] : 0.0f;
        }
        const TwoSidedDecision decision = DecideTwoSided(d.doubleSided, cutout, measured, ratio);
        const bool twoSided = decision.twoSided;
        if (d.doubleSided)
        {
            LOG_INFO(logging::LogCategory::Asset, "material {}: submesh {} of {}: {}{}", name,
                ordinal, geometry, decision.reason,
                measured ? " (seam ratio " + std::to_string(ratio) + ")" : std::string());
        }

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
        // A leaf card gets the shading the hand-tuned palm leaves carry (coconut_palm_2,
        // curly_palm_1): light through the leaf at full strength, the leaf's own normal deciding
        // it, a little less sky specular. It used to come in defaultLit like a rock -- every
        // imported fern, grass tuft and frond lit as an opaque card with nothing behind it.
        m["shadingModel"] = foliage ? "twoSidedFoliage" : "defaultLit";
        m["subsurfaceColor"] = { 1.0f, 1.0f, 1.0f };
        m["transmissionStrength"] = foliage ? 1.0f : 0.0f;
        m["transmissionAlbedoPower"] = 0.6f;
        m["transmissionNormalWeight"] = foliage ? 1.0f : 0.35f;
        m["indirectSpecularScale"] = foliage ? 0.7f : 1.0f;
        m["ambientOcclusion"] = 1.0f;
        m["normalIsRG"] = false;
        if (cutout) { m["alphaTest"] = true; m["alphaCutoff"] = cutoff; }
        if (d.alphaBlend || foliage)
        {
            LOG_INFO(logging::LogCategory::Asset, "material {}: {}{}", name,
                d.alphaBlend ? (cutout ? "glTF BLEND read as an alpha cutout at 0.5"
                                       : "glTF BLEND without a texture kept opaque")
                             : "alpha cutout",
                foliage ? ", two-sided -> twoSidedFoliage" : "");
        }
        if (twoSided) { m["twoSided"] = true; }
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
