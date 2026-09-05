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
inline constexpr unsigned kMaxShadowViews = 46; // 4 cascade + 8 spot + 4*6 point + 10 clipmap (Step 24e: directional clipmap cull views)

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
inline constexpr DXGI_FORMAT kGBufferAuxFormat       = DXGI_FORMAT_R8G8B8A8_UNORM;
inline constexpr DXGI_FORMAT kObjectIdFormat         = DXGI_FORMAT_R32_UINT;
inline constexpr DXGI_FORMAT kLightTargetFormat      = DXGI_FORMAT_R16G16B16A16_FLOAT;
inline constexpr DXGI_FORMAT kSceneColorFormat       = DXGI_FORMAT_R16G16B16A16_FLOAT;

// P16.1 -- PRE-EXPOSURE. ON since P16.2, which is what it was built for: physical light units mean
// a 100,000 lx sun, and an R16G16B16A16_FLOAT target tops out at 65,504. Without the normalisation
// the first bright specular highlight in a physically lit scene is an Inf.
//
// It was off while it was being built, deliberately -- a half-wired rollout scales SOME writers and
// not others, which is a uniform brightness error that reads as a tuning problem and gets blamed on
// the level. It was flipped only after every writer AND every reader of scene colour was wired and
// each measured transparent (glass, particles, the ocean, the SSR history, the metering, the two
// overlays; see P16.1 in the plan for the table). Back off with "--set=render.preExposure:0".
inline bool g_preExposureEnabled = true;
// The factor itself, published once per frame by SceneRenderer. A global for the same reason
// g_windFreeze is one: the writers of scene colour are independent systems (the skybox, the ocean,
// glass, particles) that have no path to the scene renderer, and they must all use the SAME number
// or the frame comes out in pieces at different brightnesses. 1.0 while the gate is off.
inline float g_preExposure = 1.0f;
// P16.10 -- REFLECTIONS CARRY RADIANCE, SO THEY NEED A FLOAT TARGET.
//
// This was R8G8B8A8_UNORM, an LDR format, and it worked only while scene colour happened to sit
// near 1. Once the lights were authored in lux every non-zero reflected pixel CLAMPED TO 1.0 and
// the buffer became a black-and-white MASK -- visible verbatim in the texture inspector, and the
// reason RT and SSR looked identical: they were both being flattened by the same target, not
// agreeing about anything.
//
// What it does downstream is worse than losing the colour. compose adds ONLY THE DIFFERENCE the
// reflection makes, `reflectionRGB - skyCol * alpha`; with the hit clamped to 1 and the sky at
// several thousand that difference is hugely NEGATIVE, so a metal reflecting anything at all went
// BLACK. That is the bronze floor.
//
// RGBA16F, not R11G11B10: the alpha carries the reflection's COVERAGE and the compose blend needs
// it. Same format scene colour uses, so a hit copied from it round-trips exactly.
inline constexpr DXGI_FORMAT kReflectionFormat              = DXGI_FORMAT_R16G16B16A16_FLOAT;
// P6B ambient occlusion. Eight bits per channel: AO is a visibility fraction in [0,1], and 8 bits
// of it sits below the noise floor of any screen-space estimate -- the denoiser is what decides the
// quality here, not the storage.
//
// P16.4: TWO channels, was one. .x is the contact-scale estimate this pass has always produced;
// .y is the same estimate at a medium radius, sized to canopies and building interiors, and it is
// what occludes the SKY FILL. One texture rather than a second chain because every stage of the
// chain (denoise, temporal, upsample) weights its taps on depth and normal alone -- the weights are
// identical for both scales, so a second set of targets would be the same arithmetic done twice
// over the same guide. Every stage of the chain and both consumers move together; the change is
// the format here plus `.r` -> `.rg` in six kernels. No new resource, descriptor or barrier.
inline constexpr DXGI_FORMAT kGtaoFormat                    = DXGI_FORMAT_R8G8_UNORM;
// P6C hierarchical depth. R32_FLOAT, not the fp16 UE uses: the pyramid stores DEVICE Z, whose
// useful precision under reversed-Z sits near 0 for distant geometry, and 16-bit floats have
// their coarsest spacing exactly there. The extra bandwidth is a half-res chain; correctness
// first, and the format is a one-line change if it ever measures as a bottleneck.
inline constexpr DXGI_FORMAT kHzbFormat                     = DXGI_FORMAT_R32_FLOAT;
// Volumetric fog (docs/volumetric_fog_sky_clouds_ssgi_plan.md, part A): the froxel grid is the
// render resolution over kFogGridPixels per axis, kFogGridZ slices distributed by UE's
// r.VolumetricFog.DepthDistributionScale (32, dimensionless), RGBA16F pre-exposed scattering +
// extinction, as UE's. 16 px / 64 slices are UE's defaults; both are compile-time because the
// textures are sized from them (a knob would need a re-create, so there is no knob).
inline constexpr unsigned    kFogGridPixels                 = 16;
inline constexpr unsigned    kFogGridZ                      = 64;
// The LIVE cell size (8 / 16 / 32): a quality knob (--set=fog.gridPixels, graphics_settings.json
// performance/fogGridPixels, the Render tab). Renderer::SetFogGridPixels recreates the deferred
// ring when it changes, the way a DLSS mode change does; kFogGridPixels stays the default.
inline unsigned              g_fogGridPixels                = kFogGridPixels;
inline constexpr float       kFogDepthDistributionScale     = 32.0f;
inline constexpr DXGI_FORMAT kFogFormat                     = DXGI_FORMAT_R16G16B16A16_FLOAT;
// P8 bloom pyramid. HDR and half-float: the chain carries scene-referred radiance ABOVE the
// threshold, which is exactly the range an 8-bit or UNORM format cannot hold. Same format as the
// scene colour it is extracted from, so nothing is quantised on the way in. It is also read through
// its own UAV (see bloom_cs.hlsl), which needs TypedUAVLoadAdditionalFormats -- already relied on by
// the ocean's mip chain, which uses this same format.
inline constexpr DXGI_FORMAT kBloomFormat                   = DXGI_FORMAT_R16G16B16A16_FLOAT;
// P8C convolution bloom: the complex grid the transform runs on. THIRTY-TWO bit, unlike the
// pyramid next door: a 1024-point transform accumulates across ten stages, and half floats lose
// enough there to show as a ring around a bright source. Two complex numbers per texel
// (.xy = R + iG, .zw = B + i0).
inline constexpr DXGI_FORMAT kBloomFftFormat                = DXGI_FORMAT_R32G32B32A32_FLOAT;
// Inspector preview: plain 8-bit colour, because ImGui draws it directly.
inline constexpr DXGI_FORMAT kDebugPreviewFormat            = DXGI_FORMAT_R8G8B8A8_UNORM;
// P16.10: the blur scratch holds the same radiance the reflection does; an LDR scratch would
// clamp it straight back after the target stopped clamping it.
inline constexpr DXGI_FORMAT kReflectionScratchFormat          = DXGI_FORMAT_R16G16B16A16_FLOAT;

} // namespace render
