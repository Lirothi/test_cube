#define FONT_SDF_RS "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), RootConstants(num32BitConstants=12, b0), DescriptorTable(SRV(t0, flags=DESCRIPTORS_VOLATILE | DATA_VOLATILE)), DescriptorTable(Sampler(s0, flags=DESCRIPTORS_VOLATILE))"

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

cbuffer TextParams : register(b0)
{
    float4 viewportAtlas;    // xy = viewport size, zw = atlas texel size
    float4 shadowOffsetBase; // xy = base shadow offset, zw unused
    float4 shadowColorRgb;   // xyz = shadow color rgb
};

[RootSignature(FONT_SDF_RS)]
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

[RootSignature(FONT_SDF_RS)]
float4 PSMain(VSOut i) : SV_Target {
    float d = tex0.Sample(samp0, i.uv).r;

    float w = fwidth(d);
    float minw = 1.0 / max(viewportAtlas.x, viewportAtlas.y);
    w = max(w, minw);

    float textCoverage = smoothstep(0.5 - w, 0.5 + w, d);
    float coverage = saturate(textCoverage);
    float4 textColor = float4(i.col.rgb, i.col.a * coverage);

    float shadowAlpha = i.shadowParams.y;
    if (shadowAlpha > 0.0f) {
        float4 shadowColor = float4(shadowColorRgb.xyz, 0.0f);
        //float2 atlasTexelSize = viewportAtlas.zw;
        float2 atlasTexelSize = 1.0f / viewportAtlas.xy;
        float2 shadowUv = i.uv - shadowOffsetBase.xy * i.shadowParams.x * atlasTexelSize;
        float shadowD = tex0.Sample(samp0, shadowUv).r;

        float shadowW = fwidth(shadowD);
        shadowW = max(shadowW, minw);

        float shadowCoverage = smoothstep(0.5 - shadowW, 0.5 + shadowW, shadowD);
        shadowCoverage = saturate(shadowCoverage);
        shadowColor.a = saturate(shadowAlpha * shadowCoverage);

        return lerp(shadowColor, textColor, saturate(coverage * 3.0f));
    }
    return textColor;
}
