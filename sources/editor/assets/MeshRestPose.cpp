#include "editor/assets/MeshRestPose.h"
#if WITH_EDITOR

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>

#include "rendering/meshes/MeshManager.h"

namespace restpose
{
namespace
{
    using Mat3 = std::array<std::array<double, 3>, 3>;

    // Symmetric 3x3 eigen-decomposition by cyclic Jacobi rotations; columns of `vectors` are the
    // eigenvectors of `values`, unsorted.
    void Eigen(Mat3 a, std::array<double, 3>& values, Mat3& vectors)
    {
        vectors = Mat3{ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
        for (int sweep = 0; sweep < 32; ++sweep)
        {
            const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
            if (off < 1e-30) { break; }
            for (int p = 0; p < 2; ++p)
            {
                for (int q = p + 1; q < 3; ++q)
                {
                    if (std::abs(a[p][q]) < 1e-300) { continue; }
                    const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                    const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                        (std::abs(theta) + std::sqrt(theta * theta + 1.0));
                    const double c = 1.0 / std::sqrt(t * t + 1.0);
                    const double s = t * c;
                    for (int k = 0; k < 3; ++k) // A <- A J
                    {
                        const double akp = a[k][p], akq = a[k][q];
                        a[k][p] = c * akp - s * akq;
                        a[k][q] = s * akp + c * akq;
                    }
                    for (int k = 0; k < 3; ++k) // A <- J^T A
                    {
                        const double apk = a[p][k], aqk = a[q][k];
                        a[p][k] = c * apk - s * aqk;
                        a[q][k] = s * apk + c * aqk;
                    }
                    for (int k = 0; k < 3; ++k)
                    {
                        const double vkp = vectors[k][p], vkq = vectors[k][q];
                        vectors[k][p] = c * vkp - s * vkq;
                        vectors[k][q] = s * vkp + c * vkq;
                    }
                }
            }
        }
        values = { a[0][0], a[1][1], a[2][2] };
    }

    double Dot(const std::array<double, 3>& a, const std::array<double, 3>& b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    // The row-vector matrix (the engine's) of the smallest rotation taking unit `from` to +Y.
    Math::mat4 RotationToUp(const std::array<double, 3>& from)
    {
        const std::array<double, 3> up{ 0.0, 1.0, 0.0 };
        std::array<double, 3> k{ from[1] * up[2] - from[2] * up[1],
                                 from[2] * up[0] - from[0] * up[2],
                                 from[0] * up[1] - from[1] * up[0] };
        const double s = std::sqrt(Dot(k, k));
        const double c = Dot(from, up);
        Mat3 r{ { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } } };
        if (s < 1e-9)
        {
            if (c < 0.0) { r = Mat3{ { { 1, 0, 0 }, { 0, -1, 0 }, { 0, 0, -1 } } }; } // half turn about X
        }
        else
        {
            for (double& v : k) { v /= s; }
            const Mat3 kx{ { { 0, -k[2], k[1] }, { k[2], 0, -k[0] }, { -k[1], k[0], 0 } } };
            for (int i = 0; i < 3; ++i)
            {
                for (int j = 0; j < 3; ++j)
                {
                    double kk = 0.0;
                    for (int m = 0; m < 3; ++m) { kk += kx[i][m] * kx[m][j]; }
                    r[i][j] += s * kx[i][j] + (1.0 - c) * kk;
                }
            }
        }
        // `r` rotates column vectors; the engine's matrices act on rows, so it goes in transposed.
        DirectX::XMFLOAT4X4 m{};
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j) { m.m[i][j] = static_cast<float>(r[j][i]); }
        }
        m.m[3][3] = 1.0f;
        return Math::mat4(m);
    }

    Math::float3 ToDegrees(const Math::float3& radians)
    {
        return Math::float3(radians.x * Math::RAD2DEG, radians.y * Math::RAD2DEG, radians.z * Math::RAD2DEG);
    }

    Math::float3 ToRadians(const Math::float3& degrees)
    {
        return Math::float3(degrees.x * Math::DEG2RAD, degrees.y * Math::DEG2RAD, degrees.z * Math::DEG2RAD);
    }

    struct Geometry
    {
        std::vector<Math::float3> positions;
        std::vector<std::uint32_t> indices;
    };

    std::mutex g_mutex;
    std::map<std::pair<std::string, long long>, std::shared_ptr<const Geometry>> g_geometry;
    std::map<std::tuple<std::string, long long, int, int, int>, float> g_lift;

