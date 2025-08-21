//RootSignature: TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3)) TABLE(SAMPLER(s0))
#pragma pack_matrix(row_major)

Texture2D GB0   : register(t0); // Albedo+Metal
Texture2D GB1   : register(t1); // NormalOcta+Rough
Texture2D GB2   : register(t2); // Emissive (не участвует здесь)
Texture2D DepthT: register(t3); // R32F SRV к D32
SamplerState gSmp : register(s0);

struct VSOut { float4 H:SV_POSITION; float2 UV:TEXCOORD0; };

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 pos = float2((vid == 2) ? 3.0 : -1.0, (vid == 1) ? 3.0 : -1.0);
    o.H = float4(pos, 0.0, 1.0);
    o.UV = float2((pos.x*0.5f)+0.5f, 1.0f - ((pos.y*0.5f)+0.5f));
    return o;
}

float3 DecodeOcta(float2 e) {
    float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-n.z);
    n.xy += (n.xy >= 0 ? -t : t);
    return normalize(n);
}

float4 PSMain(VSOut i) : SV_Target {
    float3 albedo = GB0.Sample(gSmp, i.UV).rgb;
    float2 no     = GB1.Sample(gSmp, i.UV).rg;
    float3 N      = DecodeOcta(no);

    // простая лампа «сверху-справа»; потом заменишь на реальные источники
    float3 L = normalize(float3(-0.5, -0.7, -0.5));
    float3 lightColor = float3(1,1,1);
    float exposure = 1.0;

    float  NdotL  = saturate(dot(N, L));
    float3 diffuse = albedo * NdotL;
    return float4(diffuse * lightColor * exposure, 1.0);
}