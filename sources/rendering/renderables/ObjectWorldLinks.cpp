#include "rendering/renderables/ObjectWorldLinks.h"

#include <array>

#include "core/diagnostics/InvariantFailure.h"

namespace worldlinks
{
namespace
{
// Eight at the most (a byte of mask an object): the water mask, the buoyancy, the physics today.
std::array<ObjectWorldLink*, 8> g_links{};
unsigned g_count = 0;
} // namespace

unsigned Add(ObjectWorldLink& link)
{
    // A module's link registers once, from its own function-local static -- on the game thread, the first time the module
    // links an object.
    if (g_count >= g_links.size())
    {
        // A broken contract: the objects' mask is a byte (AGENTS.md: InvariantFailure, never assert or throw).
        (void)link;
        RendererInvariantFailure("worldlinks::Add: a ninth object world link -- an object's mask holds eight");
    }
    g_links[g_count] = &link;
    return g_count++;
}

void Changed(RenderableObjectBase& object, std::uint8_t links)
{
    for (unsigned bit = 0; links != 0u; ++bit, links >>= 1u)
    {
        if ((links & 1u) != 0u && g_links[bit]) { g_links[bit]->ObjectChanged(object); }
    }
}

void Detached(RenderableObjectBase& object, std::uint8_t links)
{
    for (unsigned bit = 0; links != 0u; ++bit, links >>= 1u)
    {
        if ((links & 1u) != 0u && g_links[bit]) { g_links[bit]->ObjectDetached(object); }
    }
}
} // namespace worldlinks
