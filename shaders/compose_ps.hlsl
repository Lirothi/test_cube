// RootSignature: CBV(b0) TABLE(SRV(t0) SRV(t1) SRV(t2) SRV(t3) SRV(t4) SRV(t5)) TABLE(SAMPLER(s0) SAMPLER(s1))
// t0: LightTarget (HDR)
// t1: GB2 (Emissive)
// t2: GB0 (Albedo+Metal encoded in A)
// t3: GB1 (Normal01 + Rough encoded in A)
// t4: Depth (R32F SRV из DSV)
// t5: Skybox cubemap

#pragma pack_matrix(row_major)

#include "utils.hlsl"

// === Ресурсы ===
Texture2D LightTarget : register(t0);
Texture2D GB2 : register(t1);
Texture2D GB0 : register(t2);
Texture2D GB1 : register(t3);
Texture2D DepthT : register(t4);
TextureCube SkyboxTex : register(t5);

SamplerState gSmp : register(s0); // LinearClamp (цвет)
SamplerState gSmpPoint : register(s1); // PointClamp  (глубина)

// === Параметры SSR ===
cbuffer PerFrame : register(b0)
{
    float4x4 view; // world -> view
    float4x4 proj; // view  -> clip
    float4x4 invView; // view  -> world
    float4x4 invProj; // clip  -> view
    float depthA;
    float depthB;
    float zNear;
    float zFar;
    float skyboxIntensity; // 1.0
    float2 screenSize;
}

    // Lettier SSR params (см. статью)
static const float ssrMaxDistanceVS = 80.0f; // maxDistance (view units)
static const float ssrResolution = 0.8f; // 0..1 (шаг coarse-pass по экрану)
static const int ssrRefineSteps = 16; // steps (итерации refinement)
static const float ssrThicknessVS = 0.5f; // thickness (view units)
static const float roughCutoff = 0.95f; // отключать SSR, если rough > cutoff (напр. 0.95)
static const float ssrEdgeFadePx = 32.0f; // ширина плавного затухания у границы экрана, в пикселях (16–48)
static const float ssrGrazingMinZ = 0.05f; // при Rv.z ниже этого — начинаем гасить отражение
static const float ssrGrazingMaxZ = 0.25f; // к этому значению — полностью включаем
static const float ssrJitterStrength = 0.5f; // 0..1 — сколько пикселей сдвигаем старт

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

float Hash12(float2 p)
{
    p = frac(p * float2(0.1031, 0.11369));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}

float DepthToViewZ(float d)
{
    // z = (n*f) / (f - d*(f - n))
    float nf = zNear * zFar;
    float fnmn = zFar - zNear;
    return nf * rcp(zFar - d * fnmn);
}

