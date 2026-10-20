#include "rendering/shadows/ShadowAtlasPacker.h"

#include <algorithm>
#include <numeric>

namespace render
{
    std::uint32_t PackShadowAtlas(const std::vector<ShadowAtlasItem>& items, std::uint32_t pageWidth,
                                  std::uint32_t pageHeight, std::uint32_t maxPages, std::vector<ShadowAtlasSlot>& out)
    {
        out.assign(items.size(), ShadowAtlasSlot{});
        if (items.empty() || pageWidth == 0u || pageHeight == 0u || maxPages == 0u) { return 0u; }

        std::vector<std::uint32_t> order(items.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b)
        {
            const ShadowAtlasItem& ia = items[a];
            const ShadowAtlasItem& ib = items[b];
            if (ia.height != ib.height) { return ia.height > ib.height; }
            if (ia.width != ib.width) { return ia.width > ib.width; }
            return ia.key < ib.key;
        });

        // Next-fit decreasing height: the current shelf (y, height) on the current page, a cursor along it. With the
        // heights sorted, a shelf is opened by its tallest item and every later one fits under it.
        std::uint32_t page = 0, shelfY = 0, shelfHeight = 0, cursorX = 0;
        std::uint32_t pagesUsed = 0;
        for (const std::uint32_t i : order)
        {
            const ShadowAtlasItem& item = items[i];
            if (item.width == 0u || item.height == 0u || item.width > pageWidth || item.height > pageHeight)
            {
                continue; // never fits a page: stays page -1
            }
            if (cursorX + item.width > pageWidth || shelfHeight == 0u)
            {
                // A new shelf under the current one, or a new page when the page is full.
                const std::uint32_t nextY = shelfY + shelfHeight;
                if (shelfHeight != 0u && nextY + item.height <= pageHeight)
                {
                    shelfY = nextY;
                }
                else if (shelfHeight != 0u)
                {
                    ++page;
                    shelfY = 0u;
                }
                shelfHeight = item.height;
                cursorX = 0u;
            }
            if (page >= maxPages) { break; } // everything after this is page -1 as well (sorted: no smaller fit
                                             // could reuse an earlier page under next-fit)
            out[i].page = static_cast<std::int32_t>(page);
            out[i].x = cursorX;
            out[i].y = shelfY;
            cursorX += item.width;
            pagesUsed = page + 1u;
        }
        return pagesUsed;
    }
}
