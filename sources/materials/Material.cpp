#include "materials/Material.h"
#include "core/logging/Log.h"

#include "core/diagnostics/BootProfile.h"
#include "rendering/core/GBufferBindingGuard.h"

#include <d3dcompiler.h>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>

#include <atomic>
#include <functional>
#include <string>
#include <utility>

#include <d3d12shader.h>    // ID3D12ShaderReflection
#include <d3dcompiler.h>    // D3DReflect (DXBC)
#include <dxcapi.h>         // DXC reflection (DXIL)

#pragma comment(lib, "d3dcompiler.lib") // D3DReflect
#pragma comment(lib, "dxcompiler.lib")

#include "core/Helpers.h"
#include "rendering/core/RenderContext.h"
#include "rendering/core/CommandListBindState.h"
#include "core/task/TaskSystem.h"
#include "core/profiling/Profiler.h"
#include "core/profiling/ProfilerScopes.h"

using Microsoft::WRL::ComPtr;

// ----------------------------------------
// Utilities
// ----------------------------------------

static D3D_SHADER_MODEL QueryMaxShaderModel(ID3D12Device* dev)
{
    D3D12_FEATURE_DATA_SHADER_MODEL data = { D3D_SHADER_MODEL_6_7 };
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &data, sizeof(data))))
    {
        // data.HighestShaderModel may be 0x60..0x67 even when the SDK lacks enum names
        return data.HighestShaderModel;
    }
    return D3D_SHADER_MODEL_6_0; // safest fallback
}

static std::wstring BuildProfile(const char* stage4cc, D3D_SHADER_MODEL sm)
{
    // In D3D12 the shader model is encoded as 0xMN (6_0=0x60, 6_7=0x67)
    unsigned v = static_cast<unsigned>(sm);
    unsigned major = (v >> 4) & 0xF;   // 6
    unsigned minor = v & 0xF;          // 0..7
    // Clamp defensively
    if (major < 6) { major = 6; minor = 0; }
    if (minor > 9) { minor = 9; }

    wchar_t buf[16];
    swprintf_s(buf, L"%hs_%u_%u", stage4cc, major, minor);
    return std::wstring(buf);
}

namespace
{
constexpr uint32_t kShaderCacheMagic = 0x43435844u; // "DXCC"
constexpr uint32_t kShaderCacheVersion = 2u;
constexpr uint32_t kMaxCachedDependencies = 512u;
constexpr uint32_t kMaxCachedPathChars = 32768u;
constexpr uint64_t kMaxCachedBlobBytes = 128ull * 1024ull * 1024ull;
constexpr uint64_t kMaxCachedPsoBlobBytes = 64ull * 1024ull * 1024ull;
constexpr uint32_t kPsoCacheMagic = 0x4f535043u; // "CPSO"
constexpr uint32_t kPsoCacheVersion = 1u;
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

std::atomic<uint32_t> g_shaderCacheAttempts{ 0 };
std::atomic<uint32_t> g_shaderCacheHits{ 0 };
std::atomic<uint32_t> g_shaderCacheWrites{ 0 };
std::atomic<uint32_t> g_psoCacheAttempts{ 0 };
std::atomic<uint32_t> g_psoCacheHits{ 0 };
std::atomic<uint32_t> g_psoCacheStores{ 0 };
std::atomic<uint32_t> g_psoCacheRejects{ 0 };
std::atomic<uint64_t> g_psoCacheLoadUs{ 0 };
std::atomic<uint64_t> g_psoCacheCreateUs{ 0 };
std::atomic<uint64_t> g_psoCacheSerializedBytes{ 0 };

struct ShaderDependencyFingerprint
{
    std::wstring path;
    uint64_t size = 0;
    uint64_t hash = kFnvOffset;
};

void CacheHashBytes(uint64_t& hash, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

void CacheHashString(uint64_t& hash, const char* value)
{
    if (value)
    {
        CacheHashBytes(hash, value, std::strlen(value));
    }
    const uint8_t separator = 0xffu;
    CacheHashBytes(hash, &separator, sizeof(separator));
}

void CacheHashWString(uint64_t& hash, const std::wstring& value)
{
    CacheHashBytes(hash, value.data(), value.size() * sizeof(wchar_t));
    const wchar_t separator = static_cast<wchar_t>(0xffffu);
    CacheHashBytes(hash, &separator, sizeof(separator));
}

std::filesystem::path NormalizeShaderPath(const std::filesystem::path& source)
{
    std::error_code ec;
    std::filesystem::path path = std::filesystem::absolute(source, ec);
    if (ec)
    {
        path = source;
    }
    return path.lexically_normal();
}

const std::filesystem::path& ShaderCacheDirectory()
{
    static const std::filesystem::path directory = []
    {
        std::array<wchar_t, 32768> overridePath{};
        const DWORD length = GetEnvironmentVariableW(
            L"TEST_CUBE_SHADER_CACHE_DIR", overridePath.data(),
            static_cast<DWORD>(overridePath.size()));
        if (length > 0 && length < overridePath.size())
        {
            return std::filesystem::path(std::wstring(overridePath.data(), length));
        }
        return std::filesystem::path(L"shader_cache");
    }();
    return directory;
}

bool ShaderCacheEnabled()
{
    static const bool enabled = []
    {
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(
            L"TEST_CUBE_DISABLE_SHADER_CACHE", value, static_cast<DWORD>(std::size(value)));
        return length == 0 || value[0] == L'0';
    }();
    return enabled;
}

bool FingerprintShaderFile(const std::filesystem::path& path, ShaderDependencyFingerprint& out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }

    out = {};
    out.path = NormalizeShaderPath(path).wstring();
    out.hash = kFnvOffset;

    std::array<char, 64 * 1024> buffer{};
    while (file)
    {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0)
        {
            CacheHashBytes(out.hash, buffer.data(), static_cast<size_t>(count));
            out.size += static_cast<uint64_t>(count);
        }
    }
    return file.eof();
}

const ShaderDependencyFingerprint& DxcCompilerFingerprint()
{
    static const ShaderDependencyFingerprint fingerprint = []
    {
        ShaderDependencyFingerprint result;
        const HMODULE module = GetModuleHandleW(L"dxcompiler.dll");
        if (!module)
        {
            return result;
        }
        std::array<wchar_t, 32768> modulePath{};
        const DWORD length = GetModuleFileNameW(
            module, modulePath.data(), static_cast<DWORD>(modulePath.size()));
        if (length == 0 || length >= modulePath.size() ||
            !FingerprintShaderFile(std::wstring(modulePath.data(), length), result))
        {
            return ShaderDependencyFingerprint{};
        }
        return result;
    }();
    return fingerprint;
}

uint64_t BuildShaderCacheIdentity(const std::filesystem::path& shaderPath,
    const char* entry, const char* stage4cc, const std::wstring& target,
    const Material::DefineList& defines)
{
    uint64_t hash = kFnvOffset;
    CacheHashBytes(hash, &kShaderCacheVersion, sizeof(kShaderCacheVersion));
    CacheHashWString(hash, NormalizeShaderPath(shaderPath).wstring());
    CacheHashString(hash, entry);
    CacheHashString(hash, stage4cc);
    CacheHashWString(hash, target);
#ifdef _DEBUG
    CacheHashString(hash, "debug:-Zi,-Qembed_debug,-Od");
#else
    CacheHashString(hash, "release:-O3,-Qstrip_debug");
#endif
    CacheHashString(hash, "row-major;hlsl-2021");
    const ShaderDependencyFingerprint& compiler = DxcCompilerFingerprint();
    CacheHashBytes(hash, &compiler.size, sizeof(compiler.size));
    CacheHashBytes(hash, &compiler.hash, sizeof(compiler.hash));

    // Preserve command-line order: duplicate -D entries are unusual, but their
    // order is still part of the compiler input and therefore part of the key.
    for (const auto& define : defines)
    {
        CacheHashString(hash, define.first.c_str());
        CacheHashString(hash, define.second.c_str());
    }
    return hash;
}

std::filesystem::path ShaderCachePath(uint64_t identity)
{
    wchar_t name[32]{};
    swprintf_s(name, L"%016llx.dxilcache", static_cast<unsigned long long>(identity));
    return ShaderCacheDirectory() / name;
}

template<typename T>
bool ReadCacheValue(std::ifstream& file, T& value)
{
    return static_cast<bool>(file.read(reinterpret_cast<char*>(&value), sizeof(value)));
}

template<typename T>
bool WriteCacheValue(std::ofstream& file, const T& value)
{
    return static_cast<bool>(file.write(reinterpret_cast<const char*>(&value), sizeof(value)));
}