float DepthToViewZ_Fast(float d)   // те же результаты
{
    return depthB * rcp(d - depthA);
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

// === Lettier SSR, адаптация под D3D (z — view distance), без positionTexture ===
struct SSRHit
{
    float2 uv;
    float visibility;
    int hit;
};

float EdgeFadePx(float2 uv)
{
    // расстояние до ближайшего края экрана в пикселях
    float2 dist = min(uv, 1.0 - uv) * screenSize;
    float m = min(dist.x, dist.y);
    return saturate(m / ssrEdgeFadePx);
}

SSRHit TraceSSR_Lettier(float3 Pv, float3 Nv)
{
    SSRHit outv;
    outv.uv = 0.0.xx;
    outv.visibility = 0.0;
    outv.hit = 0;

    // Пивот = отражённое направление (как в статье: reflect(unitPositionFrom, normal))
    float3 unitPositionFrom = normalize(Pv);
    float3 pivot = normalize(reflect(unitPositionFrom, Nv));

    if (pivot.z <= 0.0f)
    {
        SSRHit r;
        r.uv = 0;
        r.visibility = 0;
        r.hit = 0;
        return r;
    }

    // Старт/финиш луча в view
    float3 startView = Pv + pivot * 0.0;
    float3 endView = Pv + pivot * ssrMaxDistanceVS;

    // В экранные фрейм-координаты (пиксели)
    float4 sClip = mul(float4(startView, 1), proj);
    float4 eClip = mul(float4(endView, 1), proj);
    sClip.xyz /= max(kEps, sClip.w);
    eClip.xyz /= max(kEps, eClip.w);
    float2 sFrag = float2((sClip.x * 0.5 + 0.5) * screenSize.x,
                       (-sClip.y * 0.5 + 0.5) * screenSize.y);
    float2 eFrag = float2((eClip.x * 0.5 + 0.5) * screenSize.x,
                       (-eClip.y * 0.5 + 0.5) * screenSize.y);

    // coarse march по экранной линии
    float deltaX = eFrag.x - sFrag.x;
    float deltaY = eFrag.y - sFrag.y;
    float useX = (abs(deltaX) >= abs(deltaY)) ? 1.0 : 0.0;
    float delta = lerp(abs(deltaY), abs(deltaX), useX) * clamp(ssrResolution, 0.0, 1.0);
    float2 incr = float2(deltaX, deltaY) / max(delta, 0.001);

    float search0 = 0.0; // last miss
    float search1 = 0.0; // current

    int hit0 = 0;
    int hit1 = 0;

    float viewDistance = startView.z; // у нас вью-дистанция = z (LH: +Z вперёд)
    float depthDiff = ssrThicknessVS;

    float2 frag = sFrag; // текущая экранная точка (в пикселях)
    float jitter = (Hash12(sFrag) - 0.5) * ssrJitterStrength;
    frag += incr * jitter;

    float2 uv; // текущие uv (0..1)

    // Первый проход: быстрый — шагами по экрану
    int coarseCount = (int) delta;
    for (int i = 0; i < coarseCount; ++i)
    {
        frag += incr;
        uv = frag / screenSize;

        if (any(uv < 0.0) || any(uv > 1.0))
        {
            break;
        }

    // 1) доля пройденного вдоль экранной линии
        search1 = lerp((frag.y - sFrag.y) / deltaY, (frag.x - sFrag.x) / deltaX, useX);
        search1 = saturate(search1);

    // 2) позиция сцены и глубина в этой точке
        float d = ReadDepth(uv);
        float dLin = DepthToViewZ_Fast(d);

    // 3) перспективно-корректная «длина» луча до текущего шага (по Lettier)
        viewDistance = (startView.z * endView.z) / lerp(endView.z, startView.z, search1);

    // 4) сравнение в view-z (толщина — в тех же единицах)
        depthDiff = viewDistance - dLin;

        if (depthDiff > 0.0 && depthDiff < ssrThicknessVS)
        {
            hit0 = 1;
            break;
        }
        else
        {
            search0 = search1;
        }
    }

    search1 = search0 + ((search1 - search0) / 2.0);

    // Второй проход: уточнение (бинарный поиск)
    int refineSteps = hit0 * ssrRefineSteps;
    for (int i = 0; i < refineSteps; ++i)
    {
        float2 fragMix = lerp(sFrag, eFrag, search1);
        uv = fragMix / screenSize;

        if (any(uv < 0.0) || any(uv > 1.0))
        {
            hit1 = 0;
            break;
        }

        float d = ReadDepth(uv);
        float dLin = DepthToViewZ_Fast(d);
        viewDistance = (startView.z * endView.z) / lerp(endView.z, startView.z, search1);
        depthDiff = viewDistance - dLin;

        if (depthDiff > 0.0 && depthDiff < ssrThicknessVS)
        {
            hit1 = 1;
            search1 = search0 + ((search1 - search0) * 0.5);
        }
        else
        {
            float temp = search1;
            search1 = search1 + ((search1 - search0) * 0.5);
            search0 = temp;
        }
    }

    // Видимость (фейды), финальные uv отражения
    float visibility = (float) hit1;
    if (visibility > 0.0)
    {
        float3 positionTo = ReconstructPosVS(uv, ReadDepth(uv));

        // 1 - facing to camera (как в статье: dot(-unitPos, pivot))
        {
            visibility *= (1.0 - max(dot(-unitPositionFrom, pivot), 0.0));
        }
        // близость к найденному «хиту»
        {
            visibility *= (1.0 - clamp(depthDiff / ssrThicknessVS, 0.0, 1.0));
        }
        // дистанционный фейд
        {
            visibility *= (1.0 - clamp(length(positionTo - Pv) / ssrMaxDistanceVS, 0.0, 1.0));
        }
        // выход за экран
        {
            visibility *= ((uv.x < 0.0 || uv.x > 1.0) ? 0.0 : 1.0) * ((uv.y < 0.0 || uv.y > 1.0) ? 0.0 : 1.0);
        }

        {
            visibility *= EdgeFadePx(uv);
			// фейд на скользящих углах (по направлению «pivot», которое мы же использовали для трассировки)
            float grazing = saturate((pivot.z - ssrGrazingMinZ) / (ssrGrazingMaxZ - ssrGrazingMinZ));
            visibility *= grazing;
        }

        {
            visibility = clamp(visibility, 0.0, 1.0);
        }
    }

    outv.uv = uv;
    outv.visibility = visibility;
    outv.hit = (visibility > 0.0) ? 1 : 0;
    return outv;
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

        float doSSR = step(rough, roughCutoff);
        float3 refl;
        SSRHit ssr;
        ssr.hit = 0.0f;
        ssr.uv = float2(0.0f, 0.0f);

        if (doSSR > 0.5f)
        {
	        ssr = TraceSSR_Lettier(Pv, Nv);
        }

        //return float4(ssr.visibility.xxx, 1);

        float3 skyCol = SkyboxTex.SampleLevel(gSmp, Rw, 0).rgb * skyboxIntensity;
        refl = skyCol;

        if (ssr.hit >= 1)
        {
            float3 ssrCol = LightTarget.Sample(gSmp, ssr.uv).rgb;

            float count = 1.0f;
            float separation = 1.5f;
            int size = 3;

            for (int i = -size; i <= size; ++i)
            {
                for (int j = -size; j <= size; ++j)
                {
                    float2 newUV = ssr.uv + (float2(i, j) * separation)/screenSize;
                    newUV = saturate(newUV);
                    ssrCol.rgb += LightTarget.Sample(gSmp, newUV).rgb;

                    count += 1.0;
                }
            }

            ssrCol.rgb /= count;

            refl = lerp(refl, ssrCol, ssr.visibility);
        }

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
