// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3)) TABLE(SAMPLER(s0) SAMPLER(s1))
#pragma pack_matrix(row_major)

// ---------- Named constants ----------
static const float kPi = 3.14159265359;
static const float kInvPi = 1.0 / kPi;
static const float kEpsilon = 1e-5;
static const float kMinRoughness = 0.03; // нижний порог "ширины" лобе
static const float kMinAlpha = kMinRoughness * kMinRoughness;
static const float3 kF0Dielectric = float3(0.04, 0.04, 0.04); // IOR~1.5 для диэлектриков

// ---------- GBuffer inputs ----------
Texture2D GB0 : register(t0); // Albedo.rgb + Metal (a)
Texture2D GB1 : register(t1); // NormalOcta.rg + Rough (b)
Texture2D GB2 : register(t2); // Emissive (в compose)
Texture2D DepthT : register(t3); // R32F (SRV к D32)
SamplerState gSmp : register(s0);
SamplerState gSmpPoint : register(s1);

// ---------- Per-frame camera/light ----------
cbuffer PerFrame : register(b0)
{
    // Направление ЛУЧЕЙ солнца в мире (куда светит). В лобе нужен вектор к источнику => -sunDirWS
    float3 sunDirWS;
    float ambientIntensity; // 0..1
    float3 lightRgb;
    float exposure; // обычно 1..2
    float3 camPosWS;
    float pad_;

    // Матрицы (те же, что были в рендере G-буфера!)
    float4x4 invView; // обратная к view (world ← view)
    float4x4 invProj; // обратная к proj (view ← clip)
}

// ---------- VS fullscreen ----------
struct VSOut
{
    float4 H : SV_POSITION;
    float2 UV : TEXCOORD0;
};
VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    float2 p = float2(vid == 2 ? 3.0 : -1.0, vid == 1 ? 3.0 : -1.0);
    o.H = float4(p, 0, 1);
    o.UV = float2(p.x * 0.5 + 0.5, 1.0 - (p.y * 0.5 + 0.5));
    return o;
}

// ---------- Helpers ----------
float2 SignNotZero(float2 v)
{
    return float2(v.x >= 0.0 ? 1.0 : -1.0,
                  v.y >= 0.0 ? 1.0 : -1.0);
}

float3 DecodeOcta(float2 e)
{
    float2 f = e * 2.0 - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0)
    {
	    n.xy = (1.0 - abs(n.yx)) * SignNotZero(n.xy);
    }
    return normalize(n);
}

float2 UVtoNDC(float2 uv)
{
    return uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
}

// Reconstruct world position from depth+uv (D3D: depth \in [0,1])
float3 ReconstructPosWS(float2 uv, float depth)
{
    const float2 ndc = UVtoNDC(uv);
    //float2 ndc;
    //ndc.x = uv.x * 2 - 1;
    //ndc.y = (1 - uv.y) * 2 - 1;
    float4 clip = float4(ndc, depth, 1.0);
    float4 vpos = mul(clip, invProj); // → view
    vpos.xyz /= max(kEpsilon, vpos.w);
    return mul(float4(vpos.xyz, 1.0), invView).xyz; // → world
}

// GGX + Schlick
float D_GGX(float NdotH, float a)
{
    float a2 = a * a;
    float d = (NdotH * NdotH) * (a2 - 1.0) + 1.0;
    return a2 / max(kEpsilon, kPi * d * d);
}
float G_SchlickGGX(float NdotX, float k)
{
    return NdotX / max(kEpsilon, NdotX * (1.0 - k) + k);
}
float3 F_Schlick(float cosT, float3 F0)
{
    float m = pow(1.0 - cosT, 5.0);
    return F0 + (1.0 - F0) * m;
}

// ---------- PS ----------
float4 PSMain(VSOut i) : SV_Target
{
    // Fetch GBuffer
    const float4 gb0 = GB0.Sample(gSmp, i.UV);
    const float4 gb1 = GB1.Sample(gSmp, i.UV);
    const float3 albedo = gb0.rgb;
    float metal = saturate(gb0.a);
    const float2 nEnc = gb1.rg;
    float rough = saturate(gb1.b);


    // World-space shading
    const float3 N = DecodeOcta(nEnc); // нормаль в world-space (так мы её писали в G-буфер!)
    const float z = DepthT.Sample(gSmpPoint, i.UV).r;
    const float3 P = ReconstructPosWS(i.UV, z);

    // !!! ключ: позицию камеры берём из invView, а не из CPU переменной
    const float3 cameraPosWS = camPosWS;//mul(invView, float4(0, 0, 0, 1)).xyz;
    const float3 V = normalize(cameraPosWS - P);
    const float3 L = normalize(-sunDirWS); // от поверхности к источнику

    const float NdotL = saturate(dot(N, L));
    //float3 N2 = GB2.Sample(gSmp, i.UV).rgb;
	//return float4(N * 0.5 + 0.5, 1.0);

	const float3 ambient = albedo * ambientIntensity;

    if (NdotL <= 0.0)
    {
        return float4(ambient * lightRgb * exposure, 1.0);
        //return float4(0,0,0, 1.0);
    }

    // Microfacet
    const float NdotV = saturate(dot(N, V));

    //return float4(V, 1.0);

    const float3 H = normalize(L + V);
    const float NdotH = saturate(dot(N, H));
    const float VdotH = saturate(dot(V, H));

    const float3 F0 = lerp(kF0Dielectric, albedo, metal);
    const float alpha = max(kMinAlpha, rough * rough);
    const float kVis = (alpha + 1.0) * (alpha + 1.0) * 0.125; // (a+1)^2 / 8

    const float D = D_GGX(NdotH, alpha);
    const float G = G_SchlickGGX(NdotV, kVis) * G_SchlickGGX(NdotL, kVis);
    const float3 F = F_Schlick(VdotH, F0);

    const float3 specBRDF = (D * G * F) / max(kEpsilon, 4.0 * NdotL * NdotV);
    const float3 kd = (1.0 - F) * (1.0 - metal);
    const float3 diffBRDF = kd * albedo * kInvPi;

    const float3 direct = (diffBRDF + specBRDF) * NdotL * lightRgb;
    const float3 color = direct + ambient;

    return float4(color * exposure, 1.0);
}