bool TryLoadShaderCache(uint64_t identity, ComPtr<ID3DBlob>& outBlob,
    std::vector<std::wstring>& outIncludes)
{
    if (!ShaderCacheEnabled())
    {
        return false;
    }

    g_shaderCacheAttempts.fetch_add(1, std::memory_order_relaxed);
    std::ifstream file(ShaderCachePath(identity), std::ios::binary);
    if (!file)
    {
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t storedIdentity = 0;
    uint32_t dependencyCount = 0;
    uint64_t blobSize = 0;
    uint64_t storedBlobHash = 0;
    if (!ReadCacheValue(file, magic) || !ReadCacheValue(file, version) ||
        !ReadCacheValue(file, storedIdentity) || !ReadCacheValue(file, dependencyCount) ||
        !ReadCacheValue(file, blobSize) || !ReadCacheValue(file, storedBlobHash) ||
        magic != kShaderCacheMagic || version != kShaderCacheVersion ||
        storedIdentity != identity || dependencyCount == 0 ||
        dependencyCount > kMaxCachedDependencies || blobSize == 0 ||
        blobSize > kMaxCachedBlobBytes)
    {
        return false;
    }

    std::vector<std::wstring> dependencies;
    dependencies.reserve(dependencyCount);
    for (uint32_t i = 0; i < dependencyCount; ++i)
    {
        uint32_t pathChars = 0;
        uint64_t storedSize = 0;
        uint64_t storedHash = 0;
        if (!ReadCacheValue(file, pathChars) || !ReadCacheValue(file, storedSize) ||
            !ReadCacheValue(file, storedHash) || pathChars == 0 ||
            pathChars > kMaxCachedPathChars)
        {
            return false;
        }

        std::wstring path(pathChars, L'\0');
        const size_t pathBytes = static_cast<size_t>(pathChars) * sizeof(wchar_t);
        if (!file.read(reinterpret_cast<char*>(path.data()), static_cast<std::streamsize>(pathBytes)))
        {
            return false;
        }

        ShaderDependencyFingerprint current;
        if (!FingerprintShaderFile(path, current) || current.size != storedSize ||
            current.hash != storedHash)
        {
            return false;
        }
        dependencies.push_back(std::move(path));
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(blobSize));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    {
        return false;
    }
    uint64_t blobHash = kFnvOffset;
    CacheHashBytes(blobHash, bytes.data(), bytes.size());
    if (blobHash != storedBlobHash)
    {
        return false;
    }

    ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob(bytes.size(), &blob)))
    {
        return false;
    }
    std::memcpy(blob->GetBufferPointer(), bytes.data(), bytes.size());
    outBlob = std::move(blob);
    outIncludes = std::move(dependencies);
    g_shaderCacheHits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void StoreShaderCache(uint64_t identity, const std::filesystem::path& shaderPath,
    ID3DBlob* blob, const std::vector<std::wstring>& includes)
{
    if (!ShaderCacheEnabled() || !blob || blob->GetBufferSize() == 0)
    {
        return;
    }

    std::vector<std::filesystem::path> dependencyPaths;
    dependencyPaths.reserve(includes.size() + 1);
    dependencyPaths.push_back(NormalizeShaderPath(shaderPath));
    for (const std::wstring& include : includes)
    {
        dependencyPaths.push_back(NormalizeShaderPath(include));
    }
    std::sort(dependencyPaths.begin(), dependencyPaths.end(), [](const auto& a, const auto& b)
    {
        return a.native() < b.native();
    });
    dependencyPaths.erase(std::unique(dependencyPaths.begin(), dependencyPaths.end()),
                          dependencyPaths.end());
    if (dependencyPaths.empty() || dependencyPaths.size() > kMaxCachedDependencies)
    {
        return;
    }

    std::vector<ShaderDependencyFingerprint> dependencies;
    dependencies.reserve(dependencyPaths.size());
    for (const std::filesystem::path& path : dependencyPaths)
    {
        ShaderDependencyFingerprint dependency;
        if (!FingerprintShaderFile(path, dependency) ||
            dependency.path.size() > kMaxCachedPathChars)
        {
            return;
        }
        dependencies.push_back(std::move(dependency));
    }

    const uint64_t blobSize = static_cast<uint64_t>(blob->GetBufferSize());
    if (blobSize > kMaxCachedBlobBytes)
    {
        return;
    }
    uint64_t blobHash = kFnvOffset;
    CacheHashBytes(blobHash, blob->GetBufferPointer(), static_cast<size_t>(blobSize));

    const std::filesystem::path finalPath = ShaderCachePath(identity);
    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec)
    {
        return;
    }

    std::filesystem::path tempPath = finalPath;
    tempPath += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetCurrentThreadId());
    std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
    const uint32_t dependencyCount = static_cast<uint32_t>(dependencies.size());
    if (!file || !WriteCacheValue(file, kShaderCacheMagic) ||
        !WriteCacheValue(file, kShaderCacheVersion) || !WriteCacheValue(file, identity) ||
        !WriteCacheValue(file, dependencyCount) || !WriteCacheValue(file, blobSize) ||
        !WriteCacheValue(file, blobHash))
    {
        file.close();
        std::filesystem::remove(tempPath, ec);
        return;
    }

    for (const ShaderDependencyFingerprint& dependency : dependencies)
    {
        const uint32_t pathChars = static_cast<uint32_t>(dependency.path.size());
        const size_t pathBytes = static_cast<size_t>(pathChars) * sizeof(wchar_t);
        if (!WriteCacheValue(file, pathChars) || !WriteCacheValue(file, dependency.size) ||
            !WriteCacheValue(file, dependency.hash) ||
            !file.write(reinterpret_cast<const char*>(dependency.path.data()),
                        static_cast<std::streamsize>(pathBytes)))
        {
            file.close();
            std::filesystem::remove(tempPath, ec);
            return;
        }
    }

    if (!file.write(static_cast<const char*>(blob->GetBufferPointer()),
                    static_cast<std::streamsize>(blobSize)))
    {
        file.close();
        std::filesystem::remove(tempPath, ec);
        return;
    }
    file.close();
    if (!file)
    {
        std::filesystem::remove(tempPath, ec);
        return;
    }

    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(tempPath, ec);
        return;
    }
    g_shaderCacheWrites.fetch_add(1, std::memory_order_relaxed);
}

bool PipelineCacheEnabled()
{
    static const bool enabled = []
    {
        if (!ShaderCacheEnabled())
        {
            return false;
        }
        wchar_t value[8]{};
        const DWORD length = GetEnvironmentVariableW(
            L"TEST_CUBE_DISABLE_PSO_CACHE", value, static_cast<DWORD>(std::size(value)));
        return length == 0 || value[0] == L'0';
    }();
    return enabled;
}

std::filesystem::path PipelineCachePath(const std::wstring& name)
{
    return ShaderCacheDirectory() / L"pso" / (name + L".bin");
}

uint64_t ElapsedMicroseconds(std::chrono::steady_clock::time_point begin)
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin).count());
}

class PipelineKeyBuilder
{
public:
    PipelineKeyBuilder()
    {
        CacheHashString(first_, "test_cube:pso-key-v1");
        CacheHashString(second_, "test_cube:pso-key-v1");
    }

    template<typename T>
    void Add(const T& value)
    {
        AddBytes(&value, sizeof(value));
    }

    void AddBytes(const void* data, size_t size)
    {
        CacheHashBytes(first_, data, size);
        CacheHashBytes(second_, data, size);
    }

    void AddString(const char* value)
    {
        CacheHashString(first_, value);
        CacheHashString(second_, value);
    }

    void AddShader(const D3D12_SHADER_BYTECODE& shader)
    {
        const uint64_t size = static_cast<uint64_t>(shader.BytecodeLength);
        Add(size);
        if (shader.pShaderBytecode && shader.BytecodeLength > 0)
        {
            AddBytes(shader.pShaderBytecode, shader.BytecodeLength);
        }
    }

    std::wstring Finish(wchar_t kind) const
    {
        wchar_t name[48]{};
        swprintf_s(name, L"%c-%016llx-%016llx", kind,
            static_cast<unsigned long long>(first_),
            static_cast<unsigned long long>(second_));
        return name;
    }

private:
    uint64_t first_ = kFnvOffset;
    uint64_t second_ = kFnvOffset ^ 0x9e3779b97f4a7c15ull;
};

void HashInputLayout(PipelineKeyBuilder& hash, const D3D12_INPUT_LAYOUT_DESC& layout)
{
    hash.Add(layout.NumElements);
    if (layout.NumElements > 0 && !layout.pInputElementDescs)
    {
        return;
    }
    for (UINT i = 0; i < layout.NumElements; ++i)
    {
        const D3D12_INPUT_ELEMENT_DESC& element = layout.pInputElementDescs[i];
        hash.AddString(element.SemanticName);
        hash.Add(element.SemanticIndex);
        hash.Add(element.Format);
        hash.Add(element.InputSlot);
        hash.Add(element.AlignedByteOffset);
        hash.Add(element.InputSlotClass);
        hash.Add(element.InstanceDataStepRate);
    }
}

void HashStreamOutput(PipelineKeyBuilder& hash, const D3D12_STREAM_OUTPUT_DESC& output)
{
    hash.Add(output.NumEntries);
    if (output.NumEntries > 0 && !output.pSODeclaration)
    {
        return;
    }
    for (UINT i = 0; i < output.NumEntries; ++i)
    {
        const D3D12_SO_DECLARATION_ENTRY& entry = output.pSODeclaration[i];
        hash.Add(entry.Stream);
        hash.AddString(entry.SemanticName);
        hash.Add(entry.SemanticIndex);
        hash.Add(entry.StartComponent);
        hash.Add(entry.ComponentCount);
        hash.Add(entry.OutputSlot);
    }
    hash.Add(output.NumStrides);
    if (output.NumStrides > 0 && !output.pBufferStrides)
    {
        return;
    }
    for (UINT i = 0; i < output.NumStrides; ++i)
    {
        hash.Add(output.pBufferStrides[i]);
    }
    hash.Add(output.RasterizedStream);
}

void HashBlendState(PipelineKeyBuilder& hash, const D3D12_BLEND_DESC& blend)
{
    hash.Add(blend.AlphaToCoverageEnable);
    hash.Add(blend.IndependentBlendEnable);
    for (const D3D12_RENDER_TARGET_BLEND_DESC& rt : blend.RenderTarget)
    {
        hash.Add(rt.BlendEnable);
        hash.Add(rt.LogicOpEnable);
        hash.Add(rt.SrcBlend);
        hash.Add(rt.DestBlend);
        hash.Add(rt.BlendOp);
        hash.Add(rt.SrcBlendAlpha);
        hash.Add(rt.DestBlendAlpha);
        hash.Add(rt.BlendOpAlpha);
        hash.Add(rt.LogicOp);
        hash.Add(rt.RenderTargetWriteMask);
    }
}

void HashRasterizerState(PipelineKeyBuilder& hash, const D3D12_RASTERIZER_DESC& raster)
{
    hash.Add(raster.FillMode);
    hash.Add(raster.CullMode);
    hash.Add(raster.FrontCounterClockwise);
    hash.Add(raster.DepthBias);
    hash.Add(raster.DepthBiasClamp);
    hash.Add(raster.SlopeScaledDepthBias);
    hash.Add(raster.DepthClipEnable);
    hash.Add(raster.MultisampleEnable);
    hash.Add(raster.AntialiasedLineEnable);
    hash.Add(raster.ForcedSampleCount);
    hash.Add(raster.ConservativeRaster);
}

void HashDepthStencilOp(PipelineKeyBuilder& hash, const D3D12_DEPTH_STENCILOP_DESC& op)
{
    hash.Add(op.StencilFailOp);
    hash.Add(op.StencilDepthFailOp);
    hash.Add(op.StencilPassOp);
    hash.Add(op.StencilFunc);
}

void HashDepthStencilState(PipelineKeyBuilder& hash, const D3D12_DEPTH_STENCIL_DESC& depth)
{
    hash.Add(depth.DepthEnable);
    hash.Add(depth.DepthWriteMask);
    hash.Add(depth.DepthFunc);
    hash.Add(depth.StencilEnable);
    hash.Add(depth.StencilReadMask);
    hash.Add(depth.StencilWriteMask);
    HashDepthStencilOp(hash, depth.FrontFace);
    HashDepthStencilOp(hash, depth.BackFace);
}

