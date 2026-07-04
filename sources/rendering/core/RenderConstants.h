#pragma once

#include <dxgiformat.h> // DXGI_FORMAT

// Rendering-wide compile-time constants, in the render:: namespace. Single
// source of truth shared across the renderer core (Renderer, SwapchainManager,
// FrameScheduler, RenderTargetManager) and consumers like materials/dispatch.
// This header depends only on the DXGI format enum, so it sits below the
// renderer subsystems in the include graph — any of them can include it freely
// without pulling in Renderer.
//
// Keep entries to values that span multiple subsystems and change rarely — this
// header is included widely, so editing it recompiles a lot.
namespace render {

// Frames in flight = swapchain backbuffer count = number of per-frame resource
// sets (command allocators/lists, descriptor heaps, deferred render targets,
// fence values). Change once (e.g. 2 -> 3 for triple buffering) and every
// per-frame array resizes together.
inline constexpr unsigned kFrameCount = 3;

// D3D12 constant-buffer placement alignment (CBV size/offset must be a multiple).
inline constexpr unsigned kConstantBufferAlignment = 256u;

// Rung 0 GPU-driven shadows: the fixed number of shadow-view slots the per-view cull
// inputs (frustum planes) + indirect buffers are sized for. Layout is
// [4 CSM cascades | kMaxShadowedSpotLights | kMaxShadowedPointLights*6 point cube faces].
// Kept here (below the light subsystem in the include graph) as a plain value; Scene.cpp
// static_asserts it equals the sum of the actual LightManager caps so the two can't drift.
inline constexpr unsigned kMaxShadowViews = 36; // 4 + 8 + 4*6

// --- Swapchain / backbuffer formats ---
inline constexpr DXGI_FORMAT kBackbufferResourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr DXGI_FORMAT kBackbufferFormat         = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;

// --- Window-sized depth (swapchain depth) formats ---
inline constexpr DXGI_FORMAT kDepthBufferResourceFormat = DXGI_FORMAT_D32_FLOAT;
inline constexpr DXGI_FORMAT kDepthBufferViewFormat     = DXGI_FORMAT_D32_FLOAT;

// --- Deferred G-buffer + intermediate target formats ---
inline constexpr DXGI_FORMAT kDeferredDepthFormat    = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
inline constexpr DXGI_FORMAT kDeferredDepthSrvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
inline constexpr DXGI_FORMAT kDeferredStencilSrvFormat = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
inline constexpr DXGI_FORMAT kGBuffer0Format         = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr DXGI_FORMAT kGBuffer1Format         = DXGI_FORMAT_R10G10B10A2_UNORM;
inline constexpr DXGI_FORMAT kGBuffer2Format         = DXGI_FORMAT_R11G11B10_FLOAT;
inline constexpr DXGI_FORMAT kGBufferVelocityFormat  = DXGI_FORMAT_R16G16_FLOAT;
inline constexpr DXGI_FORMAT kObjectIdFormat         = DXGI_FORMAT_R32_UINT;
inline constexpr DXGI_FORMAT kLightTargetFormat      = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr DXGI_FORMAT kSceneColorFormat       = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr DXGI_FORMAT kDlssBiasFormat         = DXGI_FORMAT_R8_UNORM;
inline constexpr DXGI_FORMAT kReflectionFormat              = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr DXGI_FORMAT kReflectionScratchFormat          = DXGI_FORMAT_R8G8B8A8_UNORM;

} // namespace render
