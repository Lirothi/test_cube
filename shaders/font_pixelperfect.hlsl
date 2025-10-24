// RootSignature: CONSTANTS(b1,count=12) TABLE(SRV(t0)) TABLE(SAMPLER(s0))

struct VSIn {
    float2 pos : POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
    float2 shadowParams : TEXCOORD1; // x = offset scale, y = final alpha
};
struct VSOut {
    float4 pos : SV_Position;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
    float2 shadowParams : TEXCOORD1;
};

cbuffer TextParams : register(b1)
{
    float4 viewportAtlas;    // xy = viewport size, zw = atlas texel size
    float4 shadowOffsetBase; // xy = base shadow offset, zw unused
    float4 shadowColorRgb;   // xyz = shadow color rgb
};

VSOut VSMain(VSIn i) {
    VSOut o;
    float2 viewport = viewportAtlas.xy;
    float2 p = i.pos + 0.5;
    float2 ndc = float2((p.x / viewport.x) * 2.0 - 1.0,
                         1.0 - (p.y / viewport.y) * 2.0);
    o.pos = float4(ndc, 0.0, 1.0);
    o.col = i.col;
    o.uv = i.uv;
    o.shadowParams = i.shadowParams;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSMain(VSOut i) : SV_Target {
    //const float2 atlasTexelSize = viewportAtlas.zw;
    const float2 atlasTexelSize = 1.0f / viewportAtlas.xy;
    float coverage = saturate(tex0.Sample(samp0, i.uv).r);
    float4 textColor = float4(i.col.rgb, i.col.a * coverage);

    float shadowAlpha = i.shadowParams.y;
    if (shadowAlpha > 0.0f)
    {
        float2 baseOffset = shadowOffsetBase.xy * 1.0;
        float2 shadowUv = i.uv - baseOffset * i.shadowParams.x * atlasTexelSize;
        float shadowCoverage = saturate(tex0.Sample(samp0, shadowUv).r);
        float finalAlpha = saturate(shadowAlpha * shadowCoverage);
        float4 shadowColor = float4(shadowColorRgb.xyz, finalAlpha);
        return lerp(shadowColor, textColor, saturate(coverage * 2.0f));
    }
    return textColor;
}