std::wstring BuildGraphicsPipelineName(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc)
{
    PipelineKeyBuilder hash;
    hash.AddString("graphics");
    // In Material, the root signature is extracted from the supplied shader bytecode,
    // so hashing all stages also gives the root signature a stable cross-process key.
    hash.AddShader(desc.VS);
    hash.AddShader(desc.PS);
    hash.AddShader(desc.DS);
    hash.AddShader(desc.HS);
    hash.AddShader(desc.GS);
    HashStreamOutput(hash, desc.StreamOutput);
    HashBlendState(hash, desc.BlendState);
    hash.Add(desc.SampleMask);
    HashRasterizerState(hash, desc.RasterizerState);
    HashDepthStencilState(hash, desc.DepthStencilState);
    HashInputLayout(hash, desc.InputLayout);
    hash.Add(desc.IBStripCutValue);
    hash.Add(desc.PrimitiveTopologyType);
    hash.Add(desc.NumRenderTargets);
    for (DXGI_FORMAT format : desc.RTVFormats)
    {
        hash.Add(format);
    }
    hash.Add(desc.DSVFormat);
    hash.Add(desc.SampleDesc.Count);
    hash.Add(desc.SampleDesc.Quality);
    hash.Add(desc.NodeMask);
    hash.Add(desc.Flags);
    return hash.Finish(L'g');
}

std::wstring BuildComputePipelineName(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc)
{
    PipelineKeyBuilder hash;
    hash.AddString("compute");
    hash.AddShader(desc.CS);
    hash.Add(desc.NodeMask);
    hash.Add(desc.Flags);
    return hash.Finish(L'c');
}

// Wide shader path -> narrow, for the boot profile. Shader paths are ASCII here; anything else
// degrades to '?' rather than dragging a locale-dependent conversion into a diagnostic.
std::string NarrowForLog(const std::wstring& w)
{
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w) { out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?'); }
    return out;
}

// Keep PSO creation on the normal device path so Streamline returns its expected
// proxy objects. Loading objects directly from ID3D12PipelineLibrary is unsafe with
// the current Streamline interposer; D3D12_CACHED_PIPELINE_STATE preserves the
// driver-compiled payload while still going through Create*PipelineState.
bool TryLoadPipelineBlob(const std::wstring& name, std::vector<uint8_t>& outBytes)
{
    outBytes.clear();
    std::ifstream file(PipelineCachePath(name), std::ios::binary);
    if (!file)
    {
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t blobSize = 0;
    uint64_t storedHash = 0;
    if (!ReadCacheValue(file, magic) || !ReadCacheValue(file, version) ||
        !ReadCacheValue(file, blobSize) || !ReadCacheValue(file, storedHash) ||
        magic != kPsoCacheMagic || version != kPsoCacheVersion ||
        blobSize == 0 || blobSize > kMaxCachedPsoBlobBytes)
    {
        return false;
    }

    outBytes.resize(static_cast<size_t>(blobSize));
    if (!file.read(reinterpret_cast<char*>(outBytes.data()),
                   static_cast<std::streamsize>(outBytes.size())))
    {
        outBytes.clear();
        return false;
    }
    uint64_t blobHash = kFnvOffset;
    CacheHashBytes(blobHash, outBytes.data(), outBytes.size());
    if (blobHash != storedHash)
    {
        outBytes.clear();
        return false;
    }
    return true;
}

void StorePipelineBlob(const std::wstring& name, ID3D12PipelineState* pipeline)
{
    if (!PipelineCacheEnabled() || !pipeline)
    {
        return;
    }

    ComPtr<ID3DBlob> blob;
    if (FAILED(pipeline->GetCachedBlob(&blob)) || !blob || blob->GetBufferSize() == 0 ||
        blob->GetBufferSize() > kMaxCachedPsoBlobBytes)
    {
        return;
    }

    const uint64_t blobSize = static_cast<uint64_t>(blob->GetBufferSize());
    uint64_t blobHash = kFnvOffset;
    CacheHashBytes(blobHash, blob->GetBufferPointer(), static_cast<size_t>(blobSize));

    const std::filesystem::path finalPath = PipelineCachePath(name);
    std::error_code ec;
    std::filesystem::create_directories(finalPath.parent_path(), ec);
    if (ec)
    {
        return;
    }
    std::filesystem::path tempPath = finalPath;
    tempPath += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetCurrentThreadId());
    std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
    if (!file || !WriteCacheValue(file, kPsoCacheMagic) ||
        !WriteCacheValue(file, kPsoCacheVersion) || !WriteCacheValue(file, blobSize) ||
        !WriteCacheValue(file, blobHash) ||
        !file.write(static_cast<const char*>(blob->GetBufferPointer()),
                    static_cast<std::streamsize>(blobSize)))
    {
        file.close();
        std::filesystem::remove(tempPath, ec);
        return;
    }
    file.close();
    if (!file || !MoveFileExW(tempPath.c_str(), finalPath.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        std::filesystem::remove(tempPath, ec);
        return;
    }
    g_psoCacheStores.fetch_add(1, std::memory_order_relaxed);
    g_psoCacheSerializedBytes.fetch_add(blobSize, std::memory_order_relaxed);
}

// `debugLabel` is the shader file the caller built this from. The cache key is a hash, which is
// the right identity for a file on disk and useless in a profile: under GBV the create cost is
// concentrated on a few pipelines and the only actionable output is WHICH ones.
HRESULT CreateGraphicsPipelineStateCached(ID3D12Device* device,
    const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
    ComPtr<ID3D12PipelineState>& out,
    const std::wstring& debugLabel)
{
    const std::wstring name = BuildGraphicsPipelineName(desc);
    const auto psoBegin = std::chrono::steady_clock::now();
    const std::string label = NarrowForLog(debugLabel.empty() ? name : debugLabel);
    bool fromCache = false;
    if (PipelineCacheEnabled())
    {
        g_psoCacheAttempts.fetch_add(1, std::memory_order_relaxed);
        const auto begin = std::chrono::steady_clock::now();
        std::vector<uint8_t> cachedBytes;
        if (TryLoadPipelineBlob(name, cachedBytes))
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC cachedDesc = desc;
            cachedDesc.CachedPSO = { cachedBytes.data(), cachedBytes.size() };
            out.Reset();
            const HRESULT cachedHr = device->CreateGraphicsPipelineState(
                &cachedDesc, IID_PPV_ARGS(out.ReleaseAndGetAddressOf()));
            g_psoCacheLoadUs.fetch_add(ElapsedMicroseconds(begin), std::memory_order_relaxed);
            if (SUCCEEDED(cachedHr))
            {
                g_psoCacheHits.fetch_add(1, std::memory_order_relaxed);
                g_psoCacheSerializedBytes.fetch_add(
                    static_cast<uint64_t>(cachedBytes.size()), std::memory_order_relaxed);
                boot::AddBucket("PSO from cache blob",
                                ElapsedMicroseconds(psoBegin) / 1000.0, label);
                return cachedHr;
            }
            g_psoCacheRejects.fetch_add(1, std::memory_order_relaxed);
            fromCache = true;
            out.Reset();
        }
        else
        {
            g_psoCacheLoadUs.fetch_add(ElapsedMicroseconds(begin), std::memory_order_relaxed);
        }
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC uncachedDesc = desc;
    uncachedDesc.CachedPSO = {};
    out.Reset();
    const auto begin = std::chrono::steady_clock::now();
    const HRESULT hr = device->CreateGraphicsPipelineState(
        &uncachedDesc, IID_PPV_ARGS(out.ReleaseAndGetAddressOf()));
    g_psoCacheCreateUs.fetch_add(ElapsedMicroseconds(begin), std::memory_order_relaxed);
    if (SUCCEEDED(hr))
    {
        StorePipelineBlob(name, out.Get());
    }
    // Separate bucket from the cache-blob path on purpose: "87 pipelines, 0 from cache" is a
    // different bug from "87 pipelines, all cached, still slow".
    boot::AddBucket(fromCache ? "PSO create (cache REJECTED)" : "PSO create (compile)",
                    ElapsedMicroseconds(psoBegin) / 1000.0, label);
    return hr;
}

HRESULT CreateComputePipelineStateCached(ID3D12Device* device,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc,
    ComPtr<ID3D12PipelineState>& out,
    const std::wstring& debugLabel)
{
    const std::wstring name = BuildComputePipelineName(desc);
    const auto psoBegin = std::chrono::steady_clock::now();
    const std::string label = NarrowForLog(debugLabel.empty() ? name : debugLabel);
    bool fromCache = false;
    if (PipelineCacheEnabled())
    {
        g_psoCacheAttempts.fetch_add(1, std::memory_order_relaxed);
        const auto begin = std::chrono::steady_clock::now();
        std::vector<uint8_t> cachedBytes;
        if (TryLoadPipelineBlob(name, cachedBytes))
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC cachedDesc = desc;
            cachedDesc.CachedPSO = { cachedBytes.data(), cachedBytes.size() };
            out.Reset();
            const HRESULT cachedHr = device->CreateComputePipelineState(
                &cachedDesc, IID_PPV_ARGS(out.ReleaseAndGetAddressOf()));
            g_psoCacheLoadUs.fetch_add(ElapsedMicroseconds(begin), std::memory_order_relaxed);
            if (SUCCEEDED(cachedHr))
            {
                g_psoCacheHits.fetch_add(1, std::memory_order_relaxed);
                g_psoCacheSerializedBytes.fetch_add(
                    static_cast<uint64_t>(cachedBytes.size()), std::memory_order_relaxed);
                boot::AddBucket("PSO from cache blob",
                                ElapsedMicroseconds(psoBegin) / 1000.0, label);
                return cachedHr;
            }
            g_psoCacheRejects.fetch_add(1, std::memory_order_relaxed);
            fromCache = true;
            out.Reset();
        }
        else
        {
            g_psoCacheLoadUs.fetch_add(ElapsedMicroseconds(begin), std::memory_order_relaxed);
        }
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC uncachedDesc = desc;
    uncachedDesc.CachedPSO = {};
    out.Reset();
    const auto begin = std::chrono::steady_clock::now();
    const HRESULT hr = device->CreateComputePipelineState(
        &uncachedDesc, IID_PPV_ARGS(out.ReleaseAndGetAddressOf()));
    g_psoCacheCreateUs.fetch_add(ElapsedMicroseconds(begin), std::memory_order_relaxed);
    if (SUCCEEDED(hr))
    {
        StorePipelineBlob(name, out.Get());
    }
    // Separate bucket from the cache-blob path on purpose: "87 pipelines, 0 from cache" is a
    // different bug from "87 pipelines, all cached, still slow".
    boot::AddBucket(fromCache ? "PSO create (cache REJECTED)" : "PSO create (compile)",
                    ElapsedMicroseconds(psoBegin) / 1000.0, label);
    return hr;
}
} // namespace

// Include handler for DXC that captures the list of files
struct IncludeCaptureDXC : public IDxcIncludeHandler
{
    std::atomic<ULONG> refcnt{ 1 };
    std::filesystem::path baseDir;
    Microsoft::WRL::ComPtr<IDxcUtils> utils;
    std::vector<std::wstring>& outFiles;

