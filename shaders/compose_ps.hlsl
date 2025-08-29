// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3) SRV(t4) SRV(t5) SRV(t6)) TABLE(SAMPLER(s0) SAMPLER(s1))
// t0: LightTarget (HDR)
// t1: GB2 (Emissive)
// t2: GB0 (Albedo+Metal encoded in A)
// t3: GB1 (Normal01 + Rough encoded in A)
// t4: Depth (R32F SRV из DSV)
// t5: Skybox cubemap
// t6: SSR blurred (premultiplied)

#pragma pack_matrix(row_major)

#include "utils.hlsl"

// === Ресурсы ===
Texture2D LightTarget : register(t0);
Texture2D GB2 : register(t1);
Texture2D GB0 : register(t2);
Texture2D GB1 : register(t3);
Texture2D DepthT : register(t4);
TextureCube SkyboxTex : register(t5);
Texture2D SSRBlur : register(t6);

SamplerState gSmp : register(s0); // LinearClamp (цвет)
SamplerState gSmpPoint : register(s1); // PointClamp  (глубина)

// === Параметры SSR ===
cbuffer PerFrame : register(b0)
{
    float4x4 view; // world -> view
    float4x4 proj; // view  -> clip
    float4x4 invView; // view  -> world
    float4x4 invProj; // clip  -> view
    float skyboxIntensity; // 1.0
}

static const float kEps = 1e-6;

struct VSOut
{
    float4 H : SV_POSITION;
    float2 UV : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    float2 p = float2(vid == 2 ? 3 : -1, vid == 1 ? 3 : -1);
    o.H = float4(p, 0, 1);
    o.UV = float2(p.x * 0.5 + 0.5, 1 - (p.y * 0.5 + 0.5));
    return o;
}

float2 UVtoNDC(float2 uv)
{
    return uv * float2(2, -2) + float2(-1, 1);
}

float3 ReconstructPosVS(float2 uv, float depth)
{
    float2 ndc = UVtoNDC(uv);
    float4 clip = float4(ndc, depth, 1.0);
    float4 vpos = mul(clip, invProj); // clip->view
    vpos.xyz /= max(kEps, vpos.w);
    return vpos.xyz;
}

float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0.xxx - F0) * pow(1.0 - cosTheta, 5.0);
}

inline float ReadDepth(float2 uv)
{
    return DepthT.SampleLevel(gSmpPoint, uv, 0).r; // всегда LOD0, без билинеара
}


// === Pixel ===
float4 PSMain(VSOut i) : SV_Target
{
    float3 lit = LightTarget.SampleLevel(gSmp, i.UV, 0).rgb;
    float3 emi = GB2.SampleLevel(gSmp, i.UV, 0).rgb;

	float z = ReadDepth(i.UV);
    if (z < 1.0 - kEps)
    {
        float4 gb0 = GB0.SampleLevel(gSmp, i.UV, 0);
        float4 gb1 = GB1.SampleLevel(gSmp, i.UV, 0);

        float3 albedo = gb0.rgb;
        float2 rm = UnpackRM(gb0.a);
        float rough = saturate(rm.x);
        float metal = saturate(rm.y);

        float3 N_ws = normalize(gb1.rgb * 2.0 - 1.0);
        float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);

        float3 Pv = ReconstructPosVS(i.UV, z);
        float3 Vv = normalize(-Pv);
        float3 Nv = normalize(mul(N_ws, (float3x3) view));
        float3 Rv = normalize(reflect(-Vv, Nv));
        float3 Rw = normalize(mul(Rv, (float3x3) invView));

        float4 ssrT = SSRBlur.SampleLevel(gSmp, i.UV, 0); // premultiplied
        float ssrA = ssrT.a;
        float3 ssrRGB = ssrT.rgb;

        float3 skyCol = SkyboxTex.SampleLevel(gSmp, Rw, 0).rgb * skyboxIntensity;

		// skybox как фоллбек: (ssrColor*α + sky*(1-α))
        float3 refl = ssrRGB + skyCol * (1.0 - ssrA);

        float cosT = saturate(dot(Nv, Vv));
        float3 F = FresnelSchlick(cosT, F0);
        float gloss = saturate(1.0 - rough);

        float3 spec = refl * F * pow(gloss, 2);

        return float4(lit + spec + emi, 1.0);
    }
	else
	{
        return float4(lit + emi, 1.0);
    }
}