    long long WriteStamp(const std::string& path)
    {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path, ec);
        return ec ? -1 : static_cast<long long>(t.time_since_epoch().count());
    }

    std::shared_ptr<const Geometry> LoadGeometry(const std::string& path, long long stamp)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (const auto it = g_geometry.find({ path, stamp }); it != g_geometry.end()) { return it->second; }
        }
        MeshCpuData cpu;
        MeshLoadOptions opt;
        opt.generateTangentSpace = false; // positions are all this reads
        MeshManager manager;
        if (path.empty() || stamp < 0 || !manager.ParseFileCpu(path, cpu, opt) || cpu.indices.size() < 3)
        {
            return nullptr;
        }
        auto geometry = std::make_shared<Geometry>();
        geometry->positions.reserve(cpu.vertices.size());
        for (const VertexPNTUV& v : cpu.vertices) { geometry->positions.emplace_back(v.position); }
        // LOD0 only: the vertex buffer can carry LOD3 leaf-prune copies past LOD0's range, which the
        // LOD0 indices never reach -- so everything below walks the indices, not the vertex array.
        geometry->indices = std::move(cpu.indices);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_geometry[{ path, stamp }] = geometry;
        return geometry;
    }
}

std::optional<Math::float3> Read(const nlohmann::json& asset)
{
    const auto it = asset.find("restRotationDeg");
    if (it == asset.end() || !it->is_array() || it->size() != 3) { return std::nullopt; }
    for (const nlohmann::json& v : *it) { if (!v.is_number()) { return std::nullopt; } }
    return Math::float3((*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>());
}

std::optional<Math::float3> ReadFile(const std::string& meshJsonPath)
{
    std::ifstream file(meshJsonPath);
    if (!file) { return std::nullopt; }
    const nlohmann::json asset = nlohmann::json::parse(file, nullptr, false, true);
    if (asset.is_discarded() || !asset.is_object()) { return std::nullopt; }
    return Read(asset);
}

std::optional<Math::float3> Auto(const std::vector<Math::float3>& positions,
    const std::vector<std::uint32_t>& indices)
{
    // Area-weighted first and second moments of the SURFACE. Per triangle (a, b, c), the mean of
    // x x^T over its area is (a a^T + b b^T + c c^T + s s^T) / 12 with s = a + b + c.
    double area = 0.0;
    std::array<double, 3> mean{};
    Mat3 second{};
    std::vector<std::uint32_t> used;
    used.reserve(indices.size());
    for (size_t t = 0; t + 2 < indices.size(); t += 3)
    {
        if (indices[t] >= positions.size() || indices[t + 1] >= positions.size() ||
            indices[t + 2] >= positions.size())
        {
            return std::nullopt;
        }
        const Math::float3& a = positions[indices[t]];
        const Math::float3& b = positions[indices[t + 1]];
        const Math::float3& c = positions[indices[t + 2]];
        const std::array<double, 3> e1{ b.x - a.x, b.y - a.y, b.z - a.z };
        const std::array<double, 3> e2{ c.x - a.x, c.y - a.y, c.z - a.z };
        const std::array<double, 3> n{ e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                                       e1[0] * e2[1] - e1[1] * e2[0] };
        const double w = 0.5 * std::sqrt(Dot(n, n));
        if (w <= 0.0) { continue; }
        const std::array<std::array<double, 3>, 3> v{ { { a.x, a.y, a.z }, { b.x, b.y, b.z }, { c.x, c.y, c.z } } };
        const std::array<double, 3> s{ v[0][0] + v[1][0] + v[2][0], v[0][1] + v[1][1] + v[2][1],
                                       v[0][2] + v[1][2] + v[2][2] };
        for (int i = 0; i < 3; ++i)
        {
            mean[i] += w * s[i] / 3.0;
            for (int j = 0; j < 3; ++j)
            {
                second[i][j] += w * (v[0][i] * v[0][j] + v[1][i] * v[1][j] + v[2][i] * v[2][j] + s[i] * s[j]) / 12.0;
            }
        }
        area += w;
        used.insert(used.end(), { indices[t], indices[t + 1], indices[t + 2] });
    }
    if (area <= 0.0) { return std::nullopt; }
    for (double& m : mean) { m /= area; }
    Mat3 cov{};
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j) { cov[i][j] = second[i][j] / area - mean[i] * mean[j]; }
    }
    std::array<double, 3> values{};
    Mat3 vectors{};
    Eigen(cov, values, vectors);
    const int thin = static_cast<int>(std::min_element(values.begin(), values.end()) - values.begin());
    std::array<double, 3> n{ vectors[0][thin], vectors[1][thin], vectors[2][thin] };
    const double len = std::sqrt(Dot(n, n));
    if (!(len > 0.0)) { return std::nullopt; }
    for (double& v : n) { v /= len; }

    // Which way up: a cupped shape rests rim-down. Along n, the rim is where the surface is
    // farthest from the axis and the dome where it is nearest; up points from the rim to the dome.
    // A flat or symmetric shape has no preference and keeps the sign the decomposition gave.
    std::sort(used.begin(), used.end());
    used.erase(std::unique(used.begin(), used.end()), used.end());
    std::vector<std::pair<double, double>> radialAlong; // (distance from the axis, height along n)
    radialAlong.reserve(used.size());
    for (const std::uint32_t i : used)
    {
        const std::array<double, 3> d{ positions[i].x - mean[0], positions[i].y - mean[1], positions[i].z - mean[2] };
        const double t = Dot(d, n);
        const std::array<double, 3> r{ d[0] - t * n[0], d[1] - t * n[1], d[2] - t * n[2] };
        radialAlong.emplace_back(std::sqrt(Dot(r, r)), t);
    }
    std::sort(radialAlong.begin(), radialAlong.end());
    const size_t fifth = std::max<size_t>(1, radialAlong.size() / 5);
    double dome = 0.0, rim = 0.0;
    for (size_t i = 0; i < fifth; ++i)
    {
        dome += radialAlong[i].second;
        rim += radialAlong[radialAlong.size() - 1 - i].second;
    }
    if (dome < rim) { for (double& v : n) { v = -v; } }

    return ToDegrees(Math::mat4::EulerXYZRadFromRotation(RotationToUp(n)));
}