    IncludeCaptureDXC(IDxcUtils* u, const std::filesystem::path& base, std::vector<std::wstring>& out)
        : baseDir(base), utils(u), outFiles(out) {
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IDxcIncludeHandler) || riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IDxcIncludeHandler*>(this);
            AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refcnt; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG v = --refcnt;
        if (v == 0) {
            delete this;
        }
        return v;
    }

    // IDxcIncludeHandler
    HRESULT STDMETHODCALLTYPE LoadSource(LPCWSTR pFilename, IDxcBlob** ppIncludeSource) override
    {
        std::filesystem::path req(pFilename ? pFilename : L"");
        std::filesystem::path full = req.is_absolute() ? req : (baseDir / req);
        full = full.lexically_normal();
        outFiles.push_back(full.wstring());

        Microsoft::WRL::ComPtr<IDxcBlobEncoding> enc;
        HRESULT hr = utils->LoadFile(full.c_str(), nullptr, &enc);
        if (FAILED(hr)) return hr;
        *ppIncludeSource = enc.Detach();
        return S_OK;
    }
};

static HRESULT CompileDXC(const std::wstring& file,
    const char* entry,
    const char* stage4cc,   // "vs","ps","cs"
    ID3D12Device* device,
    const Material::DefineList& defines,
    ComPtr<ID3DBlob>& outBlob,
    std::vector<std::wstring>& outIncludes)
{
    outBlob.Reset();
    outIncludes.clear();

    const std::filesystem::path path = NormalizeShaderPath(file);
    const D3D_SHADER_MODEL sm = QueryMaxShaderModel(device);
    const std::wstring target = BuildProfile(stage4cc, sm);
    const uint64_t cacheIdentity = BuildShaderCacheIdentity(
        path, entry, stage4cc, target, defines);
    const auto shaderBegin = std::chrono::steady_clock::now();
    const std::string shaderLabel = NarrowForLog(path.filename().wstring()) + ":" + entry;
    if (TryLoadShaderCache(cacheIdentity, outBlob, outIncludes))
    {
        // DXIL SIZE is in the label because it is the thing GBV's cost tracks: GBV rewrites the
        // bytecode at first use, so the biggest blob is the pass that stalls the first frame.
        // Without this the two halves of that story live in two different logs.
        boot::AddBucket("shader from cache",
                        ElapsedMicroseconds(shaderBegin) / 1000.0,
                        shaderLabel + " (" +
                            std::to_string(outBlob ? outBlob->GetBufferSize() / 1024 : 0) + " KB DXIL)");
        return S_OK;
    }

    // DXC utils/compiler
    ComPtr<IDxcUtils>     utils;
    ComPtr<IDxcCompiler3> compiler;
    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    if (FAILED(hr)) return hr;
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler));
    if (FAILED(hr)) return hr;

    // Load the source file
    ComPtr<IDxcBlobEncoding> src;
    hr = utils->LoadFile(path.c_str(), nullptr, &src);
    if (FAILED(hr)) return hr;

    // Include handler that tracks loaded files
    IncludeCaptureDXC* inc = new IncludeCaptureDXC(utils.Get(), path.parent_path(), outIncludes);

    // Build the argument list safely: keep wchar strings alive until the end of the function
    std::wstring wEntry(entry, entry + std::strlen(entry));
    std::vector<std::wstring> owned; owned.reserve(16 + defines.size());

    // Base arguments
    owned.push_back(L"-E");           // 0
    owned.push_back(wEntry);          // 1
    owned.push_back(L"-T");           // 2
    owned.push_back(target);          // 3
    owned.push_back(L"-Zpr");         // 4 (row-major)
    owned.push_back(L"-HV");          // 5
    owned.push_back(L"2021");         // 6
#ifdef _DEBUG
    owned.push_back(L"-Zi");          // 7
    owned.push_back(L"-Qembed_debug");// 8
    owned.push_back(L"-Od");          // 9
#else
    owned.push_back(L"-O3");
    owned.push_back(L"-Qstrip_debug");
    //owned.push_back(L"-Qstrip_reflect");
#endif

    // Defines
    for (auto& kv : defines) {
        std::wstring w = L"-D";
        w += std::wstring(kv.first.begin(), kv.first.end());
        if (!kv.second.empty()) {
            w += L"=" + std::wstring(kv.second.begin(), kv.second.end());
        }
        owned.push_back(std::move(w));
    }

    // Assemble the LPCWSTR array
    std::vector<LPCWSTR> args;
    args.reserve(owned.size());
    {
        for (auto& s : owned) {
            args.push_back(s.c_str());
        }
    }

    // Compilation
    DxcBuffer buf{}; buf.Ptr = src->GetBufferPointer(); buf.Size = src->GetBufferSize(); buf.Encoding = 0;
    ComPtr<IDxcResult> result;
    hr = compiler->Compile(&buf, args.data(), (UINT)args.size(), inc, IID_PPV_ARGS(&result));
    inc->Release();

    if (FAILED(hr)) return hr;

    // errors
    ComPtr<IDxcBlobUtf8> errs;
    ComPtr<IDxcBlobUtf16> dummyNameErr;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errs), dummyNameErr.ReleaseAndGetAddressOf());
    if (errs && errs->GetStringLength() > 0) {
        // Warnings when the compile succeeded, errors when it did not (the caller then says
        // which stage fell back to D3DCompile).
        HRESULT compileStatus = S_OK;
        result->GetStatus(&compileStatus);
        logging::WriteRawLines(FAILED(compileStatus) ? logging::LogLevel::Error : logging::LogLevel::Warning,
                               logging::LogCategory::Render, errs->GetStringPointer());
    }

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) return status;

    // DXIL
    ComPtr<IDxcBlob> dxil;
    ComPtr<IDxcBlobUtf16> dummyNameObj;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxil), dummyNameObj.ReleaseAndGetAddressOf());
    if (!dxil) {
        return E_FAIL;
    }

    // Convert to ID3DBlob via copy (so the rest of the code stays unchanged)
    ComPtr<ID3DBlob> blob;
    ThrowIfFailed(D3DCreateBlob(dxil->GetBufferSize(), &blob));
    std::memcpy(blob->GetBufferPointer(), dxil->GetBufferPointer(), dxil->GetBufferSize());
    outBlob = blob;
    StoreShaderCache(cacheIdentity, path, outBlob.Get(), outIncludes);
    boot::AddBucket("shader COMPILE (dxc)",
                    ElapsedMicroseconds(shaderBegin) / 1000.0,
                    shaderLabel + " (" +
                        std::to_string(outBlob ? outBlob->GetBufferSize() / 1024 : 0) + " KB DXIL)");

    return S_OK;
}

struct IncludeCapture : public ID3DInclude {
    std::wstring rootDir;
    std::vector<std::wstring>& outFiles;
    std::unordered_map<const void*, std::wstring> dirOfData;

    IncludeCapture(const std::wstring& base, std::vector<std::wstring>& out)
        : rootDir(base), outFiles(out) {
    }

    static std::wstring Widen(const char* s) {
        std::string a = (s ? s : "");
        return std::wstring(a.begin(), a.end());
    }

    HRESULT Open(D3D_INCLUDE_TYPE, LPCSTR pFileName, LPCVOID pParentData,
        LPCVOID* ppData, UINT* pBytes) override
    {
        std::filesystem::path base = rootDir;
        if (pParentData) {
            auto it = dirOfData.find(pParentData);
            if (it != dirOfData.end()) {
                base = it->second;
            }
        }
        std::filesystem::path req = Widen(pFileName);
        std::filesystem::path full = (req.is_absolute() ? req : (base / req)).lexically_normal();

        std::ifstream f(full, std::ios::binary);
        if (!f) { return E_FAIL; }

        std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        char* mem = (char*)::malloc(bytes.size());
        if (!mem) { return E_OUTOFMEMORY; }
        std::memcpy(mem, bytes.data(), bytes.size());

        *ppData = mem;
        *pBytes = (UINT)bytes.size();
        outFiles.push_back(full.wstring());
        dirOfData[mem] = full.parent_path().wstring();
        return S_OK;
    }

    HRESULT Close(LPCVOID pData) override
    {
        dirOfData.erase(pData);
        ::free((void*)pData);
        return S_OK;
    }
};

// ---- macros → D3D_SHADER_MACRO ----
static void BuildMacros(const Material::DefineList& defines,
    std::vector<D3D_SHADER_MACRO>& outMacros,
    std::vector<std::string>& storage) // owns the char* buffers
{
    outMacros.clear();
    storage.clear();
    storage.reserve(defines.size() * 2);

    for (const auto& kv : defines) {
        storage.push_back(kv.first);
        storage.push_back(kv.second);
        D3D_SHADER_MACRO m{};
        m.Name = storage[storage.size() - 2].c_str();
        m.Definition = storage[storage.size() - 1].c_str();
        outMacros.push_back(m);
    }
    outMacros.push_back({ nullptr, nullptr }); // terminator
}

// ===== Compilation =====
HRESULT Material::CompileWithIncludes(const std::wstring& file,
    const char* entry, const char* target, UINT flags,
    const DefineList& defines,
    ComPtr<ID3DBlob>& outBlob,
    std::vector<std::wstring>& outIncludes)
{
    std::filesystem::path p = std::filesystem::path(file).lexically_normal();
    IncludeCapture inc(p.parent_path().wstring(), outIncludes);
    ComPtr<ID3DBlob> errs;

    std::vector<D3D_SHADER_MACRO> macros;
    std::vector<std::string>      macroStorage;
    BuildMacros(defines, macros, macroStorage);

    HRESULT hr = D3DCompileFromFile(p.c_str(), macros.empty() ? nullptr : macros.data(),
        &inc, entry, target, flags, 0, &outBlob, &errs);
    if (FAILED(hr) && errs) {
        logging::WriteRawLines(logging::LogLevel::Error, logging::LogCategory::Render,
                               (const char*)errs->GetBufferPointer());
    }
    return hr;
}

// ===== Watch utilities =====
void Material::RefreshWatchTimes_()
{
    std::lock_guard<std::mutex> lk(watchMtx_);
    watchedTimes_.resize(watchedFiles_.size());
    for (size_t i = 0; i < watchedFiles_.size(); ++i) {
        std::error_code ec{};
        watchedTimes_[i] = std::filesystem::last_write_time(watchedFiles_[i], ec);
    }
}

