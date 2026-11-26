#pragma once

#include <cstdint>

// R4.5a: the queue enum on its own -- the submission timeline and the barrier declarations need it, the pass list
// (RenderPass.h) is the render level's.
// Async-compute plan step 4 (design D1): the QUEUE a pass runs on.
//
// It is a property of the PASS, fixed when the graph is built — not of the command list, not of the
// material, and never decided at record time. Anything later than graph-build time makes the
// barrier compile undecidable, because that compile runs before any pass body records a thing.
// With AddPass2 the graph-build moment and the declaration moment are the same moment, which is
// what makes this expressible at all.
//
// INERT at step 4: `AsyncCompute` exists and is threaded through, and NOTHING selects it. Step 5
// adds the eligibility rules, step 8 moves the first pass.
enum class RenderQueue : uint8_t {
    Graphics,      // the direct queue — every pass, today
    AsyncCompute,  // the second queue (rhi::Device::ComputeQueue)
};
