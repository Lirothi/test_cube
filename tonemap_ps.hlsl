//RootSignature: TABLE(SRV(t0)) TABLE(SAMPLER(s0))
Texture2D HDRColor : register(t0);
SamplerState gSmp  : register(s0);

struct VSOut { float4 H:SV_POSITION; float2 UV:TEXCOORD0; };

VSOut VSMain(uint vid:SV_VertexID){
    VSOut o;
    float2 p=float2(vid==2?3:-1, vid==1?3:-1);
    o.H=float4(p,0,1);
    o.UV=float2(p.x*.5+.5, 1-(p.y*.5+.5));
    return o;
}

float3 TM(float3 x)
{
    const float a=2.51,b=0.03,c=2.43,d=0.59,e=0.14;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

float4 PSMain(VSOut i):SV_Target{
    float3 hdr = HDRColor.Sample(gSmp,i.UV).rgb;
    float  gamma = 2.2;
    //float3 ldr = pow(TM(hdr), 1.0/max(1e-3,gamma));
    float3 ldr = pow(hdr, 1.0 / max(1e-3, gamma));
    return float4(ldr,1);
}