bool Material::FSProbeAndFlagPending()
{
    CPU_SCOPE(ProfilerScopes::kMaterialFSProbe);
    std::lock_guard<std::mutex> lk(watchMtx_);
    if (watchedFiles_.empty()) { return false; }

    bool changed = false;
    for (size_t i = 0; i < watchedFiles_.size(); ++i) {
        std::error_code ec{};
        auto t = std::filesystem::last_write_time(watchedFiles_[i], ec);
        if (!ec && i < watchedTimes_.size()) {
            if (t != watchedTimes_[i]) {
                changed = true; break;
            }
        }
    }

    if (changed) {
        pendingReload_.store(true, std::memory_order_release);
        return true;
    }
    else {
        return false;
    }
}

// ===== public API =====
void Material::CreateGraphics(Renderer* r, const GraphicsDesc& gd)
{
    isCompute_ = false;
    renderer_ = r;
    cachedGfxDesc_ = gd;

    std::vector<RootParameterInfo> params;
    std::vector<std::wstring> inc;

    if (!BuildGraphicsPSO(r, gd, rootSignature_, pipelineState_, vertexShader_, pixelShader_, params, inc)) {
        LOG_ERROR(logging::LogCategory::Render, "material CreateGraphics failed: {} (vs={} ps={})",
                  gd.shaderFile, gd.vsEntry, gd.psEntry);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(wireframeMtx_);
        pipelineStateWire_.Reset();
    }

    rootParams_ = std::move(params);

    {
        std::lock_guard<std::mutex> lk(watchMtx_);
        watchedFiles_ = std::move(inc);
    }
    RefreshWatchTimes_();
}

void Material::CreateCompute(Renderer* r, const ComputeDesc& cd)
{
    isCompute_ = true;
    renderer_ = r;
    cachedCmpDesc_ = cd;

    ComPtr<ID3D12RootSignature> rs;
    ComPtr<ID3D12PipelineState> pso;
    std::vector<RootParameterInfo> params;
    std::vector<std::wstring> inc;

    if (!BuildComputePSO(r, cd, rs, pso, params, inc)) {
        LOG_ERROR(logging::LogCategory::Render, "material CreateCompute failed: {} (cs={})", cd.shaderFile, cd.csEntry);
        return;
    }

    rootSignature_ = rs;
    pipelineState_ = pso;
    vertexShader_.Reset();
    pixelShader_.Reset();
    {
        std::lock_guard<std::mutex> lock(wireframeMtx_);
        pipelineStateWire_.Reset();
    }
    rootParams_ = std::move(params);

#ifdef _DEBUG
    // What the root-signature reflection ACTUALLY produced: root index -> (type, shader register).
    // Material::Bind keys every binding on that register, and when it disagrees with what the
    // caller filled in, the root argument is simply never set — which surfaces only as a
    // GPU-based-validation "Uninitialized root argument accessed", naming a dispatch and not a
    // cause. One Debug record per shader (was a table in its own file).
    {
        char line[1024];
        int used = std::snprintf(line, sizeof(line), "rootparams %ls:%s:", cd.shaderFile.c_str(), cd.csEntry ? cd.csEntry : "?");
        for (const RootParameterInfo& p : rootParams_) {
            const char* kind = "?";
            switch (p.type) {
            case RootParameterInfo::Constants:    kind = "constants"; break;
            case RootParameterInfo::CBV:          kind = "cbv";       break;
            case RootParameterInfo::TableSRV:     kind = "tableSRV";  break;
            case RootParameterInfo::TableUAV:     kind = "tableUAV";  break;
            case RootParameterInfo::TableSampler: kind = "tableSmp";  break;
            }
            if (used < 0 || used >= static_cast<int>(sizeof(line)) - 1) { break; }
            const int n = std::snprintf(line + used, sizeof(line) - static_cast<size_t>(used), " [%u]%s r%u s%u",
                                        p.rootIndex, kind, p.bindingRegister, p.bindingSpace);
            if (n <= 0) { break; }
            used += n;
        }
        LOG_DEBUG(logging::LogCategory::Render, "{}", line);
    }
#endif

    {
        std::lock_guard<std::mutex> lk(watchMtx_);
        watchedFiles_ = std::move(inc);
    }
    RefreshWatchTimes_();
}

bool Material::HotReloadIfPending(Renderer* r, uint64_t frameNumber, uint64_t keepAliveFrames)
{
    if (!pendingReload_.load(std::memory_order_acquire)) {
        return false;
    }

    ComPtr<ID3D12RootSignature> newRS;
    ComPtr<ID3D12PipelineState> newPSO;
    ComPtr<ID3DBlob> newVS;
    ComPtr<ID3DBlob> newPS;
    std::vector<RootParameterInfo> newParams;
    std::vector<std::wstring> inc;

    bool ok = false;
    if (isCompute_) {
        ok = BuildComputePSO(r, cachedCmpDesc_, newRS, newPSO, newParams, inc);
    }
    else {
        ok = BuildGraphicsPSO(r, cachedGfxDesc_, newRS, newPSO, newVS, newPS, newParams, inc);
    }

    if (!ok) {
        return false; // leave pending=true — try again on the next tick
    }

    {
        std::lock_guard<std::mutex> lock(wireframeMtx_);
        retired_.push_back({ pipelineState_, pipelineStateWire_, rootSignature_, frameNumber });
        pipelineState_ = newPSO;
        pipelineStateWire_.Reset();
        rootSignature_ = newRS;
        vertexShader_ = newVS;
        pixelShader_ = newPS;
    }
    rootParams_ = std::move(newParams);

    {
        std::lock_guard<std::mutex> lk(watchMtx_);
        watchedFiles_ = std::move(inc);
    }
    RefreshWatchTimes_();

    pendingReload_.store(false, std::memory_order_release);
    return true;
}

void Material::CollectRetired(uint64_t frameNumber, uint64_t keepAliveFrames)
{
    auto it = retired_.begin();
    while (it != retired_.end()) {
        if (frameNumber - it->retireFrame > keepAliveFrames) {
            it = retired_.erase(it);
        }
        else {
            ++it;
        }
    }
}

namespace
{
// Called from Material::Bind, i.e. once per DRAW on every path in the engine -- so it must not lock
// and must not allocate. A mutex + std::set here (the first cut) serialised every recording thread
// on a shared lock for a message that is only ever printed a handful of times.
//
// Instead: one atomic word, one bit per (shader, root index) bucket. fetch_or returns the old value,
// so the first thread to set a bit is the only one that reports. A hash collision merely suppresses
// a duplicate report, which for a diagnostic is free.
std::atomic<std::uint64_t> g_unboundReported{ 0 };

void ReportUnboundRootTable(const std::wstring& shaderFile, UINT rootIndex, const char* kind)
{
    std::uint64_t h = static_cast<std::uint64_t>(rootIndex) * 0x9E3779B97F4A7C15ull;
    h ^= std::hash<std::wstring>{}(shaderFile);
    const std::uint64_t bit = 1ull << (h & 63u);
    if (g_unboundReported.fetch_or(bit, std::memory_order_relaxed) & bit) { return; }
    // Draw path: the record is formatted into a stack buffer and pushed without a lock, which
    // is exactly what the old fopen-per-report could not promise.
    LOG_ERROR(logging::LogCategory::Render,
              "UNBOUND {} at root index {} -- shader: {}; the draw that follows reads this parameter without it ever being set",
              kind, rootIndex, shaderFile);
}
} // namespace

bool Material::Bind(ID3D12GraphicsCommandList* cmdList, const RenderContext& ctx, bool wireframe) const
{
    // A failed build (e.g. a shader missing its [RootSignature]) leaves the material
    // with no PSO/root signature. Skip binding rather than feed null state to D3D12.
    // No PSO also means no draw: a null pipeline cannot be bound, so the caller must not draw.
    if (!rootSignature_ || !pipelineState_) { return false; }

    // Step 3: skip redundant state via the per-command-list bind cache (reset at CL
    // acquire). Skipping is safe because the command list retains bound state across
    // draws; a root-signature change invalidates root arguments (handled below).
    bool allBound = true; // cleared when the root signature declares a table the context lacks
    render::CommandListBindState& cache = render::g_clBindState;
    const bool batch = render::g_bindBatchingEnabled;

    // GBV self-test (`--gbv-selftest=N`, Debug only, never armed by default). Commits the exact
    // violation the GBufferBindingGuard exists to prevent, so a GBV mode can be shown to catch it
    // rather than assumed to. Re-setting the root signature is what makes it deterministic: that
    // INVALIDATES every root argument, so the tables skipped below are genuinely unbound at draw
    // time and not merely left over from a previous draw on this command list.
    ID3D12RootSignature* rs = rootSignature_.Get();

    bool selfTestDraw = false;
#ifdef _DEBUG
    if (!isCompute_ && render::g_gbvSelfTestDraws > 0) {
        --render::g_gbvSelfTestDraws;
        selfTestDraw = true;
        cmdList->SetGraphicsRootSignature(rs);
        if (batch) { cache.rs = rs; cache.isCompute = isCompute_; cache.OnRootSignatureChanged(); }
    }
#endif

    if (!selfTestDraw && (!batch || cache.rs != rs || cache.isCompute != isCompute_)) {
        if (isCompute_) { cmdList->SetComputeRootSignature(rs); }
        else { cmdList->SetGraphicsRootSignature(rs); }
        if (batch) { cache.rs = rs; cache.isCompute = isCompute_; cache.OnRootSignatureChanged(); }
    }

    if (wireframe)
    {
        (void)EnsureWireframePipeline_();
    }
    ID3D12PipelineState* pso = (wireframe && pipelineStateWire_) ? pipelineStateWire_.Get() : pipelineState_.Get();
    if (!batch || cache.pso != pso) {
        cmdList->SetPipelineState(pso);
        if (batch) { cache.pso = pso; }
    }

    for (const auto& p : rootParams_) {
        const uint32_t reg = p.bindingRegister;
        if (reg >= RenderContext::kMaxBindings) { continue; } // shape guarded at build time
        // Self-test: leave the DESCRIPTOR TABLES unbound (CBVs and root constants still go, so the
        // draw is well-formed in every other respect and the only complaint can be this one).
        if (selfTestDraw && (p.type == RootParameterInfo::TableSRV ||
                             p.type == RootParameterInfo::TableSampler ||
                             p.type == RootParameterInfo::TableUAV)) {
            continue;
        }
        switch (p.type) {
        case RootParameterInfo::Constants:
            // Root constants are tiny and rarely repeated across draws; always set.
            if (reg < RenderContext::kMaxConstantsBindings && !ctx.constants[reg].empty()) {
                auto& vals = ctx.constants[reg];
                if (isCompute_) { cmdList->SetComputeRoot32BitConstants(p.rootIndex, (UINT)vals.size(), vals.data(), 0); }
                else { cmdList->SetGraphicsRoot32BitConstants(p.rootIndex, (UINT)vals.size(), vals.data(), 0); }
            }
            break;
        case RootParameterInfo::CBV:
            if (ctx.cbv[reg] != 0 && (!batch || cache.cbv[reg] != ctx.cbv[reg])) {
                if (isCompute_) { cmdList->SetComputeRootConstantBufferView(p.rootIndex, ctx.cbv[reg]); }
                else { cmdList->SetGraphicsRootConstantBufferView(p.rootIndex, ctx.cbv[reg]); }
                if (batch) { cache.cbv[reg] = ctx.cbv[reg]; }
            }
            break;
        case RootParameterInfo::TableSRV:
            // A root signature that DECLARES an SRV table and a context that has none means the
            // draw about to be issued leaves that root parameter UNBOUND, and the shader will
            // sample it anyway. GPU-based validation calls it "Uninitialized root argument
            // accessed"; in the wild it stops the graphics queue mid-batch with a healthy device
            // and a silent TDR. Reported HERE because every draw path converges on Bind -- a guard
            // at a call site can only cover the path it is in.
            if (ctx.srvTable[reg].ptr == 0) {
                ReportUnboundRootTable(isCompute_ ? cachedCmpDesc_.shaderFile : cachedGfxDesc_.shaderFile, p.rootIndex, "SRV table");
                allBound = false;
            }
            if (ctx.srvTable[reg].ptr != 0 && (!batch || cache.srvTable[reg].ptr != ctx.srvTable[reg].ptr)) {
                if (isCompute_) { cmdList->SetComputeRootDescriptorTable(p.rootIndex, ctx.srvTable[reg]); }
                else { cmdList->SetGraphicsRootDescriptorTable(p.rootIndex, ctx.srvTable[reg]); }
                if (batch) { cache.srvTable[reg] = ctx.srvTable[reg]; }
            }
            break;
        case RootParameterInfo::TableUAV:
            if (ctx.uavTable[reg].ptr != 0 && (!batch || cache.uavTable[reg].ptr != ctx.uavTable[reg].ptr)) {
                if (isCompute_) { cmdList->SetComputeRootDescriptorTable(p.rootIndex, ctx.uavTable[reg]); }
                else { cmdList->SetGraphicsRootDescriptorTable(p.rootIndex, ctx.uavTable[reg]); }
                if (batch) { cache.uavTable[reg] = ctx.uavTable[reg]; }
            }
            break;
        case RootParameterInfo::TableSampler:
            if (ctx.samplerTable[reg].ptr == 0) {
                ReportUnboundRootTable(isCompute_ ? cachedCmpDesc_.shaderFile : cachedGfxDesc_.shaderFile, p.rootIndex, "sampler table");
                allBound = false;
            }
            if (ctx.samplerTable[reg].ptr != 0 && (!batch || cache.samplerTable[reg].ptr != ctx.samplerTable[reg].ptr)) {
                if (isCompute_) { cmdList->SetComputeRootDescriptorTable(p.rootIndex, ctx.samplerTable[reg]); }
                else { cmdList->SetGraphicsRootDescriptorTable(p.rootIndex, ctx.samplerTable[reg]); }
                if (batch) { cache.samplerTable[reg] = ctx.samplerTable[reg]; }
            }
            break;
        }
    }
    return allBound;
}

