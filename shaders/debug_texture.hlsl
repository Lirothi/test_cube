// RootSignature: TABLE(SRV(t0)) TABLE(SAMPLER(s0))
Texture2D ShadowAtlas : register(t0);
SamplerState Smp : register(s0);

struct VSOut { float4 H:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut VSMain(uint vid:SV_VertexID)
{
    VSOut o;
    float2 p = float2(vid == 2 ? 3.0 : -1.0, vid == 1 ? 3.0 : -1.0);
    o.H = float4(p, 0, 1);
    o.UV = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5));
    return o;
}
float4 PSMain(VSOut i):SV_Target
{
    float4 d = ShadowAtlas.SampleLevel(Smp, i.UV, 0);
    return float4(d.rrr * 1,1);
    //return float4(d.rg * 20, 0, 1);
}