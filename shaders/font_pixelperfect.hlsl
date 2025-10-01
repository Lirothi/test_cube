// RootSignature: CONSTANTS(b1,count=8) TABLE(SRV(t0)) TABLE(SAMPLER(s0))

struct VSIn {
    float3 pos : POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
    float2 shadowOffset : TEXCOORD1;
    float4 shadowColor  : COLOR1;
};
struct VSOut {
    float4 pos : SV_Position;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
    float2 shadowOffset : TEXCOORD1;
    float4 shadowColor  : COLOR1;
};

cbuffer TextParams : register(b1)
{
    float2 viewport;
    float2 atlasTexelSize;
    float spread;
    float pxSize;
    float2 _pad0;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    float2 p = float2(i.pos.x + 0.5, i.pos.y + 0.5);
    float2 ndc = float2((p.x / viewport.x) * 2.0 - 1.0,
                         1.0 - (p.y / viewport.y) * 2.0);
    o.pos = float4(ndc, 0.0, 1.0);
    o.col = i.col;
    o.uv = i.uv;
    o.shadowOffset = i.shadowOffset;
    o.shadowColor = i.shadowColor;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSMain(VSOut i) : SV_Target {
    float coverage = saturate(tex0.Sample(samp0, i.uv).r);
    float4 textColor = float4(i.col.rgb, i.col.a * coverage);

    float shadowCoverage = 0.0f;
    float4 shadowColor = float4(i.shadowColor.rgb, 0.0f);
    if (i.shadowColor.a > 0.0f) {
        float2 shadowUv = i.uv - i.shadowOffset * atlasTexelSize;
        shadowCoverage = saturate(tex0.Sample(samp0, shadowUv).r);
        shadowColor.a = saturate(i.shadowColor.a * shadowCoverage);
        
        return lerp(shadowColor, textColor, saturate(coverage * 2.0f));
    }
    else
    {
        return textColor;
    }
}