// ===== Root signature from compiler-embedded blob ([RootSignature] attribute) =====
// The RS is authored in HLSL via [RootSignature(...)] and validated by the compiler
// against actual register usage. We pull the serialized RS out of the compiled shader
// container and rebuild rootParams_ from it (same bindingRegister/rootIndex mapping
// Material::Bind relies on). Every shader MUST carry an embedded RS — there is no
// legacy comment-parser fallback (it was removed); a missing RS fails material build.

// Pull the serialized root-signature blob out of a compiled shader container.
// Returns null if the shader carries no embedded RS.
static ComPtr<ID3DBlob> ExtractEmbeddedRootSig(ID3DBlob* shaderBlob)
{
    if (!shaderBlob || shaderBlob->GetBufferSize() == 0) { return nullptr; }

    // DXBC-style part table (FXC output, and DXC containers share the header).
    ComPtr<ID3DBlob> rs;
    if (SUCCEEDED(D3DGetBlobPart(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
            D3D_BLOB_ROOT_SIGNATURE, 0, &rs)) && rs && rs->GetBufferSize() > 0) {
        return rs;
    }

    // DXIL container fallback via the DXC container reflection (RTS0 part).
    ComPtr<IDxcContainerReflection> refl;
    ComPtr<IDxcUtils> utils;
    if (SUCCEEDED(DxcCreateInstance(CLSID_DxcContainerReflection, IID_PPV_ARGS(&refl))) &&
        SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
        ComPtr<IDxcBlobEncoding> enc;
        if (SUCCEEDED(utils->CreateBlob(shaderBlob->GetBufferPointer(),
                (UINT32)shaderBlob->GetBufferSize(), DXC_CP_ACP, &enc)) &&
            SUCCEEDED(refl->Load(enc.Get()))) {
            UINT32 partIdx = 0;
            if (SUCCEEDED(refl->FindFirstPartKind(DXC_PART_ROOT_SIGNATURE, &partIdx))) {
                ComPtr<IDxcBlob> part;
                if (SUCCEEDED(refl->GetPartContent(partIdx, &part)) && part && part->GetBufferSize() > 0) {
                    ComPtr<ID3DBlob> out;
                    if (SUCCEEDED(D3DCreateBlob(part->GetBufferSize(), &out))) {
                        std::memcpy(out->GetBufferPointer(), part->GetBufferPointer(), part->GetBufferSize());
                        return out;
                    }
                }
            }
        }
    }
    return nullptr;
}

static void BuildRootParamsFromDesc(const D3D12_ROOT_SIGNATURE_DESC1& d,
    std::vector<Material::RootParameterInfo>& outParams)
{
    outParams.clear();
    outParams.reserve(d.NumParameters);
    // Every binding is keyed by SHADER REGISTER (matching RenderContext): a root CBV
    // by its b#, a descriptor table by its first range's base register. This is
    // invariant to root-parameter declaration order and to dropped/unused tables.
    for (UINT i = 0; i < d.NumParameters; ++i) {
        const D3D12_ROOT_PARAMETER1& p = d.pParameters[i];
        Material::RootParameterInfo info{};
        info.rootIndex = i;
        switch (p.ParameterType) {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS:
            info.type = Material::RootParameterInfo::Constants;
            info.constantsCount = p.Constants.Num32BitValues;
            info.bindingRegister = p.Constants.ShaderRegister;
            info.bindingSpace = p.Constants.RegisterSpace;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
            info.type = Material::RootParameterInfo::CBV;
            info.bindingRegister = p.Descriptor.ShaderRegister;
            info.bindingSpace = p.Descriptor.RegisterSpace;
            break;
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV:
            // Root SRV/UAV descriptors are not used by any shader and have no
            // RenderContext slot. Use a descriptor table instead.
            assert(false && "root SRV/UAV descriptors are unsupported; use a descriptor table");
            continue;
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: {
            if (p.DescriptorTable.NumDescriptorRanges == 0) { continue; }
            const D3D12_DESCRIPTOR_RANGE1& first = p.DescriptorTable.pDescriptorRanges[0];
            switch (first.RangeType) {
            case D3D12_DESCRIPTOR_RANGE_TYPE_SRV:     info.type = Material::RootParameterInfo::TableSRV;     break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_UAV:     info.type = Material::RootParameterInfo::TableUAV;     break;
            case D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER: info.type = Material::RootParameterInfo::TableSampler; break;
            default:
                // CBV descriptor tables aren't used (CBVs bind as root descriptors).
                assert(false && "CBV descriptor tables are unsupported; use a root CBV");
                continue;
            }
            info.bindingRegister = first.BaseShaderRegister;
            info.bindingSpace = first.RegisterSpace;
            // RenderContext arrays are fixed-size (kMaxBindings); Bind skips an
            // out-of-range register, so catch that shape in debug instead of
            // failing silently at draw time.
            assert(info.bindingRegister < RenderContext::kMaxBindings &&
                   "descriptor table base register exceeds RenderContext bindings");
            break;
        }
        default:
            continue;
        }
        outParams.push_back(info);
    }
}

// Creates the RS object + rebuilds rootParams_ from an embedded serialized RS blob.
// Returns false if the blob is absent/undeserializable (caller treats this as a
// material-build failure).
static bool BuildRootFromEmbedded(ID3D12Device* device, ID3DBlob* rsBlob,
    ComPtr<ID3D12RootSignature>& outRS,
    std::vector<Material::RootParameterInfo>& outParams)
{
    if (!device || !rsBlob) { return false; }
    if (FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
            IID_PPV_ARGS(&outRS)))) {
        return false;
    }
    ComPtr<ID3D12VersionedRootSignatureDeserializer> deser;
    if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(rsBlob->GetBufferPointer(),
            rsBlob->GetBufferSize(), IID_PPV_ARGS(&deser)))) {
        return false;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    if (FAILED(deser->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc)) || !desc) {
        return false;
    }
    BuildRootParamsFromDesc(desc->Desc_1_1, outParams);
    return true;
}

