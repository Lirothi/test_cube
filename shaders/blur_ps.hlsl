// RootSignature: CBV(b0) TABLE(SRV(t0)) TABLE(SAMPLER(s0))
// t0: SSR input (RGB premultiplied, A=visibility)
// s0: LinearClamp
Texture2D SSRIn : register(t0);
SamplerState gSmp : register(s0);

cbuffer BlurCB : register(b0){
    float2 dir;         // (1/width,0) для X, (0,1/height) для Y
    float radius;     // 1..3
    float _pad;
}

struct VSOut{ float4 H:SV_POSITION; float2 UV:TEXCOORD0; };
VSOut VSMain(uint vid:SV_VertexID){
    VSOut o; float2 p=float2(vid==2?3:-1, vid==1?3:-1);
    o.H=float4(p,0,1); o.UV=float2(p.x*0.5+0.5, 1-(p.y*0.5+0.5)); return o;
}

float4 PSMain(VSOut i):SV_Target
{
    // 9-tap Гаусс; премультиплайнем — значит просто усредняем rgba
    const float w[5] = {0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216};
    float2 stepv = dir * radius;

    float4 c = SSRIn.SampleLevel(gSmp, i.UV, 0) * w[0];
    [unroll]
    for(int k=1; k < 5; ++k){
        float2 off = stepv * k;
        c += SSRIn.SampleLevel(gSmp, i.UV + off, 0) * w[k];
        c += SSRIn.SampleLevel(gSmp, i.UV - off, 0) * w[k];
    }
    return c; // rgb & a размыты одинаково (OK для premultiplied)
}