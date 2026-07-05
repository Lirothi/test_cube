// Rung 2 / Step 20: shared constants + constant buffer for the VSM page-allocation compute
// passes (init / touch / build-free / allocate). Mirrors the vsm:: packing in VirtualShadowMap.h
// and the VsmAllocConstants CPU struct. The page table maps a virtual page -> a packed entry:
//   bit 31     : resident
//   bits 0..15 : physical page index (0..kPoolPageCount-1)
#ifndef VSM_PAGE_ALLOC_COMMON_HLSLI
#define VSM_PAGE_ALLOC_COMMON_HLSLI

static const uint VSM_RESIDENT_BIT = 0x80000000u;
static const uint VSM_PHYS_MASK    = 0x0000FFFFu;
static const uint VSM_INVALID      = 0xFFFFFFFFu; // "no owner" sentinel for a free physical page

// AllocCounters[] slot layout (a small RWStructuredBuffer<uint>). Reset each frame in the touch
// pass; freeCount is built by build-free and consumed (atomic pop) by allocate.
static const uint VSM_CNT_FREE     = 0u; // # free physical pages this frame (free-list length / pop cursor)
static const uint VSM_CNT_NEEDS    = 1u; // # pages newly allocated this frame (needs-render list length)
static const uint VSM_CNT_FAIL     = 2u; // # requested pages that could not allocate (pool full)
static const uint VSM_CNT_RESIDENT = 3u; // # resident pages carried over (build-free survivors); + NEEDS = total resident
static const uint VSM_CNT_COUNT    = 4u; // number of counter slots

cbuffer VsmAllocCB : register(b0)
{
    uint gNumEntries;    // kPageTableEntries (virtual pages across all views×levels)
    uint gNumPages;      // kPoolPageCount (physical pages)
    uint gCurFrame;      // renderer total frame number (low 32 bits) — LRU clock
    uint gLruThreshold;  // free a resident page unrequested for >= this many frames
};

// Is virtual page `page` set in the request bitfield?
bool VsmRequested(RWStructuredBuffer<uint> req, uint page)
{
    return (req[page >> 5u] & (1u << (page & 31u))) != 0u;
}

#endif // VSM_PAGE_ALLOC_COMMON_HLSLI
