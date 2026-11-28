#include "rendering/renderables/ObjectWorldLinks.h"

#include <array>
#include <atomic>
#include <mutex>

#include "core/diagnostics/InvariantFailure.h"

namespace worldlinks
{
namespace
{
// Eight at the most (a byte of mask an object): the water mask, the buoyancy, the physics today. A module adds its link from
// its own function-local static the first time it links an object -- on whatever thread builds that object (a level load
// builds them on workers, R7.2c): Add is under a lock, and a link is published before its bit can reach any object's mask.
std::array<std::atomic<ObjectWorldLink*>, 8> g_links{};
std::mutex g_addMutex;
unsigned g_count = 0;
} // namespace

unsigned Add(ObjectWorldLink& link)
{
    std::lock_guard<std::mutex> lock(g_addMutex);
    if (g_count >= g_links.size())
    {
        // A broken contract: the objects' mask is a byte (AGENTS.md: InvariantFailure, never assert or throw).
        (void)link;
        RendererInvariantFailure("worldlinks::Add: a ninth object world link -- an object's mask holds eight");
    }
    g_links[g_count].store(&link, std::memory_order_release);
    return g_count++;
}

void Changed(RenderableObjectBase& object, std::uint8_t links)
{
    for (unsigned bit = 0; links != 0u; ++bit, links >>= 1u)
    {
        if ((links & 1u) == 0u) { continue; }
        if (ObjectWorldLink* link = g_links[bit].load(std::memory_order_acquire)) { link->ObjectChanged(object); }
    }
}

void Detached(RenderableObjectBase& object, std::uint8_t links)
{
    for (unsigned bit = 0; links != 0u; ++bit, links >>= 1u)
    {
        if ((links & 1u) == 0u) { continue; }
        if (ObjectWorldLink* link = g_links[bit].load(std::memory_order_acquire)) { link->ObjectDetached(object); }
    }
}
} // namespace worldlinks
