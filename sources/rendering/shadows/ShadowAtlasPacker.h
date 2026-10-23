#pragma once

#include <cstdint>
#include <vector>

// Local-light shadow plan S9 (docs/local_light_shadows_plan.md): packing the shadowed local lights into the atlas
// pages, the job UE's FTextureLayout does for its whole-scene shadow atlases (ShadowSetup.cpp
// AllocateAtlasedShadowDepthTargets :5971-6090). Every item is a power-of-two square (a spot) or a 3f x 2f block (a
// point's six cube faces), so a shelf packer with the items sorted by height wastes little: next-fit decreasing
// height, one shelf after another, a page after another. The order is total (height, width, then the caller's key),
// so an unchanged set lands in the same places every frame.
namespace render
{
    struct ShadowAtlasItem
    {
        std::uint32_t width = 0, height = 0;
        std::uint32_t key = 0; // tie-break: the light's index -- stable placement for an unchanged set
    };
    struct ShadowAtlasSlot
    {
        std::int32_t page = -1; // -1: did not fit in maxPages
        std::uint32_t x = 0, y = 0;
    };

    // Places `items` into pages of pageWidth x pageHeight, at most maxPages of them; `out` is parallel to `items`.
    // Returns the number of pages used. `orderScratch`: storage for the sort order, kept by a caller that packs every
    // frame (null = a local vector).
    std::uint32_t PackShadowAtlas(const std::vector<ShadowAtlasItem>& items, std::uint32_t pageWidth,
                                  std::uint32_t pageHeight, std::uint32_t maxPages, std::vector<ShadowAtlasSlot>& out,
                                  std::vector<std::uint32_t>* orderScratch = nullptr);

    // A frame packer's working storage (LightManager keeps one): the vectors keep their capacity from frame to frame.
    struct ShadowPackScratch
    {
        std::vector<ShadowAtlasItem> items;
        std::vector<ShadowAtlasSlot> placed;
        std::vector<std::uint32_t> order;
        std::vector<std::uint32_t> kept;
        std::vector<std::uint8_t> shrunk;
    };
}