std::optional<Math::float3> Auto(const std::string& geometryPath)
{
    const std::shared_ptr<const Geometry> geometry = LoadGeometry(geometryPath, WriteStamp(geometryPath));
    if (!geometry) { return std::nullopt; }
    return Auto(geometry->positions, geometry->indices);
}

float Lift(const std::vector<Math::float3>& positions, const Math::float3& rotationDeg)
{
    const Math::mat4 r = Math::mat4::RotationFromEulerXYZRad(ToRadians(rotationDeg));
    float lowest = std::numeric_limits<float>::max();
    for (const Math::float3& p : positions)
    {
        lowest = std::min(lowest, p.x * r.m._12 + p.y * r.m._22 + p.z * r.m._32);
    }
    return positions.empty() ? 0.0f : -lowest;
}

float Lift(const std::string& geometryPath, const Math::float3& rotationDeg)
{
    const long long stamp = WriteStamp(geometryPath);
    const auto key = std::make_tuple(geometryPath, stamp, static_cast<int>(std::lround(rotationDeg.x * 100.0f)),
        static_cast<int>(std::lround(rotationDeg.y * 100.0f)), static_cast<int>(std::lround(rotationDeg.z * 100.0f)));
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (const auto it = g_lift.find(key); it != g_lift.end()) { return it->second; }
    }
    const std::shared_ptr<const Geometry> geometry = LoadGeometry(geometryPath, stamp);
    if (!geometry) { return 0.0f; }
    std::vector<Math::float3> lod0;
    lod0.reserve(geometry->indices.size());
    for (const std::uint32_t i : geometry->indices)
    {
        if (i < geometry->positions.size()) { lod0.push_back(geometry->positions[i]); }
    }
    const float lift = Lift(lod0, rotationDeg);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lift[key] = lift;
    return lift;
}

Math::float3 ComposeYaw(const Math::float3& restDeg, float yawDeg)
{
    // Row vectors: the rest turn first, then the yaw about world up.
    const Math::mat4 m = Math::mat4::RotationFromEulerXYZRad(ToRadians(restDeg)) *
        Math::mat4::RotationY(yawDeg * Math::DEG2RAD);
    return ToDegrees(Math::mat4::EulerXYZRadFromRotation(m));
}

bool ApplyToNewObject(nlohmann::json& objectJson, float yawDeg, float groundY)
{
    const auto meshIt = objectJson.find("mesh");
    if (meshIt == objectJson.end() || !meshIt->is_string()) { return false; }
    std::ifstream file(meshIt->get<std::string>());
    if (!file) { return false; }
    const nlohmann::json asset = nlohmann::json::parse(file, nullptr, false, true);
    if (asset.is_discarded() || !asset.is_object()) { return false; }
    const std::optional<Math::float3> rest = Read(asset);
    if (!rest) { return false; }

    const Math::float3 rotation = ComposeYaw(*rest, yawDeg);
    objectJson["rotationDeg"] = nlohmann::json::array({ rotation.x, rotation.y, rotation.z });

    float scale = 1.0f;
    if (const auto scaleIt = objectJson.find("scale"); scaleIt != objectJson.end())
    {
        if (scaleIt->is_number()) { scale = scaleIt->get<float>(); }
        else if (scaleIt->is_array() && scaleIt->size() == 3)
        {
            scale = ((*scaleIt)[0].get<float>() + (*scaleIt)[1].get<float>() + (*scaleIt)[2].get<float>()) / 3.0f;
        }
    }
    const float lift = Lift(asset.value("geometry", std::string()), *rest) * scale;
    nlohmann::json& position = objectJson["position"];
    if (position.is_array() && position.size() == 3)
    {
        position[1] = groundY + lift;
    }
    return true;
}
}

#endif
