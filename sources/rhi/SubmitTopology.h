#pragma once

#include <cstdint>

// R4.5a: the submission's diagnostics, out of Renderer.h -- the frame scheduler reports with them and must not
// include the renderer.
namespace render {
// The last frame's per-queue submission TOPOLOGY, recorded so a fence stall can print the exact
// cross-queue edge it is parked on. The command-list dump (--dump-submit-order) shows WHAT was
// submitted; this shows the WAIT/SIGNAL pairs between the queues, which is the only thing that
// explains a deadlock where both queues are idle and the driver never calls it a hang.
// Plain globals for the same reason g_crossQueueWaits is one: the reporter lives in FrameScheduler,
// which knows nothing about the render graph.
struct SubmitEdge
{
    unsigned long long frame = 0;       // which frame submitted this segment
    bool     compute = false;           // which queue this segment went to
    unsigned lists = 0;                 // command lists executed
    unsigned long long waitValue = 0;   // cross-queue value waited for (0 = no wait)
    unsigned long long signalValue = 0; // cross-queue value signalled after (0 = none)
};
// A RING across frames, not just the last one: the first UNSIGNALLED cross-queue value usually
// belongs to an EARLIER frame than the one that was submitted last, and a single-frame snapshot
// therefore shows a topology that is perfectly satisfiable while the actual cycle sits off-screen.
inline constexpr unsigned kSubmitEdgeRing = 64;
inline SubmitEdge g_submitEdgeRing[kSubmitEdgeRing];
inline unsigned   g_submitEdgeNext = 0;   // write cursor, wraps
inline unsigned   g_submitEdgeCount = 0;  // saturates at kSubmitEdgeRing

// What `--no-async-compute` (Renderer.h) actually PRODUCED on the GPU last frame: command lists submitted to the
// compute queue, and cross-queue waits the graph asked for. Written by the submit loop, read by the
// developer window. A checkbox for a scheduling decision is worth nothing without the number that
// says the decision reached the queue — with every pass on Graphics both are 0 by construction.
inline std::uint32_t g_asyncComputeLists = 0;
inline std::uint32_t g_crossQueueWaits = 0;
} // namespace render
