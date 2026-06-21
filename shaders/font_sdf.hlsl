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

// #3 shadow-compositing toggle: 1 = principled text-over-shadow over-composite,
// 0 = legacy lerp(shadow, text, coverage*k). Flip and save to A/B via hot-reload.
#ifndef FONT_SHADOW_COMPOSITE
#define FONT_SHADOW_COMPOSITE 1
#endif

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
        float2 atlasTexelSize = viewportAtlas.zw;
        float2 shadowUv = i.uv - shadowOffsetBase.xy * i.shadowParams.x * atlasTexelSize;
        float shadowD = tex0.Sample(samp0, shadowUv).r;

        float shadowCoverage = saturate(smoothstep(0.5 - w, 0.5 + w, shadowD));

#if FONT_SHADOW_COMPOSITE
        // Suppress the shadow inside the glyph footprint so translucent text
        // does not get tinted by its own shadow.
        float fgA = textColor.a;                                  // i.col.a * coverage
        float shA = saturate(shadowAlpha * shadowCoverage) * (1.0 - coverage);
        float outA = fgA + shA;
        float3 outRGB = (textColor.rgb * fgA + shadowColorRgb.xyz * shA) / max(outA, 1e-5);
        return float4(outRGB, outA);
#else
        float4 shadowColor = float4(shadowColorRgb.xyz, saturate(shadowAlpha * shadowCoverage));
        return lerp(shadowColor, textColor, saturate(coverage * 3.0f));
#endif
    }
    return textColor;
}
