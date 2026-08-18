#define DEBUG_PREVIEW_CS_RS "CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(UAV(u0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

// The texture inspector's preview, drawn through a shader we control.
//
// WHY THIS EXISTS: ImGui can only display a texture multiplied by a per-vertex tint, and that tint
// is packed to 8 bits (`ImGui::ColorConvertFloat4ToU32` -> `IM_F32_TO_INT8_SAT`), so it saturates at
// 1.0. A "gain" implemented that way is bit-identical to no gain -- which is what shipped first and
// had to be pulled. Anything that needs to BRIGHTEN a preview therefore has to run before ImGui
// sees it, which is this pass: it resamples the chosen target (and mip) into an ordinary RGBA8
// texture that ImGui can then draw with no tint at all.
//
// It is not only for depth. Scene HDR is unreadable in the shadows for the same reason, and any
// future R32/R16F target will be too.
//
// t0: the inspected target. The SRV is built by the inspector, so the channel swizzle and the mip
//     selection are already baked into the view -- this shader sees a plain 2D texture.
// u0: RGBA8 preview

Texture2D SrcTex : register(t0);
RWTexture2D<float4> PreviewOut : register(u0);
SamplerState gSmpLinear : register(s0);

cbuffer PreviewCB : register(b0)
{
    uint2 previewSize;
    // Multiplier applied BEFORE the display transform. This is the control ImGui could not give us.
    float gain;
    // 0 = linear, 1 = monotone pow(1/8) stretch for depth-like data whose useful range hugs 0.
    // Kept MONOTONE on purpose: min/max relationships survive it, so a reduction can still be
    // verified on what the preview shows.
    uint stretch;
    // 1 = draw a checkerboard through the alpha, so a transparent or empty region reads as empty
    // rather than as black.
    uint showAlpha;
    uint pad0;
    uint pad1;
    uint pad2;
};

[numthreads(8, 8, 1)]
[RootSignature(DEBUG_PREVIEW_CS_RS)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= previewSize.x || tid.y >= previewSize.y)
    {
        return;
    }

    const float2 uv = (float2(tid.xy) + 0.5f) / float2(previewSize);
    float4 s = SrcTex.SampleLevel(gSmpLinear, uv, 0);

    float3 c = s.rgb * max(gain, 0.0f);
    if (stretch != 0u)
    {
        c = pow(saturate(c), 1.0f / 8.0f);
    }
    c = saturate(c);

    float alpha = 1.0f;
    if (showAlpha != 0u)
    {
        // Checkerboard behind the alpha, the usual way to tell "black" from "not there".
        const uint2 cell = tid.xy / 8u;
        const float checker = ((cell.x + cell.y) & 1u) ? 0.35f : 0.55f;
        c = lerp(float3(checker, checker, checker), c, saturate(s.a));
        alpha = 1.0f;
    }

    PreviewOut[tid.xy] = float4(c, alpha);
}
