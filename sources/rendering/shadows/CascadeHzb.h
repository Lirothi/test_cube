#pragma once
// Occlusion plan S5b.1 (docs/occlusion_culling_plan.md): the per-cascade LIGHT-space depth
// pyramids of the Legacy CSM atlas and the matrices the cull tests boxes against.
//
// Transcription target: UE's VSM HZB (VirtualShadowMapArray.cpp:3807-3894 BuildHZBPerPageCS,
// NaniteCullingCommon.ush:463-497 the two-pass rule) moved onto a cascade TILE: one R32 pyramid
// per cascade over the tile's content rect (mip 0 = half the content, the fold rules of
// hzb_build_cs.hlsl), built ONCE per frame right after pass A (Main_CsmHzb) and used twice --
// by the post cull of THIS frame (against this frame's matrices) and by the main cull of the
// NEXT frame (against this frame's matrices, kept here as `prev`). One pyramid per cascade is
// enough: the next frame's cull reads it before that frame's build overwrites it, and a pyramid
// missing pass B's casters is DEEPER than the truth, which only ever keeps a box visible.
//
// Depth convention: the atlas is forward-Z, the library (hzb_cull.hlsli) reverse-Z. Nothing in
// the library is duplicated for the other sign: the pyramid stores 1 - z (HZB_LIGHT permutation
// of hzb_build_cs.hlsl) and the matrices handed to the cull are viewProj * FlipZ, so a box's clip
// z comes out as 1 - z too. Validity: `prevValid[c]` = the pyramid holds frame N-1's tile with
// the matrices in `prev` (built last frame, same content resolution). A level switch, a resize
// or a frame without the build invalidates it; the cull then treats every caster as visible.

#include <array>
#include <cstdint>
#include <memory>

#include <d3d12.h>
#include <wrl/client.h>

#include "core/math/Math.h"
#include "rendering/core/ResourceDeclarations.h"

class Material;
class Renderer;

namespace render
{
class CascadeHzb
{
public:
    static constexpr unsigned kCascades = 4;
    static constexpr unsigned kMaxMips = 13; // 1020 -> 1 is 11 levels; RenderTargetManager's kHzbMaxMips

    // Mirrors CascadeHzbCB (b1) of shadow_cull_cs.hlsl / shadow_cull_post_cs.hlsl.
    struct GpuParams
    {
        Math::mat4 prevViewProj[kCascades]; // last frame's light view-projection, z flipped
        Math::mat4 viewProj[kCascades];     // this frame's, z flipped
        std::uint32_t prevValid[4];
        std::int32_t viewRect[4];           // (0, 0, content, content)
        std::uint32_t hzbSize[2];           // mip 0
        std::uint32_t on;
        std::uint32_t pad0;
    };
    static_assert(sizeof(GpuParams) == 560, "CascadeHzbCB layout");

    // Pyramids for a tile whose content rect is `contentRes` texels square; recreated (and the
    // history invalidated) when it changes. False (sticky) when the device objects failed.
    bool EnsureResources(Renderer* renderer, UINT contentRes);
    bool Ready() const { return !failed_ && buildMat_ && pyramid_[0] && contentRes_ != 0; }

    // Once per frame from Scene, after UpdateCascades: this frame's cascade light view-projection
    // matrices (forward-Z, row vectors) and whether the cull should test this frame at all.
    // Whatever was set last frame becomes `prev`.
    void SetFrameViews(const Math::mat4* lightViewProj, std::uint64_t frameNumber, bool active);
    bool Active() const { return active_; }
    // The pyramid of cascade `c` holds last frame's tile, built with `prev`'s matrices.
    bool PrevValid(unsigned c) const;
    void FillParams(GpuParams& out, bool on) const;

    ID3D12Resource* Pyramid(unsigned c) const { return c < kCascades ? pyramid_[c].Get() : nullptr; }
    D3D12_CPU_DESCRIPTOR_HANDLE Srv(unsigned c) const { return c < kCascades ? srv_[c] : D3D12_CPU_DESCRIPTOR_HANDLE{}; }

    // The Main_CsmHzb body: every cascade's pyramid from its tile of the atlas (atlas in
    // NON_PIXEL, pyramids in UNORDERED_ACCESS -- both the builder's). `tileRes` is the atlas tile
    // edge, `border` the S5 gutter: the content origin of cascade c is ((c % 2) * tileRes +
    // border, (c / 2) * tileRes + border).
    void RecordBuild(Renderer* renderer, ID3D12GraphicsCommandList* cl, D3D12_CPU_DESCRIPTOR_HANDLE atlasSrv,
                     UINT tileRes, UINT border);
    // Committed by the builder that registered the build (cross-frame state lives with the
    // decision, not the record): after this frame the pyramids hold frame `frameNumber`.
    void MarkBuilt(std::uint64_t frameNumber);
    void Invalidate();

    UINT ContentRes() const { return contentRes_; }
    UINT Width() const { return width_; }
    UINT Height() const { return height_; }
    UINT Mips() const { return mips_; }

private:
    void ReleasePyramids();

    std::array<GpuResource, kCascades> pyramid_{};
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_; // non-shader-visible: per cascade 1 SRV + kMaxMips UAVs
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kCascades> srv_{};
    std::array<std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kMaxMips>, kCascades> mipUav_{};
    std::shared_ptr<Material> buildMat_; // hzb_build_cs.hlsl, HZB_LIGHT=1
    bool failed_ = false;
    UINT contentRes_ = 0;
    UINT width_ = 0, height_ = 0, mips_ = 0;

    std::array<Math::mat4, kCascades> viewProjRev_{};
    std::array<Math::mat4, kCascades> prevViewProjRev_{};
    std::array<std::uint64_t, kCascades> builtFrame_{};
    std::uint64_t frame_ = 0;
    bool active_ = false;
    bool haveViews_ = false;
};
} // namespace render
