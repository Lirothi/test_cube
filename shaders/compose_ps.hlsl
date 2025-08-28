//RootSignature: TABLE(SRV(t0) SRV(t1)) TABLE(SAMPLER(s0))
Texture2D LightTarget : register(t0);
Texture2D GB2         : register(t1);
SamplerState gSmp     : register(s0);

struct VSOut { float4 H:SV_POSITION; float2 UV:TEXCOORD0; };

VSOut VSMain(uint vid:SV_VertexID){
    VSOut o;
    float2 p=float2(vid==2?3:-1, vid==1?3:-1);
    o.H=float4(p,0,1);
    o.UV=float2(p.x*.5+.5, 1-(p.y*.5+.5));
    return o;
}

float4 PSMain(VSOut i):SV_Target{
    float3 lit = LightTarget.Sample(gSmp,i.UV).rgb;
    float3 emi = GB2.Sample(gSmp, i.UV).rgb;
    //if ((lit.r + lit.g + lit.b) <= 0.0)
    //{
    //    lit = float3(0.1f, 0.1f, 0.3f) * 0.6; //hack: smth not black while we dont have sky
    //}
    return float4(lit + emi, 1.0);
}