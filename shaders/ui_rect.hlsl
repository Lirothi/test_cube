// RootSignature: CONSTANTS(b0,count=4)
struct VSIn
{
    float2 pos : POSITION;
    float4 col : COLOR0;
    float2 uv : TEXCOORD0;
    float2 shadowParams : TEXCOORD1;
};
struct VSOut
{
    float4 H : SV_POSITION;
    float4 col : COLOR0;
};

// Same root constants as for text rendering: we only need the first two (viewport.xy)
cbuffer UIConsts : register(b0)
{
    float2 uViewport;
    float2 uAtlasTexelSize;
};

static float2 PixelToNDC(float2 p, float2 vp)
{
    // pixels (0..vp) → NDC (-1..1), top-left origin
    float x = (p.x / max(1.0, vp.x)) * 2.0 - 1.0;
    float y = 1.0 - (p.y / max(1.0, vp.y)) * 2.0;
    return float2(x, y);
}

VSOut VSMain(VSIn i)
{
    VSOut o;
    float2 ndc = PixelToNDC(i.pos, uViewport);
    o.H = float4(ndc, 0.0, 1.0);
    o.col = i.col;
    return o;
}

float4 PSMain(VSOut i) : SV_Target
{
    return i.col;
}