// ===== Shared builder: Graphics =====
bool Material::BuildGraphicsPSO(Renderer* r, const GraphicsDesc& gd,
    ComPtr<ID3D12RootSignature>& outRS,
    ComPtr<ID3D12PipelineState>& outPSO,
    ComPtr<ID3DBlob>& outVS,
    ComPtr<ID3DBlob>& outPS,
    std::vector<RootParameterInfo>& outParams,
    std::vector<std::wstring>& outIncludes)
{
    ComPtr<ID3DBlob> vs, ps;
    std::vector<std::wstring> incVS, incPS;

    // SM6+ via DXC
    if (FAILED(CompileDXC(gd.shaderFile, gd.vsEntry, "vs", r->GetDevice(), gd.defines, vs, incVS))) {
        LOG_WARNING(logging::LogCategory::Render, "DXC VS compile failed for {} ({}); falling back to D3DCompile SM5",
                    gd.shaderFile, gd.vsEntry);
        UINT cf =
#ifdef _DEBUG
        (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
            D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        if (FAILED(CompileWithIncludes(gd.shaderFile, gd.vsEntry, "vs_5_0", cf, gd.defines, vs, incVS))) {
            return false;
        }
    }
    if (FAILED(CompileDXC(gd.shaderFile, gd.psEntry, "ps", r->GetDevice(), gd.defines, ps, incPS))) {
        LOG_WARNING(logging::LogCategory::Render, "DXC PS compile failed for {} ({}); falling back to D3DCompile SM5",
                    gd.shaderFile, gd.psEntry);
        UINT cf =
#ifdef _DEBUG
        (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
            D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        if (FAILED(CompileWithIncludes(gd.shaderFile, gd.psEntry, "ps_5_0", cf, gd.defines, ps, incPS))) {
            return false;
        }
    }

    // Reflection (works for DXIL and DXBC)
    cbInfos_.clear();
    ReflectShaderBlob(vs.Get(), cbInfos_);
    ReflectShaderBlob(ps.Get(), cbInfos_);

    // Root signature comes from the shader's compiler-validated [RootSignature].
    // CONVENTION: graphics shaders tag [RootSignature] on BOTH VSMain and PSMain
    // (so the compiler validates each stage's register usage against the same RS),
    // but the serialized blob is taken from the VS only. A graphics shader that tags
    // only PS (or has no embedded RS) therefore fails the build below.
    ComPtr<ID3DBlob> embeddedRS = ExtractEmbeddedRootSig(vs.Get());
    if (!embeddedRS || !BuildRootFromEmbedded(r->GetDevice(), embeddedRS.Get(), outRS, outParams)) {
        LOG_ERROR(logging::LogCategory::Render, "missing/invalid [RootSignature] in {} (vs={})", gd.shaderFile, gd.vsEntry);
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = outRS.Get();

    if (gd.inputLayoutKey.empty()) {
        pso.InputLayout = { nullptr, 0 };
    }
    else {
        auto il = r->GetInputLayoutManager()->Get(gd.inputLayoutKey);
        pso.InputLayout = { il.desc, il.count };
    }

    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.RasterizerState = gd.raster;
    pso.BlendState = gd.blend;
    pso.DepthStencilState = gd.depth;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = gd.topologyType;
    pso.NumRenderTargets = gd.numRT;
    for (UINT i = 0; i < gd.numRT; ++i) {
        pso.RTVFormats[i] = gd.rtvFormats[i];
    }
    pso.DSVFormat = gd.dsvFormat;
    pso.SampleDesc.Count = gd.sampleCount;

    if (FAILED(CreateGraphicsPipelineStateCached(r->GetDevice(), pso, outPSO, gd.shaderFile))) {
        return false;
    }
    outPSO->SetName(gd.shaderFile.c_str()); // so GBV/PIX name the shader, not 'Unnamed'

    outVS = vs;
    outPS = ps;

    outIncludes.clear();
    outIncludes.push_back(gd.shaderFile);
    outIncludes.insert(outIncludes.end(), incVS.begin(), incVS.end());
    outIncludes.insert(outIncludes.end(), incPS.begin(), incPS.end());
    std::sort(outIncludes.begin(), outIncludes.end());
    outIncludes.erase(std::unique(outIncludes.begin(), outIncludes.end()), outIncludes.end());

    return true;
}

bool Material::EnsureWireframePipeline_() const
{
    if (isCompute_ || cachedGfxDesc_.topologyType != D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(wireframeMtx_);
    if (pipelineStateWire_)
    {
        return true;
    }
    if (!renderer_ || !renderer_->GetDevice() || !rootSignature_ ||
        !vertexShader_ || !pixelShader_)
    {
        return false;
    }

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = rootSignature_.Get();
    if (!cachedGfxDesc_.inputLayoutKey.empty())
    {
        const auto inputLayout = renderer_->GetInputLayoutManager()->Get(cachedGfxDesc_.inputLayoutKey);
        pso.InputLayout = { inputLayout.desc, inputLayout.count };
    }
    pso.VS = { vertexShader_->GetBufferPointer(), vertexShader_->GetBufferSize() };
    pso.PS = { pixelShader_->GetBufferPointer(), pixelShader_->GetBufferSize() };
    pso.RasterizerState = cachedGfxDesc_.raster;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    pso.BlendState = cachedGfxDesc_.blend;
    pso.DepthStencilState = cachedGfxDesc_.depth;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = cachedGfxDesc_.topologyType;
    pso.NumRenderTargets = cachedGfxDesc_.numRT;
    for (UINT i = 0; i < cachedGfxDesc_.numRT; ++i)
    {
        pso.RTVFormats[i] = cachedGfxDesc_.rtvFormats[i];
    }
    pso.DSVFormat = cachedGfxDesc_.dsvFormat;
    pso.SampleDesc.Count = cachedGfxDesc_.sampleCount;

    ComPtr<ID3D12PipelineState> wireframe;
    if (FAILED(CreateGraphicsPipelineStateCached(renderer_->GetDevice(), pso, wireframe,
                                                 cachedGfxDesc_.shaderFile + L" (wireframe)")))
    {
        return false;
    }
    wireframe->SetName((cachedGfxDesc_.shaderFile + L":wire").c_str());
    pipelineStateWire_ = std::move(wireframe);
    return true;
}

// ===== Shared builder: Compute =====
bool Material::BuildComputePSO(Renderer* r, const ComputeDesc& cd,
    ComPtr<ID3D12RootSignature>& outRS,
    ComPtr<ID3D12PipelineState>& outPSO,
    std::vector<RootParameterInfo>& outParams,
    std::vector<std::wstring>& outIncludes)
{
    ComPtr<ID3DBlob> cs;
    std::vector<std::wstring> incCS;
    if (FAILED(CompileDXC(cd.shaderFile, cd.csEntry, "cs", r->GetDevice(), cd.defines, cs, incCS))) {
        LOG_WARNING(logging::LogCategory::Render, "DXC CS compile failed for {} ({}); falling back to D3DCompile SM5",
                    cd.shaderFile, cd.csEntry);
        UINT cf =
#ifdef _DEBUG
        (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION);
#else
            D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        if (FAILED(CompileWithIncludes(cd.shaderFile, cd.csEntry, "cs_5_0", cf, cd.defines, cs, incCS))) {
            return false;
        }
    }

    cbInfos_.clear();
    ReflectShaderBlob(cs.Get(), cbInfos_);

    // Root signature comes from the shader's compiler-validated [RootSignature]
    // attribute (extracted from the CS blob, deserialized into rootParams_).
    ComPtr<ID3DBlob> embeddedRS = ExtractEmbeddedRootSig(cs.Get());
    if (!embeddedRS || !BuildRootFromEmbedded(r->GetDevice(), embeddedRS.Get(), outRS, outParams)) {
        LOG_ERROR(logging::LogCategory::Render, "missing/invalid [RootSignature] in {} (cs={})", cd.shaderFile, cd.csEntry);
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = outRS.Get();
    pso.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(CreateComputePipelineStateCached(r->GetDevice(), pso, outPSO, cd.shaderFile))) {
        return false;
    }
    // Name the PSO after the shader that made it. Every GPU-based-validation message carries
    // "Pipeline State: 0x...:'<name>'", and until now that read 'Unnamed ID3D12PipelineState
    // Object' — so a GBV report named a dispatch INDEX and never a shader, which is most of the
    // work in reading one. Costs a SetName per material build.
    {
        std::wstring name = cd.shaderFile;
        name.append(L":");
        for (const char* p = cd.csEntry ? cd.csEntry : ""; *p; ++p) {
            name.push_back(static_cast<wchar_t>(*p)); // entry names are ASCII
        }
        outPSO->SetName(name.c_str());
    }

    outIncludes = incCS;
    outIncludes.push_back(cd.shaderFile);
    std::sort(outIncludes.begin(), outIncludes.end());
    outIncludes.erase(std::unique(outIncludes.begin(), outIncludes.end()), outIncludes.end());

    return true;
}

// ====== Manager ======
static std::wstring JoinDefines(const Material::DefineList& defs) {
    std::vector<std::string> s;
    s.reserve(defs.size());
    for (auto& kv : defs) {
        s.push_back(kv.first + "=" + kv.second);
    }
    std::sort(s.begin(), s.end());
    std::wstring out;
    for (auto& e : s) {
        out += std::wstring(e.begin(), e.end());
        out += L";";
    }
    return out;
}

static inline void HashMem(uint64_t& h, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 1099511628211ull; }
}
static inline void HashU32(uint64_t& h, uint32_t v) { HashMem(h, &v, sizeof(v)); }
static inline void HashU64(uint64_t& h, uint64_t v) { HashMem(h, &v, sizeof(v)); }
static inline void HashStrA(uint64_t& h, const char* s)
{
    if (!s) { return; }
    const size_t n = std::strlen(s);
    if (n > 0) { HashMem(h, s, n); }
}
static inline void HashStrW(uint64_t& h, const std::wstring& s)
{
    if (s.empty()) { return; }
    HashMem(h, s.data(), s.size() * sizeof(wchar_t));
}

template<typename T>
static inline void HashStruct(uint64_t& h, const T& s) { HashMem(h, &s, sizeof(T)); }

std::wstring MaterialManager::BuildKey(const Material::GraphicsDesc& gd)
{
    // Stable key that depends on every meaningful part of the PSO state.
    uint64_t H = 1469598103934665603ull; // FNV-1a offset basis

    // File and entry points
    HashStrW(H, gd.shaderFile);
    HashStrA(H, gd.vsEntry);
    HashStrA(H, gd.psEntry);

    // Defines (sorted for determinism)
    auto defs = gd.defines;
    std::sort(defs.begin(), defs.end(), [](const auto& a, const auto& b) {
        if (a.first == b.first) { return a.second < b.second; }
        return a.first < b.first;
        });
    for (const auto& kv : defs) {
        HashStrA(H, kv.first.c_str());
        HashStrA(H, kv.second.c_str());
    }

    // IA / topology / RS flags
    HashStrA(H, gd.inputLayoutKey.c_str());
    HashU32(H, (uint32_t)gd.topologyType);
    HashU32(H, (uint32_t)gd.rsFlags);

    // RT/DS
    HashU32(H, (uint32_t)gd.numRT);
    for (UINT i = 0; i < gd.numRT; ++i) { HashU32(H, (uint32_t)gd.rtvFormats[i]); }
    HashU32(H, (uint32_t)gd.dsvFormat);
    HashU32(H, (uint32_t)gd.sampleCount);

    // Full Raster/Blend/DepthStencil — copy the structs as-is
    // (ZeroMemory in FillDefaultsTriangle ensures deterministic padding)
    HashStruct(H, gd.raster);
    HashStruct(H, gd.blend);
    HashStruct(H, gd.depth);

    // Produce a readable wstring key: prefix + hex hash
    wchar_t hex[32] = {};
    swprintf_s(hex, L"%016llX", (unsigned long long)H);

    // Add some human-readable context at the start — file and entry points.
    std::wstring key = L"GFX3|";
    key += gd.shaderFile;
    key += L"|";
    key += std::wstring(gd.vsEntry, gd.vsEntry + std::strlen(gd.vsEntry));
    key += L"/";
    key += std::wstring(gd.psEntry, gd.psEntry + std::strlen(gd.psEntry));
    key += L"|";
    key += hex;

    return key;
}

std::wstring MaterialManager::BuildKey(const Material::ComputeDesc& cd)
{
    uint64_t H = 1469598103934665603ull;

    HashStrW(H, cd.shaderFile);
    HashStrA(H, cd.csEntry);
    HashU32(H, (uint32_t)cd.rsFlags);

    auto defs = cd.defines;
    std::sort(defs.begin(), defs.end(), [](const auto& a, const auto& b) {
        if (a.first == b.first) { return a.second < b.second; }
        return a.first < b.first;
        });
    for (const auto& kv : defs) {
        HashStrA(H, kv.first.c_str());
        HashStrA(H, kv.second.c_str());
    }

    wchar_t hex[32] = {};
    swprintf_s(hex, L"%016llX", (unsigned long long)H);

    std::wstring key = L"CMP3|";
    key += cd.shaderFile;
    key += L"|";
    key += std::wstring(cd.csEntry, cd.csEntry + std::strlen(cd.csEntry));
    key += L"|";
    key += hex;

    return key;
}

std::shared_ptr<Material> MaterialManager::GetOrCreateGraphics(Renderer* r, const Material::GraphicsDesc& gd)
{
    std::wstring key = BuildKey(gd);
    auto it = materials_.find(key);
    if (it != materials_.end()) { return it->second; }
    auto m = std::make_shared<Material>();
    m->CreateGraphics(r, gd);
    materials_[key] = m;
    return m;
}

std::shared_ptr<Material> MaterialManager::GetOrCreateCompute(Renderer* r, const Material::ComputeDesc& cd)
{
    std::wstring key = BuildKey(cd);
    auto it = materials_.find(key);
    if (it != materials_.end()) { return it->second; }
    auto m = std::make_shared<Material>();
    m->CreateCompute(r, cd);
    materials_[key] = m;
    return m;
}

bool MaterialManager::RequestFSProbeAsync()
{
    bool expected = false;
    if (!fsProbeInFlight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }

    auto requestJob = [this]() {
        for (auto& kv : materials_) {
            auto& mat = kv.second;
            if (mat) {
                (void)mat->FSProbeAndFlagPending();
            }
        }
        fsProbeInFlight_.store(false, std::memory_order_release);
    };

#if TASKSYSTEM_ENABLE_PARALLEL_EXECUTION
    TaskSystem::Get().SubmitDetach(std::move(requestJob));
#else
    requestJob();
#endif

    return true;
}

void MaterialManager::Clear()
{
    const uint32_t attempts = g_shaderCacheAttempts.load(std::memory_order_relaxed);
    const uint32_t hits = g_shaderCacheHits.load(std::memory_order_relaxed);
    const uint32_t writes = g_shaderCacheWrites.load(std::memory_order_relaxed);
    char line[192]{};
    std::snprintf(line, sizeof(line),
        "[shadercache] %u hits, %u misses, %u writes\n",
        hits, attempts >= hits ? attempts - hits : 0u, writes);
    logging::WriteRaw(logging::LogLevel::Info, logging::LogCategory::Render, line);

    const uint32_t psoAttempts = g_psoCacheAttempts.load(std::memory_order_relaxed);
    const uint32_t psoHits = g_psoCacheHits.load(std::memory_order_relaxed);
    const uint32_t psoStores = g_psoCacheStores.load(std::memory_order_relaxed);
    const uint32_t psoRejects = g_psoCacheRejects.load(std::memory_order_relaxed);
    const uint64_t psoLoadUs = g_psoCacheLoadUs.load(std::memory_order_relaxed);
    const uint64_t psoCreateUs = g_psoCacheCreateUs.load(std::memory_order_relaxed);
    const uint64_t psoBytes = g_psoCacheSerializedBytes.load(std::memory_order_relaxed);
    char psoLine[256]{};
    std::snprintf(psoLine, sizeof(psoLine),
        "[psocache] %u hits, %u misses, %u stores, %u rejects, "
        "load %.3f ms, create %.3f ms, blob I/O %llu bytes\n",
        psoHits, psoAttempts >= psoHits ? psoAttempts - psoHits : 0u,
        psoStores, psoRejects, static_cast<double>(psoLoadUs) / 1000.0,
        static_cast<double>(psoCreateUs) / 1000.0,
        static_cast<unsigned long long>(psoBytes));
    logging::WriteRaw(logging::LogLevel::Info, logging::LogCategory::Render, psoLine);
    materials_.clear();
}

bool MaterialManager::ApplyPendingHotReloads(Renderer* r, uint64_t frameNumber, uint64_t keepAliveFrames)
{
    bool anyReloaded = false;
    for (auto& kv : materials_) {
        auto& mat = kv.second;
        if (mat) {
            if (mat->HotReloadIfPending(r, frameNumber, keepAliveFrames)) {
                anyReloaded = true;
                // Logging hook: material was rebuilt
            }
            mat->CollectRetired(frameNumber, keepAliveFrames);
        }
    }
    return anyReloaded;
}

const Material::CBufferInfo* Material::GetCBInfo(UINT bRegister) const {
    auto it = cbInfos_.find(bRegister);
    return (it == cbInfos_.end()) ? nullptr : &it->second;
}

bool Material::GetCBFieldInfo(UINT bRegister, const std::string& name, CBufferField& out) const
{
    const CBufferInfo* cb = GetCBInfo(bRegister);
    if (!cb) { return false; }
    auto it = cb->fieldsByName.find(name);
    if (it == cb->fieldsByName.end()) { return false; }
    out = it->second;
    // Safety: backfill stride for old shaders if it was not provided
    if (out.elementStride == 0) {
        out.elementStride = (out.size > 0 ? out.size : 16);
        out.elementCount = 1;
    }
    return true;
}

bool Material::GetCBFieldOffset(UINT bRegister, const std::string& name, UINT& outOffset, UINT& outSize) const {
    auto* cb = GetCBInfo(bRegister);
    if (!cb)
    {
	    return false;
    }
    auto it = cb->fieldsByName.find(name);
    if (it == cb->fieldsByName.end())
    {
	    return false;
    }
    outOffset = it->second.offset;
    outSize = it->second.size;
    return true;
}

Material::CBFieldHandle Material::ComputeCBFieldHandle(UINT bRegister, const std::string& name) const
{
    CBFieldHandle handle{};
    const CBufferInfo* cbInfo = GetCBInfo(bRegister);
    if (!cbInfo) {
        handle.destCBSizeBytes = 0;
        assert(false && "Bad constant buffer register!");
        return handle;
    }

    handle.destCBSizeBytes = cbInfo->sizeBytes;
    auto it = cbInfo->fieldsByName.find(name);
    if (it == cbInfo->fieldsByName.end()) {
        handle.destCBSizeBytes = 0;
        return handle;
    }

    CBufferField* field = const_cast<CBufferField*>(&it->second);
    if (field->elementStride == 0) {
        field->elementStride = (field->size > 0 ? field->size : 16);
        field->elementCount = 1;
    }

    handle.field = field;
    return handle;
}

void Material::ProcessReflection(ID3D12ShaderReflection* refl,
    robin_hood::unordered_map<UINT, CBufferInfo>& io)
{
    if (!refl) { return; }

    D3D12_SHADER_DESC sd{};
    if (FAILED(refl->GetDesc(&sd))) { return; }

    std::unordered_map<std::string, UINT> bindOfCB;
    for (UINT r = 0; r < sd.BoundResources; ++r) {
        D3D12_SHADER_INPUT_BIND_DESC bd{};
        if (SUCCEEDED(refl->GetResourceBindingDesc(r, &bd))) {
            if (bd.Type == D3D_SIT_CBUFFER) {
                bindOfCB[bd.Name ? bd.Name : ""] = bd.BindPoint; // bN
            }
        }
    }

    for (UINT i = 0; i < sd.ConstantBuffers; ++i) {
        ID3D12ShaderReflectionConstantBuffer* cb = refl->GetConstantBufferByIndex(i);
        D3D12_SHADER_BUFFER_DESC cbd{};
        if (FAILED(cb->GetDesc(&cbd))) { continue; }

        std::string cbName = cbd.Name ? cbd.Name : "";
        auto itBind = bindOfCB.find(cbName);
        if (itBind == bindOfCB.end()) { continue; }

        const UINT bReg = itBind->second;
        auto& dst = io[bReg];
        dst.bindRegister = bReg;
        dst.sizeBytes = std::max<UINT>(dst.sizeBytes, cbd.Size);

        for (UINT v = 0; v < cbd.Variables; ++v) {
            ID3D12ShaderReflectionVariable* var = cb->GetVariableByIndex(v);
            D3D12_SHADER_VARIABLE_DESC vd{};
            if (FAILED(var->GetDesc(&vd))) { continue; }

            CBufferField f{};
            std::string name = vd.Name ? vd.Name : "";
            f.offset = vd.StartOffset;
            f.size = vd.Size;

            // === NEW: stride / length extracted from the type ===
            UINT elements = 1;
            UINT stride = vd.Size;
            if (ID3D12ShaderReflectionType* ty = var->GetType()) {
                D3D12_SHADER_TYPE_DESC td{};
                if (SUCCEEDED(ty->GetDesc(&td))) {
                    elements = (td.Elements > 0 ? td.Elements : 1);
                    if (elements > 0) {
                        stride = vd.Size / elements; // In HLSL the size is already padded to 16 bytes per element
                    }
                }
            }
            f.elementCount = elements;
            f.elementStride = stride;

            dst.fieldsByName[name] = f;
        }
    }
}

void Material::ReflectShaderBlob(ID3DBlob* blob,
    robin_hood::unordered_map<UINT, CBufferInfo>& io)
{
    if (!blob) {
        return;
    }

    // First try DXIL (DXC)
    {
        Microsoft::WRL::ComPtr<IDxcUtils> utils;
        if (SUCCEEDED(DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
            DxcBuffer db{};
            db.Ptr = blob->GetBufferPointer();
            db.Size = blob->GetBufferSize();
            db.Encoding = 0;

            Microsoft::WRL::ComPtr<ID3D12ShaderReflection> refl;
            if (SUCCEEDED(utils->CreateReflection(&db, IID_PPV_ARGS(&refl))) && refl) {
                ProcessReflection(refl.Get(), io);
                return;
            }
        }
    }

    // If it is not DXIL, fall back to the legacy DXBC path
    {
        Microsoft::WRL::ComPtr<ID3D12ShaderReflection> refl;
        if (SUCCEEDED(D3DReflect(blob->GetBufferPointer(),
            blob->GetBufferSize(),
            __uuidof(ID3D12ShaderReflection),
            (void**)refl.GetAddressOf())))
        {
            ProcessReflection(refl.Get(), io);
            return;
        }
    }
}
