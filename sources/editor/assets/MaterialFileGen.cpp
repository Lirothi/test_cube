#include "editor/assets/MaterialFileGen.h"
#if WITH_EDITOR

#include "assets/AssetImporter.h"
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
#include <cstdio>
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

    float MeasureCutShare(const MeshCpuData& cpu, size_t submesh, const assets::AlphaImage& alpha,
        float cutoff, int samples)
    {
        if (alpha.width == 0 || alpha.height == 0 || alpha.alpha.size() < alpha.width * alpha.height ||
            samples <= 0)
        {
            return -1.0f;
        }
        size_t first = 0;
        size_t count = cpu.indices.size();
        if (!cpu.submeshes.empty())
        {
            if (submesh >= cpu.submeshes.size()) { return -1.0f; }
            first = cpu.submeshes[submesh].indexOffset;
            count = cpu.submeshes[submesh].indexCount;
        }
        else if (submesh != 0)
        {
            return -1.0f;
        }
        const size_t end = std::min(cpu.indices.size(), first + count);

        // Area-weighted samples: equal steps along the running sum of triangle areas, so a triangle
        // takes samples in proportion to its area, each placed in it by a low-discrepancy (R2) point.
        // Deterministic, so the dialog, the import and the gate read the same number.
        std::vector<double> runningArea;
        runningArea.reserve((end - std::min(first, end)) / 3);
        double total = 0.0;
        for (size_t t = first; t + 2 < end; t += 3)
        {
            const DirectX::XMFLOAT3& a = cpu.vertices[cpu.indices[t]].position;
            const DirectX::XMFLOAT3& b = cpu.vertices[cpu.indices[t + 1]].position;
            const DirectX::XMFLOAT3& c = cpu.vertices[cpu.indices[t + 2]].position;
            const double ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
            const double vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
            const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
            total += 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
            runningArea.push_back(total);
        }
        if (!(total > 0.0)) { return -1.0f; }

        const float threshold = cutoff * 255.0f;
        const double step = total / samples;
        size_t tri = 0;
        int counted = 0;
        int below = 0;
        for (int k = 0; k < samples; ++k)
        {
            const double target = (k + 0.5) * step;
            while (tri + 1 < runningArea.size() && runningArea[tri] < target) { ++tri; }
            const double r1 = std::fmod(0.5 + k * 0.7548776662466927, 1.0);
            const double r2 = std::fmod(0.5 + k * 0.5698402909980532, 1.0);
            const double s = std::sqrt(r1);
            const double wa = 1.0 - s, wb = s * (1.0 - r2), wc = s * r2;
            const size_t t = first + tri * 3;
            const DirectX::XMFLOAT2& ua = cpu.vertices[cpu.indices[t]].uv;
            const DirectX::XMFLOAT2& ub = cpu.vertices[cpu.indices[t + 1]].uv;
            const DirectX::XMFLOAT2& uc = cpu.vertices[cpu.indices[t + 2]].uv;
            double u = wa * ua.x + wb * ub.x + wc * uc.x;
            double v = wa * ua.y + wb * ub.y + wc * uc.y;
            if (!std::isfinite(u) || !std::isfinite(v)) { continue; }
            u -= std::floor(u);
            v -= std::floor(v);
            const size_t x = std::min(alpha.width - 1, static_cast<size_t>(u * alpha.width));
            const size_t y = std::min(alpha.height - 1, static_cast<size_t>(v * alpha.height));
            ++counted;
            below += alpha.alpha[y * alpha.width + x] < threshold ? 1 : 0;
        }
        return counted > 0 ? static_cast<float>(below) / static_cast<float>(counted) : -1.0f;
    }

    LeafVerdict DecideLeafCard(const GltfMaterialDesc& d, float cutShare)
    {
        LeafVerdict v;
        v.materialIndex = d.materialIndex;
        v.name = d.name;
        v.cutShare = cutShare;
        if (!d.valid)
        {
            v.reason = "no material";
            return v;
        }
        if (!IsAlphaCutout(d))
        {
            v.reason = "surface: opaque";
            return v;
        }
        if (!d.doubleSided)
        {
            v.reason = "surface: a single-sided cutout";
            return v;
        }
        v.candidate = true;
        char text[192];
        if (cutShare < 0.0f)
        {
            // Nothing to measure with is not evidence against a leaf: keep what the flags say.
            v.leaf = true;
            v.reason = d.albedoPath.empty()
                ? "leaf card by its flags alone: no albedo file to measure the cutout on"
                : "leaf card by its flags alone: " + fs::path(d.albedoPath).filename().string() +
                  " could not be read to measure the cutout";
            return v;
        }
        v.leaf = cutShare >= kLeafCutShare;
        std::snprintf(text, sizeof(text), "%s: the cutout removes %.1f%% of the surface (a leaf: %.0f%% or more)",
            v.leaf ? "leaf card" : "surface", cutShare * 100.0f, kLeafCutShare * 100.0f);
        v.reason = text;
        return v;
    }

    std::vector<LeafVerdict> ClassifyLeafCards(const std::string& geometry)
    {
        const size_t groups = std::max<size_t>(1, MeshManager::CountSubmeshes(geometry));
        std::vector<GltfMaterialDesc> descs(groups);
        bool anyToMeasure = false;
        for (size_t i = 0; i < groups; ++i)
        {
            descs[i] = MeshManager::DescribeGltfMaterial(geometry, static_cast<int>(i));
            anyToMeasure = anyToMeasure || (descs[i].valid && descs[i].doubleSided && IsAlphaCutout(descs[i]) &&
                !descs[i].albedoPath.empty());
        }
        MeshCpuData cpu;
        bool parsed = false;
        if (anyToMeasure)
        {
            MeshLoadOptions options;
            options.generateTangentSpace = false; // positions and texcoords are all this reads
            MeshManager parser;
            parsed = parser.ParseFileCpu(geometry, cpu, options) && !cpu.indices.empty();
        }
        // One decode per texture: a file's materials often share an atlas.
        std::unordered_map<std::string, assets::AlphaImage> images;
        std::vector<LeafVerdict> out;
        out.reserve(groups);
        for (size_t i = 0; i < groups; ++i)
        {
            const GltfMaterialDesc& d = descs[i];
            float cutoff = 0.5f;
            float cutShare = -1.0f;
            if (parsed && d.valid && d.doubleSided && IsAlphaCutout(d, &cutoff) && !d.albedoPath.empty())
            {
                const auto [it, inserted] = images.try_emplace(d.albedoPath);
                if (inserted) { assets::LoadAlphaChannel(d.albedoPath, 1024, it->second); }
                cutShare = MeasureCutShare(cpu, i, it->second, cutoff);
            }
            out.push_back(DecideLeafCard(d, cutShare));
        }
        return out;
    }

    GlassVerdict ClassifyGlass(const std::string& geometry, int ordinal)
    {
        GlassVerdict v;
        const GltfMaterialDesc d = MeshManager::DescribeGltfMaterial(geometry, ordinal);
        if (!d.valid || !(d.transmission > 0.0f))
        {
            v.reason = "not glass: no KHR_materials_transmission";
            return v;
        }
        float cutoff = 0.5f;
        const bool cutout = IsAlphaCutout(d, &cutoff);
        const bool needGeometry = !d.transmissionPath.empty() || cutout;
        MeshCpuData cpu;
        bool parsed = false;
        if (needGeometry)
        {
            MeshLoadOptions options;
            options.generateTangentSpace = false; // positions and texcoords are all this reads
            MeshManager parser;
            parsed = parser.ParseFileCpu(geometry, cpu, options) && !cpu.indices.empty();
        }
        // A split import's geometry is the node: its own group 0. A whole file numbers them.
        const size_t submesh = ordinal > 0 ? static_cast<size_t>(ordinal) : 0u;
        // The transmitting share: factor * map R over kGlassTransmissionMin (MeasureCutShare counts BELOW).
        v.glassShare = 1.0f;
        if (!d.transmissionPath.empty())
        {
            assets::AlphaImage red;
            const float below = (parsed && assets::LoadAlphaChannel(d.transmissionPath, 1024, red, 0))
                ? MeasureCutShare(cpu, submesh, red, kGlassTransmissionMin / d.transmission)
                : -1.0f;
            v.glassShare = below >= 0.0f ? 1.0f - below : -1.0f;
        }
        char text[224];
        if (v.glassShare == 0.0f)
        {
            v.reason = "not glass: no texel of its surface transmits (the transmission map is 0 there)";
            return v;
        }
        v.glass = true; // unmeasurable (-1) is not evidence against the glTF's own word
        if (cutout && parsed && !d.albedoPath.empty())
        {
            assets::AlphaImage alpha;
            const float cut = assets::LoadAlphaChannel(d.albedoPath, 1024, alpha)
                ? MeasureCutShare(cpu, submesh, alpha, cutoff) : -1.0f;
            v.opaqueShare = cut >= 0.0f ? 1.0f - cut : -1.0f;
        }
        v.split = cutout && v.opaqueShare >= kGlassOpaqueShareMin;
        std::snprintf(text, sizeof(text), "glass: %.1f%% of the surface transmits; %s", v.glassShare * 100.0f,
            v.split ? "its cutout keeps an opaque part, which stays in the G-buffer" : "all of it is glass");
        v.reason = text;
        if (v.split)
        {
            std::snprintf(text, sizeof(text), " (%.1f%% opaque at alpha %.2f)", v.opaqueShare * 100.0f, cutoff);
            v.reason += text;
        }
        return v;
    }

    namespace
    {
        // The keys a leaf card's material says differently from a surface's. WriteFromGltf writes
        // them for a new file and ApplyLeafShading switches them in an existing one, so the two
        // cannot drift apart.
        void WriteLeafKeys(nlohmann::json& m, bool leaf)
        {
            // A leaf card gets the shading the hand-tuned palm leaves carry (coconut_palm_2,
            // curly_palm_1): light through the leaf at full strength, the leaf's own normal deciding
            // it, a little less sky specular. It used to come in defaultLit like a rock -- every
            // imported fern, grass tuft and frond lit as an opaque card with nothing behind it.
            m["shadingModel"] = leaf ? "twoSidedFoliage" : "defaultLit";
            m["transmissionStrength"] = leaf ? 1.0f : 0.0f;
            m["transmissionNormalWeight"] = leaf ? 1.0f : 0.35f;
            m["indirectSpecularScale"] = leaf ? 0.7f : 1.0f;
        }
    }

    bool ApplyLeafShading(const std::string& name, bool leaf)
    {
        const fs::path matPath = fs::path("data/materials") / (name + ".json");
        nlohmann::json m;
        {
            std::ifstream in(matPath);
            if (!in) { return false; }
            m = nlohmann::json::parse(in, nullptr, false, true);
        }
        if (m.is_discarded() || !m.is_object()) { return false; }
        const nlohmann::json before = m;
        WriteLeafKeys(m, leaf);
        if (leaf) { m["twoSided"] = true; }
        if (m == before) { return false; }
        std::ofstream out(matPath, std::ios::trunc);
        if (!out) { return false; }
        out << m.dump(2) << '\n';
        LOG_INFO(logging::LogCategory::Asset, "material {}: switched to {}", name,
            leaf ? "leaf-card shading (twoSidedFoliage)" : "surface shading (defaultLit)");
        return static_cast<bool>(out);
    }

    std::string WriteFromGltf(const std::string& geometry, int ordinal,
        const std::string& name, bool overwrite, SurfaceMeasure* measure, std::optional<bool> leaf)
    {
        const GltfMaterialDesc d = MeshManager::DescribeGltfMaterial(geometry, ordinal);
        if (!d.valid) { return "auto"; } // null-material slot -> resolve from glTF at runtime
        float cutoff = 0.5f;
        const bool cutout = IsAlphaCutout(d, &cutoff);
        if (!leaf.has_value())
        {
            const std::vector<LeafVerdict> verdicts = ClassifyLeafCards(geometry);
            leaf = ordinal >= 0 && static_cast<size_t>(ordinal) < verdicts.size() &&
                verdicts[static_cast<size_t>(ordinal)].leaf;
        }
        const fs::path matPath = fs::path("data/materials") / (name + ".json");
        std::error_code ec;
        fs::create_directories("data/materials", ec);
        if (!overwrite && fs::exists(matPath, ec)) { return name; } // preserve prior/edited file

        // Glass first: a glass slot is never a leaf card, and its cutout survives only as the split.
        const GlassVerdict glass = d.transmission > 0.0f ? ClassifyGlass(geometry, ordinal) : GlassVerdict{};
        if (d.transmission > 0.0f)
        {
            LOG_INFO(logging::LogCategory::Asset, "material {}: submesh {} of {}: {}", name, ordinal, geometry,
                glass.reason);
        }
        const bool foliage = *leaf && !glass.glass;

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
        // A leaf card's normal decides both its lighting and the light through it, so a map whose
        // texels lean ~60 degrees on average (ground_palm's, made from its own albedo) turns the
        // whole plant dark and grainy. Such a map is kept -- its detail is real -- but at the
        // strength that brings it to the steepness a good leaf map has (coconut/date/curly palm
        // fronds and fern_02 measure 0.91-0.95). Opaque surfaces are not touched: a brain coral's
        // grooves measure 0.55 and are meant to.
        if (foliage && !d.normalPath.empty())
        {
            constexpr float kSteepLeafMeanZ = 0.75f;
            constexpr float kLeafMeanZ = 0.9f;
            const assets::NormalMapSteepness steep =
                assets::MeasureNormalMapSteepness(d.normalPath, d.albedoPath);
            if (steep.valid && steep.meanZ < kSteepLeafMeanZ)
            {
                const float strength = std::round(steep.StrengthFor(kLeafMeanZ) * 100.0f) / 100.0f;
                m["normalStrength"] = strength;
                LOG_WARNING(logging::LogCategory::Asset,
                    "material {}: normal map {} leans {:.0f} deg on average over the {:.0f}% of texels "
                    "the leaf covers (mean z {:.2f}; a good leaf map is 0.9+) -> normalStrength {:.2f}",
                    name, normalize(d.normalPath), std::acos(steep.meanZ) * 57.2958f,
                    steep.coverage * 100.0f, steep.meanZ, strength);
            }
        }
        WriteLeafKeys(m, foliage);
        m["subsurfaceColor"] = { 1.0f, 1.0f, 1.0f };
        m["transmissionAlbedoPower"] = 0.6f;
        m["ambientOcclusion"] = 1.0f;
        m["normalIsRG"] = false;
        if (cutout && (!glass.glass || glass.split)) { m["alphaTest"] = true; m["alphaCutoff"] = cutoff; }
        if (glass.glass)
        {
            m["shadingModel"] = "thinTranslucent";
            m["transmission"] = d.transmission;
            if (!d.transmissionPath.empty()) { m["transmissionMap"] = normalize(d.transmissionPath); }
            m["ior"] = d.ior;
        }
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
            // KHR_materials_emissive_strength (1 without it). The bathroom's candles ship 10 and an
            // emissive map that says WHERE -- both used to be dropped, and the whole candle glowed
            // at 1 or nothing did.
            m["emissiveStrength"] = d.emissiveStrength;
            if (!d.emissivePath.empty()) { m["emissive"] = normalize(d.emissivePath); }
        }

        std::ofstream mo(matPath, std::ios::trunc);
        if (!mo) { return {}; }
        mo << m.dump(2) << '\n';
        return name;
    }
}

#endif // WITH_EDITOR
