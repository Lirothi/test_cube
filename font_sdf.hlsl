// RootSignature: CONSTANTS(b1,count=8) TABLE(SRV(t0)) TABLE(SAMPLER(s0))

struct VSIn { float3 pos:POSITION; float4 col:COLOR; float2 uv:TEXCOORD; };
struct VSOut{ float4 pos:SV_Position; float4 col:COLOR;  float2 uv:TEXCOORD; };

cbuffer PC : register(b1)
{
    float2 viewport; float2 _pad0;
    float spread; float pxSize; float2 _pad1;
};

VSOut VSMain(VSIn i) {
    VSOut o;
    float2 p = float2(i.pos.x + 0.5, i.pos.y + 0.5);
    float2 ndc = float2((p.x / viewport.x) * 2.0 - 1.0,
                         1.0 - (p.y / viewport.y) * 2.0);
    o.pos = float4(ndc, 0.0, 1.0);
    o.col = i.col;
    o.uv = i.uv;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSMain(VSOut i) : SV_Target {
    float d = tex0.Sample(samp0, i.uv).r;

    float w = fwidth(d);
    // минимальная ширина размытия ~ один экранный пиксель по меньшей стороне
    float minw = 1.0 / max(viewport.x, viewport.y);
    w = max(w, minw);

    float alpha = smoothstep(0.5 - w, 0.5 + w, d);
    return float4(i.col.rgb, i.col.a * alpha);
}