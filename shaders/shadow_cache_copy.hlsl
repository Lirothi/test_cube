// Local-light shadow plan S7: a cached light's static depth, copied from its square of the cache pool into its atlas
// rectangle -- the bound depth target's viewport -- before the movable casters draw on top (UE CopyCachedShadowMap,
// SDCM_MovablePrimitivesOnly). D3D12 copies depth only as whole subresources, and both sides are rectangles inside
// pages, so it is a draw: one triangle over the viewport, SV_Depth from the pool, depth func ALWAYS.
#define SHADOW_CACHE_COPY_RS \
    "CBV(b0), " \
    "DescriptorTable(SRV(t0, numDescriptors=1, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE))"

cbuffer CopyParams : register(b0)
{
    int2 gSrcMinusDst; // pool square corner - atlas rectangle corner, in texels
    uint gPage;        // the pool page
    uint gCopyPad;
};

Texture2DArray<float> Pool : register(t0);

struct VSOut
{
    float4 H : SV_POSITION;
};

[RootSignature(SHADOW_CACHE_COPY_RS)]
VSOut VSMain(uint vid : SV_VertexID)
{
    // (0,0) (2,0) (0,2) in uv -> a triangle covering the whole viewport.
    const float2 uv = float2((vid << 1) & 2u, vid & 2u);
    VSOut o;
    o.H = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

[RootSignature(SHADOW_CACHE_COPY_RS)]
float PSMain(VSOut i) : SV_Depth
{
    // SV_Position is in render-target texels (the viewport offset included): the same texel of the pool square.
    return Pool.Load(int4(int2(i.H.xy) + gSrcMinusDst, gPage, 0));
}
