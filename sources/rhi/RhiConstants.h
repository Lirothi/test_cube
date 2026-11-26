#pragma once

// R4.5a: the constants of the device and the frame, below RenderConstants.h (which includes this): the frame
// scheduler, the swapchain and the bindless heap size their per-frame arrays by kFrameCount without taking the
// render targets' formats and the shadow layout with it.
namespace render {

// Frames in flight = swapchain backbuffer count = number of per-frame resource
// sets (command allocators/lists, descriptor heaps, deferred render targets,
// fence values). Change once (e.g. 2 -> 3 for triple buffering) and every
// per-frame array resizes together.
inline constexpr unsigned kFrameCount = 3;

// D3D12 constant-buffer placement alignment (CBV size/offset must be a multiple).
inline constexpr unsigned kConstantBufferAlignment = 256u;

} // namespace render
