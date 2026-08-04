#pragma once

#include <d3d12.h>

// Barrier plan step 11 — one place that creates a TEXTURE.
//
// Enhanced barriers track a texture's LAYOUT, and a layout can only be declared at creation:
// `CreateCommittedResource3` takes a `D3D12_BARRIER_LAYOUT` where the old call took a
// `D3D12_RESOURCE_STATES`. Mixing the two is exactly what this helper exists to prevent — a
// texture created with an initial STATE and later handed an enhanced barrier has no layout the
// runtime ever agreed to.
//
// GATED: with the enhanced path off — the default, see GraphicsDevice::UseEnhancedBarriers — this
// is byte-for-byte the old `CreateCommittedResource`, same arguments and same result. "The flag
// off changes nothing at all" is this step's acceptance bar.
//
// Buffers are NOT routed through here: they have no layout, so their creation is untouched.
//
// Shaped to take a raw `ID3D12Device*` rather than the GraphicsDevice, so the ~11 creation sites
// (RenderTargetManager, Texture2D, TextureCube, VirtualShadowMap, ocean, reflection history …)
// change by one identifier each instead of threading a new parameter through six signatures. The
// enhanced decision is published once by GraphicsDevice, in the same process-global style the
// file already uses for DRED/GBV/--legacy-barriers.
namespace render {

// Published by GraphicsDevice::InitDevice once the capability and the opt-in are resolved.
void SetEnhancedTextureCreation(bool enable);
bool UsingEnhancedTextureCreation();

// `initialState` is the resource's canonical/resting state — exactly what the old call passed. On
// the enhanced path it is translated to the matching layout by barriers::LegacyStateToBarrier.
// `clearValue` may be null. Returns the HRESULT of whichever API ran.
HRESULT CreateCommittedTexture(ID3D12Device* device,
                               const D3D12_HEAP_PROPERTIES& heapProps,
                               D3D12_HEAP_FLAGS heapFlags,
                               const D3D12_RESOURCE_DESC& desc,
                               D3D12_RESOURCE_STATES initialState,
                               const D3D12_CLEAR_VALUE* clearValue,
                               ID3D12Resource** out);

} // namespace render
