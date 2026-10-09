#define DEBUG_TEX_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), CBV(b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"
Texture2D ShadowAtlas : register(t0);
SamplerState Smp : register(s0);

cbuffer DebugTexCB : register(b0)
{
    // Which mip to show. Only meaningful for a target that HAS mips (the P6C depth pyramid);
    // everything else is single-level and passes 0.
    uint mipLevel;
    // Monotone stretch for depth-like targets. Reversed-Z device depth spends almost its whole
    // useful range within a hair of 0, so shown raw an 8-bit blit is a black rectangle. `pow` is
    // MONOTONE, which is what makes this safe for verification: min() commutes with it, so a
    // min-reduction checked on the stretched image is the same check as on the raw values.
    uint depthStretch;
    // Which channel: 0 R, 1 G, 2 B, 3 A, 4 RGB as colour. ALPHA for the SSR reflection buffer IS the
    // answer: it carries the ray's visibility, so "did this pixel's ray find anything" becomes a
    // value that can be counted headlessly instead of guessed at from the composited frame. B of
    // GBAux is the shading model; RGB of GB1 is the normal.
    uint channel;
    // Linear gain after the channel pick (1 = raw). GBAux.b x5 puts the water's ID 3 at white,
    // terrain at 0.67 and foliage at 0.33.
    float gain;
};

struct VSOut { float4 H:SV_POSITION; float2 UV:TEXCOORD0; };
[RootSignature(DEBUG_TEX_RS)]
VSOut VSMain(uint vid:SV_VertexID)
{
    VSOut o;
    float2 p = float2(vid == 2 ? 3.0 : -1.0, vid == 1 ? 3.0 : -1.0);
    o.H = float4(p, 0, 1);
    o.UV = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5));
    return o;
}
[RootSignature(DEBUG_TEX_RS)]
float4 PSMain(VSOut i):SV_Target
{
    float4 d = ShadowAtlas.SampleLevel(Smp, i.UV, float(mipLevel));
    if (channel == 4u)
    {
        return float4(d.rgb * gain, 1);
    }
    float v = (channel == 3u) ? d.a : (channel == 2u) ? d.b : (channel == 1u) ? d.g : d.r;
    if (depthStretch != 0u)
    {
        v = pow(saturate(v), 1.0f / 8.0f);
    }
    v *= gain;
    return float4(v, v, v, 1);
    //return float4(d.rg * 20, 0, 1);
}
