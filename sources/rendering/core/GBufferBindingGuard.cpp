#include "rendering/core/GBufferBindingGuard.h"

#include "core/diagnostics/DiagPaths.h"

#include <cstdio>
#include <atomic>
#include <cstdint>

namespace render
{

void ReportMissingGBufferBindings(const void* owner, std::size_t slot, const char* where)
{
    // Same rule as the reporter in Material.cpp: this sits on a draw path, so no lock and no
    // allocation. One atomic word, one bit per (owner, slot) bucket; a collision only suppresses a
    // duplicate line.
    static std::atomic<std::uint64_t> reported{ 0 };
    std::uint64_t h = reinterpret_cast<std::uintptr_t>(owner) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<std::uint64_t>(slot) * 0xC2B2AE3D27D4EB4Full;
    const std::uint64_t bit = 1ull << ((h >> 17) & 63u);
    if (reported.fetch_or(bit, std::memory_order_relaxed) & bit) { return; }
    FILE* f = nullptr;
    if (fopen_s(&f, diag::LogPath("missing_material.log").c_str(), "a") == 0 && f)
    {
        std::fprintf(f,
                     "%s: gbuffer slot %zu (owner %p) has no SRV/sampler table -- submesh SKIPPED\n"
                     "  cause: the mesh has no material, or its material has no textures at all\n"
                     "  drawing it would leave gbuffer.hlsl root parameters 3 and 4 unbound\n",
                     where ? where : "?", slot, owner);
        std::fclose(f);
    }
}

} // namespace render
