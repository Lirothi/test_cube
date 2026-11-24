#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>

#include "rendering/core/ResourceDeclarations.h"

// "Free it once the GPU is done with frame N" -- ONE service (docs/architecture_review_plan.md R16) instead of a retire
// bin per subsystem. What frames in flight may still read -- a resource, a descriptor slot, anything with a release step
// -- is handed here instead of being released at once, and released at a frame boundary once
// Renderer::IsFrameNumberComplete says the GPU has finished the frame current when it was retired, plus kMargin. A frame
// number, never a slot: a slot answers for the frame kFrameCount back until its submission signals (AGENTS.md).
// Retire is any thread's; the boundary and the shutdown drain are the game thread's (Renderer::BeginFrame,
// Renderer::Shutdown). What waits shows as the mem: line's "retire" provider (WaitingBytes, registered by the Renderer).
namespace render::deferred
{
// Frames after the one current at Retire that may still use what was retired: a game-half submit is deferred to the next
// hand-over (Renderer::ExecuteOrDefer) and executes with the next frame's work.
inline constexpr std::uint64_t kMargin = 1;

// `what` names the owner for the logs (a static string). The service holds the last reference until the GPU is done.
void Retire(Microsoft::WRL::ComPtr<ID3D12Resource> resource, const char* what);
// A GpuResource: its declaration (the canonical state registry) is forgotten with it, at the release -- not while a
// frame in flight may still be tracked against it.
void Retire(GpuResource&& resource, const char* what);
// Anything else with a release step (a descriptor slot back to its free list, a pooled object); `bytes` for the mem:
// line (0 when it holds no memory of its own).
void Retire(std::function<void()> release, std::uint64_t bytes, const char* what);

// The frame boundary (Renderer::BeginFrame, the game thread): later Retires are stamped from this frame number, and what
// the GPU has finished is released now. isComplete(ctx, n) answers Renderer::IsFrameNumberComplete(n).
using FrameCompleteFn = bool (*)(const void* ctx, std::uint64_t frameNumber);
void BeginFrame(std::uint64_t frameNumber, FrameCompleteFn isComplete, const void* ctx);
// The GPU idle and the queues stopped (Renderer::Shutdown): everything still waiting goes now, and a Retire from here on
// releases at once.
void Shutdown();
// The caller has idled the GPU (a harness with a device of its own, which runs no Renderer frames): everything waiting
// goes now; later Retires wait for their frames as before.
void DrainIdle();

struct Stats
{
    std::uint64_t waiting = 0;      // entries held now
    std::uint64_t waitingBytes = 0; // their bytes (resources: the allocation's size)
    std::uint64_t released = 0;     // since the process started
};
Stats GetStats();
// The mem: line's "retire" provider (render::MemoryProviderFn's shape; the Renderer registers it -- the service itself
// stays free of the memory report, so a tool that compiles a retiring class needs nothing more).
std::uint64_t WaitingBytes(const void* unused);
std::uint64_t WaitingCount(const void* unused); // its entry count (render::MemoryCountFn's shape)
} // namespace render::deferred
